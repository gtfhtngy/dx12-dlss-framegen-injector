// diag.inc  (v25) - "can this game run DLSS frame generation?" checklist + debug report
//
// Everything that decides whether frame generation can work is recorded here as a checklist item (OK / WARN / FAIL / INFO) with a plain-language explanation
// and "what to check". When frame generation cannot work, a complete report is written next to the game exe:
//        SN_DLSSG_DEBUG_REPORT.txt
// and the user is told (overlay banner, or a one-time message box when the overlay cannot be drawn) to attach that file to a GitHub Issue.
//
// This file only OBSERVES: it never touches the render path. Every function is exception-safe.

#ifndef SN_ISSUES_URL
#define SN_ISSUES_URL "https://github.com/gtfhtngy/dx12-dlss-framegen-injector/issues"
#endif
#define SN_REPORT_NAME "SN_DLSSG_DEBUG_REPORT.txt"

enum { DS_INFO = 0, DS_OK = 1, DS_WARN = 2, DS_FAIL = 3 };
struct DiagItem { std::string id, title, detail, fix; int sev = DS_INFO; bool fatal = false; int order = 0; };
static std::vector<DiagItem> g_diagItems; static std::mutex g_diagMx; static int g_diagOrder = 0;

static std::string Fmt(const char* f, ...) {
    char b[2048]; va_list ap; va_start(ap, f); int n = vsnprintf(b, sizeof b, f, ap); va_end(ap);
    if (n < 0) return std::string(); if ((size_t)n >= sizeof b) n = (int)sizeof b - 1; return std::string(b, (size_t)n);
}
static std::string Narrow(const wchar_t* w) { char b[1024]; int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr); return n > 0 ? std::string(b) : std::string(); }

// ---- verdict / report state
struct DiagVerdict { int state = 0;   // 0 = still checking, 1 = frame generation verified working, 2 = cannot work / did not start, 3 = switched off by the user / config
                     std::string headline; std::vector<std::string> reasons; std::string firstFixes; };
static DiagVerdict g_verdict; static std::mutex g_verdictMx;
static std::wstring g_reportPath; static std::atomic<int> g_reportWrites{ 0 }; static std::atomic<bool> g_reportSaved{ false };
static std::atomic<bool> g_bannerActive{ false }, g_bannerSeen{ false };

// snapshot of the runtime state (filled by DiagRuntimeFill, defined next to the globals it reads)
struct DiagRuntime {
    int frame = 0, warmup = 0, busyFrames = 0, scene3dFrames = 0, camAge = -1; bool fgWanted = false, everOn = false, wasOn = false, susp = false, compatBlocked = false;
    bool slInit = false, slDev = false, slRuntime = false, haveQueue = false, haveDevice = false, depthPinned = false, velPinned = false, velDummy = false, camLocked = false, mvReady = false, mvFailed = false, d3d12Swap = true;
    long projSeen = 0, scanRuns = 0; int tagRes = -1, constRes = -1, optRes = -1; unsigned status = 0; bool haveState = false; double ratio = 1.0, baseFps = 0; unsigned maxGen = 0, minWH = 0; unsigned long long vramMB = 0;
    int projOff = 0, noaaOff = 0, mvOff = 0; int effW = 0, effH = 0; unsigned dispW = 0, dispH = 0, bufCount = 0; int fmt = 0, swapEffect = 0; const char* why = "";
    int stage = 0; bool focused = true;
};
static void DiagRuntimeFill(DiagRuntime& rt);    // defined next to the globals (overlay.inc)
static F_CreateDXGIFactory GetOrigFactory1();    // the ORIGINAL (un-hooked) CreateDXGIFactory1, defined in main.cpp
static void Diag_OnVerdictChanged();             // defined in overlay.inc (shows the banner)

static void DiagSet(const char* id, int sev, const char* title, const std::string& detail = std::string(), const std::string& fix = std::string(), bool fatal = false) {
    std::lock_guard<std::mutex> lk(g_diagMx);
    for (auto& it : g_diagItems) if (it.id == id) { it.sev = sev; it.title = title; it.detail = detail; it.fix = fix; it.fatal = fatal; return; }
    DiagItem d; d.id = id; d.sev = sev; d.title = title; d.detail = detail; d.fix = fix; d.fatal = fatal; d.order = g_diagOrder++; g_diagItems.push_back(d);
}
static const char* SevTag(int s) { return s == DS_OK ? "[ OK ]" : s == DS_WARN ? "[WARN]" : s == DS_FAIL ? "[FAIL]" : "[INFO]"; }

// ---------------------------------------------------------------- system probes
struct GpuInfo { std::string name; unsigned vendor = 0, device = 0, rev = 0; unsigned long long vramMB = 0, sharedMB = 0; unsigned long long umd = 0; std::string driver; bool nvidia = false; bool software = false; std::string genGuess; bool supportKnown = false; bool supported = false; std::string supportText; };
static std::vector<GpuInfo> g_gpus; static std::once_flag g_gpuOnce; static std::atomic<bool> g_gpuDone{ false };

