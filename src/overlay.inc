// overlay.inc  (v25) - in-game overlay + menu for the DLSS frame-generation injector
//
//   * one hotkey (default INSERT, cfg: overlaykey=) opens a keyboard menu:  Up/Down = select, Left/Right = change, Enter = toggle/activate, hotkey/Esc = close
//   * HUD lines (each one independently switchable): REAL fps (what the game renders), DLSS-FG output fps, frame time + 1% low
//   * switches: frame generation itself, multiplier, auto-pause threshold, Reflex cap / sleep, camera hold, cut detection, overlay position / size / opacity
//   * every change is written back to SN_DLSSG_cfg.txt (cfgsave.inc)
//
// How it is drawn: the panel text is rendered with GDI into a small memory bitmap (only when the text changes, ~2x per second), uploaded to a texture and drawn as ONE
// alpha-blended quad on the swap-chain back buffer right before the game's Present (so it is part of the frame, no extra window, no loss of flip-model presentation).
// It runs on its own command list on the game's direct queue (same pattern as the motion-vector pass) and uses nothing the game owns except the back buffer.
// Any failure switches the overlay off (log line) - frame generation itself is never affected.

static bool g_ovBroken = false;

// ------------------------------------------------------------------ small helpers
static LARGE_INTEGER g_ovQf = {};
static inline double OvNowMs() { if (!g_ovQf.QuadPart) QueryPerformanceFrequency(&g_ovQf); LARGE_INTEGER n; QueryPerformanceCounter(&n); return (double)n.QuadPart * 1000.0 / (double)g_ovQf.QuadPart; }
static bool OvForegroundIsUs() { HWND h = GetForegroundWindow(); if (!h) return false; DWORD pid = 0; GetWindowThreadProcessId(h, &pid); return pid == GetCurrentProcessId(); }

