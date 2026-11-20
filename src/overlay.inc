// overlay.inc  (v25) - in-game overlay + menu for the DLSS frame-generation injector
//
//   * one hotkey (default INSERT, cfg: overlaykey=) opens a keyboard menu:  Up/Down = select, Left/Right = change, Enter = toggle/activate, hotkey/Esc = close
//   * HUD lines (each one independently switchable): REAL fps (what the game renders), DLSS-FG output fps, frame time + 1% low
//   * switches: frame generation itself, multiplier, auto-pause threshold, Reflex cap / sleep, camera hold, cut detection, overlay position / size / opacity
//   * every change is written back to SN_DLSSG_cfg.txt (cfgsave.inc)
//
// How it is drawn: the panel is a modern glass card (rounded corners, soft shadow, switches, accent selection) composed on the CPU into a small premultiplied bitmap (text via a GDI coverage mask;
// only when the content changes, ~2x per second), uploaded to a texture and drawn as ONE alpha-blended quad on the swap-chain back buffer right before the game's Present (so it is part of the frame, no extra window, no loss of flip-model presentation).
// It runs on its own command list on the game's direct queue (same pattern as the motion-vector pass) and uses nothing the game owns except the back buffer.
// Any failure switches the overlay off (log line) - frame generation itself is never affected.

static bool g_ovBroken = false;

// ------------------------------------------------------------------ small helpers
static LARGE_INTEGER g_ovQf = {};
static inline double OvNowMs() { if (!g_ovQf.QuadPart) QueryPerformanceFrequency(&g_ovQf); LARGE_INTEGER n; QueryPerformanceCounter(&n); return (double)n.QuadPart * 1000.0 / (double)g_ovQf.QuadPart; }
static bool OvForegroundIsUs() { HWND h = GetForegroundWindow(); if (!h) return false; DWORD pid = 0; GetWindowThreadProcessId(h, &pid); return pid == GetCurrentProcessId(); }

enum { C_TEXT, C_GREEN, C_YELLOW, C_RED, C_GRAY, C_TITLE };
enum OvItem { I_FG, I_PRESET, I_MULT, I_TARGET, I_SHOWFPS, I_SHOWFG, I_SHOWFT, I_SHOWGRAPH, I_SHOWVRAM, I_MINFPS, I_REFLEXMODE, I_CAMHOLD, I_CUT, I_MENUDET, I_DYNRES, I_HUDLESS, I_POS, I_SCALE, I_OPACITY, I_REPORT, I_BUNDLE, I_CLOSE, I_COUNT };   // v27: preset, output cap, hudless, support zip
struct OvGlobals {
    std::atomic<bool> menuOpen{ false }; int sel = 0;
    std::atomic<unsigned> qHead{ 0 }, qTail{ 0 }; unsigned char q[64] = {};
    std::atomic<bool> hookOk{ false }, drawnOnce{ false };
    std::string msg; DWORD msgTick = 0; bool msgBad = false;
    std::string toast; DWORD toastTick = 0; int toastCol = 0;   // v27: short on-screen note for hotkeys
} g_ov;

// ------------------------------------------------------------------ keyboard
static void OvPush(unsigned char vk) { unsigned h = g_ov.qHead.load(); unsigned n = (h + 1) & 63; if (n == g_ov.qTail.load()) return; g_ov.q[h] = vk; g_ov.qHead.store(n); }
static int OvPop() { unsigned t = g_ov.qTail.load(); if (t == g_ov.qHead.load()) return 0; int v = g_ov.q[t]; g_ov.qTail.store((t + 1) & 63); return v; }
static bool OvIsNavKey(unsigned vk) { switch (vk) { case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_RETURN: case VK_ESCAPE: case VK_NUMPAD8: case VK_NUMPAD2: case VK_NUMPAD4: case VK_NUMPAD6: case VK_NUMPAD5: return true; default: return false; } }