static std::string GuessGeneration(const std::string& n) {
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("RTX PRO") || has("Blackwell")) return "RTX Pro / Blackwell (50-series class)";
    size_t p = n.find("RTX ");
    if (p != std::string::npos) {
        const char* q = n.c_str() + p + 4; while (*q == ' ') q++;
        if (*q >= '2' && *q <= '5' && q[1] >= '0' && q[1] <= '9' && q[2] >= '0' && q[2] <= '9') { int g = (*q - '0') * 10; return Fmt("GeForce RTX %d-series (%s)", g, g >= 40 ? "DLSS frame generation supported" : "NO DLSS frame generation: needs RTX 40 or newer"); }
        return "RTX (workstation / unknown generation)";
    }
    if (has("GTX") || has("GeForce")) return "GeForce GTX / older: NO DLSS frame generation";
    return "";
}
// NVIDIA driver number from the DXGI user-mode-driver version (31.0.15.4601 -> 546.01)
static std::string DriverString(unsigned long long umd, bool nvidia) {
    unsigned a = (unsigned)((umd >> 48) & 0xFFFF), b = (unsigned)((umd >> 32) & 0xFFFF), c = (unsigned)((umd >> 16) & 0xFFFF), d = (unsigned)(umd & 0xFFFF);
    if (!umd) return "unknown";
    if (nvidia) return Fmt("%u.%02u  (DXGI UMD %u.%u.%u.%u)", (c % 10) * 100 + d / 100, d % 100, a, b, c, d);
    return Fmt("%u.%u.%u.%u", a, b, c, d);
}
static void DiagScanAdapters() {
    std::call_once(g_gpuOnce, [] {
        try {
            F_CreateDXGIFactory f1 = GetOrigFactory1();
            if (!f1) { HMODULE mdx = GetModuleHandleW(L"dxgi.dll"); if (!mdx) mdx = LoadLibraryW(L"dxgi.dll"); if (mdx) f1 = (F_CreateDXGIFactory)GetProcAddress(mdx, "CreateDXGIFactory1"); }   // the ORIGINAL export (our own hook would wait for the shader warm-up)
            IDXGIFactory1* fac = nullptr;
            if (f1 && SUCCEEDED(f1(__uuidof(IDXGIFactory1), (void**)&fac)) && fac) {
                for (UINT i = 0;; i++) {
                    IDXGIAdapter1* a = nullptr; if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
                    DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d);
                    GpuInfo g; g.name = Narrow(d.Description); g.vendor = d.VendorId; g.device = d.DeviceId; g.rev = d.Revision; g.vramMB = d.DedicatedVideoMemory >> 20; g.sharedMB = d.SharedSystemMemory >> 20;
                    g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0; g.nvidia = d.VendorId == 0x10DE;
                    LARGE_INTEGER umd = {}; if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) g.umd = (unsigned long long)umd.QuadPart;
                    g.driver = DriverString(g.umd, g.nvidia); g.genGuess = g.nvidia ? GuessGeneration(g.name) : std::string();
                    g_gpus.push_back(g); a->Release();
                }
                fac->Release();
            }
        } catch (...) {}
        g_gpuDone.store(true);
    });
}
static bool DiagHasNvidia() { DiagScanAdapters(); for (auto& g : g_gpus) if (g.nvidia && !g.software) return true; return false; }