enum { C_TEXT, C_GREEN, C_YELLOW, C_RED, C_GRAY, C_TITLE };
enum OvItem { I_FG, I_PRESET, I_MULT, I_SHOWFPS, I_SHOWFG, I_SHOWFT, I_SHOWVRAM, I_MINFPS, I_CAP, I_OUTCAP, I_REFLEX, I_REFLEXMODE, I_CAMHOLD, I_CUT, I_MENUDET, I_DYNRES, I_HUDLESS, I_POS, I_SCALE, I_OPACITY, I_REPORT, I_BUNDLE, I_CLOSE, I_COUNT };   // v27: preset, output cap, hudless, support zip
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
    rt.frame = g_frame.load(); rt.warmup = g_cfg.warmup; rt.busyFrames = g_busyFrames.load(); rt.scene3dFrames = g_scene3dFrames.load(); int cf = g_camFrame.load(); rt.camAge = cf >= 0 ? rt.frame - cf : -1;
    rt.fgWanted = g_cfg.fg && g_cfg.stage >= 2; rt.everOn = g_everOn; rt.wasOn = g_wasOn; rt.susp = g_fgSusp; rt.compatBlocked = g_compatBlocked.load(); rt.stage = g_cfg.stage;
    rt.slInit = g_slInitOk; rt.slDev = g_slDevSet; rt.slRuntime = g_slRuntimeOk; rt.haveQueue = g_queue != nullptr; rt.haveDevice = g_device != nullptr;
    rt.depthPinned = g_pinDepth != nullptr; rt.velPinned = g_pinVel != nullptr; rt.velDummy = g_pinVel && g_pinVel == g_dummyVel; rt.camLocked = g_camLocked.load() != 0;
    rt.mvReady = g_mvPSO && g_mvOutA[0]; rt.mvFailed = g_mvFailed && !g_mvPreIniting.load(); rt.d3d12Swap = g_swapD3D12;
    rt.projSeen = g_projSeen.load(); rt.scanRuns = g_scanRuns.load(); rt.tagRes = g_lastTagRes; rt.constRes = g_lastConstRes; rt.optRes = g_lastOptRes;
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
    static int sv = 0, sg = 0, sf = 0, sm = 0; bool any = g_cfg.showFps || g_cfg.showFgFps || g_cfg.showFrameTime || g_cfg.showVram;
    if (any) { sv = g_cfg.showFps; sg = g_cfg.showFgFps; sf = g_cfg.showFrameTime; sm = g_cfg.showVram; g_cfg.showFps = g_cfg.showFgFps = g_cfg.showFrameTime = g_cfg.showVram = 0; OvToast("HUD: OFF", C_GRAY); }
    else { if (sv || sg || sf || sm) { g_cfg.showFps = sv; g_cfg.showFgFps = sg; g_cfg.showFrameTime = sf; g_cfg.showVram = sm; } else { g_cfg.showFps = 1; g_cfg.showFgFps = 1; } OvToast("HUD: ON", C_GREEN); }
}
static void OvAdjust(int item, int dir, bool enter) {
    auto tog = [&](int& v, const char* key, const char* name) { v = v ? 0 : 1; CfgSetInt(key, v); Log("menu: %s = %d", name, v); };
    switch (item) {
    case I_FG: if (g_acTripped.load()) { OvSay("Frame generation is blocked: an anti-cheat was found.", true); break; } tog(g_cfg.fg, "fg", "frame generation"); if (g_cfg.fg) g_forceReset = true; break;
    case I_PRESET: OvApplyPreset((g_cfg.preset + (dir >= 0 ? 1 : SN_PRESET_COUNT - 1)) % SN_PRESET_COUNT); break;
    case I_MULT: { int mx = g_maxGen ? (int)g_maxGen + 1 : 2; int m = g_cfg.mult + dir; if (m > mx) m = 2; if (m < 2) m = mx; g_cfg.mult = m; CfgSetInt("mult", m); OvPresetCustom(); Log("menu: multiplier = %d", m); if (g_slRuntimeOk) ApplyReflexOptions("menu"); break; }
    case I_SHOWFPS: tog(g_cfg.showFps, "showfps", "show real fps"); break;
    case I_SHOWFG: tog(g_cfg.showFgFps, "showfgfps", "show DLSS-FG fps"); break;
    case I_SHOWFT: tog(g_cfg.showFrameTime, "showframetime", "show frame time"); break;
    case I_SHOWVRAM: tog(g_cfg.showVram, "showvram", "show DLSS-G VRAM"); g_vramReq = true; break;
    case I_MINFPS: g_cfg.fgMinFps = StepCycle(kMinFpsSteps, g_cfg.fgMinFps, dir >= 0 ? 1 : -1); if (!g_cfg.fgMinFps) g_fgSusp = false; CfgSetInt("fgminfps", g_cfg.fgMinFps); OvPresetCustom(); break;
    case I_CAP: g_cfg.baseFpsLimit = StepCycle(kCapSteps, g_cfg.baseFpsLimit, dir >= 0 ? 1 : -1); CfgSetInt("baseFpsLimit", g_cfg.baseFpsLimit); if (g_slRuntimeOk) ApplyReflexOptions("menu"); break;
    case I_OUTCAP: g_cfg.fpsCap = OutCapCycle(g_cfg.fpsCap, dir); CfgSet("fpscap", g_cfg.fpsCap < 0 ? "auto" : std::to_string(g_cfg.fpsCap)); if (g_slRuntimeOk) ApplyReflexOptions("menu"); break;
    case I_HUDLESS: { int nv = g_cfg.hudless == 0 ? 2 : g_cfg.hudless == 2 ? 1 : 0; if (dir < 0) nv = g_cfg.hudless == 0 ? 1 : g_cfg.hudless == 1 ? 2 : 0; g_cfg.hudless = nv; CfgSetInt("hudless", nv); if (nv) EnsureDrawHooks(); if (nv == 2) HudCalReset(); Log("menu: hudless = %d", nv); break; }
    case I_REFLEX: tog(g_cfg.reflexSleep, "reflexsleep", "reflex sleep"); break;
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
struct OvLine { std::string t; int col = C_TEXT; bool sel = false; };
static void OvWrap(std::vector<OvLine>& out, const std::string& s, int col, size_t width, const char* indent = "") {
    size_t p = 0; bool first = true;
    while (p < s.size()) { size_t e = std::min(s.size(), p + width); if (e < s.size()) { size_t sp = s.rfind(' ', e); if (sp != std::string::npos && sp > p + width / 2) e = sp; }
        out.push_back({ (first ? "" : indent) + s.substr(p, e - p), col, false }); first = false; p = e; while (p < s.size() && s[p] == ' ') p++; }
}
static std::string OnOff(int v) { return v ? "ON" : "OFF"; }
static void OvBuild(std::vector<OvLine>& L) {
    // ---- compatibility banner (until the user has looked at the menu once)
    DiagVerdict v; { std::lock_guard<std::mutex> lk(g_verdictMx); v = g_verdict; }
    if (g_bannerActive.load() && v.state == 2) {
        L.push_back({ "!! DLSS Frame Generation is NOT working here !!", C_RED, false });
        OvWrap(L, v.reasons.empty() ? v.headline : v.reasons[0], C_YELLOW, 58);
        if (g_reportSaved.load()) {
            L.push_back({ "A debug report was saved as:", C_TEXT, false }); L.push_back({ "   " SN_REPORT_NAME, C_GREEN, false });
            OvWrap(L, "Please attach it to a new Issue on the project's GitHub page (Issues tab) and write the NAME of this game.", C_TEXT, 58);
        }
        OvWrap(L, std::string("(If you are still in a menu / loading screen, wait until gameplay.)  Press ") + KeyNameOf(g_cfg.overlayKey) + " to open the menu and dismiss this notice.", C_GRAY, 58);
    }
    // ---- HUD lines
    bool anyHud = g_cfg.showFps || g_cfg.showFgFps || g_cfg.showFrameTime || g_cfg.showVram;
    if (anyHud) {
        if (g_cfg.showFps) { char b[96]; snprintf(b, sizeof b, "Real FPS : %6.1f", g_st.fps); L.push_back({ b, C_TEXT, false }); }
        if (g_cfg.showFrameTime) { char b[96]; snprintf(b, sizeof b, "Frame    : %5.2f ms   1%% low %5.1f", g_st.ft, g_st.low1); L.push_back({ b, C_TEXT, false }); }
        if (g_cfg.showVram) { char b[96]; if (g_wasOn && g_stVramMB > 0) snprintf(b, sizeof b, "FG VRAM  : ~%llu MB (DLSS-G)", g_stVramMB); else snprintf(b, sizeof b, "FG VRAM  : %s", g_wasOn ? "measuring ..." : "- (FG off)"); L.push_back({ b, C_TEXT, false }); }
        if (g_cfg.showFgFps) {
            char b[128];
            if (!g_slInitOk) { L.push_back({ "DLSS-FG  : unavailable (Streamline not loaded)", C_RED, false }); }
            else if (g_compatBlocked.load()) { L.push_back({ "DLSS-FG  : not supported on this system", C_RED, false }); }
            else if (g_wasOn) { double r = g_fgRatio.load(); snprintf(b, sizeof b, "DLSS-FG  : %6.1f fps  (x%.1f)", g_st.fps * r, r); L.push_back({ b, C_GREEN, false }); }
            else { snprintf(b, sizeof b, "DLSS-FG  : OFF  (%s)", g_whyNot[0] ? g_whyNot : "off"); L.push_back({ b, C_GRAY, false }); }
        }
    }
    if (!g_ov.menuOpen.load() && !g_ov.toast.empty() && GetTickCount() - g_ov.toastTick < 2500) L.push_back({ g_ov.toast, g_ov.toastCol, false });
    // ---- menu
    if (g_ov.menuOpen.load()) {
        if (!L.empty()) L.push_back({ "", C_TEXT, false });
        L.push_back({ "SN_DLSSG " SN_VERSION "  -  DLSS Frame Generation", C_TITLE, false });
        { DiagRuntime rt; DiagRuntimeFill(rt); std::string gpu; { for (auto& g : g_gpus) if (g.nvidia && !g.software) { gpu = g.name; break; } }
          if (!gpu.empty()) L.push_back({ "GPU      : " + gpu, C_GRAY, false });
          char b[160];
          if (!g_slInitOk) snprintf(b, sizeof b, "FG state : UNAVAILABLE (Streamline not loaded)");
          else if (g_compatBlocked.load()) snprintf(b, sizeof b, "FG state : UNSUPPORTED on this system");
          else if (g_wasOn) snprintf(b, sizeof b, "FG state : ACTIVE  x%.1f   (real %.0f fps -> %.0f fps)", g_fgRatio.load(), g_st.fps, g_st.fps * g_fgRatio.load());
          else snprintf(b, sizeof b, "FG state : off - %s", g_whyNot[0] ? g_whyNot : "off");
          L.push_back({ b, g_wasOn ? C_GREEN : (g_compatBlocked.load() || !g_slInitOk) ? C_RED : C_YELLOW, false });
          int col = v.state == 1 ? C_GREEN : v.state == 2 ? C_RED : v.state == 3 ? C_GRAY : C_YELLOW;
          L.push_back({ std::string("Compat   : ") + (v.state == 1 ? "OK - verified working" : v.state == 2 ? "PROBLEM - see report" : v.state == 3 ? "frame generation switched off" : "checking..."), col, false }); }
        L.push_back({ "", C_TEXT, false });
        auto row = [&](int id, const char* label, const std::string& val) { char b[160]; snprintf(b, sizeof b, "%s %-26s %s", g_ov.sel == id ? ">" : " ", label, val.c_str()); L.push_back({ b, C_TEXT, g_ov.sel == id }); };
        static const char* posN[] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
        row(I_FG, "Frame Generation", OnOff(g_cfg.fg));
        { static const char* pn[] = { "Custom", "Quality", "Balanced", "Performance" }; row(I_PRESET, "Preset", pn[g_cfg.preset & 3]); }
        row(I_MULT, "Multiplier", Fmt("%dx%s", g_cfg.mult, g_maxGen ? Fmt("   (GPU max %ux)", g_maxGen + 1).c_str() : ""));
        row(I_SHOWFPS, "Show REAL fps", OnOff(g_cfg.showFps));
        row(I_SHOWFG, "Show DLSS-FG fps", OnOff(g_cfg.showFgFps));
        row(I_SHOWFT, "Show frame time / 1% low", OnOff(g_cfg.showFrameTime));
        row(I_SHOWVRAM, "Show DLSS-G VRAM estimate", OnOff(g_cfg.showVram));
        row(I_MINFPS, "Pause FG below base fps", g_cfg.fgMinFps ? Fmt("%d fps", g_cfg.fgMinFps) : std::string("OFF"));
        row(I_CAP, "Base fps cap (Reflex)", g_cfg.baseFpsLimit ? Fmt("%d fps", g_cfg.baseFpsLimit) : std::string("OFF"));
        { std::string v = g_cfg.fpsCap == 0 ? "OFF" : g_cfg.fpsCap < 0 ? (g_refreshHz.load() ? Fmt("AUTO  (%d Hz -> %d fps)", g_refreshHz.load(), SnAutoOutCap(g_refreshHz.load())) : std::string("AUTO  (refresh unknown)")) : Fmt("%d fps", g_cfg.fpsCap);
          int bl = EffectiveBaseLimit(); if (g_cfg.fpsCap != 0 && bl > 0) v += Fmt("  base<=%d", bl); row(I_OUTCAP, "Output fps cap (VRR)", v); }
        row(I_REFLEX, "Reflex sleep (needed by FG)", OnOff(g_cfg.reflexSleep));
        { static const char* rn[] = { "OFF (not recommended)", "Low latency", "Low latency + Boost" }; row(I_REFLEXMODE, "Reflex mode", rn[g_cfg.reflexMode % 3]); }
        row(I_CAMHOLD, "Keep FG when camera is lost", OnOff(g_cfg.camHold));
        row(I_CUT, "Camera-cut detection", OnOff(g_cfg.cutDetect));
        row(I_MENUDET, "Full-screen menu detection", OnOff(g_cfg.menuDetect));
        row(I_DYNRES, "Dynamic resolution (auto-detect)", OnOff(g_cfg.dynRes));
        { std::string v = g_cfg.hudless == 0 ? "OFF" : g_cfg.hudless == 1 ? Fmt("ON  (draw %d)", g_cfg.hudlessDraw) : g_hc.phase.load() == 4 ? std::string("AUTO: no clear result (see log)") : Fmt("AUTO: learning %d/6 votes", HudCalVotes()); row(I_HUDLESS, "HUD-less capture", v); }
        row(I_POS, "Overlay position", posN[g_cfg.overlayPos & 3]);
        row(I_SCALE, "Overlay size", Fmt("%d%%", g_cfg.overlayScale));
        row(I_OPACITY, "Overlay opacity", Fmt("%d%%", g_cfg.overlayOpacity));
        row(I_REPORT, "Write debug report now", "[Enter]");
        row(I_BUNDLE, "Create support zip (logs)", "[Enter]");
        row(I_CLOSE, "Close menu", "[Enter]");
        L.push_back({ "", C_TEXT, false });
        L.push_back({ std::string("Up/Down select   Left/Right change   Enter act   ") + KeyNameOf(g_cfg.overlayKey) + "/Esc close", C_GRAY, false });
        if (g_cfg.fgKey || g_cfg.hudKey) L.push_back({ std::string("Hotkeys (this session): ") + (g_cfg.fgKey ? std::string(KeyNameOf(g_cfg.fgKey)) + " = FG on/off   " : std::string()) + (g_cfg.hudKey ? std::string(KeyNameOf(g_cfg.hudKey)) + " = HUD on/off" : std::string()), C_GRAY, false });
        if (g_cfg.autoSave) { if (g_cfgSaveState.load() < 0) L.push_back({ "Cannot write SN_DLSSG_cfg.txt (read-only folder?)", C_RED, false }); else L.push_back({ "Changes are saved to SN_DLSSG_cfg.txt automatically", C_GRAY, false }); }
        if (!g_ov.msg.empty() && GetTickCount() - g_ov.msgTick < 6000) OvWrap(L, g_ov.msg, g_ov.msgBad ? C_RED : C_GREEN, 58);
    }
}

// ------------------------------------------------------------------ GDI panel -> BGRA bitmap
struct OvPanel { HDC dc = nullptr; HBITMAP bmp = nullptr, oldBmp = nullptr; HFONT font = nullptr, oldFont = nullptr; void* bits = nullptr; int capW = 0, capH = 0, px = 0; int w = 0, h = 0; size_t hash = 0; bool valid = false; } g_pn;
static void OvPanelFree() {
    if (g_pn.dc) { if (g_pn.oldBmp) SelectObject(g_pn.dc, g_pn.oldBmp); if (g_pn.oldFont) SelectObject(g_pn.dc, g_pn.oldFont); DeleteDC(g_pn.dc); }
    if (g_pn.bmp) DeleteObject(g_pn.bmp); if (g_pn.font) DeleteObject(g_pn.font);
    g_pn = OvPanel();
}
static bool OvPanelEnsure(int px) {
    if (g_pn.dc && g_pn.px == px) return true;
    OvPanelFree();
    int cw = (px * 40 + 63) & ~63, ch = px * 80; if (cw > 4096) cw = 4096; if (ch > 4096) ch = 4096;
    HDC dc = CreateCompatibleDC(nullptr); if (!dc) return false;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); if (!bmp || !bits) { DeleteDC(dc); if (bmp) DeleteObject(bmp); return false; }
    HFONT f = CreateFontA(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!f) f = CreateFontA(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
    g_pn.dc = dc; g_pn.bmp = bmp; g_pn.bits = bits; g_pn.font = f; g_pn.oldBmp = (HBITMAP)SelectObject(dc, bmp); g_pn.oldFont = f ? (HFONT)SelectObject(dc, f) : nullptr;
    g_pn.capW = cw; g_pn.capH = ch; g_pn.px = px; SetBkMode(dc, TRANSPARENT); return true;
}
static COLORREF OvColor(int c) { switch (c) { case C_GREEN: return RGB(110, 235, 130); case C_YELLOW: return RGB(255, 213, 90); case C_RED: return RGB(255, 100, 90); case C_GRAY: return RGB(165, 170, 180); case C_TITLE: return RGB(120, 200, 255); default: return RGB(240, 242, 246); } }
// renders the lines; returns false if nothing to draw
static bool OvPanelRender(const std::vector<OvLine>& L, int px) {
    if (!OvPanelEnsure(px)) return false;
    HDC dc = g_pn.dc; TEXTMETRICA tm = {}; GetTextMetricsA(dc, &tm); int lh = tm.tmHeight + std::max(2, px / 7), pad = std::max(6, px / 3);
    int maxW = 0; for (auto& l : L) { SIZE sz = {}; GetTextExtentPoint32A(dc, l.t.c_str(), (int)l.t.size(), &sz); maxW = std::max(maxW, (int)sz.cx); }
    int nl = (int)std::min<size_t>(L.size(), (size_t)((g_pn.capH - 2 * pad) / lh));
    int w = std::min(g_pn.capW, maxW + 2 * pad), h = std::min(g_pn.capH, nl * lh + 2 * pad); if (w < 8 || h < 8) return false;
    RECT rc = { 0, 0, w, h }; HBRUSH bg = CreateSolidBrush(RGB(12, 14, 20)); FillRect(dc, &rc, bg); DeleteObject(bg);
    HBRUSH bd = CreateSolidBrush(RGB(70, 80, 100)); FrameRect(dc, &rc, bd); DeleteObject(bd);
    for (int i = 0; i < nl; i++) {
        int y = pad + i * lh;
        if (L[i].sel) { RECT sr = { 2, y - 1, w - 2, y + lh - 1 }; HBRUSH sb = CreateSolidBrush(RGB(34, 66, 112)); FillRect(dc, &sr, sb); DeleteObject(sb); }
        SetTextColor(dc, OvColor(L[i].col)); TextOutA(dc, pad, y, L[i].t.c_str(), (int)L[i].t.size());
    }
    GdiFlush(); g_pn.w = w; g_pn.h = h; g_pn.valid = true; return true;
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
"    float3 t = T.Load(int3(q, 0)).rgb * gain;\n"
"    return float4(t * alpha, alpha);\n"
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
    float W = (float)bd.Width, H = (float)bd.Height; float mg = std::max(8.f, (float)g_pn.px * 0.6f); float pw = (float)g_pn.w, ph = (float)g_pn.h; float x, y;
    switch (g_cfg.overlayPos & 3) { case 1: x = W - pw - mg; y = mg; break; case 2: x = mg; y = H - ph - mg; break; case 3: x = W - pw - mg; y = H - ph - mg; break; default: x = mg; y = mg; }
    x = floorf(std::max(0.f, x)); y = floorf(std::max(0.f, y));
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
    struct { float r[4]; float scr[2]; float alpha; float gain; } k = { { x, y, pw, ph }, { W, H }, (float)g_cfg.overlayOpacity / 100.f, rtvFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2.5f : 1.0f };
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
    std::string key; for (auto& l : L) { key += l.t; key += (char)('0' + l.col); key += l.sel ? '*' : '.'; key += '\n'; }
    size_t h = std::hash<std::string>()(key) ^ ((size_t)px << 40);
    if (h != g_pn.hash || !g_pn.valid || g_pn.px != px) {
        if (!OvPanelRender(L, px)) return; g_pn.hash = h; g_ovNeedUpload = true;
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