static bool g_ovHeld[256], g_ovSwallowed[256];   // hook thread only
static LRESULT CALLBACK OvKbProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && l) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        if (!(k->flags & LLKHF_INJECTED)) {
            unsigned vk = k->vkCode & 0xFF; bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN), up = (w == WM_KEYUP || w == WM_SYSKEYUP);
            if (up) { g_ovHeld[vk] = false; if (g_ovSwallowed[vk]) { g_ovSwallowed[vk] = false; return 1; } }
            else if (down && g_cfg.overlay) {
                bool isOv = (int)vk == g_cfg.overlayKey && vk != 0; bool menu = g_ov.menuOpen.load(); bool isHot = vk != 0 && ((int)vk == g_cfg.fgKey || (int)vk == g_cfg.hudKey);
                if ((isOv || isHot || (menu && OvIsNavKey(vk))) && OvForegroundIsUs()) {
                    if (isOv || isHot) { if (!g_ovHeld[vk]) { g_ovHeld[vk] = true; OvPush((unsigned char)vk); } } else OvPush((unsigned char)vk);
                    if (menu && g_cfg.overlayBlock && !isHot) { g_ovSwallowed[vk] = true; return 1; }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}
static DWORD WINAPI OvInputThread(LPVOID) {
    HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, OvKbProc, g_hSelf, 0);
    if (!hk) { Log("overlay: keyboard hook failed (err %u) -> the overlay key is polled inside Present instead", (unsigned)GetLastError()); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    g_ov.hookOk = true; Log("overlay: keyboard hook installed (overlay key %s)", KeyNameOf(g_cfg.overlayKey));
    MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    UnhookWindowsHookEx(hk); g_ov.hookOk = false; return 0;
}
static void Ov_StartInput() {
    if (!g_cfg.overlay) { Log("overlay disabled in the config (overlay=0)"); return; }
    if (!g_cfg.overlayHook) { Log("overlay: keyboard hook disabled (overlayhook=0), polling GetAsyncKeyState"); return; }
    HANDLE h = CreateThread(nullptr, 0, OvInputThread, nullptr, 0, nullptr); if (h) CloseHandle(h);
}
// fallback when no hook: poll the keys once per frame (a key tapped shorter than one frame at very low fps can be missed)
static void OvPoll() {
    static bool prev[256]; static DWORD nextRep[256]; bool fg = OvForegroundIsUs(); DWORD now = GetTickCount();
    auto chk = [&](unsigned vk, bool repeat) {
        bool d = fg && vk && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
        if (d && !prev[vk]) { OvPush((unsigned char)vk); nextRep[vk] = now + 350; }
        else if (d && repeat && now >= nextRep[vk]) { OvPush((unsigned char)vk); nextRep[vk] = now + 80; }
        prev[vk] = d; };
    chk((unsigned)g_cfg.overlayKey, false); if (g_cfg.fgKey) chk((unsigned)g_cfg.fgKey, false); if (g_cfg.hudKey) chk((unsigned)g_cfg.hudKey, false);
    if (g_ov.menuOpen.load()) { chk(VK_UP, true); chk(VK_DOWN, true); chk(VK_LEFT, true); chk(VK_RIGHT, true); chk(VK_RETURN, false); chk(VK_ESCAPE, false); chk(VK_NUMPAD8, true); chk(VK_NUMPAD2, true); chk(VK_NUMPAD4, true); chk(VK_NUMPAD6, true); chk(VK_NUMPAD5, false); }
}

// ------------------------------------------------------------------ statistics (real fps / 1% low); updated every Present
struct OvStats { LARGE_INTEGER last = {}; float ring[600] = {}; int n = 0, pos = 0; double accMs = 0; int accN = 0; double fps = 0, ft = 0, low1 = 0; double lastUpd = 0; } g_st;
static void OvUpdateStats() {
    LARGE_INTEGER nw; QueryPerformanceCounter(&nw); if (!g_ovQf.QuadPart) QueryPerformanceFrequency(&g_ovQf);
    if (g_st.last.QuadPart) {
        double dt = (double)(nw.QuadPart - g_st.last.QuadPart) * 1000.0 / (double)g_ovQf.QuadPart;
        if (dt > 0.0 && dt < 2000.0) { g_st.ring[g_st.pos] = (float)dt; g_st.pos = (g_st.pos + 1) % 600; if (g_st.n < 600) g_st.n++; g_st.accMs += dt; g_st.accN++; }
    }
    g_st.last = nw;
    if (g_st.accMs >= 500.0 && g_st.accN > 0) {
        g_st.ft = g_st.accMs / g_st.accN; g_st.fps = 1000.0 / g_st.ft; g_st.accMs = 0; g_st.accN = 0;
        std::vector<float> v(g_st.ring, g_st.ring + g_st.n); std::sort(v.begin(), v.end());
        size_t idx = (size_t)((double)v.size() * 0.99); if (idx >= v.size()) idx = v.size() - 1; g_st.low1 = v.empty() ? 0.0 : 1000.0 / std::max(1.0f, v[idx]);
    }
}

// ------------------------------------------------------------------ diagnostics glue
static void DiagRuntimeFill(DiagRuntime& rt) {
    rt.frame = g_frame.load(); rt.warmup = g_cfg.warmup; rt.busyFrames = g_busyFrames.load(); rt.scene3dFrames = g_scene3dFrames.load(); int cf = g_cam.camFrame.load(); rt.camAge = cf >= 0 ? rt.frame - cf : -1;
    rt.fgWanted = g_cfg.fg && g_cfg.stage >= 2; rt.everOn = g_everOn; rt.wasOn = g_wasOn; rt.susp = g_fgSusp; rt.compatBlocked = g_compatBlocked.load(); rt.stage = g_cfg.stage;
    rt.slInit = g_slInitOk; rt.slDev = g_slDevSet; rt.slRuntime = g_slRuntimeOk; rt.haveQueue = g_queue != nullptr; rt.haveDevice = g_device != nullptr;
    rt.depthPinned = g_pinDepth != nullptr; rt.velPinned = g_pinVel != nullptr; rt.velDummy = g_pinVel && g_pinVel == g_dummyVel; rt.camLocked = g_cam.locked.load() != 0;
    rt.mvReady = g_mvPSO && g_mvOutA[0]; rt.mvFailed = g_mvFailed && !g_mvPreIniting.load(); rt.d3d12Swap = g_swapD3D12;
    rt.projSeen = g_cam.projSeen.load(); rt.scanRuns = g_cam.scanRuns.load(); rt.tagRes = g_lastTagRes; rt.constRes = g_lastConstRes; rt.optRes = g_lastOptRes;
    rt.status = g_lastStatus; rt.haveState = g_haveState; rt.ratio = g_fgRatio.load(); rt.baseFps = g_st.fps; rt.maxGen = g_maxGen; rt.minWH = g_stMinWH; rt.vramMB = g_stVramMB;
    rt.projOff = g_cfg.projOff; rt.noaaOff = g_cfg.noAAOff; rt.mvOff = g_cfg.mvOff; rt.effW = g_effW.load(); rt.effH = g_effH.load();
    rt.dispW = g_dispW; rt.dispH = g_dispH; rt.bufCount = g_scBufs; rt.fmt = g_scFmt; rt.swapEffect = g_scEffect; rt.why = g_whyNot; rt.focused = OvForegroundIsUs();
}
// one-time message box when a fatal problem was found but the overlay cannot show it (DX11 game, Streamline not loaded, ...)
static std::atomic<bool> g_noticeThread{ false }, g_boxShown{ false };
static DWORD WINAPI OvNoticeThread(LPVOID) {
    for (int i = 0; i < 40 && !g_ov.drawnOnce.load(); i++) Sleep(500);   // ~20 s for the overlay banner to appear
    DiagVerdict v; { std::lock_guard<std::mutex> lk(g_verdictMx); v = g_verdict; }
    if (v.state == 2 && !g_ov.drawnOnce.load() && g_cfg.msgBox && g_cfg.autoReport && !g_boxShown.exchange(true)) {
        std::wstring path = g_reportPath.empty() ? L"(could not be written)" : g_reportPath;
        std::string r1 = v.reasons.empty() ? v.headline : v.reasons[0]; if (r1.size() > 600) r1.resize(600);
        wchar_t rw[700]; MultiByteToWideChar(CP_UTF8, 0, r1.c_str(), -1, rw, 700);
        std::wstring m = L"DLSS Frame Generation (SN_DLSSG) cannot work in this game / on this PC.\n\nReason:\n" + std::wstring(rw) + L"\n\nA detailed debug report was saved as:\n" + path +
            L"\n\nPlease attach that file to a new GitHub Issue (Issues tab) and write the NAME of the game:\n" L"" SN_ISSUES_URL
            L"\n\nTo stop this message: delete winmm.dll from the game folder, or set msgbox=0 in SN_DLSSG_cfg.txt.";
        MessageBoxW(nullptr, m.c_str(), L"SN_DLSSG - frame generation unavailable", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
    g_noticeThread.store(false); return 0;
}
static void Diag_OnVerdictChanged() {
    DiagVerdict v; { std::lock_guard<std::mutex> lk(g_verdictMx); v = g_verdict; }
    if (v.state == 2) { g_bannerActive = true; g_bannerSeen = false; if (!g_noticeThread.exchange(true)) { HANDLE h = CreateThread(nullptr, 0, OvNoticeThread, nullptr, 0, nullptr); if (h) CloseHandle(h); else g_noticeThread.store(false); } }
    else if (v.state == 1) g_bannerActive = false;
}

// ------------------------------------------------------------------ menu model
static const int kMinFpsSteps[] = { 0, 20, 24, 30, 40, 45, 60 };
static const int kTargetSteps[] = { 0, 60, 72, 90, 100, 120, 144, 165, 180, 240 };   // 1.0.0: target-fps mode
static const int kCapSteps[] = { 0, 30, 40, 45, 60, 72, 90, 120, 144, 165 };
static const int kScaleSteps[] = { 75, 100, 125, 150, 175, 200 };
static const int kOpacitySteps[] = { 40, 60, 80, 100 };
template <size_t N> static int StepCycle(const int(&a)[N], int cur, int dir) {   // next value in the step list (nearest entry to 'cur' is the starting point)
    int bi = 0, bd = 1 << 30; for (size_t i = 0; i < N; i++) { int d = abs(a[i] - cur); if (d < bd) { bd = d; bi = (int)i; } }
    bi = (bi + dir + (int)N) % (int)N; return a[bi];
}
static void OvToast(const std::string& s, int col) { g_ov.toast = s; g_ov.toastTick = GetTickCount(); g_ov.toastCol = col; }
static void OvSay(const std::string& s, bool bad = false) { g_ov.msg = s; g_ov.msgTick = GetTickCount(); g_ov.msgBad = bad; }
static DWORD WINAPI OvReportThread(LPVOID) { DiagWriteReport("manual (menu)"); return 0; }
static void OvCloseMenu() { g_ov.menuOpen = false; CfgSaveSoon(); if (g_bannerSeen.load()) g_bannerActive = false; }
static const int kOutCapSteps[] = { 0, -1, 60, 72, 90, 120, 144, 165, 240 };
static int OutCapCycle(int cur, int dir) { int n = (int)(sizeof kOutCapSteps / sizeof kOutCapSteps[0]), i = 0; for (int k = 0; k < n; k++) if (kOutCapSteps[k] == cur) i = k; return kOutCapSteps[(i + (dir >= 0 ? 1 : n - 1)) % n]; }
static void OvPresetCustom() { if (g_cfg.preset) { g_cfg.preset = 0; CfgSet("preset", "custom"); } }   // a hand-made change to a preset key makes the preset 'custom'
static void OvApplyPreset(int id) {
    g_cfg.preset = id; CfgSet("preset", SnPresetName(id)); if (id == SN_PRESET_CUSTOM) return;
    ApplyPresetValues(id); if (!g_cfg.fgMinFps) g_fgSusp = false;
    CfgSetInt("fgminfps", g_cfg.fgMinFps); CfgSetInt("fgdelay", g_cfg.fgDelay); CfgSetInt("camstale", g_cfg.camStale); CfgSetInt("mult", g_cfg.mult);
    Log("menu: preset %s (fgminfps=%d fgdelay=%d camstale=%d mult=%d)", SnPresetName(id), g_cfg.fgMinFps, g_cfg.fgDelay, g_cfg.camStale, g_cfg.mult);
}
static void OvToggleFg() {
    if (g_acTripped.load()) { OvToast("Frame Generation stays OFF: anti-cheat found", C_RED); return; }
    g_cfg.fg = g_cfg.fg ? 0 : 1; if (g_cfg.fg) g_forceReset = true; Log("hotkey: frame generation = %d (this session only)", g_cfg.fg);
    OvToast(g_cfg.fg ? "Frame Generation: ON" : "Frame Generation: OFF", g_cfg.fg ? C_GREEN : C_YELLOW);
}
static void OvToggleHud() {
    static int sv = 0, sg = 0, sf = 0, sm = 0, sgr = 0; bool any = g_cfg.showFps || g_cfg.showFgFps || g_cfg.showFrameTime || g_cfg.showVram || g_cfg.showGraph;
    if (any) { sv = g_cfg.showFps; sg = g_cfg.showFgFps; sf = g_cfg.showFrameTime; sm = g_cfg.showVram; sgr = g_cfg.showGraph; g_cfg.showFps = g_cfg.showFgFps = g_cfg.showFrameTime = g_cfg.showVram = g_cfg.showGraph = 0; OvToast("HUD: OFF", C_GRAY); }
    else { if (sv || sg || sf || sm || sgr) { g_cfg.showFps = sv; g_cfg.showFgFps = sg; g_cfg.showFrameTime = sf; g_cfg.showVram = sm; g_cfg.showGraph = sgr; } else { g_cfg.showFps = 1; g_cfg.showFgFps = 1; } OvToast("HUD: ON", C_GREEN); }
}
static void OvAdjust(int item, int dir, bool enter) {
    auto tog = [&](int& v, const char* key, const char* name) { v = v ? 0 : 1; CfgSetInt(key, v); Log("menu: %s = %d", name, v); };
    switch (item) {
    case I_FG: if (g_acTripped.load()) { OvSay("Frame generation is blocked: an anti-cheat was found.", true); break; } tog(g_cfg.fg, "fg", "frame generation"); if (g_cfg.fg) g_forceReset = true; break;
    case I_PRESET: OvApplyPreset((g_cfg.preset + (dir >= 0 ? 1 : SN_PRESET_COUNT - 1)) % SN_PRESET_COUNT); break;
    case I_MULT: { int mx = g_maxGen ? (int)g_maxGen + 1 : 2; int m = g_cfg.mult + dir; if (m > mx) m = 2; if (m < 2) m = mx; g_cfg.mult = m; CfgSetInt("mult", m); OvPresetCustom(); Log("menu: multiplier = %d", m); if (g_slRuntimeOk) ApplyReflexOptions("menu"); break; }
    case I_TARGET: g_cfg.targetFps = StepCycle(kTargetSteps, g_cfg.targetFps, dir >= 0 ? 1 : -1); CfgSetInt("targetfps", g_cfg.targetFps); OvPresetCustom(); Log("menu: target fps = %d", g_cfg.targetFps); if (g_slRuntimeOk) ApplyReflexOptions("menu"); break;
    case I_SHOWGRAPH: tog(g_cfg.showGraph, "showgraph", "show frame-time graph"); break;
    case I_SHOWFPS: tog(g_cfg.showFps, "showfps", "show real fps"); break;
    case I_SHOWFG: tog(g_cfg.showFgFps, "showfgfps", "show DLSS-FG fps"); break;
    case I_SHOWFT: tog(g_cfg.showFrameTime, "showframetime", "show frame time"); break;
    case I_SHOWVRAM: tog(g_cfg.showVram, "showvram", "show DLSS-G VRAM"); g_vramReq = true; break;
    case I_MINFPS: g_cfg.fgMinFps = StepCycle(kMinFpsSteps, g_cfg.fgMinFps, dir >= 0 ? 1 : -1); if (!g_cfg.fgMinFps) g_fgSusp = false; CfgSetInt("fgminfps", g_cfg.fgMinFps); OvPresetCustom(); break;
    case I_HUDLESS: { int nv = g_cfg.hudless == 0 ? 2 : g_cfg.hudless == 2 ? 1 : 0; if (dir < 0) nv = g_cfg.hudless == 0 ? 1 : g_cfg.hudless == 1 ? 2 : 0; g_cfg.hudless = nv; CfgSetInt("hudless", nv); if (nv) EnsureDrawHooks(); if (nv == 2) HudCalReset(); Log("menu: hudless = %d", nv); break; }
    case I_REFLEXMODE: { int m = (g_cfg.reflexMode + (dir >= 0 ? 1 : 2)) % 3; g_cfg.reflexMode = m; CfgSet("reflexmode", SnReflexModeName(m)); Log("menu: reflex mode = %s", SnReflexModeName(m));
        if (g_slRuntimeOk) ApplyReflexOptions("menu"); if (m == 0) OvSay("Reflex OFF: Frame Generation needs Reflex markers - if FG stops, switch back to Low latency.", true); break; }
    case I_CAMHOLD: tog(g_cfg.camHold, "camhold", "camera hold"); break;
    case I_CUT: tog(g_cfg.cutDetect, "cutdetect", "cut detection"); break;
    case I_MENUDET: tog(g_cfg.menuDetect, "menudetect", "full-screen menu detection"); break;
    case I_DYNRES: tog(g_cfg.dynRes, "dynres", "dynamic resolution flag"); break;
    case I_POS: g_cfg.overlayPos = (g_cfg.overlayPos + (dir >= 0 ? 1 : 3)) % 4; CfgSetInt("overlaypos", g_cfg.overlayPos); break;
    case I_SCALE: g_cfg.overlayScale = StepCycle(kScaleSteps, g_cfg.overlayScale, dir >= 0 ? 1 : -1); CfgSetInt("overlayscale", g_cfg.overlayScale); break;
    case I_OPACITY: g_cfg.overlayOpacity = StepCycle(kOpacitySteps, g_cfg.overlayOpacity, dir >= 0 ? 1 : -1); CfgSetInt("overlayopacity", g_cfg.overlayOpacity); break;
    case I_REPORT: if (enter) { HANDLE h = CreateThread(nullptr, 0, OvReportThread, nullptr, 0, nullptr); if (h) CloseHandle(h); OvSay("Debug report is being written next to the game exe: " SN_REPORT_NAME); } break;
    case I_BUNDLE: if (enter) { BundleStart(); OvSay("Creating the support zip ..."); } break;
    case I_CLOSE: if (enter) OvCloseMenu(); break;
    }
}
static void OvProcessKeys() {
    if (!g_ov.hookOk.load()) OvPoll();
    while (int vk = OvPop()) {
        if (g_cfg.fgKey && vk == g_cfg.fgKey && vk != g_cfg.overlayKey) { OvToggleFg(); continue; }
        if (g_cfg.hudKey && vk == g_cfg.hudKey && vk != g_cfg.overlayKey) { OvToggleHud(); continue; }
        if (vk == g_cfg.overlayKey) { if (g_ov.menuOpen.load()) OvCloseMenu(); else { g_ov.menuOpen = true; g_bannerSeen = true; Log("overlay menu opened"); } continue; }
        if (!g_ov.menuOpen.load()) continue;
        switch (vk) {
        case VK_UP: case VK_NUMPAD8: g_ov.sel = (g_ov.sel + I_COUNT - 1) % I_COUNT; break;
        case VK_DOWN: case VK_NUMPAD2: g_ov.sel = (g_ov.sel + 1) % I_COUNT; break;
        case VK_LEFT: case VK_NUMPAD4: OvAdjust(g_ov.sel, -1, false); break;
        case VK_RIGHT: case VK_NUMPAD6: OvAdjust(g_ov.sel, +1, false); break;
        case VK_RETURN: case VK_NUMPAD5: OvAdjust(g_ov.sel, +1, true); break;
        case VK_ESCAPE: OvCloseMenu(); break;
        }
    }
}

// ------------------------------------------------------------------ panel content
// OvBuild only describes WHAT is shown (a list of typed lines); OvPanelRender decides how it looks.
enum OvKind { OVK_TEXT, OVK_HUD, OVK_TOAST, OVK_TITLE, OVK_STAT, OVK_SECTION, OVK_ROW, OVK_NOTE, OVK_HINT, OVK_GAP, OVK_DIV, OVK_GRAPH, OVK_ALERT };
enum OvCtl { OVC_NONE, OVC_TOGGLE, OVC_CHOICE, OVC_ACTION };
// t = label / text, v = value (right column), v2 = small dim text left of the value, on = toggle state (OVK_STAT: show a status dot)
struct OvLine { std::string t, v, v2; int col = C_TEXT; bool sel = false; int kind = OVK_TEXT; int ctl = OVC_NONE; bool on = false; };
static OvLine OvMk(int kind, const std::string& t, int col = C_TEXT, const std::string& v = std::string()) { OvLine l; l.kind = kind; l.t = t; l.col = col; l.v = v; return l; }
static void OvPushGraph(std::vector<OvLine>& L) { L.push_back(OvMk(OVK_GRAPH, "", C_GRAY)); }
static void OvWrap(std::vector<OvLine>& out, const std::string& s, int col, size_t width, const char* indent = "", int kind = OVK_NOTE) {
    size_t p = 0; bool first = true;
    while (p < s.size()) { size_t e = std::min(s.size(), p + width); if (e < s.size()) { size_t sp = s.rfind(' ', e); if (sp != std::string::npos && sp > p + width / 2) e = sp; }
        out.push_back(OvMk(kind, (first ? std::string() : std::string(indent)) + s.substr(p, e - p), col)); first = false; p = e; while (p < s.size() && s[p] == ' ') p++; }
}
static std::string OnOff(int v) { return v ? "ON" : "OFF"; }
static void OvBuild(std::vector<OvLine>& L) {
    // ---- compatibility banner (until the user has looked at the menu once)
    DiagVerdict v; { std::lock_guard<std::mutex> lk(g_verdictMx); v = g_verdict; }
    if (g_bannerActive.load() && v.state == 2) {
        L.push_back(OvMk(OVK_ALERT, "DLSS Frame Generation is NOT working here", C_RED));
        OvWrap(L, v.reasons.empty() ? v.headline : v.reasons[0], C_YELLOW, 58);
        if (g_reportSaved.load()) {
            L.push_back(OvMk(OVK_NOTE, "A debug report was saved as:", C_TEXT)); L.push_back(OvMk(OVK_NOTE, "   " SN_REPORT_NAME, C_GREEN));
            OvWrap(L, "Please attach it to a new Issue on the project's GitHub page (Issues tab) and write the NAME of this game.", C_TEXT, 58);
        }
        OvWrap(L, std::string("(If you are still in a menu / loading screen, wait until gameplay.)  Press ") + KeyNameOf(g_cfg.overlayKey) + " to open the menu and dismiss this notice.", C_GRAY, 58, "", OVK_HINT);
    }
    // ---- HUD lines
    bool anyHud = g_cfg.showFps || g_cfg.showFgFps || g_cfg.showFrameTime || g_cfg.showVram || g_cfg.showGraph;
    if (anyHud) {
        if (!L.empty()) L.push_back(OvMk(OVK_GAP, ""));
        if (g_cfg.showFps) { char b[96]; snprintf(b, sizeof b, "%.1f", g_st.fps); L.push_back(OvMk(OVK_HUD, "Real FPS", C_TEXT, b)); }
        if (g_cfg.showFrameTime) { char b[96], c[96]; snprintf(b, sizeof b, "%.2f ms", g_st.ft); snprintf(c, sizeof c, "1%% low %.1f", g_st.low1); OvLine l = OvMk(OVK_HUD, "Frame time", C_TEXT, b); l.v2 = c; L.push_back(l); }
        if (g_cfg.showGraph) OvPushGraph(L);
        if (g_cfg.showVram) {
            char b[96]; OvLine l = OvMk(OVK_HUD, "FG VRAM", C_TEXT);
            if (g_wasOn && g_stVramMB > 0) { snprintf(b, sizeof b, "~%llu MB", g_stVramMB); l.v = b; l.v2 = "DLSS-G"; }
            else if (g_wasOn) { l.v = "measuring ..."; l.col = C_GRAY; }
            else { l.v = "-"; l.v2 = "FG off"; l.col = C_GRAY; }
            L.push_back(l);
        }
        if (g_cfg.showFgFps) {
            char b[128];
            if (!g_slInitOk) { OvLine l = OvMk(OVK_HUD, "DLSS-FG", C_RED, "unavailable"); l.v2 = "Streamline not loaded"; L.push_back(l); }
            else if (g_compatBlocked.load()) { L.push_back(OvMk(OVK_HUD, "DLSS-FG", C_RED, "not supported on this system")); }
            else if (g_wasOn) { double r = g_fgRatio.load(); snprintf(b, sizeof b, "%.1f fps", g_st.fps * r); OvLine l = OvMk(OVK_HUD, "DLSS-FG", C_GREEN, b); snprintf(b, sizeof b, "x%.1f", r); l.v2 = b; L.push_back(l); }
            else { OvLine l = OvMk(OVK_HUD, "DLSS-FG", C_GRAY, "OFF"); l.v2 = g_whyNot[0] ? g_whyNot : "off"; L.push_back(l); }
        }
    }
    if (!g_ov.menuOpen.load() && !g_ov.toast.empty() && GetTickCount() - g_ov.toastTick < 2500) { if (!L.empty()) L.push_back(OvMk(OVK_GAP, "")); L.push_back(OvMk(OVK_TOAST, g_ov.toast, g_ov.toastCol)); }
    // ---- menu
    if (g_ov.menuOpen.load()) {
        if (!L.empty()) L.push_back(OvMk(OVK_DIV, ""));
        L.push_back(OvMk(OVK_TITLE, "DLSS Frame Generation", C_TITLE, "SN_DLSSG " SN_VERSION));
        { DiagRuntime rt; DiagRuntimeFill(rt); std::string gpu; { for (auto& g : g_gpus) if (g.nvidia && !g.software) { gpu = g.name; break; } }
          if (!gpu.empty()) L.push_back(OvMk(OVK_STAT, "GPU", C_TEXT, gpu));
          char b[160];
          if (!g_slInitOk) snprintf(b, sizeof b, "UNAVAILABLE (Streamline not loaded)");
          else if (g_compatBlocked.load()) snprintf(b, sizeof b, "UNSUPPORTED on this system");
          else if (g_wasOn) snprintf(b, sizeof b, "ACTIVE  x%.1f   %.0f -> %.0f fps", g_fgRatio.load(), g_st.fps, g_st.fps * g_fgRatio.load());
          else snprintf(b, sizeof b, "off - %s", g_whyNot[0] ? g_whyNot : "off");
          { OvLine l = OvMk(OVK_STAT, "FG state", g_wasOn ? C_GREEN : (g_compatBlocked.load() || !g_slInitOk) ? C_RED : C_YELLOW, b); l.on = true; L.push_back(l); }
          int col = v.state == 1 ? C_GREEN : v.state == 2 ? C_RED : v.state == 3 ? C_GRAY : C_YELLOW;
          { OvLine l = OvMk(OVK_STAT, "Compat", col, v.state == 1 ? "OK - verified working" : v.state == 2 ? "PROBLEM - see report" : v.state == 3 ? "frame generation switched off" : "checking..."); l.on = true; L.push_back(l); } }
        auto sec = [&](const char* name) { L.push_back(OvMk(OVK_SECTION, name)); };
        auto tgl = [&](int id, const char* label, int on) { OvLine l = OvMk(OVK_ROW, label); l.sel = g_ov.sel == id; l.ctl = OVC_TOGGLE; l.on = on != 0; L.push_back(l); };
        auto cho = [&](int id, const char* label, const std::string& val) { OvLine l = OvMk(OVK_ROW, label, C_TEXT, val); l.sel = g_ov.sel == id; l.ctl = OVC_CHOICE; L.push_back(l); };
        auto act = [&](int id, const char* label) { OvLine l = OvMk(OVK_ROW, label, C_TEXT, "ENTER"); l.sel = g_ov.sel == id; l.ctl = OVC_ACTION; L.push_back(l); };
        static const char* posN[] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
        sec("FRAME GENERATION");
        tgl(I_FG, "Frame Generation", g_cfg.fg);
        { static const char* pn[] = { "Custom", "Quality", "Balanced", "Performance" }; cho(I_PRESET, "Preset", pn[g_cfg.preset & 3]); }
        cho(I_MULT, "Multiplier", g_cfg.targetFps > 0 ? Fmt("AUTO  (now %dx, see target fps)", CurMult()) : Fmt("%dx%s", g_cfg.mult, g_maxGen ? Fmt("   (GPU max %ux)", g_maxGen + 1).c_str() : ""));
        cho(I_TARGET, "Target output fps (auto x)", g_cfg.targetFps ? Fmt("%d fps%s", g_cfg.targetFps, g_maxGen ? Fmt("   (GPU max %ux)", g_maxGen + 1).c_str() : "") : std::string("OFF"));
        sec("HUD");
        tgl(I_SHOWFPS, "Show REAL fps", g_cfg.showFps);
        tgl(I_SHOWFG, "Show DLSS-FG fps", g_cfg.showFgFps);
        tgl(I_SHOWFT, "Show frame time / 1% low", g_cfg.showFrameTime);
        tgl(I_SHOWGRAPH, "Show frame-time graph", g_cfg.showGraph);
        tgl(I_SHOWVRAM, "Show DLSS-G VRAM estimate", g_cfg.showVram);
        sec("PACING");
        cho(I_MINFPS, "Pause FG below base fps", g_cfg.fgMinFps ? Fmt("%d fps", g_cfg.fgMinFps) : std::string("OFF"));
        { static const char* rn[] = { "OFF (not recommended)", "Low latency", "Low latency + Boost" }; cho(I_REFLEXMODE, "Reflex mode", rn[g_cfg.reflexMode % 3]); }
        sec("DETECTION");
        tgl(I_CAMHOLD, "Keep FG when camera is lost", g_cfg.camHold);
        tgl(I_CUT, "Camera-cut detection", g_cfg.cutDetect);
        tgl(I_MENUDET, "Full-screen menu detection", g_cfg.menuDetect);
        tgl(I_DYNRES, "Dynamic resolution (auto-detect)", g_cfg.dynRes);
        { std::string vv = g_cfg.hudless == 0 ? "OFF" : g_cfg.hudless == 1 ? Fmt("ON  (draw %d)", g_cfg.hudlessDraw) : g_hc.phase.load() == 4 ? std::string("AUTO: no clear result (see log)") : Fmt("AUTO: learning %d/6 votes", HudCalVotes()); cho(I_HUDLESS, "HUD-less capture", vv); }
        sec("OVERLAY");
        cho(I_POS, "Overlay position", posN[g_cfg.overlayPos & 3]);
        cho(I_SCALE, "Overlay size", Fmt("%d%%", g_cfg.overlayScale));
        cho(I_OPACITY, "Overlay opacity", Fmt("%d%%", g_cfg.overlayOpacity));
        sec("TOOLS");
        act(I_REPORT, "Write debug report now");
        act(I_BUNDLE, "Create support zip (logs)");
        act(I_CLOSE, "Close menu");
        L.push_back(OvMk(OVK_GAP, ""));
        L.push_back(OvMk(OVK_HINT, std::string("Up/Down select   Left/Right change   Enter act   ") + KeyNameOf(g_cfg.overlayKey) + "/Esc close", C_GRAY));
        if (g_cfg.fgKey || g_cfg.hudKey) L.push_back(OvMk(OVK_HINT, std::string("Hotkeys (this session): ") + (g_cfg.fgKey ? std::string(KeyNameOf(g_cfg.fgKey)) + " = FG on/off   " : std::string()) + (g_cfg.hudKey ? std::string(KeyNameOf(g_cfg.hudKey)) + " = HUD on/off" : std::string()), C_GRAY));
        if (g_cfg.autoSave) { if (g_cfgSaveState.load() < 0) L.push_back(OvMk(OVK_HINT, "Cannot write SN_DLSSG_cfg.txt (read-only folder?)", C_RED)); else L.push_back(OvMk(OVK_HINT, "Changes are saved to SN_DLSSG_cfg.txt automatically", C_GRAY)); }
        if (!g_ov.msg.empty() && GetTickCount() - g_ov.msgTick < 6000) OvWrap(L, g_ov.msg, g_ov.msgBad ? C_RED : C_GREEN, 58);
    }
}

// ------------------------------------------------------------------ panel renderer: GDI text mask + CPU composited "glass" card -> premultiplied BGRA
// The look (rounded glass card, soft shadow, switches, chevrons, graph) is composed on the CPU with small anti-aliased shape helpers;
// GDI is only used to rasterise the text into a white-on-black coverage mask. It all happens only when the content changes (~2x per second,
// 10x per second while the graph is shown), the static glass background is cached per size, nothing runs on the GPU except one textured quad.
struct OvMet { int fs, fsSm, fsTi, rowH, hudH, statH, secH, titleH, hintH, noteH, gapH, divH, graphH, pad, padY, rad, sh; };
static OvMet OvMetrics(int px) {
    OvMet m; m.fs = std::max(9, (int)(px * 0.82 + 0.5)); m.fsSm = std::max(8, (int)(m.fs * 0.84 + 0.5)); m.fsTi = (int)(m.fs * 1.22 + 0.5);
    m.rowH = (int)(m.fs * 1.58 + 0.5); m.hudH = (int)(m.fs * 1.5 + 0.5); m.statH = (int)(m.fs * 1.4 + 0.5); m.secH = (int)(m.fs * 1.8 + 0.5);
    m.titleH = (int)(m.fs * 2.0 + 0.5); m.hintH = (int)(m.fsSm * 1.45 + 0.5); m.noteH = (int)(m.fs * 1.4 + 0.5);
    m.gapH = std::max(3, m.fs / 2); m.divH = m.gapH * 2; m.graphH = (int)(m.fs * 3.4 + 0.5);
    m.pad = (int)(m.fs * 1.0 + 0.5); m.padY = (int)(m.fs * 0.8 + 0.5); m.rad = (int)(m.fs * 0.95 + 0.5); m.sh = (int)(m.fs * 1.5 + 0.5);
    return m;
}
struct OvPanel {
    HDC dc = nullptr; HBITMAP maskBmp = nullptr, oldBmp = nullptr, outBmp = nullptr; void* maskBits = nullptr; void* bits = nullptr;   // bits = final premultiplied BGRA panel (uploaded as the texture)
    HFONT fReg = nullptr, fSemi = nullptr, fCap = nullptr, fHint = nullptr, fTitle = nullptr, oldFont = nullptr;
    int capW = 0, capH = 0, px = 0, reqPx = 0, w = 0, h = 0, sh = 0; size_t hash = 0; bool valid = false;   // w/h include the shadow margin sh
    std::vector<uint32_t> bg; int bgW = 0, bgH = 0, bgFs = 0, bgAlert = -1, bgOp = -1;                        // cached glass background
} g_pn;
static void OvPanelFree() {
    if (g_pn.dc) { if (g_pn.oldBmp) SelectObject(g_pn.dc, g_pn.oldBmp); if (g_pn.oldFont) SelectObject(g_pn.dc, g_pn.oldFont); DeleteDC(g_pn.dc); }
    if (g_pn.maskBmp) DeleteObject(g_pn.maskBmp); if (g_pn.outBmp) DeleteObject(g_pn.outBmp);
    HFONT fs_[5] = { g_pn.fReg, g_pn.fSemi, g_pn.fCap, g_pn.fHint, g_pn.fTitle }; for (HFONT f : fs_) if (f) DeleteObject(f);
    g_pn = OvPanel();
}
static bool OvPanelEnsure(int px) {
    if (g_pn.dc && g_pn.px == px) return true;
    OvPanelFree(); const OvMet m = OvMetrics(px);
    int cw = (px * 40 + 63) & ~63, ch = px * 80; if (cw > 4096) cw = 4096; if (ch > 4096) ch = 4096;
    HDC dc = CreateCompatibleDC(nullptr); if (!dc) return false;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* mb = nullptr; HBITMAP mbmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &mb, nullptr, 0);
    void* ob = nullptr; HBITMAP obmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ob, nullptr, 0);
    if (!mbmp || !mb || !obmp || !ob) { if (mbmp) DeleteObject(mbmp); if (obmp) DeleteObject(obmp); DeleteDC(dc); return false; }
    auto mk = [&](int hgt, int wt) { return CreateFontA(-hgt, 0, 0, 0, wt, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI"); };
    g_pn.fReg = mk(m.fs, FW_NORMAL); g_pn.fSemi = mk(m.fs, FW_SEMIBOLD); g_pn.fCap = mk(m.fsSm, FW_SEMIBOLD); g_pn.fHint = mk(m.fsSm, FW_NORMAL); g_pn.fTitle = mk(m.fsTi, FW_BOLD);
    g_pn.dc = dc; g_pn.maskBmp = mbmp; g_pn.maskBits = mb; g_pn.outBmp = obmp; g_pn.bits = ob;
    g_pn.oldBmp = (HBITMAP)SelectObject(dc, mbmp); g_pn.oldFont = g_pn.fReg ? (HFONT)SelectObject(dc, g_pn.fReg) : nullptr;
    g_pn.capW = cw; g_pn.capH = ch; g_pn.px = px; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255)); return true;
}
static void OvRGB(int c, int& r, int& g, int& b) {
    switch (c) { case C_GREEN: r = 82; g = 224; b = 138; break; case C_YELLOW: r = 250; g = 204; b = 72; break; case C_RED: r = 255; g = 105; b = 97; break;
                 case C_GRAY: r = 142; g = 150; b = 168; break; case C_TITLE: r = 125; g = 190; b = 255; break; default: r = 236; g = 240; b = 247; break; }
}
static std::wstring OvWide(const std::string& s) {
    if (s.empty()) return std::wstring(); int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(0, n), L'\0'); if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
// --- tiny anti-aliased 2D helpers on the premultiplied panel buffer (stride = capW pixels)
static inline void OvBlend(uint32_t* d, int r, int g, int b, float a) {   // "over" with a straight colour + alpha onto a premultiplied pixel
    if (a <= 0.003f) return; if (a > 1.f) a = 1.f;
    uint32_t o = *d; float ia = 1.f - a;
    int na = (int)(((o >> 24) & 255) * ia + 255.f * a + 0.5f), nr = (int)(((o >> 16) & 255) * ia + r * a + 0.5f), ng = (int)(((o >> 8) & 255) * ia + g * a + 0.5f), nb = (int)((o & 255) * ia + b * a + 0.5f);
    *d = ((uint32_t)std::min(na, 255) << 24) | ((uint32_t)std::min(nr, 255) << 16) | ((uint32_t)std::min(ng, 255) << 8) | (uint32_t)std::min(nb, 255);
}
static inline float OvSdRR(float px, float py, float cx, float cy, float hw, float hh, float r) {   // signed distance to a rounded rectangle (negative inside)
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    if (qx <= 0.f && qy <= 0.f) return std::max(qx, qy) - r;
    float ax = std::max(qx, 0.f), ay = std::max(qy, 0.f); return sqrtf(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.f) - r;
}
static void OvRect(int x0, int y0, int x1, int y1, int r, int g, int b, float a) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, g_pn.w); y1 = std::min(y1, g_pn.h); uint32_t* P = (uint32_t*)g_pn.bits;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) OvBlend(&P[(size_t)y * g_pn.capW + x], r, g, b, a);
}
static void OvRR(float x, float y, float w, float h, float r, float th, int cr, int cg, int cb, float a) {   // th <= 0: filled, th > 0: outline of that thickness
    if (w <= 0.f || h <= 0.f || a <= 0.f) return; r = std::min(r, std::min(w, h) * 0.5f);
    int bx0 = std::max(0, (int)floorf(x) - 1), by0 = std::max(0, (int)floorf(y) - 1), bx1 = std::min(g_pn.w, (int)ceilf(x + w) + 1), by1 = std::min(g_pn.h, (int)ceilf(y + h) + 1);
    float cx = x + w * .5f, cy = y + h * .5f, hw = w * .5f, hh = h * .5f; uint32_t* P = (uint32_t*)g_pn.bits;
    for (int yy = by0; yy < by1; yy++) for (int xx = bx0; xx < bx1; xx++) {
        float sd = OvSdRR(xx + .5f, yy + .5f, cx, cy, hw, hh, r); if (th > 0.f) sd = fabsf(sd + th * .5f) - th * .5f;
        float c = .5f - sd; if (c <= 0.f) continue; if (c > 1.f) c = 1.f;
        OvBlend(&P[(size_t)yy * g_pn.capW + xx], cr, cg, cb, a * c);
    }
}
static void OvLn(float x0, float y0, float x1, float y1, float th, int cr, int cg, int cb, float a) {
    int bx0 = std::max(0, (int)floorf(std::min(x0, x1) - th) - 1), by0 = std::max(0, (int)floorf(std::min(y0, y1) - th) - 1);
    int bx1 = std::min(g_pn.w, (int)ceilf(std::max(x0, x1) + th) + 1), by1 = std::min(g_pn.h, (int)ceilf(std::max(y0, y1) + th) + 1);
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy; uint32_t* P = (uint32_t*)g_pn.bits;
    for (int yy = by0; yy < by1; yy++) for (int xx = bx0; xx < bx1; xx++) {
        float px = xx + .5f - x0, py = yy + .5f - y0; float t = l2 > 0.f ? std::min(1.f, std::max(0.f, (px * dx + py * dy) / l2)) : 0.f;
        float ex = px - t * dx, ey = py - t * dy; float c = th * .5f + .5f - sqrtf(ex * ex + ey * ey); if (c <= 0.f) continue; if (c > 1.f) c = 1.f;
        OvBlend(&P[(size_t)yy * g_pn.capW + xx], cr, cg, cb, a * c);
    }
}
// the glass card: soft shadow, dark frosted tint (vertical gradient), diagonal sheen at the top, thin light border (brighter on top). Opacity is baked in here
// (the text stays fully opaque, only the glass gets more / less transparent).
static void OvBuildBg(int w, int h, const OvMet& m, bool alert, int opPct) {
    g_pn.bg.assign((size_t)w * (size_t)h, 0u);
    float op = (float)opPct / 100.f, gx = (float)m.sh, gy = (float)m.sh, gw = (float)(w - 2 * m.sh), gh = (float)(h - 2 * m.sh);
    float r = std::min((float)m.rad, std::min(gw, gh) * 0.5f), cx = gx + gw * .5f, cy = gy + gh * .5f, hw = gw * .5f, hh = gh * .5f, blur = (float)m.sh * 0.95f, soff = (float)m.sh * 0.28f;
    for (int y = 0; y < h; y++) {
        float fy = y + .5f, t = std::min(1.f, std::max(0.f, (fy - gy) / gh));
        float br = 22.f + (11.f - 22.f) * t, bgc = 27.f + (13.f - 27.f) * t, bb = 40.f + (21.f - 40.f) * t;
        if (alert) { br = br * 0.7f + 70.f * 0.3f; bgc = bgc * 0.7f + 18.f * 0.3f; bb = bb * 0.7f + 22.f * 0.3f; }
        float baseA = op * (0.92f + 0.08f * t), sheen0 = std::max(0.f, 1.f - t / 0.5f) * 0.075f, borderA = 0.36f - 0.24f * t;
        float bcr = alert ? 255.f : 255.f, bcg = alert ? 120.f : 255.f, bcb = alert ? 110.f : 255.f; if (alert) borderA += 0.25f;
        uint32_t* row = &g_pn.bg[(size_t)y * (size_t)w];
        for (int x = 0; x < w; x++) {
            float fx = x + .5f, sd = OvSdRR(fx, fy, cx, cy, hw, hh, r);
            if (sd > 0.5f) {   // outside the card: shadow only
                float sd2 = OvSdRR(fx, fy - soff, cx, cy, hw, hh, r), k = 1.f - sd2 / blur; if (k <= 0.f) continue;
                float a = 0.46f * k * k * (0.45f + 0.55f * op); row[x] = (uint32_t)(a * 255.f + 0.5f) << 24; continue;
            }
            float cov = std::min(1.f, 0.5f - sd), nx = (fx - gx) / gw, s = std::max(0.f, sheen0 * (1.f - 0.55f * nx));
            float a = baseA, pr = br * a, pg = bgc * a, pb = bb * a;
            pr = pr * (1.f - s) + 255.f * s; pg = pg * (1.f - s) + 255.f * s; pb = pb * (1.f - s) + 255.f * s; a = a * (1.f - s) + s;
            float bc = std::min(1.f, std::max(0.f, 1.5f - (-sd))), bA = bc * borderA;
            pr = pr * (1.f - bA) + bcr * bA; pg = pg * (1.f - bA) + bcg * bA; pb = pb * (1.f - bA) + bcb * bA; a = a * (1.f - bA) + bA;
            auto q = [&](float v) { int i = (int)(v * cov + 0.5f); return (uint32_t)(i < 0 ? 0 : i > 255 ? 255 : i); };
            row[x] = (q(a * 255.f) << 24) | (q(pr) << 16) | (q(pg) << 8) | q(pb);
        }
    }
    g_pn.bgW = w; g_pn.bgH = h; g_pn.bgFs = m.fs; g_pn.bgAlert = alert ? 1 : 0; g_pn.bgOp = opPct;
}
// 1.0.0: frame-time graph of the last ~120 real frames (newest on the right). Bars: green = normal, yellow = 25 % above the average, red = 70 % above (a visible spike = stutter / "ticking").
// Returns the top of the scale in ms (0 if there is nothing to show yet).
static double OvDrawGraph(int x0, int y0, int x1, int y1, int fs) {
    const int N = 120; int n = std::min(N, g_st.n); int w = x1 - x0, h = y1 - y0; if (w < 20 || h < 8) return 0.0;
    OvRR((float)x0, (float)y0, (float)w, (float)h, fs * 0.4f, 0.f, 0, 0, 0, 0.38f); OvRR((float)x0, (float)y0, (float)w, (float)h, fs * 0.4f, 1.f, 255, 255, 255, 0.07f);
    if (n < 2) return 0.0;
    float v[N]; double sum = 0, mx = 0;
    for (int i = 0; i < n; i++) { int idx = ((g_st.pos - n + i) % 600 + 600) % 600; v[i] = g_st.ring[idx]; sum += v[i]; mx = std::max<double>(mx, v[i]); }
    double avg = sum / n; static const double ladder[] = { 8.4, 16.8, 25.0, 33.4, 50.0, 66.8, 100.0 }; double top = ladder[6]; for (double c : ladder) if (mx * 1.05 <= c) { top = c; break; }
    int ix0 = x0 + 2, ix1 = x1 - 2, iy0 = y0 + 2, iy1 = y1 - 2, iw = ix1 - ix0, ih = iy1 - iy0; if (iw < 10 || ih < 4) return top;
    float step = (float)iw / (float)N;   // bars fill the whole graph width
    for (int i = 0; i < n; i++) {
        int bh = std::max(1, std::min(ih, (int)(std::min<double>(v[i], top) / top * ih + 0.5))); int xr = ix1 - (int)((n - 1 - i) * step + 0.5f), xl = std::max(ix0, xr - std::max(1, (int)step - (step > 2.f ? 1 : 0)));
        int cr, cg, cb; OvRGB(v[i] <= avg * 1.25 ? C_GREEN : v[i] <= avg * 1.7 ? C_YELLOW : C_RED, cr, cg, cb);
        OvRect(xl, iy1 - bh, xr, iy1, cr, cg, cb, 0.80f); OvRect(xl, iy1 - bh, xr, iy1 - bh + 1, std::min(255, cr + 40), std::min(255, cg + 30), std::min(255, cb + 30), 1.f);
    }
    int ya = iy1 - (int)(avg / top * ih + 0.5); if (ya >= iy0 && ya < iy1) OvRect(ix0, ya, ix1, ya + 1, 255, 255, 255, 0.35f);
    return top;
}
// lays out + draws the lines; returns false if nothing to draw
static bool OvPanelRender(const std::vector<OvLine>& L, int px) {
    if (!OvPanelEnsure(px)) return false;
    const OvMet m = OvMetrics(px); HDC dc = g_pn.dc;
    auto tw = [&](HFONT f, const std::string& s) -> int { if (s.empty()) return 0; std::wstring ws = OvWide(s); SelectObject(dc, f); SIZE z = {}; GetTextExtentPoint32W(dc, ws.c_str(), (int)ws.size(), &z); return (int)z.cx; };
    const int chevW = m.fs, swW = (int)(m.fs * 2.1f + .5f), swH = (int)(m.fs * 1.12f + .5f);
    const int chipW = tw(g_pn.fCap, "ENTER") + (int)(m.fs * 1.1f + .5f), chipH = (int)(m.fs * 1.4f + .5f);
    bool menu = false, alert = false; for (auto& l : L) { if (l.kind == OVK_TITLE) menu = true; if (l.kind == OVK_ALERT) alert = true; }
    // ---- pass 1: heights + needed width
    std::vector<int> ys(L.size()), hs(L.size()); int y = m.padY, contentW = 0;
    for (size_t i = 0; i < L.size(); i++) {
        const OvLine& l = L[i]; int h = 0, need = 0;
        switch (l.kind) {
        case OVK_HUD: h = m.hudH; need = tw(g_pn.fReg, l.t) + m.fs * 2 + tw(g_pn.fSemi, l.v) + (l.v2.empty() ? 0 : (int)(m.fs * .7f) + tw(g_pn.fHint, l.v2)); break;
        case OVK_STAT: h = m.statH; need = (int)(m.fs * 1.25f) + tw(g_pn.fReg, l.t) + m.fs * 2 + tw(g_pn.fSemi, l.v); break;
        case OVK_ROW: h = m.rowH; need = tw(g_pn.fReg, l.t) + m.fs * 2 + chevW + (l.ctl == OVC_TOGGLE ? swW : l.ctl == OVC_ACTION ? chipW : tw(g_pn.fSemi, l.v)); break;
        case OVK_SECTION: h = m.secH; need = tw(g_pn.fCap, l.t) + m.fs * 2; break;
        case OVK_TITLE: h = m.titleH; need = 11 + tw(g_pn.fTitle, l.t) + m.fs * 2 + tw(g_pn.fHint, l.v); break;
        case OVK_ALERT: h = m.rowH; need = (int)(m.fs * 1.8f) + tw(g_pn.fSemi, l.t); break;
        case OVK_TOAST: h = m.hudH; need = (int)(m.fs * 1.25f) + tw(g_pn.fSemi, l.t); break;
        case OVK_HINT: h = m.hintH; need = tw(g_pn.fHint, l.t); break;
        case OVK_GRAPH: h = m.graphH; need = m.fs * 14; break;
        case OVK_GAP: h = m.gapH; break;
        case OVK_DIV: h = m.divH; break;
        default: h = m.noteH; need = tw(g_pn.fReg, l.t); break;
        }
        ys[i] = y; hs[i] = h; y += h; contentW = std::max(contentW, need);
    }
    int innerW = std::max(contentW + 2 * m.pad, menu ? m.fs * 30 : m.fs * 15), innerH = y + m.padY;
    int w = std::min(g_pn.capW, innerW + 2 * m.sh), h = std::min(g_pn.capH, innerH + 2 * m.sh); if (w < 8 || h < 8) return false;
    innerW = w - 2 * m.sh; innerH = h - 2 * m.sh;
    g_pn.w = w; g_pn.h = h; g_pn.sh = m.sh;
    // ---- background: cached glass card copied into the (cleared) output, text mask cleared
    GdiFlush(); memset(g_pn.maskBits, 0, (size_t)g_pn.capW * 4 * (size_t)h);
    if (g_pn.bgW != w || g_pn.bgH != h || g_pn.bgFs != m.fs || g_pn.bgAlert != (alert ? 1 : 0) || g_pn.bgOp != g_cfg.overlayOpacity) OvBuildBg(w, h, m, alert, g_cfg.overlayOpacity);
    for (int yy = 0; yy < h; yy++) memcpy((uint32_t*)g_pn.bits + (size_t)yy * g_pn.capW, &g_pn.bg[(size_t)yy * (size_t)w], (size_t)w * 4);
    // ---- pass 2: draw
    struct TOp { int x, y, w, h, r, g, b; float a; }; std::vector<TOp> ops; ops.reserve(L.size() * 3);
    auto txt = [&](HFONT f, const std::string& s, int x, int cy, int align, int r, int g, int b, float a, int spacing) -> int {   // align 0 = left, 1 = right, 2 = centre; (x, cy) = anchor, vertically centred on cy
        if (s.empty()) return 0; std::wstring ws = OvWide(s); SelectObject(dc, f); SetTextCharacterExtra(dc, spacing);
        SIZE z = {}; GetTextExtentPoint32W(dc, ws.c_str(), (int)ws.size(), &z);
        int xx = align == 1 ? x - (int)z.cx : align == 2 ? x - (int)z.cx / 2 : x, yy = cy - (int)z.cy / 2;
        TextOutW(dc, xx, yy, ws.c_str(), (int)ws.size()); SetTextCharacterExtra(dc, 0);
        ops.push_back(TOp{ xx, yy, (int)z.cx + 2, (int)z.cy, r, g, b, a }); return (int)z.cx; };
    const int gx = m.sh, gy = m.sh, L0 = gx + m.pad, R0 = gx + innerW - m.pad;
    for (size_t i = 0; i < L.size(); i++) {
        const OvLine& l = L[i]; const int Y = gy + ys[i], H = hs[i], cy = Y + H / 2; if (Y + H > h - m.sh) continue;
        int cr, cg, cb; OvRGB(l.col, cr, cg, cb);
        switch (l.kind) {
        case OVK_ROW: {
            const bool sel = l.sel;
            if (sel) {   // accent "pill" behind the selected row
                float hx = (float)(gx + m.pad / 2), hw_ = (float)(innerW - m.pad), hy = (float)Y + 1.f, hh_ = (float)H - 2.f;
                OvRR(hx, hy, hw_, hh_, hh_ * 0.32f, 0.f, 96, 165, 250, 0.20f); OvRR(hx, hy, hw_, hh_, hh_ * 0.32f, 1.f, 130, 190, 255, 0.42f);
                OvRR(hx + 2.f, (float)Y + H * 0.26f, 3.f, H * 0.48f, 1.5f, 0.f, 120, 190, 255, 1.f);
            }
            txt(g_pn.fReg, l.t, L0, cy, 0, sel ? 246 : 204, sel ? 249 : 210, sel ? 253 : 222, 1.f, 0);
            const int xr = R0 - chevW;
            if (l.ctl == OVC_TOGGLE) {
                float sx = (float)(xr - swW), sy = (float)cy - swH * .5f, kr = swH * .5f - 2.5f, kx = l.on ? sx + swW - swH * .5f : sx + swH * .5f;
                if (l.on) OvRR(sx, sy, (float)swW, (float)swH, swH * .5f, 0.f, 52, 199, 120, 0.95f); else OvRR(sx, sy, (float)swW, (float)swH, swH * .5f, 0.f, sel ? 110 : 88, sel ? 118 : 95, sel ? 136 : 112, 0.95f);
                OvRR(kx - kr, (float)cy - kr, kr * 2.f, kr * 2.f, kr, 0.f, 255, 255, 255, l.on ? 1.f : 0.88f);
            } else if (l.ctl == OVC_ACTION) {
                OvRR((float)(xr - chipW), (float)cy - chipH * .5f, (float)chipW, (float)chipH, chipH * .3f, 0.f, 255, 255, 255, sel ? 0.18f : 0.08f);
                OvRR((float)(xr - chipW), (float)cy - chipH * .5f, (float)chipW, (float)chipH, chipH * .3f, 1.f, 255, 255, 255, sel ? 0.45f : 0.16f);
                txt(g_pn.fCap, l.v, xr - chipW / 2, cy, 2, 214, 222, 235, 1.f, 0);
            } else {
                const bool off = l.v == "OFF"; int vr = sel ? 246 : 225, vg = sel ? 249 : 230, vb = sel ? 253 : 240; if (off) OvRGB(C_GRAY, vr, vg, vb);
                int vw = txt(g_pn.fSemi, l.v, xr, cy, 1, vr, vg, vb, 1.f, 0);
                if (sel) {   // < value >
                    float cs = m.fs * 0.28f, lx = (float)(xr - vw) - m.fs * 0.75f, rx = (float)xr + m.fs * 0.45f;
                    OvLn(lx + cs, cy - cs * 1.25f, lx, (float)cy, 1.7f, 150, 200, 255, 1.f); OvLn(lx, (float)cy, lx + cs, cy + cs * 1.25f, 1.7f, 150, 200, 255, 1.f);
                    OvLn(rx, cy - cs * 1.25f, rx + cs, (float)cy, 1.7f, 150, 200, 255, 1.f); OvLn(rx + cs, (float)cy, rx, cy + cs * 1.25f, 1.7f, 150, 200, 255, 1.f);
                }
            }
            break; }
        case OVK_SECTION: { int sy = Y + (int)(H * 0.68f); int t_ = txt(g_pn.fCap, l.t, L0, sy, 0, 118, 160, 222, 1.f, 1); int lx0 = L0 + t_ + (int)(m.fs * .7f); if (lx0 < R0) OvRect(lx0, sy, R0, sy + 1, 255, 255, 255, 0.10f); break; }
        case OVK_TITLE: {
            int ty = Y + H / 2 - 1; OvRR((float)L0, (float)ty - m.fsTi * .5f, 4.f, (float)m.fsTi, 2.f, 0.f, 96, 165, 250, 1.f);
            txt(g_pn.fTitle, l.t, L0 + 11, ty, 0, 244, 247, 252, 1.f, 0); txt(g_pn.fHint, l.v, R0, ty + 1, 1, 142, 150, 168, 1.f, 0);
            int ly = Y + H - 3; int span = std::max(1, R0 - L0); for (int x = L0; x < R0; x++) { float t = (float)(x - L0) / (float)span; OvRect(x, ly, x + 1, ly + 1, 96, 165, 250, 0.55f * (1.f - t) * (1.f - t) + 0.04f); }
            break; }
        case OVK_STAT: {
            if (l.on) { float dr = m.fs * .2f, dx = (float)L0 + m.fs * .3f; OvRR(dx - dr * 2.1f, cy - dr * 2.1f, dr * 4.2f, dr * 4.2f, dr * 2.1f, 0.f, cr, cg, cb, 0.18f); OvRR(dx - dr, cy - dr, dr * 2.f, dr * 2.f, dr, 0.f, cr, cg, cb, 1.f); }
            txt(g_pn.fReg, l.t, L0 + (int)(m.fs * 1.25f), cy, 0, 150, 158, 175, 1.f, 0);
            if (l.col == C_TEXT) { cr = 205; cg = 211; cb = 224; } txt(g_pn.fSemi, l.v, R0, cy, 1, cr, cg, cb, 1.f, 0); break; }
        case OVK_HUD: {
            if (l.col == C_TEXT) { cr = 244; cg = 247; cb = 252; }
            txt(g_pn.fReg, l.t, L0, cy, 0, 150, 158, 175, 1.f, 0); int vw = txt(g_pn.fSemi, l.v, R0, cy, 1, cr, cg, cb, 1.f, 0);
            if (!l.v2.empty()) txt(g_pn.fHint, l.v2, R0 - vw - (int)(m.fs * .7f), cy + 1, 1, 128, 136, 154, 1.f, 0);
            break; }
        case OVK_TOAST: { float dr = m.fs * .2f, dx = (float)L0 + m.fs * .3f; OvRR(dx - dr * 2.1f, cy - dr * 2.1f, dr * 4.2f, dr * 4.2f, dr * 2.1f, 0.f, cr, cg, cb, 0.18f); OvRR(dx - dr, cy - dr, dr * 2.f, dr * 2.f, dr, 0.f, cr, cg, cb, 1.f);
                          txt(g_pn.fSemi, l.t, L0 + (int)(m.fs * 1.25f), cy, 0, cr, cg, cb, 1.f, 0); break; }
        case OVK_ALERT: { float ir = m.fs * .62f; OvRR((float)L0, cy - ir, ir * 2.f, ir * 2.f, ir, 0.f, 255, 105, 97, 1.f); txt(g_pn.fSemi, "!", L0 + (int)ir, cy, 2, 255, 255, 255, 1.f, 0);
                          txt(g_pn.fSemi, l.t, L0 + (int)(ir * 2.f + m.fs * .55f), cy, 0, 255, 120, 110, 1.f, 0); break; }
        case OVK_HINT: if (l.col == C_GRAY) { cr = 128; cg = 136; cb = 154; } txt(g_pn.fHint, l.t, L0, cy, 0, cr, cg, cb, 1.f, 0); break;
        case OVK_DIV: OvRect(gx + m.pad / 2, cy, gx + innerW - m.pad / 2, cy + 1, 255, 255, 255, 0.10f); break;
        case OVK_GRAPH: {
            int g0 = Y + 2, g1 = Y + H - 2; double top = OvDrawGraph(L0, g0, R0, g1, m.fs);
            if (top > 0.0) { char tb[32]; snprintf(tb, sizeof tb, "%.0f ms", top); txt(g_pn.fHint, tb, L0 + 6, g0 + (int)(m.fsSm * 0.9f), 0, 138, 146, 164, 0.95f, 0); }
            break; }
        case OVK_GAP: break;
        default: if (l.col == C_TEXT) { cr = 214; cg = 219; cb = 230; } txt(g_pn.fReg, l.t, L0, cy, 0, cr, cg, cb, 1.f, 0); break;   // OVK_TEXT / OVK_NOTE
        }
    }
    // ---- composite the text: coverage mask (white on black) tinted per run, drawn over the card
    GdiFlush();
    static float cov[256]; static bool covInit = false; if (!covInit) { for (int i = 0; i < 256; i++) cov[i] = powf((float)i / 255.f, 0.85f); covInit = true; }
    const uint8_t* mk = (const uint8_t*)g_pn.maskBits;
    for (const TOp& o : ops) {
        int x0 = std::max(0, o.x), x1 = std::min(w, o.x + o.w), y0 = std::max(0, o.y), y1 = std::min(h, o.y + o.h);
        for (int yy = y0; yy < y1; yy++) {
            const uint32_t* mr = (const uint32_t*)(mk + (size_t)yy * g_pn.capW * 4); uint32_t* pr = (uint32_t*)g_pn.bits + (size_t)yy * g_pn.capW;
            for (int xx = x0; xx < x1; xx++) { uint32_t mp = mr[xx]; if (!(mp & 0xFFFFFFu)) continue; OvBlend(&pr[xx], o.r, o.g, o.b, cov[(mp >> 8) & 255] * o.a); }
        }
    }
    g_pn.valid = true; return true;
}