static bool ReadPeMachine(const std::wstring& p, unsigned* machine) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return false;
    unsigned char b[4096]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
    if (n < 0x100 || b[0] != 'M' || b[1] != 'Z') return false;
    unsigned lf = *(unsigned*)(b + 0x3C); if (lf + 8 > n) return false;
    if (memcmp(b + lf, "PE\0\0", 4)) return false;
    *machine = *(unsigned short*)(b + lf + 4); return true;
}
static bool FileVer(const std::wstring& p, std::string* out, unsigned long long* key) {
    DWORD h = 0, sz = GetFileVersionInfoSizeW(p.c_str(), &h); if (!sz) return false;
    std::vector<BYTE> buf(sz); if (!GetFileVersionInfoW(p.c_str(), 0, sz, buf.data())) return false;
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0; if (!VerQueryValueW(buf.data(), L"\\", (LPVOID*)&fi, &len) || !fi) return false;
    *out = Fmt("%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    *key = ((unsigned long long)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS; return true;
}
static std::string FileInfoLine(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a = {}; if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return "MISSING";
    unsigned long long sz = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow; std::string v; unsigned long long k = 0; unsigned m = 0;
    bool hv = FileVer(p, &v, &k); bool hm = ReadPeMachine(p, &m);
    return Fmt("%llu KB, version %s, %s", sz >> 10, hv ? v.c_str() : "n/a", hm ? (m == 0x8664 ? "x64" : m == 0x14C ? "x86 (32-bit!)" : "other arch") : "not a valid PE file");
}

static const char* SlFixText(sl::Result r) {
    switch (r) {
    case sl::Result::eErrorDriverOutOfDate: return "Update the NVIDIA graphics driver to the newest Game Ready / Studio driver (https://www.nvidia.com/drivers), then reboot. The report lists the detected and the required driver version when Streamline reported them.";
    case sl::Result::eErrorOSOutOfDate: return "Update Windows (Windows 10 version 2004 / build 19041 or newer, Windows 11 recommended).";
    case sl::Result::eErrorOSDisabledHWS: return "Turn ON 'Hardware-accelerated GPU scheduling': Windows Settings > System > Display > Graphics > Default graphics settings, switch it On and REBOOT.";
    case sl::Result::eErrorNoSupportedAdapterFound: case sl::Result::eErrorAdapterNotSupported:
        return "This GPU does not support DLSS Frame Generation: it needs a GeForce RTX 40 series or newer (RTX 50 for 3x/4x). RTX 20/30 and GTX cards cannot run it. On laptops with two GPUs make sure the game runs on the NVIDIA GPU (Windows Settings > Graphics > the game's exe > High performance).";
    case sl::Result::eErrorNoPlugins: case sl::Result::eErrorFeatureMissing: case sl::Result::eErrorFeatureFailedToLoad: case sl::Result::eErrorFeatureMissingHooks: case sl::Result::eErrorFeatureMissingDependency:
        return "A Streamline plugin could not be loaded. Copy ALL of these from the SAME Streamline SDK (production build, bin/x64): sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll - next to the game exe (or set sldir=). Do not mix SDK versions; check that no file is 32-bit or blocked by antivirus.";
    case sl::Result::eErrorNGXFailed: return "NGX (nvngx_dlssg.dll) failed to start: the file is missing, from a different SDK version, or the driver is too old. Use nvngx_dlssg.dll from the same SDK as the sl.*.dll files and update the driver.";
    case sl::Result::eErrorIO: case sl::Result::eErrorJSONParsing: return "Streamline could not read its files (missing / damaged / blocked). Re-copy the DLLs from the SDK; check the folder is readable and the files are not 'blocked' (right-click > Properties > Unblock).";
    case sl::Result::eErrorD3DAPI: case sl::Result::eErrorDXGIAPI: return "A Direct3D / DXGI call made by Streamline failed. Make sure the game really runs in DirectX 12 mode, the GPU driver is current and no other overlay/injector (ReShade, Special K, RTSS, ...) is hooking the swap chain.";
    case sl::Result::eErrorInvalidIntegration: case sl::Result::eErrorMissingProxy: case sl::Result::eErrorInitNotCalled: case sl::Result::eErrorMissingOrInvalidAPI:
        return "Streamline did not see the device / swap chain through its proxy. Usually another tool (ReShade, other DLL proxies such as dxgi.dll/d3d12.dll) is in the way or the game created its D3D12 device before this injector was loaded. Remove other injectors and try again.";
    default: return "See the Streamline error name above (sl_result.h) and the log lines labelled [SL ...] in the log tail below.";
    }
}
static std::string Win32Hint(DWORD e) {
    if (e == ERROR_MOD_NOT_FOUND || e == 126) return "A DLL that sl.interposer.dll depends on is missing (install the 'Visual C++ Redistributable 2015-2022 x64') or sl.interposer.dll itself is not where sldir points.";
    if (e == ERROR_BAD_EXE_FORMAT || e == 193) return "The file is not a valid 64-bit DLL (wrong architecture or damaged download).";
    if (e == ERROR_ACCESS_DENIED || e == 5) return "Access denied: unblock the file (Properties > Unblock) and check antivirus quarantine.";
    return "Check that the file is a complete, 64-bit, unblocked DLL.";
}

// ---------------------------------------------------------------- pre-flight (static facts)
static std::atomic<bool> g_preflightDone{ false };
static void DiagFatal(const char* id, const char* title, const std::string& detail, const std::string& fix);

static void DiagPreflight() {
    if (g_preflightDone.exchange(true)) return;
    try {
        // ---- OS
        { typedef LONG(WINAPI* FRtl)(OSVERSIONINFOEXW*); OSVERSIONINFOEXW vi = {}; vi.dwOSVersionInfoSize = sizeof vi; auto f = (FRtl)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
          if (f && f(&vi) == 0) { bool ok = vi.dwBuildNumber >= 19041; DiagSet("os", ok ? DS_OK : DS_WARN, "Windows version", Fmt("Windows %lu.%lu build %lu%s", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, vi.dwBuildNumber >= 22000 ? " (Windows 11)" : ""),
              ok ? "" : "DLSS frame generation needs Windows 10 2004 (build 19041) or newer. Update Windows."); }
          else DiagSet("os", DS_INFO, "Windows version", "could not be read"); }
        // ---- HAGS
        { DWORD v = 0, sz = sizeof v; LONG r = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &v, &sz);
          if (r == ERROR_SUCCESS) DiagSet("hags", v == 2 ? DS_OK : DS_WARN, "Hardware-accelerated GPU scheduling (HAGS)", v == 2 ? "ON" : "OFF (HwSchMode=1)", v == 2 ? "" : SlFixText(sl::Result::eErrorOSDisabledHWS));
          else DiagSet("hags", DS_INFO, "Hardware-accelerated GPU scheduling (HAGS)", "setting not present in the registry (older Windows / driver, or the GPU does not offer it)", "If DLSS-G reports 'eErrorOSDisabledHWS' below, enable HAGS in Windows graphics settings."); }
        // ---- GPUs
        DiagScanAdapters();
        { std::string all; bool nv = false;
          for (size_t i = 0; i < g_gpus.size(); i++) { auto& g = g_gpus[i]; if (g.software) continue; nv |= g.nvidia;
              all += Fmt("  #%zu %s | vendor 0x%04X device 0x%04X rev %u | VRAM %llu MB | driver %s%s%s\n", i, g.name.c_str(), g.vendor, g.device, g.rev, g.vramMB, g.driver.c_str(), g.genGuess.empty() ? "" : " | ", g.genGuess.c_str()); }
          if (g_gpus.empty()) DiagSet("gpu", DS_WARN, "Graphics adapters", "DXGI could not list any adapter");
          else if (!nv) DiagFatal("gpu", "NVIDIA GPU", all + "  -> no NVIDIA adapter found", "DLSS Frame Generation only runs on NVIDIA GeForce RTX 40 series or newer. This PC has no NVIDIA GPU, so the injector cannot work here. (Remove winmm.dll from the game folder to stop this notice.)");
          else DiagSet("gpu", DS_OK, "NVIDIA GPU present", all); }
        // ---- files
        { std::wstring dir = g_cfg.slDir.empty() ? g_dir : g_cfg.slDir; const wchar_t* names[] = { L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll" };
          std::string det = "folder: " + Narrow(dir.c_str()) + "\n"; bool missing = false, badArch = false; unsigned long long ver0 = 0; bool verMismatch = false; std::string v0;
          for (auto n : names) { std::wstring p = dir + L"\\" + n; std::string li = FileInfoLine(p); det += "  " + Narrow(n) + ": " + li + "\n";
              if (li == "MISSING") missing = true; else if (li.find("(32-bit") != std::string::npos || li.find("not a valid") != std::string::npos || li.find("other arch") != std::string::npos) badArch = true;
              if (wcsncmp(n, L"sl.", 3) == 0) { std::string v; unsigned long long k = 0; if (FileVer(p, &v, &k)) { if (!ver0) { ver0 = k; v0 = v; } else if (k != ver0) verMismatch = true; } } }
          if (missing) DiagFatal("files", "Streamline files", det, "Copy ALL six files (sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll) from the SAME Streamline SDK 'bin/x64' (production) folder next to the game exe. See README > Installation.");
          else if (badArch) DiagFatal("files", "Streamline files", det, "One of the files is not a valid 64-bit DLL. Download the SDK again and copy the x64 production files.");
          else if (verMismatch) DiagSet("files", DS_WARN, "Streamline files", det + "  -> the sl.*.dll files have DIFFERENT versions", "Use the files from ONE Streamline SDK version (README: tested with 2.14.1). Mixed versions are a common cause of crashes / 'feature failed to load'.");
          else DiagSet("files", DS_OK, "Streamline files", det); }
        // ---- game exe
        { wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH); std::string e = Narrow(exe); std::string lower = e; for (auto& c : lower) c = (char)tolower((unsigned char)c);
          std::string eng = (lower.find("-win64-shipping") != std::string::npos || lower.find("-wingdk-shipping") != std::string::npos) ? "Unreal Engine (exe name pattern)" : "";
          DiagSet("exe", DS_INFO, "Game executable", Fmt("%s\n  engine hint from exe name: %s", e.c_str(), eng.empty() ? "none (not an obvious Unreal Engine shipping exe)" : eng.c_str())); }
        // ---- stage / config
        if (g_cfg.stage < 2) DiagSet("stage", DS_WARN, "Config: stage", Fmt("stage=%d", g_cfg.stage), "stage must be 2 for frame generation (0 = pass-through, 1 = only initialise + log support).");
    } catch (...) {}
}
// called once when the first swap chain exists / at the first Present: API + engine facts
static void DiagGameFacts(unsigned w, unsigned h, int fmt, unsigned bufs, int effect, bool d3d12) {
    try {
        std::string mods; auto chk = [&](const wchar_t* m, const char* label) { if (GetModuleHandleW(m)) mods += std::string("  loaded: ") + label + "\n"; };
        chk(L"d3d12.dll", "d3d12.dll"); chk(L"d3d11.dll", "d3d11.dll"); chk(L"vulkan-1.dll", "vulkan-1.dll (Vulkan)"); chk(L"UnityPlayer.dll", "UnityPlayer.dll (Unity engine)"); chk(L"libcef.dll", "libcef.dll");
        chk(L"nvngx_dlss.dll", "nvngx_dlss.dll (the game already ships DLSS super resolution)"); chk(L"sl.interposer.dll", "sl.interposer.dll"); chk(L"ReShade64.dll", "ReShade64.dll (ReShade - may conflict)"); chk(L"SpecialK64.dll", "SpecialK64.dll (Special K - may conflict)"); chk(L"RTSSHooks64.dll", "RTSSHooks64.dll (RTSS overlay - may conflict)");
        bool unity = GetModuleHandleW(L"UnityPlayer.dll") != nullptr;
        DiagSet("modules", DS_INFO, "Loaded graphics-related modules", mods.empty() ? "(none of the known ones)" : mods);
        if (!d3d12) DiagFatal("api", "Game uses Direct3D 12", Fmt("The swap chain was created with a queue that is not a D3D12 command queue (the game is running DirectX 11 / another API).\n%s", mods.c_str()), "Start the game in DirectX 12 mode (launch option -dx12, in-game setting, or the DX12 executable). This injector only supports Direct3D 12 games.");
        else {
            bool hdr = fmt == 10 || fmt == 24 || fmt == 11; (void)hdr;
            std::string d = Fmt("swap chain %ux%u, DXGI format %d, %u buffers, swap effect %d (%s)", w, h, fmt, bufs, effect, effect == 3 || effect == 4 ? "flip model - good" : effect == 0 || effect == 1 ? "bitblt model - DLSS-G needs a flip-model swap chain" : "unknown");
            bool flip = effect == 3 || effect == 4;
            DiagSet("swapchain", flip ? DS_OK : DS_WARN, "Swap chain", d, flip ? "" : "DLSS-G only works with flip-model swap chains (FLIP_DISCARD / FLIP_SEQUENTIAL). If the game offers it, use 'Borderless/Fullscreen' with DX12 and turn off exclusive-fullscreen options.");
            if (w < 1280 && h < 720) DiagSet("res", DS_WARN, "Output resolution", Fmt("%ux%u", w, h), "DLSS-G has a minimum output size (see 'minWidthOrHeight' in the DLSS-G state below). Use a higher output resolution.");
        }
        if (unity) DiagSet("engine", DS_WARN, "Engine", "Unity detected", "Only Unreal Engine 4/5 games have been tested: the camera / motion data are read from the Unreal Engine view uniform buffer. A Unity game will most likely not get a camera (see CAMERA below).");
    } catch (...) {}
}

