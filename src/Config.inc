// config.inc (v27, split out of main.cpp unchanged): Cfg struct (all config keys), log + ring buffer, key-name parsing, LoadCfg (+ template, presets).
// Included once by main.cpp (unity build) - order matters.
#pragma once
// ------------------------------------------------------------------ config / log
struct Cfg {
    int fg = 1;              // master switch
    int mult = 2;            // output frames per game frame (2 = numFramesToGenerate 1). Clamped to what the GPU supports
    int stage = 2;           // 0 = pass-through (no Streamline), 1 = Streamline init + support query only, 2 = frame generation, (hudless is separate)
    int log = 1;
    int hudless = 0;         // 0 = off, 1 = on (hudlessdraw=N), 2 = AUTO (v27: measures N in the first minute, then saves hudless=1 + hudlessdraw=N)  | 1 = copy the back buffer right before the Nth draw into it and tag it as HUD-less
    int hudlessDraw = 2;     // N for hudless=1 (draw calls that target the back buffer, counted per frame)
    int renderW = 2000, renderH = 1124, tol = 100;  // internal render resolution of the game (same defaults as the DLAA project). v18: renderw=0 = AUTO (UE5 / TSR / any screen percentage: same aspect as the swap chain, 1/4x..2x of its size)
    int mvOff = 492;         // float index of ClipToPrevClip in the UE4 view uniform buffer (same as DLAA 'mvoff')
    int noAAOff = 132;       // float index of ViewToClipNoAA
    int projOff = 116;       // float index of ViewToClip (jittered)
    int jitSX = 1, jitSY = 1, mvSX = 1, mvSY = 1;
    int velFallback = 1;     // v19: 1 = if no velocity buffer is found for velwait frames, run frame generation with camera-only motion vectors (all-zero dummy velocity -> the MV shader derives MVs from depth + ClipToPrevClip)
    int velWait = 90;        // v19: frames to wait for a real velocity buffer before the dummy is used (a real one found later replaces the dummy)
    int velFmt = 0;          // v19: extra DXGI_FORMAT number accepted as velocity target: 34=R16G16_FLOAT 37=R16G16_SNORM 16=R32G32_FLOAT 15=R32G32_TYPELESS (0 = none). See the census lines in the log
    int viewRect = 1;        // v20: 1 = use the REAL view rect (viewport the game renders the scene depth with) instead of the whole depth/velocity buffer (dynamic resolution: buffer > view rect -> stale L-shaped band at the bottom/right edge)
    int cutThresh100 = 120;  // v23: camera-cut guard fires only if ClipToPrevClip deviates from identity by >= cutthresh/100 (v22 used 0.25 -> fired on every fast camera turn in Code Vein)
    int cutFov = 8;          // v23: ... or the projection (FOV) changes by >= cutfov percent between two frames (v22: 2)
    int cutDetect = 1;       // v22: 1 = detect camera cuts / FOV jumps / huge ClipToPrevClip jumps (Special Shots, cutscenes) and pause frame generation for a few frames + reset history
    int c2pAuto = 1;         // v21: 1 = if the locked ClipToPrevClip candidate never moves while another identity-like candidate clearly does, switch to that one (log: "c2p-monitor: SWITCH")
    int c2pIdx = 0;          // v18 (UE5): which identity-like matrix after ViewToClipNoAA is ClipToPrevClip (0 = first, like UE4). The log lists them ("cam-scan: identity-like matrices ...")
    int warmup = 150;        // frames to wait before enabling
    int psoRetry = 1;        // v17: 1 = if the GAME's CreateGraphicsPipelineState fails with E_INVALIDARG, log the full desc and retry once without CachedPSO
    int camStale = 30;       // frames without a fresh view uniform buffer before FG is turned off (menus, loading, cutscenes without view). v16: 6 -> 30 (short hitches no longer toggle FG)
    int farPlane = 1000000;
    int camAuto = 1;         // 1 = if the view buffer is not found at projoff/noaaoff/mvoff, scan the buffer and lock onto the real offsets (other UE4 games)
    int reflexSleep = 1;     // 1 = call slReflexSleep every frame (needed for DLSS-G pacing)
    int baseFpsLimit = 0;    // Reflex frame limiter for the BASE frame rate (needs reflexsleep=1)
    int showConsole = 0;     // only works with the 'development' Streamline DLLs
    int fgMinFps = 0;        // >0: switch frame generation OFF while the BASE fps stays below this value (busy scenes), back ON when it recovers
    int fgDelay = 30;        // frames the camera must be valid IN A ROW before frame generation is (re)enabled (stops on/off flapping during loading/fades)
    int camHold = 1;         // v24: 1 = if the camera view buffer is lost while frame generation is running, KEEP frame generation on (last projection, identity ClipToPrevClip = optical flow only) instead of switching it off for the rest of the session
    int camHoldMax = 7200;   // v24: max frames to hold without a camera (then FG is switched off like before)
    int camRescan = 90;      // v24: frames without camera before the tracker re-scans the view buffer with relaxed checks (0 = never)
    int reflexAlways = 0;    // v24: 1 = call slReflexSleep even while frame generation is off/paused (v23 did, and slept ~20 ms per frame at the 9 fps start of the game for nothing)
    int fgMinBackoff = 1;    // v24: 1 = every FG pause by fgminfps doubles the time the base fps must stay good before FG resumes (stops the on/off flapping = 100+ ms hitch per switch)
    int bufMaxMB = 192;      // upper bound for upload buffers kept alive by the camera tracker (texture-streaming staging buffers are huge during loads)
    int reflexPlace = 0;     // 1 = call slReflexSleep + SimulationStart at the START of the game frame (end of the previous Present) instead of inside Present
    int ngxLog = 0;
    int shaderCache = 1;     // 1 = build shaders before the game starts and keep them in SN_DLSSG_shaders.snsc (no run-time compile hitch)
    int splash = 1;          // 1 = show the progress window while the cache is being built (only when something has to be built)
    // ---- v26: game pipeline cache
    int psoCache = 1;        // 1 = persistent cache for the GAME's own pipeline states (graphics + compute): every shader is compiled by the driver only once (SN_DLSSG_psocache.bin)
    int psoCacheMB = 384;    // upper bound for the pipeline cache (kept in RAM while the game runs)
    // ---- v25: in-game overlay / menu, compatibility diagnostics
    int overlay = 1;         // 1 = overlay + menu available (hotkey), 0 = never draw anything, never hook the keyboard
    int overlayKey = 0x2D;   // virtual-key code that opens/closes the menu (cfg: overlaykey=INSERT / F10 / HOME / 0x2D ...)
    int showFps = 0;         // HUD line: real (base) FPS = how fast the GAME renders
    int showFgFps = 0;       // HUD line: DLSS-FG output FPS (base FPS x generated frames)
    int showFrameTime = 0;   // HUD lines: frame time + 1% low
    int overlayPos = 0;      // 0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right
    int overlayScale = 100;  // percent
    int overlayOpacity = 80; // percent (background)
    int overlayBlock = 1;    // 1 = while the menu is open, swallow the menu keys so the game does not see them
    int overlayHook = 1;     // 1 = low-level keyboard hook (never misses a key press, can swallow keys); 0 = polling inside Present only
    int autoSave = 1;        // 1 = every change made in the menu is written back to SN_DLSSG_cfg.txt
    int autoReport = 1;      // 1 = write SN_DLSSG_DEBUG_REPORT.txt + show a notice when frame generation cannot work in this game
    int msgBox = 1;          // 1 = if the overlay cannot be drawn (DX11 game, ...) show a one-time message box for a fatal incompatibility
    // ---- v27
    int preset = 0;          // 0 custom (nothing applied) | 1 quality | 2 balanced | 3 performance  (sets fgminfps, fgdelay, camstale, mult; single keys written below it still win)
    int fpsCap = 0;          // output fps cap: 0 = off, -1 = auto (display refresh - 3, keeps VRR / G-Sync inside its window), N = fixed output fps (Reflex limits the BASE fps = cap / multiplier)
    int fgKey = 0x78;        // hotkey: frame generation on/off for this session (F9). 0 = none. needs overlay=1
    int hudKey = 0x77;       // hotkey: all HUD lines on/off for this session (F8). 0 = none
    int antiCheat = 1;       // 1 = switch the injector OFF when an anti-cheat is found (files next to the exe / loaded modules)
    int crashDump = 1;       // 1 = mini-dump + summary next to the exe when a crash happens in this injector / Streamline / the driver (or an unhandled crash)
    int engine = 0;          // detected only when the config is created (4 / 5 / 0 = unknown), info only
    int autoLearn = 1;       // 1 = scan results (projoff/noaaoff/mvoff, hudless=2 result) are written back to the config
    std::wstring slDir;      // folder with sl.interposer.dll etc. (default: next to the game exe)
} g_cfg;