// ------------------------------------------------------------------ D3D12 renderer
static const char* kOvHlsl =
"cbuffer C : register(b0) { float4 rect; float2 scr; float alpha; float gain; };\n"
"Texture2D<float4> T : register(t0);\n"
"struct VO { float4 p : SV_Position; };\n"
"VO vs(uint id : SV_VertexID) {\n"
"    float2 c = float2(id & 1, id >> 1);\n"
"    float2 px = rect.xy + c * rect.zw;\n"
"    VO o; o.p = float4(px.x / scr.x * 2.0 - 1.0, 1.0 - px.y / scr.y * 2.0, 0.0, 1.0); return o;\n"
"}\n"
"float4 ps(VO i) : SV_Target {\n"
"    int2 q = int2(i.p.xy - rect.xy);\n"
"    float4 t = T.Load(int3(q, 0));\n"
"    return float4(t.rgb * gain * alpha, t.a * alpha);\n"
"}\n";
static const int OV_RING = 6, OV_UP = 3;
static ID3D12RootSignature* g_ovRS = nullptr; static std::map<int, ID3D12PipelineState*> g_ovPso; static std::mutex g_ovPsoMx;
static std::vector<uint8_t> g_ovVs, g_ovPs;
static std::atomic<int> g_ovBuild{ 0 };   // 0 idle, 1 worker running, -1 failed for good (shader compile / RS)
static ID3D12DescriptorHeap* g_ovSrvHeap = nullptr; static ID3D12DescriptorHeap* g_ovRtvHeap = nullptr; static UINT g_ovRtvInc = 0;
static ID3D12Resource* g_ovTex = nullptr; static int g_ovTexW = 0, g_ovTexH = 0; static bool g_ovTexValid = false;
static ID3D12Resource* g_ovUpBuf[OV_UP] = {}; static UINT64 g_ovUpVal[OV_UP] = {}; static UINT64 g_ovUpSize[OV_UP] = {}; static int g_ovUpPos = 0;
static ID3D12CommandAllocator* g_ovAl[OV_RING] = {}; static ID3D12GraphicsCommandList* g_ovCl[OV_RING] = {}; static UINT64 g_ovVal[OV_RING] = {}; static int g_ovRingPos = 0;
static ID3D12Fence* g_ovFence = nullptr; static UINT64 g_ovFenceNext = 0;
static size_t g_ovUploadedHash = 0; static int g_ovUploadedPx = 0; static bool g_ovNeedUpload = true;
static int g_ovFail = 0;