// ---------------------------------------------------------------- fatal helper (also starts the notification)
static void DiagEvaluate(bool force);
static void DiagFatal(const char* id, const char* title, const std::string& detail, const std::string& fix) {
    DiagSet(id, DS_FAIL, title, detail, fix, true);
    Log("COMPAT FATAL: %s -> %s", title, detail.c_str());
    DiagEvaluate(true);   // verdict + report + notice right away (the game may never reach a Present we could hook)
}

// ---------------------------------------------------------------- evaluation
static std::string DecodeDlssgStatus(unsigned st) {
    if (!st) return "ok";
    std::string r;
    if (st & 1) r += "resolution too low; "; if (st & 2) r += "Reflex not detected at runtime (reflexsleep must be 1); "; if (st & 4) r += "HDR format not supported; ";
    if (st & 8) r += "common constants invalid (camera matrices); "; if (st & 16) r += "GetCurrentBackBufferIndex not called; "; if (st & ~31u) r += "other bit set; ";
    return r;
}
// builds the list of reasons from the runtime snapshot (used both for the verdict and for the report)
static void DiagBlockers(const DiagRuntime& rt, std::vector<std::string>& why, std::vector<std::string>& fixes) {
    auto add = [&](const std::string& w, const std::string& f) { why.push_back(w); if (!f.empty()) fixes.push_back(f); };
    if (!rt.d3d12Swap) add("The game does not use a Direct3D 12 swap chain.", "Run the game in DX12 mode.");
    if (!rt.slInit) add("Streamline did not initialise (see the checklist: files / driver / GPU).", "");
    if (rt.slInit && !rt.haveDevice) add("No Direct3D 12 device was captured: the game created its device before this injector hooked D3D12, or it is not a DX12 game.", "Make sure winmm.dll is next to the game exe that really loads it (the game's own 'Binaries\\Win64' for Unreal Engine) and that no other proxy DLL replaces d3d12/dxgi.");
    if (rt.slInit && rt.haveDevice && !rt.slDev) add("Streamline never received the D3D12 device (slSetD3DDevice failed).", "Remove other injectors (ReShade, Special K, ...); check the [SL] lines in the log tail.");
    if (rt.slInit && rt.slDev && !rt.slRuntime) add("Streamline runtime functions (DLSS-G / Reflex / PCL plugins) could not be resolved.", "A plugin DLL did not load: re-copy all sl.*.dll + nvngx_dlssg.dll from one SDK.");
    if (!rt.haveQueue) add("No D3D12 DIRECT command queue was captured from the swap chain.", "");
    if (rt.compatBlocked) add("Streamline reports that DLSS frame generation is NOT supported on this system.", "");
    if (rt.fgWanted && rt.slRuntime && !rt.depthPinned) add(Fmt("No depth buffer was found at the internal render resolution (auto mode=%s, cfg renderw=%d renderh=%d, swap chain %ux%u).", g_cfg.renderW <= 0 ? "yes" : "no", g_cfg.renderW, g_cfg.renderH, rt.dispW, rt.dispH),
        "Set renderw=0 renderh=0 (automatic) in the config. If the game renders without a hardware depth buffer (or at a size far from the screen size) this injector cannot see it.");
    if (rt.fgWanted && rt.slRuntime && rt.depthPinned && !rt.velPinned) add("Depth buffer found, but no velocity buffer and the camera-only fallback is not active yet (velfallback / velwait).", "Keep velfallback=1. The census lines in the log tail list the render-target formats the game uses.");
    if (rt.fgWanted && rt.slRuntime && !rt.camLocked) add(Fmt("The camera matrices were never found (view uniform buffer scan: %ld projection-like matrices seen, %ld scans). The engine's view buffer layout is probably not the Unreal Engine layout this injector understands.", rt.projSeen, rt.scanRuns),
        "Unreal Engine 4.2x / 5.x are supported. For other engines the camera / jitter / motion data are not available, so frame generation cannot be fed. If this IS an Unreal Engine game, attach this report: the numbers above show the scan result.");
    if (rt.fgWanted && rt.camLocked && rt.camAge > 120 && !rt.everOn) add(Fmt("The camera was found once but has not been updated for %d frames (loading screen / cutscene / menu?).", rt.camAge), "Load into the actual 3D gameplay and wait ~5 seconds.");
    if (rt.mvFailed) add("The motion-vector compute pass could not be created (shader / PSO / resource creation failed).", "See the 'MV:' lines in the log tail below (shader compile or CreateCommittedResource error).");
    if (rt.tagRes > 0) add(Fmt("slSetTag returned error %d.", rt.tagRes), ""); if (rt.constRes > 0) add(Fmt("slSetConstants returned error %d.", rt.constRes), "");
    if (rt.optRes > 0) add(Fmt("slDLSSGSetOptions returned error %d.", rt.optRes), "");
    if (rt.haveState && rt.status) add(Fmt("DLSS-G state status = 0x%X: %s", rt.status, DecodeDlssgStatus(rt.status).c_str()), "");
    if (rt.haveState && rt.maxGen == 0) add("DLSS-G reports numFramesToGenerateMax = 0: frame generation is not available on this GPU / driver combination.", SlFixText(sl::Result::eErrorAdapterNotSupported));
}
static std::vector<std::string> g_lastFailReasons; static int g_everFailed = 0;