static std::wstring g_dir;
static FILE* g_log = nullptr;
static std::mutex g_logMx;
static size_t g_logBytes = 0; static DWORD g_lastFlush = 0;
static void LogFlush() { if (!g_log) return; std::lock_guard<std::mutex> lk(g_logMx); fflush(g_log); }
// v25: the last log lines are ALSO kept in memory (even with log=0) so the debug report can contain them
static std::mutex g_ringMx; static std::vector<std::string> g_logRing; static size_t g_logRingPos = 0; static const size_t kLogRingMax = 400;
static void Log(const char* fmt, ...) {
    char buf[1536];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return; if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    { std::lock_guard<std::mutex> rk(g_ringMx);
      if (g_logRing.size() < kLogRingMax) g_logRing.emplace_back(buf, (size_t)n); else { g_logRing[g_logRingPos] = std::string(buf, (size_t)n); g_logRingPos = (g_logRingPos + 1) % kLogRingMax; } }
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logBytes > (24u << 20)) return;                 // never let the log grow without bound in long sessions
    fwrite(buf, 1, (size_t)n, g_log); fputc('\n', g_log);
    g_logBytes += (size_t)n + 1;
    DWORD now = GetTickCount(); if (now - g_lastFlush > 1000) { fflush(g_log); g_lastFlush = now; }   // flushing every line stalls the render thread
}
static std::vector<std::string> LogRingSnapshot() {
    std::lock_guard<std::mutex> rk(g_ringMx); std::vector<std::string> out; out.reserve(g_logRing.size());
    if (g_logRing.size() < kLogRingMax) out = g_logRing; else for (size_t i = 0; i < kLogRingMax; i++) out.push_back(g_logRing[(g_logRingPos + i) % kLogRingMax]);
    return out;
}
static void LogW(const wchar_t* w) {
    if (!g_log) return; char b[1024]; WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr); Log("%s", b);
}
// "INSERT", "F10", "HOME", "A", "0x2D", "45" ... -> virtual-key code (0 = none)
static int ParseKeyName(const char* in) {
    char t[48]; size_t n = 0; while (*in == ' ' || *in == '\t') in++;
    for (; *in && *in != ' ' && *in != '\t' && *in != '\r' && *in != '\n' && n + 1 < sizeof t; in++) t[n++] = (char)toupper((unsigned char)*in);
    t[n] = 0; if (!n) return 0;
    struct { const char* n; int vk; } tbl[] = { {"NONE",0},{"INSERT",0x2D},{"INS",0x2D},{"DELETE",0x2E},{"DEL",0x2E},{"HOME",0x24},{"END",0x23},{"PAGEUP",0x21},{"PGUP",0x21},{"PAGEDOWN",0x22},{"PGDN",0x22},
        {"PAUSE",0x13},{"SCROLL",0x91},{"SCROLLLOCK",0x91},{"TAB",0x09},{"BACKSPACE",0x08},{"CAPSLOCK",0x14},{"NUMLOCK",0x90},{"PRINTSCREEN",0x2C},
        {"NUMPAD0",0x60},{"NUMPAD1",0x61},{"NUMPAD2",0x62},{"NUMPAD3",0x63},{"NUMPAD4",0x64},{"NUMPAD5",0x65},{"NUMPAD6",0x66},{"NUMPAD7",0x67},{"NUMPAD8",0x68},{"NUMPAD9",0x69},
        {"MULTIPLY",0x6A},{"ADD",0x6B},{"SUBTRACT",0x6D},{"DIVIDE",0x6F},{"GRAVE",0xC0},{"TILDE",0xC0} };
    for (auto& e : tbl) if (!strcmp(t, e.n)) return e.vk;
    if (t[0] == 'F' && t[1] >= '0' && t[1] <= '9') { int f = atoi(t + 1); if (f >= 1 && f <= 24) return 0x6F + f; }
    if (n == 1 && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) return t[0];
    char* e = nullptr; long v = strtol(t, &e, 0); if (e && !*e && v > 0 && v < 256) return (int)v;
    return 0x2D;   // unknown name -> default
}
static const char* KeyNameOf(int vk) {
    static char b[16];
    switch (vk) { case 0: return "NONE"; case 0x2D: return "INSERT"; case 0x2E: return "DELETE"; case 0x24: return "HOME"; case 0x23: return "END"; case 0x21: return "PAGEUP"; case 0x22: return "PAGEDOWN"; case 0x13: return "PAUSE"; case 0x91: return "SCROLL"; case 0xC0: return "GRAVE"; default: break; }
    if (vk >= 0x70 && vk <= 0x87) { snprintf(b, sizeof b, "F%d", vk - 0x6F); return b; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(b, sizeof b, "%c", vk); return b; }
    snprintf(b, sizeof b, "0x%02X", vk); return b;
}
#include "detect.inc"   // v27: engine version scan + anti-cheat guard (pure name lists in logic.h)
static void ApplyPresetValues(int id) { SnPreset p; if (!SnPresetGet(id, &p)) return; g_cfg.fgMinFps = p.fgMinFps; g_cfg.fgDelay = p.fgDelay; g_cfg.camStale = p.camStale; g_cfg.mult = p.mult; }
static void LoadCfg() {
    std::wstring p = g_dir + L"\\SN_DLSSG_cfg.txt";
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) {
        {   // v27: the template is built in memory first: the right renderw/renderh for the engine of THIS game is picked (no UE4 / UE5 template to choose by hand)
            static char tpl[24000];
            snprintf(tpl, sizeof tpl,
                "# Scarlet Nexus - DLSS Frame Generation (Streamline) config\n"
                "# fg        : 1 = on, 0 = off\n"
                "# mult      : 2 = 2x (1 generated frame). 3/4 need an RTX 50 series card (clamped automatically)\n"
                "# stage     : 0 = pass-through, 1 = only init Streamline + log feature support, 2 = full frame generation\n"
                "# log       : 1 = write SN_DLSSG_log.txt + Streamline log files next to the exe\n"
                "# hudless   : 1 = experimental HUD-less capture (copy of the back buffer right before the Nth draw to it)\n"
                "# hudlessdraw: N for hudless=1 (try 2,3,4,... until UI artifacts disappear; see log 'bbdraws')\n"
                "# renderw/renderh/tol : internal render resolution of the game (depth/velocity buffers) +- tolerance. renderw=0 renderh=0 = AUTO (recommended for UE5 / TSR / dynamic screen percentage)\n# c2pidx    : v18 (UE5) which identity-like matrix after ViewToClipNoAA is ClipToPrevClip (0 = first). Only used by the automatic camera scan\n"
                "# camauto   : 1 = (default) automatically find the camera matrices in the UE4 view buffer when projoff/noaaoff/mvoff do not match this game (log: cam-scan LOCKED ...)\n# projoff/noaaoff : float index of ViewToClip / ViewToClipNoAA (116 / 132 in Scarlet Nexus)\n# mvoff     : float index of ClipToPrevClip inside the UE4 view uniform buffer (492 for Scarlet Nexus)\n"
                "# warmup    : frames before frame generation is switched on\n"
                "# reflexsleep : 1 = also call slReflexSleep every frame, baseFpsLimit = cap for the BASE fps (0 = none)\n"
                "# shadercache : 1 = pre-build shaders before the game starts and store them in SN_DLSSG_shaders.snsc (delete the file to force a rebuild)\n"
                "# splash    : 1 = show a progress window while shaders are being built (only appears when the cache has to be built)\n"
                "# sldir     : folder containing sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll (empty = exe folder)\n"
                "# fgminfps : >0 = turn frame generation off while the BASE fps is below this value (busy scenes), on again when it recovers (0 = never)\n# reflexplace : 1 = Reflex sleep/markers at the start of the game frame (experimental, try it if pacing is uneven)\n# log       : 1 = normal log, 2 = also verbose Streamline log (slow, big files - only for debugging)\n# fgdelay : frames the camera must be valid in a row before FG is switched on again (default 30) - avoids on/off flapping while a level loads\n# bufmaxmb : max MB of upload buffers the camera tracker may keep alive (default 192)\nfgminfps=0\nreflexplace=0\nfgdelay=30\nbufmaxmb=192\nfg=1\nmult=2\nstage=2\nlog=1\nhudless=0\nhudlessdraw=2\nrenderw=2000\nrenderh=1124\ntol=100\nmvoff=492\nwarmup=150\nreflexsleep=1\nbaseFpsLimit=0\nshowconsole=0\nshadercache=1\nsplash=1\nsldir=\n"
                "# ---- v25 overlay / menu (press the overlay key in game; arrows + Enter, changes are saved here automatically)\n"
                "# overlay      : 1 = overlay + menu available, 0 = completely off\n# overlaykey   : key that opens/closes the menu: INSERT (default), F1..F12, HOME, END, PAGEUP, ... or a virtual-key number like 0x2D\n"
                "# showfps      : 1 = show the REAL fps (how fast the game itself renders)\n# showfgfps    : 1 = show the DLSS Frame Generation output fps\n# showframetime: 1 = show frame time + 1%% low\n"
                "# overlaypos   : 0 = top-left 1 = top-right 2 = bottom-left 3 = bottom-right ; overlayscale / overlayopacity in percent\n"
                "# overlayblock : 1 = while the menu is open the menu keys are NOT sent to the game ; overlayhook=0 = no keyboard hook (polling only)\n"
                "# autosave     : 1 = changes made in the menu are written back to this file ; autoreport=1 = write SN_DLSSG_DEBUG_REPORT.txt when FG cannot work\n"
                "overlay=1\noverlaykey=INSERT\nshowfps=0\nshowfgfps=0\nshowframetime=0\noverlaypos=0\noverlayscale=100\noverlayopacity=80\noverlayblock=1\noverlayhook=1\nautosave=1\nautoreport=1\nmsgbox=1\n"
                "# ---- v26: psocache = persistent cache of the GAME's pipelines (SN_DLSSG_psocache.bin)\n"
                "psocache=1\npsocachemb=384\n");
            std::string t = tpl; int ue = DetectUeVersion(); g_cfg.engine = ue;
            if (ue == 5) { size_t q = t.find("renderw=2000\nrenderh=1124"); if (q != std::string::npos) t.replace(q, 25, "renderw=0\nrenderh=0"); }
            t += "# ---- v27\n# preset      : custom (default: nothing applied) | quality | balanced | performance - sets fgminfps, fgdelay, camstale and mult in one go; single keys written BELOW this line still win\n"
                 "# fpscap      : OUTPUT fps cap: 0 = off, auto = display refresh - 3 (keeps VRR / G-Sync inside its window), or a number. Needs reflexsleep=1. Applies while frame generation runs\n"
                 "# fgkey/hudkey: hotkeys (this session only): frame generation on/off, HUD on/off. NONE = off. They need overlay=1\n"
                 "# hudless     : 0 = off, 1 = on (hudlessdraw=N), 2 = AUTO: measures the right N in the first minute of 3D gameplay, then saves hudless=1 + hudlessdraw=N here\n"
                 "# anticheat   : 1 = (default) the injector switches itself OFF when an anti-cheat is found. 0 = skip that check (offline / anti-cheat-free launch only, at your own risk)\n"
                 "# crashdump   : 1 = mini-dump + summary (SN_DLSSG_crash_*.dmp/.txt) when a crash happens inside this injector / Streamline / the driver\n"
                 "# autolearn   : 1 = camera offsets found by the scan (projoff/noaaoff/mvoff) are written back here so the next start skips the scan\n"
                 "preset=custom\nfpscap=0\nfgkey=F9\nhudkey=F8\nanticheat=1\ncrashdump=1\nautolearn=1\n";
            f = _wfopen(p.c_str(), L"w"); if (f) { fwrite(t.data(), 1, t.size(), f);
            fclose(f); }
        }
    } else {
        char line[512];
        while (fgets(line, sizeof line, f)) {   // v27 pre-pass: a preset is applied FIRST so single keys in the file still override it
            char k[64], v[400] = {}; if (line[0] == '#') continue; if (sscanf(line, " %63[^=]=%399[^\r\n]", k, v) < 1) continue;
            if (!strcmp(k, "preset")) { g_cfg.preset = SnPresetParse(v); ApplyPresetValues(g_cfg.preset); }
        }
        rewind(f);
        while (fgets(line, sizeof line, f)) {
            char k[64], v[400] = {};
            if (line[0] == '#') continue;
            if (sscanf(line, " %63[^=]=%399[^\r\n]", k, v) < 1) continue;
            int iv = atoi(v);
            if (!strcmp(k, "fg")) g_cfg.fg = iv; else if (!strcmp(k, "mult")) g_cfg.mult = iv; else if (!strcmp(k, "stage")) g_cfg.stage = iv;
            else if (!strcmp(k, "log")) g_cfg.log = iv; else if (!strcmp(k, "hudless")) g_cfg.hudless = iv; else if (!strcmp(k, "hudlessdraw")) g_cfg.hudlessDraw = iv;
            else if (!strcmp(k, "renderw")) g_cfg.renderW = iv; else if (!strcmp(k, "renderh")) g_cfg.renderH = iv; else if (!strcmp(k, "tol")) g_cfg.tol = iv;
            else if (!strcmp(k, "mvoff")) g_cfg.mvOff = iv; else if (!strcmp(k, "noaaoff")) g_cfg.noAAOff = iv; else if (!strcmp(k, "projoff")) g_cfg.projOff = iv;
            else if (!strcmp(k, "velfallback")) g_cfg.velFallback = iv; else if (!strcmp(k, "velwait")) g_cfg.velWait = iv; else if (!strcmp(k, "velfmt")) g_cfg.velFmt = iv; else if (!strcmp(k, "c2pidx")) g_cfg.c2pIdx = iv; else if (!strcmp(k, "viewrect")) g_cfg.viewRect = iv ? 1 : 0; else if (!strcmp(k, "c2pauto")) g_cfg.c2pAuto = iv ? 1 : 0; else if (!strcmp(k, "jitsx")) g_cfg.jitSX = iv; else if (!strcmp(k, "jitsy")) g_cfg.jitSY = iv; else if (!strcmp(k, "mvsx")) g_cfg.mvSX = iv; else if (!strcmp(k, "mvsy")) g_cfg.mvSY = iv;
            else if (!strcmp(k, "warmup")) g_cfg.warmup = iv; else if (!strcmp(k, "camstale")) g_cfg.camStale = iv; else if (!strcmp(k, "psoretry")) g_cfg.psoRetry = iv; else if (!strcmp(k, "farplane")) g_cfg.farPlane = iv; else if (!strcmp(k, "camauto")) g_cfg.camAuto = iv;
            else if (!strcmp(k, "reflexsleep")) g_cfg.reflexSleep = iv; else if (!strcmp(k, "baseFpsLimit")) g_cfg.baseFpsLimit = iv;
            else if (!strcmp(k, "showconsole")) g_cfg.showConsole = iv;
            else if (!strcmp(k, "fgminfps")) g_cfg.fgMinFps = iv; else if (!strcmp(k, "reflexplace")) g_cfg.reflexPlace = iv;
            else if (!strcmp(k, "camhold")) g_cfg.camHold = iv ? 1 : 0; else if (!strcmp(k, "camholdmax")) g_cfg.camHoldMax = iv; else if (!strcmp(k, "camrescan")) g_cfg.camRescan = iv; else if (!strcmp(k, "reflexalways")) g_cfg.reflexAlways = iv ? 1 : 0; else if (!strcmp(k, "fgminbackoff")) g_cfg.fgMinBackoff = iv ? 1 : 0;
            else if (!strcmp(k, "fgdelay")) g_cfg.fgDelay = iv; else if (!strcmp(k, "bufmaxmb")) g_cfg.bufMaxMB = iv;
            else if (!strcmp(k, "cutdetect")) g_cfg.cutDetect = iv ? 1 : 0; else if (!strcmp(k, "cutthresh")) g_cfg.cutThresh100 = iv; else if (!strcmp(k, "cutfov")) g_cfg.cutFov = iv; else if (!strcmp(k, "shadercache")) g_cfg.shaderCache = iv; else if (!strcmp(k, "splash")) g_cfg.splash = iv;
            else if (!strcmp(k, "psocache")) g_cfg.psoCache = iv ? 1 : 0; else if (!strcmp(k, "psocachemb")) g_cfg.psoCacheMB = iv;
            else if (!strcmp(k, "preset")) g_cfg.preset = SnPresetParse(v);
            else if (!strcmp(k, "fpscap")) { std::string lv = SnLower(v); g_cfg.fpsCap = (lv.compare(0, 4, "auto") == 0) ? -1 : (lv.compare(0, 3, "off") == 0 ? 0 : std::max(0, iv)); }
            else if (!strcmp(k, "fgkey")) g_cfg.fgKey = ParseKeyName(v); else if (!strcmp(k, "hudkey")) g_cfg.hudKey = ParseKeyName(v);
            else if (!strcmp(k, "anticheat")) g_cfg.antiCheat = iv ? 1 : 0; else if (!strcmp(k, "crashdump")) g_cfg.crashDump = iv ? 1 : 0; else if (!strcmp(k, "autolearn")) g_cfg.autoLearn = iv ? 1 : 0;
            else if (!strcmp(k, "sldir")) { wchar_t w[400]; MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 400); g_cfg.slDir = w; }
            else if (!strcmp(k, "overlay")) g_cfg.overlay = iv ? 1 : 0; else if (!strcmp(k, "overlaykey")) g_cfg.overlayKey = ParseKeyName(v);
            else if (!strcmp(k, "showfps")) g_cfg.showFps = iv ? 1 : 0; else if (!strcmp(k, "showfgfps")) g_cfg.showFgFps = iv ? 1 : 0; else if (!strcmp(k, "showframetime")) g_cfg.showFrameTime = iv ? 1 : 0;
            else if (!strcmp(k, "overlaypos")) g_cfg.overlayPos = iv; else if (!strcmp(k, "overlayscale")) g_cfg.overlayScale = iv; else if (!strcmp(k, "overlayopacity")) g_cfg.overlayOpacity = iv;
            else if (!strcmp(k, "overlayblock")) g_cfg.overlayBlock = iv ? 1 : 0; else if (!strcmp(k, "overlayhook")) g_cfg.overlayHook = iv ? 1 : 0;
            else if (!strcmp(k, "autosave")) g_cfg.autoSave = iv ? 1 : 0; else if (!strcmp(k, "autoreport")) g_cfg.autoReport = iv ? 1 : 0; else if (!strcmp(k, "msgbox")) g_cfg.msgBox = iv ? 1 : 0;
        }
        fclose(f);
    }
    if (g_cfg.mult < 2) g_cfg.mult = 2;
    if (g_cfg.velFmt != 15 && g_cfg.velFmt != 16 && g_cfg.velFmt != 34 && g_cfg.velFmt != 37) g_cfg.velFmt = 0;
    if (g_cfg.velWait < 0) g_cfg.velWait = 0;
    if (g_cfg.c2pIdx < 0) g_cfg.c2pIdx = 0; if (g_cfg.c2pIdx > 3) g_cfg.c2pIdx = 3;
    if (g_cfg.renderW < 0 || g_cfg.renderH < 0) g_cfg.renderW = g_cfg.renderH = 0;
    if (g_cfg.overlayPos < 0 || g_cfg.overlayPos > 3) g_cfg.overlayPos = 0;
    g_cfg.overlayScale = std::min(250, std::max(50, g_cfg.overlayScale)); g_cfg.overlayOpacity = std::min(100, std::max(20, g_cfg.overlayOpacity));
    g_cfg.psoCacheMB = std::min(4096, std::max(32, g_cfg.psoCacheMB));
    if (g_cfg.fgMinFps < 0) g_cfg.fgMinFps = 0; if (g_cfg.baseFpsLimit < 0) g_cfg.baseFpsLimit = 0;
    if (g_cfg.hudless < 0 || g_cfg.hudless > 2) g_cfg.hudless = 0; if (g_cfg.fpsCap < -1) g_cfg.fpsCap = 0; if (g_cfg.fpsCap > 1000) g_cfg.fpsCap = 1000;
    if (g_cfg.fgKey == g_cfg.overlayKey) g_cfg.fgKey = 0; if (g_cfg.hudKey == g_cfg.overlayKey || g_cfg.hudKey == g_cfg.fgKey) g_cfg.hudKey = 0;
    if (g_cfg.log && !g_log) g_log = _wfopen((g_dir + L"\\SN_DLSSG_log.txt").c_str(), L"w");
    Log("SN_DLSSG v28 = v27 + micro-stutter fixes: HUD-less capture only on the real swap-chain back buffer (stale pointer cache re-created the HUD-less texture hundreds of times), no FG history reset on same-size depth swap / short camera hold");
    Log("SN_DLSSG v27 = v26 + presets, output fps cap (VRR), hotkeys, hudless=2 auto detection, anti-cheat guard, crash mini-dumps + support zip, auto-saved scan results");
    Log("CFG v27 preset=%s fpscap=%d fgkey=%s hudkey=%s hudless=%d anticheat=%d crashdump=%d autolearn=%d", SnPresetName(g_cfg.preset), g_cfg.fpsCap, KeyNameOf(g_cfg.fgKey), KeyNameOf(g_cfg.hudKey), g_cfg.hudless, g_cfg.antiCheat, g_cfg.crashDump, g_cfg.autoLearn);
    Log("SN_DLSSG v26 = v25 + persistent game pipeline (PSO) cache");
    Log("CFG v26 psocache=%d psocachemb=%d", g_cfg.psoCache, g_cfg.psoCacheMB);
    Log("SN_DLSSG v25 = v24 + in-game overlay/menu (key %s), FPS / DLSS-FG FPS HUD, auto config save, compatibility check + debug report, safe start-up optimisations", KeyNameOf(g_cfg.overlayKey));
    Log("SN_DLSSG v24 (v23 + camera-registry LRU eviction (fixes FG lost after a level load), camera re-scan + hold, async buffer release, no useless Reflex sleep, FG-pause backoff) build " __DATE__ " " __TIME__ "  (Streamline SDK headers %d.%d.%d)", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    Log("CFG velfallback=%d velwait=%d velfmt=%d viewrect=%d c2pauto=%d cutdetect=%d", g_cfg.velFallback, g_cfg.velWait, g_cfg.velFmt, g_cfg.viewRect, g_cfg.c2pAuto, g_cfg.cutDetect);
    Log("CFG v24 camhold=%d camholdmax=%d camrescan=%d reflexalways=%d fgminbackoff=%d fgminfps=%d bufmaxmb=%d", g_cfg.camHold, g_cfg.camHoldMax, g_cfg.camRescan, g_cfg.reflexAlways, g_cfg.fgMinBackoff, g_cfg.fgMinFps, g_cfg.bufMaxMB);
    Log("CFG fg=%d mult=%d stage=%d hudless=%d(N=%d) render=%dx%d+-%d%s mvoff=%d c2pidx=%d warmup=%d", g_cfg.fg, g_cfg.mult, g_cfg.stage, g_cfg.hudless, g_cfg.hudlessDraw, g_cfg.renderW, g_cfg.renderH, g_cfg.tol, g_cfg.renderW <= 0 ? " (AUTO)" : "", g_cfg.mvOff, g_cfg.c2pIdx, g_cfg.warmup);
}