static DXGI_FORMAT OvRtvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM; case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM; case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM; case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT: return f;
    default: return DXGI_FORMAT_UNKNOWN; }
}
static DWORD WINAPI OvBuildThread(LPVOID p) {
    DXGI_FORMAT fmt = (DXGI_FORMAT)(INT_PTR)p; DWORD t0 = GetTickCount();
    try {
        ID3D12Device* dev = g_device; if (!dev) { g_ovBuild = 0; return 0; }
        if (!g_ovRS) {
            HMODULE m12 = GetModuleHandleW(L"d3d12.dll"); HMODULE mc = LoadLibraryW(L"d3dcompiler_47.dll");
            typedef HRESULT(WINAPI* PFN_SRS)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
            typedef HRESULT(WINAPI* PFN_Comp)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            auto pSRS = m12 ? (PFN_SRS)GetProcAddress(m12, "D3D12SerializeRootSignature") : nullptr; auto pComp = mc ? (PFN_Comp)GetProcAddress(mc, "D3DCompile") : nullptr;
            if (!pSRS || !pComp) { Log("overlay: d3dcompiler_47.dll / D3D12SerializeRootSignature missing -> overlay disabled"); g_ovBuild = -1; return 0; }
            ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* err = nullptr;
            HRESULT hr = pComp(kOvHlsl, strlen(kOvHlsl), "ov.hlsl", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
            if (FAILED(hr)) { Log("overlay: VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); g_ovBuild = -1; return 0; } if (err) { err->Release(); err = nullptr; }
            hr = pComp(kOvHlsl, strlen(kOvHlsl), "ov.hlsl", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
            if (FAILED(hr)) { Log("overlay: PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); vs->Release(); g_ovBuild = -1; return 0; } if (err) { err->Release(); err = nullptr; }
            D3D12_DESCRIPTOR_RANGE rg = {}; rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1; rg.BaseShaderRegister = 0; rg.OffsetInDescriptorsFromTableStart = 0;
            D3D12_ROOT_PARAMETER rp[2] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 8; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd = {}; rsd.NumParameters = 2; rsd.pParameters = rp; rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
            ID3DBlob* rb = nullptr; hr = pSRS(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &err);
            if (FAILED(hr) || !rb) { Log("overlay: root signature serialisation failed"); vs->Release(); ps->Release(); g_ovBuild = -1; return 0; } if (err) err->Release();
            ID3D12RootSignature* rs = nullptr; hr = dev->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&rs); rb->Release();
            if (FAILED(hr) || !rs) { Log("overlay: CreateRootSignature failed 0x%08X", (unsigned)hr); vs->Release(); ps->Release(); g_ovBuild = -1; return 0; }
            g_ovVs.assign((uint8_t*)vs->GetBufferPointer(), (uint8_t*)vs->GetBufferPointer() + vs->GetBufferSize()); g_ovPs.assign((uint8_t*)ps->GetBufferPointer(), (uint8_t*)ps->GetBufferPointer() + ps->GetBufferSize());
            vs->Release(); ps->Release(); g_ovRS = rs;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = g_ovRS; pd.VS = { g_ovVs.data(), g_ovVs.size() }; pd.PS = { g_ovPs.data(), g_ovPs.size() };
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0]; b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD; b.LogicOp = D3D12_LOGIC_OP_NOOP; b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu; pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.StencilEnable = FALSE; pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.FrontFace.StencilFunc = pd.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.FrontFace.StencilFailOp = pd.DepthStencilState.FrontFace.StencilDepthFailOp = pd.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt; pd.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr; HRESULT hr = dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso);
        if (FAILED(hr) || !pso) { Log("overlay: CreateGraphicsPipelineState(format %d) failed 0x%08X -> overlay disabled", (int)fmt, (unsigned)hr); g_ovBuild = -1; return 0; }
        { std::lock_guard<std::mutex> lk(g_ovPsoMx); g_ovPso[(int)fmt] = pso; }
        Log("overlay: renderer ready for back buffer format %d (%u ms)", (int)fmt, (unsigned)(GetTickCount() - t0));
    } catch (...) { Log("overlay: exception while building the renderer"); g_ovBuild = -1; return 0; }
    g_ovBuild = 0; return 0;
}
static ID3D12PipelineState* OvGetPso(DXGI_FORMAT fmt) {
    { std::lock_guard<std::mutex> lk(g_ovPsoMx); auto it = g_ovPso.find((int)fmt); if (it != g_ovPso.end()) return it->second; }
    int exp = 0; if (g_ovBuild.compare_exchange_strong(exp, 1)) { HANDLE h = CreateThread(nullptr, 0, OvBuildThread, (LPVOID)(INT_PTR)fmt, 0, nullptr); if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); } else g_ovBuild = 0; }
    return nullptr;
}
static bool OvEnsureGpu(int needW, int needH) {
    if (!g_ovFence && FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_ovFence))) return false;
    if (!g_ovRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 8; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_ovRtvHeap))) return false; g_ovRtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if (!g_ovSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_ovSrvHeap))) return false;
    }
    if (!g_ovTex || g_ovTexW != needW || g_ovTexH != needH) {   // (re)create the panel texture (size depends on the font size)
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = (UINT64)needW; rd.Height = (UINT)needH; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource* t = nullptr; HRESULT hr = (oCreateCommitted ? oCreateCommitted(g_device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&t)
                                                                : g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&t));
        if (FAILED(hr) || !t) { Log("overlay: panel texture %dx%d failed 0x%08X", needW, needH, (unsigned)hr); return false; }
        DeferResRelease(g_ovTex); for (int i = 0; i < OV_UP; i++) { DeferResRelease(g_ovUpBuf[i]); g_ovUpBuf[i] = nullptr; g_ovUpSize[i] = 0; }
        g_ovTex = t; g_ovTexW = needW; g_ovTexH = needH; g_ovTexValid = false; g_ovNeedUpload = true;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(t, &sv, CpuStart(g_ovSrvHeap));
    }
    return true;
}
// one overlay frame: optionally upload the panel, draw one quad on the back buffer. Returns false if skipped (nothing was submitted).
static bool OvRecordAndSubmit(IDXGISwapChain* sc, bool upload) {
    IDXGISwapChain3* s3 = nullptr; if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&s3)) || !s3) return false;
    UINT idx = s3->GetCurrentBackBufferIndex(); s3->Release();
    ID3D12Resource* bb = nullptr; if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D12Resource), (void**)&bb)) || !bb) return false;
    struct Rel { ID3D12Resource* r; ~Rel() { if (r) r->Release(); } } rel{ bb };
    D3D12_RESOURCE_DESC bd = ResDesc(bb); if (bd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !(bd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) return false;
    DXGI_FORMAT rtvFmt = OvRtvFormat(bd.Format); if (rtvFmt == DXGI_FORMAT_UNKNOWN) { static bool once = false; if (!once) { once = true; Log("overlay: back buffer format %d is not supported by the overlay renderer", (int)bd.Format); } return false; }
    ID3D12PipelineState* pso = OvGetPso(rtvFmt); if (!pso || !g_ovRS) return false;
    // command list slot
    int s = g_ovRingPos % OV_RING;
    if (!g_ovAl[s]) {
        ID3D12CommandAllocator* a = nullptr; ID3D12GraphicsCommandList* c = nullptr;
        if (SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a)) && SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&c))) { c->Close(); g_ovAl[s] = a; g_ovCl[s] = c; g_ovVal[s] = 0; }
        else { if (c) c->Release(); if (a) a->Release(); return false; }
    }
    if (g_ovFence->GetCompletedValue() < g_ovVal[s]) return false;   // GPU still busy with this slot: skip this frame (overlay repeats next frame)
    // upload slot
    int us = -1; UINT64 rowPitch = ((UINT64)g_pn.w * 4 + 255) & ~255ull, upSize = rowPitch * (UINT64)g_pn.h;
    if (upload && g_pn.valid) {
        for (int k = 0; k < OV_UP; k++) { int c = (g_ovUpPos + k) % OV_UP; if (g_ovFence->GetCompletedValue() >= g_ovUpVal[c]) { us = c; break; } }
        if (us >= 0 && (!g_ovUpBuf[us] || g_ovUpSize[us] < upSize)) {
            UINT64 want = ((UINT64)g_ovTexW * 4 + 255) & ~255ull; want *= (UINT64)g_ovTexH;
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = want; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* ub = nullptr; HRESULT hr = (oCreateCommitted ? oCreateCommitted(g_device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&ub)
                                                                      : g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&ub));
            if (FAILED(hr) || !ub) { Log("overlay: upload buffer failed 0x%08X", (unsigned)hr); us = -1; } else { DeferResRelease(g_ovUpBuf[us]); g_ovUpBuf[us] = ub; g_ovUpSize[us] = want; }
        }
        if (us >= 0) {
            void* map = nullptr; D3D12_RANGE none = { 0, 0 };
            if (SUCCEEDED(g_ovUpBuf[us]->Map(0, &none, &map)) && map) {
                const BYTE* src = (const BYTE*)g_pn.bits; BYTE* dst = (BYTE*)map; size_t srcPitch = (size_t)g_pn.capW * 4;
                for (int y = 0; y < g_pn.h; y++) memcpy(dst + (size_t)y * rowPitch, src + (size_t)y * srcPitch, (size_t)g_pn.w * 4);
                D3D12_RANGE wr = { 0, (SIZE_T)upSize }; g_ovUpBuf[us]->Unmap(0, &wr);
            } else us = -1;
        }
    }
    if (!g_ovTexValid && us < 0) return false;   // nothing to show yet
    ID3D12CommandAllocator* al = g_ovAl[s]; ID3D12GraphicsCommandList* cl = g_ovCl[s];
    if (FAILED(al->Reset()) || FAILED(cl->Reset(al, nullptr))) return false;
    g_ovRingPos++;
    D3D12_RESOURCE_BARRIER bar[2] = {};
    if (us >= 0) {
        bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar[0].Transition.pResource = g_ovTex; bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        oResBarrier(cl, 1, bar);
        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl_ = {}; dl.pResource = g_ovTex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
        sl_.pResource = g_ovUpBuf[us]; sl_.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sl_.PlacedFootprint.Offset = 0; sl_.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sl_.PlacedFootprint.Footprint.Width = (UINT)g_pn.w; sl_.PlacedFootprint.Footprint.Height = (UINT)g_pn.h; sl_.PlacedFootprint.Footprint.Depth = 1; sl_.PlacedFootprint.Footprint.RowPitch = (UINT)rowPitch;
        D3D12_BOX box = { 0, 0, 0, (UINT)g_pn.w, (UINT)g_pn.h, 1 };
        if (oCopyTex) oCopyTex(cl, &dl, 0, 0, 0, &sl_, &box); else cl->CopyTextureRegion(&dl, 0, 0, 0, &sl_, &box);
        bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; oResBarrier(cl, 1, bar);
        g_ovTexValid = true; g_ovNeedUpload = false; g_ovUploadedHash = g_pn.hash; g_ovUploadedPx = g_pn.px; g_ovUpVal[us] = g_ovFenceNext + 1; g_ovUpPos = (us + 1) % OV_UP;
    }
    // position of the panel
    float W = (float)bd.Width, H = (float)bd.Height; float mg = std::max(10.f, (float)g_pn.px * 0.8f); float pw = (float)g_pn.w, ph = (float)g_pn.h; float sh = (float)g_pn.sh, x, y;   // pw/ph include the soft shadow margin 'sh' around the glass card
    switch (g_cfg.overlayPos & 3) { case 1: x = W - pw - mg + sh; y = mg - sh; break; case 2: x = mg - sh; y = H - ph - mg + sh; break; case 3: x = W - pw - mg + sh; y = H - ph - mg + sh; break; default: x = mg - sh; y = mg - sh; }
    x = floorf(x); y = floorf(y);
    // draw
    D3D12_RESOURCE_BARRIER b0 = {}; b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b0.Transition.pResource = bb; b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b0.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b0.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    oResBarrier(cl, 1, &b0);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = CpuStart(g_ovRtvHeap); rtv.ptr += (SIZE_T)(idx % 8) * g_ovRtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rvd = {}; rvd.Format = rtvFmt; rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    if (oCreateRTV) oCreateRTV(g_device, bb, &rvd, rtv); else g_device->CreateRenderTargetView(bb, &rvd, rtv);
    ID3D12DescriptorHeap* hh[1] = { g_ovSrvHeap }; cl->SetDescriptorHeaps(1, hh);
    cl->SetGraphicsRootSignature(g_ovRS); cl->SetPipelineState(pso);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D12_VIEWPORT vp = { 0.f, 0.f, W, H, 0.f, 1.f }; D3D12_RECT sr = { 0, 0, (LONG)bd.Width, (LONG)bd.Height };
    if (oRSSetVP) oRSSetVP(cl, 1, &vp); else cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (oOMSetRT) oOMSetRT(cl, 1, &rtv, FALSE, nullptr); else cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    struct { float r[4]; float scr[2]; float alpha; float gain; } k = { { x, y, pw, ph }, { W, H }, 1.0f, rtvFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2.5f : 1.0f };
    cl->SetGraphicsRoot32BitConstants(0, 8, &k, 0); cl->SetGraphicsRootDescriptorTable(1, GpuStart(g_ovSrvHeap));
    if (oDrawInst) oDrawInst(cl, 4, 1, 0, 0); else cl->DrawInstanced(4, 1, 0, 0);
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b0.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT; oResBarrier(cl, 1, &b0);
    cl->Close();
    ID3D12CommandList* lists[1] = { cl }; oExec(g_queue, 1, lists);
    g_ovVal[s] = ++g_ovFenceNext; g_queue->Signal(g_ovFence, g_ovVal[s]);
    return true;
}