static void DiagWriteReport(const char* trigger);
static void DiagEvaluate(bool force) {
    static std::mutex s_evalMx; std::lock_guard<std::mutex> evk(s_evalMx);
    static int s_lastEval = -100000; static int s_lowRatio = 0, s_badStatus = 0; static DWORD s_firstFatalTick = 0;
    DiagRuntime rt; DiagRuntimeFill(rt);
    if (!force && rt.frame - s_lastEval < 120) return; s_lastEval = rt.frame;
    DiagVerdict v; std::vector<std::string> why, fixes;
    // 1. fatal checklist items (found before / without needing the game to run)
    { std::lock_guard<std::mutex> lk(g_diagMx); for (auto& it : g_diagItems) if (it.sev == DS_FAIL && it.fatal) { why.push_back(it.title + ": " + it.detail); if (!it.fix.empty()) fixes.push_back(it.fix); } }
    if (!why.empty()) { v.state = 2; v.headline = "Frame generation cannot work on this system / game"; }
    else if (!rt.fgWanted && !rt.everOn && rt.stage >= 2) { v.state = 3; v.headline = "Frame generation is switched off (menu / config)"; }
    else if (rt.stage < 2) { v.state = 3; v.headline = Fmt("Frame generation is switched off (stage=%d)", rt.stage); }
    else {
        if (rt.wasOn && rt.haveState) {
            if (rt.status) { if (++s_badStatus >= 3) { DiagBlockers(rt, why, fixes); } } else s_badStatus = 0;
            if (!rt.focused) { /* Streamline does not interpolate while the game window is not focused - nothing to judge */ } else if (rt.ratio < 1.3 && rt.baseFps > 8.0 && !rt.status) s_lowRatio++; else if (rt.ratio >= 1.5) s_lowRatio = 0;
            if (s_lowRatio >= 4) { why.push_back(Fmt("Frame generation was switched on but the swap chain is not presenting extra frames (measured output/real ratio %.2f, expected about 2.0).", rt.ratio));
                fixes.push_back("Make sure the game runs in a flip-model windowed/borderless mode with V-Sync handled by the game, no other overlay hooks Present, and that the GPU is not the bottleneck at the moment."); }
        }
        if (!rt.everOn && rt.fgWanted && rt.busyFrames >= 1500 && rt.frame > rt.warmup + 600) { DiagBlockers(rt, why, fixes); if (why.empty()) why.push_back("Frame generation has not started yet although the game has been rendering for a while (no single blocker identified - see the log tail)."); }
        if (!why.empty()) { v.state = 2; v.headline = rt.everOn ? "Frame generation started but is not working correctly" : "Frame generation did not start in this game"; }
        else if (rt.wasOn && rt.haveState && !rt.status && rt.ratio >= 1.5) { v.state = 1; v.headline = Fmt("Frame generation verified working (x%.1f)", rt.ratio); }
        else v.state = 0;
    }
    v.reasons = why; for (auto& f : fixes) { if (!v.firstFixes.empty()) v.firstFixes += " | "; v.firstFixes += f; }
    bool changed; std::string hl;
    { std::lock_guard<std::mutex> lk(g_verdictMx); changed = v.state != g_verdict.state || v.reasons != g_verdict.reasons; g_verdict = v; hl = v.headline; }
    if (!changed) return;
    Log("COMPAT verdict: state=%d \"%s\"", v.state, hl.c_str()); for (auto& r : v.reasons) Log("COMPAT   reason: %s", r.c_str());
    if (v.state == 2) { g_everFailed++; g_lastFailReasons = v.reasons; if (g_cfg.autoReport && g_reportWrites.load() < 4) DiagWriteReport(rt.frame ? "auto: frame generation cannot work / did not start" : "auto: fatal problem found before the game started"); }
    if (v.state == 1 && g_everFailed) Log("COMPAT: frame generation recovered and is verified working");
    Diag_OnVerdictChanged();
}

// ---------------------------------------------------------------- report file
static std::wstring ReportPathPick() {
    std::wstring p = g_dir + L"\\" SN_REPORT_NAME;
    FILE* f = _wfopen(p.c_str(), L"ab"); if (f) { fclose(f); return p; }
    wchar_t t[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, t); if (n) return std::wstring(t) + L"" SN_REPORT_NAME;
    return p;
}
static void DiagWriteReport(const char* trigger) {
    try {
        g_reportWrites++;
        DiagRuntime rt; DiagRuntimeFill(rt);
        std::vector<DiagItem> items; { std::lock_guard<std::mutex> lk(g_diagMx); items = g_diagItems; }
        DiagVerdict v; { std::lock_guard<std::mutex> lk(g_verdictMx); v = g_verdict; }
        std::string o; SYSTEMTIME st; GetLocalTime(&st);
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        o += "==================================================================================\n";
        o += " SN_DLSSG  -  DEBUG REPORT   (DX12 DLSS Frame Generation injector)\n";
        o += "==================================================================================\n";
        o += Fmt("Generated : %04u-%02u-%02u %02u:%02u:%02u   (trigger: %s)\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, trigger);
        o += Fmt("Build     : v25  %s %s   (Streamline headers %d.%d.%d)\n", __DATE__, __TIME__, SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
        o += "Game exe  : " + Narrow(exe) + "\n\n";
        o += "HOW TO REPORT THIS\n------------------\n";
        o += "  1. Open a new Issue on the project's GitHub page:\n       " SN_ISSUES_URL "\n";
        o += "  2. Write the NAME of the game (and where you got it: Steam / Epic / ...), your GPU and what you saw.\n";
        o += "  3. Attach THIS file:   " SN_REPORT_NAME "   (it is in the game's exe folder), and, if it exists, SN_DLSSG_log.txt (set log=1 in SN_DLSSG_cfg.txt and run the game once more for a full log).\n";
        o += "  Nothing personal is collected: the report contains hardware / driver / file names and the injector's own log lines only.\n\n";
        o += "VERDICT\n-------\n";
        o += Fmt("  state    : %s\n", v.state == 2 ? "FRAME GENERATION CANNOT WORK / DID NOT START" : v.state == 1 ? "WORKING" : v.state == 3 ? "switched off" : "still checking (no problem found yet)");
        if (!v.headline.empty()) o += "  headline : " + v.headline + "\n";
        for (auto& r : v.reasons) o += "  reason   : " + r + "\n";
        if (!v.reasons.empty()) { o += "\n  WHAT TO CHECK (most likely first):\n"; int n = 1; std::vector<std::string> shown;
            for (auto& it : items) if (it.sev == DS_FAIL && !it.fix.empty() && std::find(shown.begin(), shown.end(), it.fix) == shown.end()) { o += Fmt("   %d. %s\n", n++, it.fix.c_str()); shown.push_back(it.fix); }
            std::vector<std::string> why, fixes; DiagBlockers(rt, why, fixes); for (auto& f : fixes) if (std::find(shown.begin(), shown.end(), f) == shown.end()) { o += Fmt("   %d. %s\n", n++, f.c_str()); shown.push_back(f); }
            if (n == 1) o += "   (no automatic suggestion - please send the report)\n"; }
        o += "\nCHECKLIST\n---------\n";
        for (auto& it : items) { o += Fmt("%s %s\n", SevTag(it.sev), it.title.c_str()); if (!it.detail.empty()) { size_t p = 0; while (p <= it.detail.size()) { size_t e = it.detail.find('\n', p); if (e == std::string::npos) e = it.detail.size(); o += "         " + it.detail.substr(p, e - p) + "\n"; p = e + 1; if (e == it.detail.size()) break; } }
            if (!it.fix.empty() && it.sev >= DS_WARN) o += "         -> " + it.fix + "\n"; }
        o += "\nRUNTIME STATE (frame " + std::to_string(rt.frame) + ")\n-------------------------------\n";
        o += Fmt("  Streamline: init=%d device=%d runtime=%d | D3D12 device=%d direct queue=%d d3d12 swap chain=%d | compat-blocked=%d\n", rt.slInit, rt.slDev, rt.slRuntime, rt.haveDevice, rt.haveQueue, rt.d3d12Swap, rt.compatBlocked);
        o += Fmt("  Swap chain: %ux%u fmt=%d buffers=%u effect=%d | internal render size seen: %dx%d\n", rt.dispW, rt.dispH, rt.fmt, rt.bufCount, rt.swapEffect, rt.effW, rt.effH);
        o += Fmt("  CAMERA    : locked=%d (offsets proj=%d noaa=%d mv=%d) projection-like matrices seen=%ld scans=%ld last-update age=%d frames\n", rt.camLocked, rt.projOff, rt.noaaOff, rt.mvOff, rt.projSeen, rt.scanRuns, rt.camAge);
        o += Fmt("  DEPTH     : %s | VELOCITY: %s%s | MV pass: %s\n", rt.depthPinned ? "found" : "NOT found", rt.velPinned ? (rt.velDummy ? "none -> camera-only dummy" : "found") : "NOT found", "", rt.mvFailed ? "FAILED" : rt.mvReady ? "ready" : "not built yet");
        o += Fmt("  Streamline calls: tags=%d constants=%d setOptions=%d\n", rt.tagRes, rt.constRes, rt.optRes);
        o += Fmt("  DLSS-G state: %s status=0x%X (%s) maxGenerated=%u minWidthOrHeight=%u vram=%lluMB | measured output/real frame ratio %.2f | real fps %.1f\n", rt.haveState ? "read" : "not read yet", rt.status, DecodeDlssgStatus(rt.status).c_str(), rt.maxGen, rt.minWH, rt.vramMB, rt.ratio, rt.baseFps);
        o += Fmt("  Frame generation: wanted=%d everOn=%d on-now=%d paused=%d | frames=%d warmup=%d busy-3D-frames=%d | now: %s\n", rt.fgWanted, rt.everOn, rt.wasOn, rt.susp, rt.frame, rt.warmup, rt.busyFrames, rt.why);
        o += "\nCONFIG (as loaded / changed in the menu)\n----------------------------------------\n";
        o += Fmt("  fg=%d mult=%d stage=%d warmup=%d fgminfps=%d baseFpsLimit=%d reflexsleep=%d reflexplace=%d renderw=%d renderh=%d tol=%d\n", g_cfg.fg, g_cfg.mult, g_cfg.stage, g_cfg.warmup, g_cfg.fgMinFps, g_cfg.baseFpsLimit, g_cfg.reflexSleep, g_cfg.reflexPlace, g_cfg.renderW, g_cfg.renderH, g_cfg.tol);
        o += Fmt("  camauto=%d camhold=%d velfallback=%d viewrect=%d c2pauto=%d cutdetect=%d hudless=%d shadercache=%d psoretry=%d sldir=%s\n", g_cfg.camAuto, g_cfg.camHold, g_cfg.velFallback, g_cfg.viewRect, g_cfg.c2pAuto, g_cfg.cutDetect, g_cfg.hudless, g_cfg.shaderCache, g_cfg.psoRetry, Narrow(g_cfg.slDir.c_str()).c_str());
        o += Fmt("  menudetect=%d dynres=%d reflexmode=%s showvram=%d\n", g_cfg.menuDetect, g_cfg.dynRes, SnReflexModeName(g_cfg.reflexMode), g_cfg.showVram);
        o += Fmt("  overlay=%d key=%s hook=%d\n", g_cfg.overlay, KeyNameOf(g_cfg.overlayKey), g_cfg.overlayHook);
        o += "\nLOG TAIL (last lines of the injector's own log, kept in memory even when log=0)\n----------------------------------------------------------------------------\n";
        for (auto& l : LogRingSnapshot()) o += l + "\n";
        o += "\n==== end of report ====\n";
        std::wstring path = ReportPathPick(); std::wstring tmp = path + L".tmp"; bool ok = false;
        if (FILE* f = _wfopen(tmp.c_str(), L"wb")) { ok = fwrite(o.data(), 1, o.size(), f) == o.size(); ok = (fclose(f) == 0) && ok; if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; if (!ok) DeleteFileW(tmp.c_str()); }
        if (ok) { g_reportPath = path; g_reportSaved.store(true); }
        Log("debug report %s: %s (%zu bytes) trigger=%s", ok ? "written" : "WRITE FAILED", Narrow(path.c_str()).c_str(), o.size(), trigger);
    } catch (...) { Log("exception while writing the debug report"); }
}