// ------------------------------------------------------------------ per-frame entry
static void OvDraw(IDXGISwapChain* sc) {
    if (!g_device || !g_queue || !g_swapD3D12 || !oExec || !oResBarrier || !sc) return;
    std::vector<OvLine> L; OvBuild(L);
    if (L.empty()) return;
    // font size from the back-buffer height
    double fh = g_dispH ? (double)g_dispH / 1080.0 : 1.0; int px = (int)(18.0 * fh * (double)g_cfg.overlayScale / 100.0 + 0.5); px = std::min(64, std::max(12, px));
    std::string key; for (auto& l : L) { key += l.t; key += '\x01'; key += l.v; key += '\x01'; key += l.v2; key += (char)('0' + l.col); key += l.sel ? '*' : '.'; key += (char)('a' + l.kind); key += (char)('a' + l.ctl); key += l.on ? '1' : '0'; if (l.kind == OVK_GRAPH) key += std::to_string((long long)(OvNowMs() / 100.0)); key += '\n'; }   // 1.0.0: a graph is redrawn 10x per second
    key += std::to_string(g_cfg.overlayOpacity);
    size_t h = std::hash<std::string>()(key) ^ ((size_t)px << 40);
    if (h != g_pn.hash || !g_pn.valid || g_pn.reqPx != px) {
        int use = px;   // a tall menu on a small screen is shrunk until it fits (the user's size setting itself stays untouched)
        for (int pass = 0; pass < 3; pass++) {
            if (!OvPanelRender(L, use)) return;
            double mg = std::max(10.0, use * 0.8), availH = (g_dispH ? (double)g_dispH : 1080.0) - 2.0 * mg, inner = (double)(g_pn.h - 2 * g_pn.sh);
            if (inner <= availH || use <= 10) break;
            use = std::max(10, std::min(use - 1, (int)((double)use * availH / inner)));
        }
        g_pn.hash = h; g_pn.reqPx = px; g_ovNeedUpload = true;
    }
    if (!OvEnsureGpu(g_pn.capW, g_pn.capH)) { if (++g_ovFail >= 5) { g_ovBroken = true; Log("overlay: GPU resources could not be created -> overlay disabled"); } return; }
    if (g_ovBuild.load() < 0) { g_ovBroken = true; return; }
    bool ok = OvRecordAndSubmit(sc, g_ovNeedUpload);
    if (ok) { if (!g_ov.drawnOnce.exchange(true)) Log("overlay: first frame drawn"); }
}
static void Ov_OnPresent(IDXGISwapChain* sc) {
    OvUpdateStats();
    int fr = g_frame.load();
    if (g_cfg.overlay) OvProcessKeys();
    if ((fr % 30) == 0) DiagEvaluate(false);
    CfgPoll();
    { std::string bm; bool bb = false; if (BundlePoll(bm, bb)) OvSay(bm, bb); }
    if (fr == 5) { try { DiagPreflight(); } catch (...) {} }
    if (!g_cfg.overlay || g_ovBroken) return;
    OvDraw(sc);
}
