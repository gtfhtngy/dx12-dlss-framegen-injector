// SN_DLSSG - real NVIDIA DLSS Frame Generation (via Streamline / sl.dlss_g) injector, D3D12.
// Proxy for winmm.dll. Written for Scarlet Nexus (UE4, D3D12) - separate from the DLAA/DLSS-SR project.
//
// What it does:
//   1. Loads sl.interposer.dll (+ plugins next to it) and calls slInit BEFORE the game creates its D3D12 device.
//   2. Inline-hooks D3D12CreateDevice / CreateDXGIFactory* and redirects them to the Streamline interposer,
//      so Streamline sees the device/factory/swap chain (automatic hooking mode).
//   3. Finds the UE4 depth + velocity buffers and the view uniform buffer (camera matrices) exactly like the DLAA project,
//      converts velocity -> pixel motion vectors with a small compute shader and tags depth / MVs (and optionally a
//      HUD-less copy of the back buffer) every frame, sets Streamline common constants, Reflex/PCL markers and
//      DLSS-G options (mode on, numFramesToGenerate = mult-1).
// NOT verified on hardware by the author of this file - see README for the staged bring-up (stage=1,2,3).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcommon.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cfloat>

#include "MinHook.h"
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>

// ------------------------------------------------------------------ winmm forwarding (same as FG v5)
#include "winmm_stubs.inc"
extern "C" { void* g_fwd[WINMM_EXPORT_COUNT]; }
static HMODULE g_realWinmm = nullptr;
static void LoadRealWinmm() {
    if (g_realWinmm) return;
    wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\winmm.dll");
    g_realWinmm = LoadLibraryW(p);
    if (!g_realWinmm) return;
    for (int i = 0; i < WINMM_EXPORT_COUNT; i++) g_fwd[i] = (void*)GetProcAddress(g_realWinmm, g_fwdNames[i]);
}

// ------------------------------------------------------------------ MSVC-ABI struct returns (mingw passes the hidden pointer in a different order)
template <class T> static T VRet(void* obj, int idx) {
    typedef T* (STDMETHODCALLTYPE* F)(void*, T*);
    T out; memset(&out, 0, sizeof out);
    ((F)(*(void***)obj)[idx])(obj, &out);
    return out;
}
static D3D12_RESOURCE_DESC ResDesc(ID3D12Resource* r) { return VRet<D3D12_RESOURCE_DESC>(r, 10); }
static D3D12_COMMAND_QUEUE_DESC QDesc(ID3D12CommandQueue* q) { return VRet<D3D12_COMMAND_QUEUE_DESC>(q, 18); }
static D3D12_CPU_DESCRIPTOR_HANDLE CpuStart(ID3D12DescriptorHeap* h) { return VRet<D3D12_CPU_DESCRIPTOR_HANDLE>(h, 9); }
static D3D12_GPU_DESCRIPTOR_HANDLE GpuStart(ID3D12DescriptorHeap* h) { return VRet<D3D12_GPU_DESCRIPTOR_HANDLE>(h, 10); }

// ------------------------------------------------------------------ config / log
struct Cfg {
    int fg = 1;              // master switch
    int mult = 2;            // output frames per game frame (2 = numFramesToGenerate 1). Clamped to what the GPU supports
    int stage = 2;           // 0 = pass-through (no Streamline), 1 = Streamline init + support query only, 2 = frame generation, (hudless is separate)
    int log = 1;
    int hudless = 0;         // 1 = copy the back buffer right before the Nth draw into it and tag it as HUD-less
    int hudlessDraw = 2;     // N for hudless=1 (draw calls that target the back buffer, counted per frame)
    int renderW = 2000, renderH = 1124, tol = 100;  // internal render resolution of the game (same defaults as the DLAA project)
    int mvOff = 492;         // float index of ClipToPrevClip in the UE4 view uniform buffer (same as DLAA 'mvoff')
    int noAAOff = 132;       // float index of ViewToClipNoAA
    int projOff = 116;       // float index of ViewToClip (jittered)
    int jitSX = 1, jitSY = 1, mvSX = 1, mvSY = 1;
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
    int bufMaxMB = 192;      // upper bound for upload buffers kept alive by the camera tracker (texture-streaming staging buffers are huge during loads)
    int reflexPlace = 0;     // 1 = call slReflexSleep + SimulationStart at the START of the game frame (end of the previous Present) instead of inside Present
    int ngxLog = 0;
    int shaderCache = 1;     // 1 = build shaders before the game starts and keep them in SN_DLSSG_shaders.snsc (no run-time compile hitch)
    int splash = 1;          // 1 = show the progress window while the cache is being built (only when something has to be built)
    std::wstring slDir;      // folder with sl.interposer.dll etc. (default: next to the game exe)
} g_cfg;

static std::wstring g_dir;
static FILE* g_log = nullptr;
static std::mutex g_logMx;
static size_t g_logBytes = 0; static DWORD g_lastFlush = 0;
static void LogFlush() { if (!g_log) return; std::lock_guard<std::mutex> lk(g_logMx); fflush(g_log); }
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_logBytes > (24u << 20)) return;                 // never let the log grow without bound in long sessions
    va_list ap; va_start(ap, fmt); int n = vfprintf(g_log, fmt, ap); va_end(ap); fputc('\n', g_log);
    if (n > 0) g_logBytes += (size_t)n + 1;
    DWORD now = GetTickCount(); if (now - g_lastFlush > 1000) { fflush(g_log); g_lastFlush = now; }   // flushing every line stalls the render thread
}
static void LogW(const wchar_t* w) {
    if (!g_log) return; char b[1024]; WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr); Log("%s", b);
}
static void LoadCfg() {
    std::wstring p = g_dir + L"\\SN_DLSSG_cfg.txt";
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) {
        f = _wfopen(p.c_str(), L"w");
        if (f) {
            fprintf(f,
                "# Scarlet Nexus - DLSS Frame Generation (Streamline) config\n"
                "# fg        : 1 = on, 0 = off\n"
                "# mult      : 2 = 2x (1 generated frame). 3/4 need an RTX 50 series card (clamped automatically)\n"
                "# stage     : 0 = pass-through, 1 = only init Streamline + log feature support, 2 = full frame generation\n"
                "# log       : 1 = write SN_DLSSG_log.txt + Streamline log files next to the exe\n"
                "# hudless   : 1 = experimental HUD-less capture (copy of the back buffer right before the Nth draw to it)\n"
                "# hudlessdraw: N for hudless=1 (try 2,3,4,... until UI artifacts disappear; see log 'bbdraws')\n"
                "# renderw/renderh/tol : internal render resolution of the game (depth/velocity buffers) +- tolerance\n"
                "# camauto   : 1 = (default) automatically find the camera matrices in the UE4 view buffer when projoff/noaaoff/mvoff do not match this game (log: cam-scan LOCKED ...)\n# projoff/noaaoff : float index of ViewToClip / ViewToClipNoAA (116 / 132 in Scarlet Nexus)\n# mvoff     : float index of ClipToPrevClip inside the UE4 view uniform buffer (492 for Scarlet Nexus)\n"
                "# warmup    : frames before frame generation is switched on\n"
                "# reflexsleep : 1 = also call slReflexSleep every frame, baseFpsLimit = cap for the BASE fps (0 = none)\n"
                "# shadercache : 1 = pre-build shaders before the game starts and store them in SN_DLSSG_shaders.snsc (delete the file to force a rebuild)\n"
                "# splash    : 1 = show a progress window while shaders are being built (only appears when the cache has to be built)\n"
                "# sldir     : folder containing sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll (empty = exe folder)\n"
                "# fgminfps : >0 = turn frame generation off while the BASE fps is below this value (busy scenes), on again when it recovers (0 = never)\n# reflexplace : 1 = Reflex sleep/markers at the start of the game frame (experimental, try it if pacing is uneven)\n# log       : 1 = normal log, 2 = also verbose Streamline log (slow, big files - only for debugging)\n# fgdelay : frames the camera must be valid in a row before FG is switched on again (default 30) - avoids on/off flapping while a level loads\n# bufmaxmb : max MB of upload buffers the camera tracker may keep alive (default 192)\nfgminfps=0\nreflexplace=0\nfgdelay=30\nbufmaxmb=192\nfg=1\nmult=2\nstage=2\nlog=1\nhudless=0\nhudlessdraw=2\nrenderw=2000\nrenderh=1124\ntol=100\nmvoff=492\nwarmup=150\nreflexsleep=1\nbaseFpsLimit=0\nshowconsole=0\nshadercache=1\nsplash=1\nsldir=\n");
            fclose(f);
        }
    } else {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char k[64], v[400] = {};
            if (line[0] == '#') continue;
            if (sscanf(line, " %63[^=]=%399[^\r\n]", k, v) < 1) continue;
            int iv = atoi(v);
            if (!strcmp(k, "fg")) g_cfg.fg = iv; else if (!strcmp(k, "mult")) g_cfg.mult = iv; else if (!strcmp(k, "stage")) g_cfg.stage = iv;
            else if (!strcmp(k, "log")) g_cfg.log = iv; else if (!strcmp(k, "hudless")) g_cfg.hudless = iv; else if (!strcmp(k, "hudlessdraw")) g_cfg.hudlessDraw = iv;
            else if (!strcmp(k, "renderw")) g_cfg.renderW = iv; else if (!strcmp(k, "renderh")) g_cfg.renderH = iv; else if (!strcmp(k, "tol")) g_cfg.tol = iv;
            else if (!strcmp(k, "mvoff")) g_cfg.mvOff = iv; else if (!strcmp(k, "noaaoff")) g_cfg.noAAOff = iv; else if (!strcmp(k, "projoff")) g_cfg.projOff = iv;
            else if (!strcmp(k, "jitsx")) g_cfg.jitSX = iv; else if (!strcmp(k, "jitsy")) g_cfg.jitSY = iv; else if (!strcmp(k, "mvsx")) g_cfg.mvSX = iv; else if (!strcmp(k, "mvsy")) g_cfg.mvSY = iv;
            else if (!strcmp(k, "warmup")) g_cfg.warmup = iv; else if (!strcmp(k, "camstale")) g_cfg.camStale = iv; else if (!strcmp(k, "psoretry")) g_cfg.psoRetry = iv; else if (!strcmp(k, "farplane")) g_cfg.farPlane = iv; else if (!strcmp(k, "camauto")) g_cfg.camAuto = iv;
            else if (!strcmp(k, "reflexsleep")) g_cfg.reflexSleep = iv; else if (!strcmp(k, "baseFpsLimit")) g_cfg.baseFpsLimit = iv;
            else if (!strcmp(k, "showconsole")) g_cfg.showConsole = iv;
            else if (!strcmp(k, "fgminfps")) g_cfg.fgMinFps = iv; else if (!strcmp(k, "reflexplace")) g_cfg.reflexPlace = iv;
            else if (!strcmp(k, "fgdelay")) g_cfg.fgDelay = iv; else if (!strcmp(k, "bufmaxmb")) g_cfg.bufMaxMB = iv;
            else if (!strcmp(k, "shadercache")) g_cfg.shaderCache = iv; else if (!strcmp(k, "splash")) g_cfg.splash = iv;
            else if (!strcmp(k, "sldir")) { wchar_t w[400]; MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 400); g_cfg.slDir = w; }
        }
        fclose(f);
    }
    if (g_cfg.mult < 2) g_cfg.mult = 2;
    if (g_cfg.log && !g_log) g_log = _wfopen((g_dir + L"\\SN_DLSSG_log.txt").c_str(), L"w");
    Log("SN_DLSSG build " __DATE__ " " __TIME__ "  (Streamline SDK headers %d.%d.%d)", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
    Log("CFG fg=%d mult=%d stage=%d hudless=%d(N=%d) render=%dx%d+-%d mvoff=%d warmup=%d", g_cfg.fg, g_cfg.mult, g_cfg.stage, g_cfg.hudless, g_cfg.hudlessDraw, g_cfg.renderW, g_cfg.renderH, g_cfg.tol, g_cfg.mvOff, g_cfg.warmup);
}

static LONG CALLBACK DiagVEH(PEXCEPTION_POINTERS ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC000001D || c == 0xC0000094 || c == 0xC00000FD || c == 0xC0000409) {
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 8) {
            void* a = ep->ExceptionRecord->ExceptionAddress; HMODULE m = nullptr; char name[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m);
            if (m) GetModuleFileNameA(m, name, MAX_PATH);
            Log("first-chance exception 0x%08X at %p (%s +0x%llX) tid=%u  [informational: the game/OS may handle this itself, e.g. while shutting down - NOT necessarily a crash]", (unsigned)c, a, name, (unsigned long long)((BYTE*)a - (BYTE*)m), (unsigned)GetCurrentThreadId());
            void* st[24]; USHORT nf = CaptureStackBackTrace(0, 24, st, nullptr);
            for (USHORT i = 0; i < nf; i++) {
                HMODULE sm = nullptr; char sn[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)st[i], &sm);
                if (sm) GetModuleFileNameA(sm, sn, MAX_PATH);
                const char* b = strrchr(sn, '\\');
                Log("  #%02u %p %s+0x%llX", (unsigned)i, st[i], b ? b + 1 : sn, (unsigned long long)((BYTE*)st[i] - (BYTE*)sm));
            }
            LogFlush();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ vtable patch helpers
static void* Patch(void** vt, int idx, void* hk) {
    DWORD old; VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    void* o = vt[idx]; vt[idx] = hk; VirtualProtect(&vt[idx], sizeof(void*), old, &old); return o;
}
// per-vtable original lookup (a proxy class and a native class may have different vtables)
struct VHook { std::unordered_map<void**, void*> orig; std::mutex mx; };
static bool HookVt(VHook& h, void** vt, int idx, void* hk) {
    std::lock_guard<std::mutex> lk(h.mx);
    if (h.orig.count(vt)) return false;
    h.orig[vt] = Patch(vt, idx, hk); return true;
}
static void* OrigOf(VHook& h, void* obj) {
    std::lock_guard<std::mutex> lk(h.mx);
    auto it = h.orig.find(*(void***)obj); return it != h.orig.end() ? it->second : nullptr;
}

// ------------------------------------------------------------------ Streamline loading
using F_slInit = sl::Result(*)(const sl::Preferences&, uint64_t);
using F_slShutdown = sl::Result(*)();
using F_slIsFeatureSupported = sl::Result(*)(sl::Feature, const sl::AdapterInfo&);
using F_slGetFeatureFunction = sl::Result(*)(sl::Feature, const char*, void*&);
using F_slGetNewFrameToken = sl::Result(*)(sl::FrameToken*&, const uint32_t*);
using F_slSetConstants = sl::Result(*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
using F_slSetTagForFrame = sl::Result(*)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using F_slSetTag = sl::Result(*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using F_slGetNativeInterface = sl::Result(*)(void*, void**);
using F_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using F_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using F_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

static HMODULE g_sl = nullptr;
static F_slInit p_slInit; static F_slShutdown p_slShutdown; static F_slIsFeatureSupported p_slIsFeatureSupported;
static F_slGetFeatureFunction p_slGetFeatureFunction; static F_slGetNewFrameToken p_slGetNewFrameToken;
static F_slSetConstants p_slSetConstants; static F_slSetTagForFrame p_slSetTagForFrame; static F_slSetTag p_slSetTag;
static F_slGetNativeInterface p_slGetNativeInterface;
static F_D3D12CreateDevice pSL_D3D12CreateDevice; static F_CreateDXGIFactory pSL_CreateDXGIFactory, pSL_CreateDXGIFactory1; static F_CreateDXGIFactory2 pSL_CreateDXGIFactory2;
static PFun_slDLSSGSetOptions* p_DLSSGSetOptions; static PFun_slDLSSGGetState* p_DLSSGGetState;
static PFun_slPCLSetMarker* p_PCLSetMarker; static PFun_slReflexSetOptions* p_ReflexSetOptions; static PFun_slReflexSleep* p_ReflexSleep;
static bool g_slInitOk = false, g_slRuntimeOk = false, g_useFrameTags = false;

static const char* SlStr(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "eOk"; case sl::Result::eErrorIO: return "eErrorIO"; case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
    case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate"; case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS(enable Hardware-accelerated GPU scheduling!)";
    case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated"; case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
    case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported"; case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins";
    case sl::Result::eErrorVulkanAPI: return "eErrorVulkanAPI"; case sl::Result::eErrorDXGIAPI: return "eErrorDXGIAPI"; case sl::Result::eErrorD3DAPI: return "eErrorD3DAPI";
    case sl::Result::eErrorNRDAPI: return "eErrorNRDAPI"; case sl::Result::eErrorNVAPI: return "eErrorNVAPI"; case sl::Result::eErrorReflexAPI: return "eErrorReflexAPI";
    case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed"; case sl::Result::eErrorJSONParsing: return "eErrorJSONParsing";
    case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy"; case sl::Result::eErrorMissingResourceState: return "eErrorMissingResourceState";
    case sl::Result::eErrorInvalidIntegration: return "eErrorInvalidIntegration"; case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
    case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized"; case sl::Result::eErrorComputeFailed: return "eErrorComputeFailed";
    case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled"; case sl::Result::eErrorExceptionHandler: return "eErrorExceptionHandler";
    case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter"; case sl::Result::eErrorMissingConstants: return "eErrorMissingConstants";
    case sl::Result::eErrorDuplicatedConstants: return "eErrorDuplicatedConstants"; case sl::Result::eErrorMissingOrInvalidAPI: return "eErrorMissingOrInvalidAPI";
    case sl::Result::eErrorCommonConstantsMissing: return "eErrorCommonConstantsMissing"; case sl::Result::eErrorUnsupportedInterface: return "eErrorUnsupportedInterface";
    case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing"; case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
    case sl::Result::eErrorFeatureMissingHooks: return "eErrorFeatureMissingHooks"; case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
    case sl::Result::eErrorFeatureWrongPriority: return "eErrorFeatureWrongPriority"; case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
    case sl::Result::eErrorFeatureManagerInvalidState: return "eErrorFeatureManagerInvalidState"; case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
    case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
    default: return "(other, see sl_result.h)"; }
}
static void SlLogCb(sl::LogType t, const char* msg) { Log("[SL %d] %s", (int)t, msg ? msg : "(null)"); }

static bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool InitStreamline() {
    if (g_cfg.slDir.empty()) g_cfg.slDir = g_dir;
    LogW((L"Streamline dir: " + g_cfg.slDir).c_str());
    const wchar_t* need[] = { L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll" };
    bool all = true;
    for (auto n : need) { bool ok = FileExists(g_cfg.slDir + L"\\" + n); all &= ok; LogW((std::wstring(L"  ") + n + (ok ? L": found" : L": MISSING")).c_str()); }
    if (!FileExists(g_cfg.slDir + L"\\sl.interposer.dll")) { Log("sl.interposer.dll not found -> frame generation disabled (game runs normally)"); return false; }
    if (!all) Log("WARNING: some Streamline files are missing, DLSS-G will not load");
    g_sl = LoadLibraryExW((g_cfg.slDir + L"\\sl.interposer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_sl) { Log("LoadLibrary sl.interposer.dll failed err=%u", (unsigned)GetLastError()); return false; }
    auto gp = [&](const char* n) { return (void*)GetProcAddress(g_sl, n); };
    p_slInit = (F_slInit)gp("slInit"); p_slShutdown = (F_slShutdown)gp("slShutdown"); p_slIsFeatureSupported = (F_slIsFeatureSupported)gp("slIsFeatureSupported");
    p_slGetFeatureFunction = (F_slGetFeatureFunction)gp("slGetFeatureFunction"); p_slGetNewFrameToken = (F_slGetNewFrameToken)gp("slGetNewFrameToken");
    p_slSetConstants = (F_slSetConstants)gp("slSetConstants"); p_slSetTagForFrame = (F_slSetTagForFrame)gp("slSetTagForFrame"); p_slSetTag = (F_slSetTag)gp("slSetTag");
    p_slGetNativeInterface = (F_slGetNativeInterface)gp("slGetNativeInterface");
    pSL_D3D12CreateDevice = (F_D3D12CreateDevice)gp("D3D12CreateDevice"); pSL_CreateDXGIFactory = (F_CreateDXGIFactory)gp("CreateDXGIFactory");
    pSL_CreateDXGIFactory1 = (F_CreateDXGIFactory)gp("CreateDXGIFactory1"); pSL_CreateDXGIFactory2 = (F_CreateDXGIFactory2)gp("CreateDXGIFactory2");
    Log("SL exports: slInit=%p slGetFeatureFunction=%p slGetNewFrameToken=%p slSetConstants=%p slSetTagForFrame=%p slSetTag=%p native=%p | D3D12CreateDevice=%p Factory1=%p Factory2=%p",
        (void*)p_slInit, (void*)p_slGetFeatureFunction, (void*)p_slGetNewFrameToken, (void*)p_slSetConstants, (void*)p_slSetTagForFrame, (void*)p_slSetTag, (void*)p_slGetNativeInterface,
        (void*)pSL_D3D12CreateDevice, (void*)pSL_CreateDXGIFactory1, (void*)pSL_CreateDXGIFactory2);
    if (!p_slInit || !p_slGetFeatureFunction || !p_slGetNewFrameToken || !p_slSetConstants || !(p_slSetTagForFrame || p_slSetTag) || !pSL_D3D12CreateDevice || !pSL_CreateDXGIFactory1) {
        Log("interposer is missing required exports (wrong/old Streamline version?)"); return false; }
    g_useFrameTags = (p_slSetTagForFrame != nullptr);

    sl::Preferences pref{};
    pref.showConsole = g_cfg.showConsole != 0;
    pref.logLevel = (g_cfg.log >= 2) ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;   // verbose SL logging = thousands of lines/second -> stutter + huge files
    static const wchar_t* paths[1]; paths[0] = g_cfg.slDir.c_str();
    pref.pathsToPlugins = paths; pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = g_cfg.log ? g_dir.c_str() : nullptr;
    pref.logMessageCallback = &SlLogCb;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | (g_useFrameTags ? sl::PreferenceFlags::eUseFrameBasedResourceTagging : (sl::PreferenceFlags)0);
    static sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    pref.featuresToLoad = feats; pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eUnreal; pref.engineVersion = "4.26";
    pref.projectId = "5f0c8c2e-7a39-4b1e-9d6a-2c1b8e4f7d30";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    Log("slInit -> %d %s (frame tags: %d)", (int)r, SlStr(r), (int)g_useFrameTags);
    g_slInitOk = (r == sl::Result::eOk);
    return g_slInitOk;
}

// ------------------------------------------------------------------ resource tracking (ported from the DLAA project)
static bool IsInternal(UINT w, UINT h) { return abs((int)w - g_cfg.renderW) <= g_cfg.tol && abs((int)h - g_cfg.renderH) <= g_cfg.tol; }
struct ResMeta { ID3D12Resource* res = nullptr; UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE; std::atomic<int> useClear{0}, useSetRT{0}, useCopySrc{0}, useCopyDst{0}, useSRV{0}; int score = 0; bool held = false; bool cand = false; };
static std::unordered_map<void*, ResMeta> g_res;
static std::unordered_map<SIZE_T, void*> g_rtvToRes, g_dsvToRes;
static std::shared_mutex g_resMx; static std::mutex g_stateMx;
static UINT g_rtvInc = 0, g_dsvInc = 0;
static ID3D12Device* g_device = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;          // the game's direct queue (the one the swap chain was created with)
static std::atomic<int> g_frame{0};
static std::atomic<int> g_effW{0}, g_effH{0};   // real size of the pinned depth/velocity buffers (0 = not known yet -> cfg renderw/renderh)
static inline int EffW() { int v = g_effW.load(); return v ? v : g_cfg.renderW; }
static inline int EffH() { int v = g_effH.load(); return v ? v : g_cfg.renderH; }
static ID3D12Resource* g_pinVel = nullptr; static ID3D12Resource* g_pinDepth = nullptr;
static std::atomic<bool> g_forceReset{false};   // next DLSS-G frame is sent with reset=true (pins changed, hitch, MV buffers re-created)

static void RegisterRes(ID3D12Resource* res) {
    if (!res) return;
    D3D12_RESOURCE_DESC d = ResDesc(res);
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    bool depth = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    bool cand = IsInternal((UINT)d.Width, d.Height) && (depth || d.Format == DXGI_FORMAT_R16G16_UNORM);
    if (!cand) { std::shared_lock<std::shared_mutex> sl(g_resMx); if (!g_res.count((void*)res)) return; }   // loading creates thousands of RTV/DSV/SRVs: no exclusive lock for those
    std::lock_guard<std::shared_mutex> lk(g_resMx);
    auto& m = g_res[(void*)res];
    m.res = res; m.w = (UINT)d.Width; m.h = d.Height; m.fmt = d.Format; m.flags = d.Flags; m.cand = cand;
    if (cand && !m.held) { res->AddRef(); m.held = true; }   // keep alive: we keep raw pointers across frames
}
static void NoteUse(void* p, int kind) {   // hot path (every render-target bind / clear / copy on every worker thread): shared lock + atomic counters
    if (!p) return;
    std::shared_lock<std::shared_mutex> lk(g_resMx);
    auto it = g_res.find(p); if (it == g_res.end()) return;
    if (kind == 0) it->second.useClear++; else if (kind == 1) it->second.useSetRT++; else if (kind == 2) it->second.useCopySrc++; else if (kind == 3) it->second.useCopyDst++; else it->second.useSRV++;
}
static void* LookupRTV(D3D12_CPU_DESCRIPTOR_HANDLE h) { std::shared_lock<std::shared_mutex> lk(g_resMx); auto it = g_rtvToRes.find(h.ptr); return it != g_rtvToRes.end() ? it->second : nullptr; }

// ---- barrier state tracking (last StateAfter for candidate resources)
using PFN_ResBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
static PFN_ResBarrier oResBarrier = nullptr;
struct SubSt { UINT st[2] = { 0, 0 }; bool known[2] = { false, false }; };
static std::unordered_map<void*, SubSt> g_state;
static void STDMETHODCALLTYPE hkResBarrier(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    oResBarrier(cl, n, b);
    std::shared_lock<std::shared_mutex> lk(g_resMx);     // thousands of barriers per frame on many threads: shared lock, exclusive only for the (rare) candidate
    for (UINT i = 0; i < n; i++) {
        if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        void* r = b[i].Transition.pResource; UINT sub = b[i].Transition.Subresource;
        if (sub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && sub > 1) continue;
        auto it = g_res.find(r); if (it == g_res.end() || !it->second.cand) continue;
        std::lock_guard<std::mutex> sl(g_stateMx);
        auto& ss = g_state[r]; UINT st = (UINT)b[i].Transition.StateAfter;
        if (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) { ss.st[0] = ss.st[1] = st; ss.known[0] = ss.known[1] = true; }
        else { ss.st[sub] = st; ss.known[sub] = true; }
    }
}
static bool GetSubState(void* r, UINT sub, UINT* st) { std::lock_guard<std::mutex> lk(g_stateMx); auto it = g_state.find(r); if (it == g_state.end() || !it->second.known[sub]) return false; *st = it->second.st[sub]; return true; }

// ---- camera / jitter from the UE4 view uniform buffer (upload heap buffers are mapped once and kept)
struct BufInfo { UINT64 size = 0; ID3D12Resource* res = nullptr; BYTE* map = nullptr; std::atomic<int> lastUse{0}; };
static std::map<UINT64, BufInfo> g_bufs; static std::shared_mutex g_bufMx;
static float g_vtcNoAA[16] = {}, g_c2p[16] = {}; static float g_jitPX = 0.f, g_jitPY = 0.f;
static std::atomic<int> g_camFrame{-1}; static std::mutex g_camMx;
// The registry keeps one reference to every upload buffer (>=64 KB) so the mapped pointer stays valid.
// v13 never let go of them: after a long session the 4096-entry cap was full of buffers the game had destroyed long ago,
// NEW constant-buffer pages were no longer registered -> the view buffer was "not found" -> frame generation flapped on/off (stutter + ghosting).
// Now: a buffer whose only remaining reference is ours is dead for the game -> unmap + release it.   (caller holds g_bufMx exclusively)
static UINT64 g_bufBytes = 0; static DWORD g_lastPrune = 0; static int g_regSincePrune = 0; static long g_prunedTotal = 0;
static void PruneBuffersLocked() {
    for (auto it = g_bufs.begin(); it != g_bufs.end();) {
        ID3D12Resource* r = it->second.res; r->AddRef(); ULONG c = r->Release();
        if (c <= 1) { g_bufBytes -= std::min<UINT64>(g_bufBytes, it->second.size); r->Unmap(0, nullptr); r->Release(); it = g_bufs.erase(it); g_prunedTotal++; } else ++it;
    }
    g_lastPrune = GetTickCount(); g_regSincePrune = 0;
}
static void RegisterUploadBuf(ID3D12Resource* r) {
    if (!r) return;
    D3D12_RESOURCE_DESC d = ResDesc(r); if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width < 65536) return;
    if (d.Width > (UINT64)g_cfg.bufMaxMB * (1ull << 20) / 4) return;          // a camera/constant-buffer page is never this big; these are texture-streaming / mesh staging buffers
    D3D12_HEAP_PROPERTIES hp; D3D12_HEAP_FLAGS hf; if (FAILED(r->GetHeapProperties(&hp, &hf)) || hp.Type != D3D12_HEAP_TYPE_UPLOAD) return;
    std::unique_lock<std::shared_mutex> lk(g_bufMx);
    // while the game streams/loads it creates and destroys lots of big upload buffers. Every one we hold keeps its memory alive -> prune by TIME and BYTES, not by frame count
    // (frames are rare during loading screens, so the v14 "every 120 frames" prune could let hundreds of MB of dead staging memory pile up)
    DWORD now = GetTickCount();
    if (g_bufBytes > (UINT64)g_cfg.bufMaxMB * (1ull << 20) || (++g_regSincePrune >= 64 && now - g_lastPrune > 250) || g_bufs.size() >= 6000) PruneBuffersLocked();
    if (g_bufBytes > (UINT64)g_cfg.bufMaxMB * (1ull << 20) || g_bufs.size() >= 12000) return;
    void* ptr = nullptr; if (FAILED(r->Map(0, nullptr, &ptr)) || !ptr) return;
    r->AddRef();
    auto& e = g_bufs[r->GetGPUVirtualAddress()];
    e.size = d.Width; e.res = r; e.map = (BYTE*)ptr; e.lastUse.store(g_frame.load()); g_bufBytes += d.Width;
}
static std::atomic<long> g_cbvCalls{0}, g_cbvMiss{0}, g_projSeen{0}, g_scanRuns{0}; static std::atomic<int> g_camLocked{0};
static std::atomic<int> g_lastJitFrame{-1000};
static bool IsProj(const float* m, bool loose) {
    float ref = (float)EffW() / (float)EffH(); float asp = m[5] / m[0];
    bool aspOk = loose ? (asp > 1.2f && asp < 2.6f) : fabsf(asp - ref) < 0.03f;
    return m[0] > 0.2f && m[5] > 0.2f && m[1] == 0.f && m[2] == 0.f && m[3] == 0.f && m[4] == 0.f && m[6] == 0.f && m[7] == 0.f
        && fabsf(m[11]) == 1.f && m[12] == 0.f && m[13] == 0.f && m[15] == 0.f && aspOk;
}
static bool LooksLikeC2P(const float* c) {
    for (int i = 0; i < 16; i++) if (!std::isfinite(c[i]) || fabsf(c[i]) > 4.f) return false;
    return fabsf(c[0] - 1.f) < 0.5f && fabsf(c[5] - 1.f) < 0.5f && fabsf(c[10] - 1.f) < 0.5f && fabsf(c[15] - 1.f) < 0.5f;
}
// Auto-detect: find ViewToClip (jittered) + ViewToClipNoAA (+16..64 floats later) + ClipToPrevClip (identity-like) in a UE4 view uniform buffer
static void DumpViewBuf(const float* p, int avail, UINT64 off, const char* why) {
    static int nd = 0, lastFr = -100000; int fr = g_frame.load();
    if (nd >= 4 || fr < 300 || fr - lastFr < 300) return;
    nd++; lastFr = fr;
    FILE* f = _wfopen((g_dir + L"\\SN_DLSSG_viewbuf.txt").c_str(), L"a"); if (!f) return;
    fprintf(f, "=== dump %d frame %d bufoffset %llu floats %d (%s) ===\n", nd, fr, (unsigned long long)off, avail, why);
    for (int i = 0; i + 4 <= avail; i += 4) fprintf(f, "%4d: %.8g %.8g %.8g %.8g\n", i, p[i], p[i + 1], p[i + 2], p[i + 3]);
    fclose(f); Log("cam-scan: wrote view-buffer dump %d to SN_DLSSG_viewbuf.txt (move/rotate the camera a bit before the next one)", nd);
}
static void ScanViewBuffer(const float* p, int avail, UINT64 off) {
    static int s_frame = -1, s_budget = 0; int fr = g_frame.load();
    if (fr != s_frame) { s_frame = fr; s_budget = 24; }   // v16: was 600 full-buffer scans per frame if (s_budget-- <= 0) return;
    g_scanRuns++;
    int ji = -1, nj = -1;
    for (int i = 0; i + 16 <= avail && ji < 0; i += 4) {
        if (!IsProj(p + i, true)) continue; g_projSeen++;
        for (int j = i + 4; j <= i + 128 && j + 16 <= avail; j += 4) {
            const float* a = p + i; const float* b = p + j;
            if (IsProj(b, true) && fabsf(b[8]) < 1e-9f && fabsf(b[9]) < 1e-9f && fabsf(a[0] - b[0]) <= 1e-5f * fabsf(a[0]) && fabsf(a[5] - b[5]) <= 1e-5f * fabsf(a[5]) && ((a[8] != 0.f || a[9] != 0.f) || !memcmp(a, b, 64))) { ji = i; nj = j; break; }
        }
    }
    if (ji < 0) {
        static unsigned seen[16]; static int nseen = 0, nlog = 0; unsigned h = 2166136261u; int cnt = 0; char line[900]; int len = 0; line[0] = 0;
        for (int i = 0; i + 16 <= avail; i += 4) if (IsProj(p + i, true)) { cnt++; h = (h ^ (unsigned)i) * 16777619u; if (len < 760) len += snprintf(line + len, sizeof line - len, " [%d: m0=%.4f m5=%.4f m8=%.5f m9=%.5f m10=%.3f m11=%.1f m14=%.3f]", i, p[i], p[i + 5], p[i + 8], p[i + 9], p[i + 10], p[i + 11], p[i + 14]); }
        int jc = 0; for (int i = 0; i + 16 <= avail; i += 4) if (IsProj(p + i, true) && (p[i + 8] != 0.f || p[i + 9] != 0.f)) jc++;
        if (jc >= 2) DumpViewBuf(p, avail, off, "2+ jittered projections, no NoAA pair");
        if (cnt > 0 && nlog < 25) { bool dup = false; for (int q = 0; q < nseen; q++) if (seen[q] == h) dup = true; if (!dup) { if (nseen < 16) seen[nseen++] = h; nlog++; Log("cam-scan: no jittered/NoAA pair, but %d projection-like matrices (buffer offset %llu, avail %d floats):%s", cnt, (unsigned long long)off, avail, line); } }
        return;
    }
    int k = -1; for (int q = nj + 16; q + 16 <= avail; q += 4) if (LooksLikeC2P(p + q)) { k = q; break; }
    static int n = 0; static int li = -1, lj = -1, lk = -1, hits = 0;
    if (k < 0) { DumpViewBuf(p, avail, off, "pair found, no ClipToPrevClip"); if (n++ < 10) Log("cam-scan: projection pair at floats %d/%d (buffer offset %llu, avail %d floats, m0=%.4f m5=%.4f aspect=%.4f) but no identity-like ClipToPrevClip found after it", ji, nj, (unsigned long long)off, avail, p[ji], p[ji + 5], p[ji + 5] / p[ji]); return; }
    if (ji == li && nj == lj && k == lk) hits++; else { li = ji; lj = nj; lk = k; hits = 1; if (n++ < 20) Log("cam-scan: candidate projoff=%d noaaoff=%d mvoff=%d (jitter=%.5f,%.5f aspect=%.4f c2p diag=%.5f %.5f %.5f %.5f)", ji, nj, k, p[ji + 8], p[ji + 9], p[ji + 5] / p[ji], p[k], p[k + 5], p[k + 10], p[k + 15]); }
    if (hits >= 5) {
        g_cfg.projOff = ji; g_cfg.noAAOff = nj; g_cfg.mvOff = k; g_camLocked = 1;
        Log("cam-scan: LOCKED projoff=%d noaaoff=%d mvoff=%d  -> put these three lines in SN_DLSSG_cfg.txt to skip the scan next time", ji, nj, k);
    }
}

// ---- "legacy" UE4 view-buffer layout (CODE VEIN 1; found from the raw dumps): no ClipToPrevClip / ViewToClipNoAA in the buffer.
//   float index: 16 WorldToClip(jittered) | 96 ViewToClip(jittered) | 252 PrevViewProj(prev jitter) | 456 TemporalAAJitter(cur xy, prev xy) | 472 ViewSizeAndInvSize
//   ClipToPrevClip (no-AA) is built here: C = inverse(WorldToClip_noAA) * PrevViewProj_noAA   (row-vector convention like UE)
static bool Inv4d(const double* m, double* out) {
    double a[4][8]; for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { a[r][c] = m[r * 4 + c]; a[r][4 + c] = (r == c) ? 1.0 : 0.0; }
    for (int i = 0; i < 4; i++) {
        int piv = i; for (int r = i + 1; r < 4; r++) if (fabs(a[r][i]) > fabs(a[piv][i])) piv = r;
        if (fabs(a[piv][i]) < 1e-30) return false;
        if (piv != i) for (int c = 0; c < 8; c++) std::swap(a[i][c], a[piv][c]);
        double d = 1.0 / a[i][i]; for (int c = 0; c < 8; c++) a[i][c] *= d;
        for (int r = 0; r < 4; r++) if (r != i) { double f = a[r][i]; if (f != 0.0) for (int c = 0; c < 8; c++) a[r][c] -= f * a[i][c]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][4 + c];
    return true;
}
static std::atomic<int> g_lastRealViewFrame{-100000};
static bool TryLegacyLayout(const float* p, int availF) {
    if (availF < 480) return false;
    const float* a = p + 96;
    if (!IsProj(a, false)) return false;
    if (fabsf(p[456] - a[8]) > 1e-7f || fabsf(p[457] - a[9]) > 1e-7f) return false;                 // TemporalAAJitter must match the projection
    if (!(p[472] >= 320.f && p[473] >= 200.f) || fabsf(p[472] / p[473] - a[5] / a[0]) > 0.05f) return false;   // ViewSizeAndInvSize consistent with the projection aspect
    if (fabsf(p[458]) > 0.02f || fabsf(p[459]) > 0.02f || !std::isfinite(p[458]) || !std::isfinite(p[459])) return false;
    for (int i = 0; i < 16; i++) if (!std::isfinite(p[16 + i]) || !std::isfinite(p[252 + i])) return false;
    // main camera vs. dummy identity view (UI / default view): identity rotation + zero pre-view translation
    static const float kRot[16] = { 0,0,1,0, 1,0,0,0, 0,1,0,0, 0,0,0,1 };
    bool dummy = !memcmp(p + 32, kRot, sizeof kRot) && !memcmp(p, p + 16, 64);
    int fr = g_frame.load();
    if (!dummy) g_lastRealViewFrame = fr; else if (fr - g_lastRealViewFrame < 120) return true;   // a real view was seen recently: ignore the dummy view (return true = handled)
    double W[16], Wp[16], Wi[16], C[16];
    double jx = p[456], jy = p[457], pjx = p[458], pjy = p[459];
    for (int i = 0; i < 16; i++) { W[i] = p[16 + i]; Wp[i] = p[252 + i]; }
    for (int r = 0; r < 4; r++) { W[r * 4 + 0] -= W[r * 4 + 3] * jx; W[r * 4 + 1] -= W[r * 4 + 3] * jy; Wp[r * 4 + 0] -= Wp[r * 4 + 3] * pjx; Wp[r * 4 + 1] -= Wp[r * 4 + 3] * pjy; }
    if (!Inv4d(W, Wi)) return true;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double v = 0; for (int k = 0; k < 4; k++) v += Wi[r * 4 + k] * Wp[k * 4 + c]; C[r * 4 + c] = v; }
    float c2p[16], vtc[16]; for (int i = 0; i < 16; i++) { c2p[i] = (float)C[i]; if (!std::isfinite(c2p[i]) || fabsf(c2p[i]) > 1e6f) return true; }
    memcpy(vtc, a, sizeof vtc); vtc[8] = 0.f; vtc[9] = 0.f;   // ViewToClipNoAA = ViewToClip without the jitter
    { std::lock_guard<std::mutex> lk2(g_camMx);
      memcpy(g_vtcNoAA, vtc, sizeof g_vtcNoAA); memcpy(g_c2p, c2p, sizeof g_c2p);
      g_jitPX = a[8] * 0.5f * (float)EffW() * (float)g_cfg.jitSX; g_jitPY = -a[9] * 0.5f * (float)EffH() * (float)g_cfg.jitSY;
      g_camFrame = fr; }
    if (g_lastJitFrame < fr) g_lastJitFrame = fr;
    static int n = 0; if (n++ < 8) Log("camera(legacy layout): frame=%d %s jitter px=(%.4f,%.4f) m0=%.5f m5=%.5f near=%.3f viewsize=%.0fx%.0f c2p diag=%.5f %.5f %.5f %.5f", fr, dummy ? "DUMMY-identity-view" : "main-view", g_jitPX, g_jitPY, a[0], a[5], a[14], p[472], p[473], c2p[0], c2p[5], c2p[10], c2p[15]);
    return true;
}
// Parses one view-buffer candidate (a COPY in normal memory). true = recognised as a camera view buffer.
static bool ParseViewBuffer(const float* p, int availF, UINT64 off) {
    bool strict = availF >= std::max(g_cfg.mvOff + 16, 148) && g_cfg.projOff >= 0 && g_cfg.noAAOff >= 0 && g_cfg.mvOff >= 0;
    const float* a = nullptr; const float* b = nullptr; const float* c = nullptr;
    if (strict) { a = p + g_cfg.projOff; b = p + g_cfg.noAAOff; c = p + g_cfg.mvOff;
        if (!IsProj(a, false) || !IsProj(b, false) || b[8] != 0.f || b[9] != 0.f || a[0] != b[0] || a[5] != b[5]) strict = false; }
    if (strict) { for (int i = 0; i < 16; i++) if (!std::isfinite(c[i])) strict = false; }
    if (!strict) {
        if (g_cfg.camAuto && TryLegacyLayout(p, availF)) return true;
        if (g_cfg.camAuto && !g_camLocked.load() && availF >= 64) ScanViewBuffer(p, availF, off);
        return false;
    }
    int fr = g_frame.load();
    bool jittered = (a[8] != 0.f || a[9] != 0.f);
    if (jittered) g_lastJitFrame = fr;
    else if (fr - g_lastJitFrame < 90) return true;   // un-jittered view of a non-TAA pass while TAA is active: keep the last real one (no TAA at all -> accepted after 90 frames)
    std::lock_guard<std::mutex> lk2(g_camMx);
    memcpy(g_vtcNoAA, b, sizeof g_vtcNoAA); memcpy(g_c2p, c, sizeof g_c2p);
    g_jitPX = a[8] * 0.5f * (float)EffW() * (float)g_cfg.jitSX; g_jitPY = -a[9] * 0.5f * (float)EffH() * (float)g_cfg.jitSY;
    g_camFrame = fr;
    static int n = 0; if (n++ < 6) Log("camera: frame=%d jitter px=(%.4f,%.4f) near=%.3f fov-ish m0=%.5f m5=%.5f c2p diag=%.5f %.5f %.5f %.5f%s", fr, g_jitPX, g_jitPY, b[14], b[0], b[5], c[0], c[5], c[10], c[15], jittered ? "" : " (NO TAA jitter)");
    return true;
}

// TrackCamera runs on EVERY SetGraphicsRootConstantBufferView / SetComputeRootConstantBufferView of the game, on every worker thread
// (thousands per frame in a busy scene). v13 took a global mutex, did a std::map lookup and read the (write-combined = very slow to read)
// upload memory on each of those calls. Now the common case is a few thread-local compares and an early return:
//   - addresses already recognised as view buffers are re-read at most 3x per thread per frame (only the first ~2.5 KB)
//   - unknown addresses are examined only when bound >=2x in a frame on one thread, max 24 probes per frame, and a rejected address is skipped for 30 frames
struct TcEntry { UINT64 va = 0; int frame = -1; int cnt = 0; };
struct TcTls { TcEntry e[256]; int readFrame = -1, readCnt = 0; };
static thread_local TcTls t_tls;
static std::atomic<UINT64> g_knownVA[16]; static std::atomic<unsigned> g_knownPos{0};
static std::atomic<UINT64> g_negVA[256]; static std::atomic<int> g_negFrame[256];
static std::atomic<int> g_probeFrame{-1}, g_probeCnt{0};
static inline unsigned HashVA(UINT64 va) { return (unsigned)(((va >> 8) * 0x9E3779B97F4A7C15ull) >> 56); }
static bool IsKnownVA(UINT64 va) { for (int i = 0; i < 16; i++) if (g_knownVA[i].load(std::memory_order_relaxed) == va) return true; return false; }
static void AddKnownVA(UINT64 va) { if (!IsKnownVA(va)) g_knownVA[g_knownPos.fetch_add(1) & 15].store(va); }
static void DropKnownVA(UINT64 va) { for (int i = 0; i < 16; i++) { UINT64 e = va; g_knownVA[i].compare_exchange_strong(e, 0); } }
static __attribute__((noinline)) void ProbeView(UINT64 va, bool known, int fr) {
    float tmp[3072]; int nf = 0; UINT64 off = 0;
    {   // copy the head of the buffer once into normal memory (never parse straight from the write-combined mapping)
        std::shared_lock<std::shared_mutex> lk(g_bufMx);
        auto it = g_bufs.upper_bound(va); if (it == g_bufs.begin()) { g_cbvMiss++; return; } --it;
        off = va - it->first; if (off >= it->second.size) { g_cbvMiss++; return; }
        int availF = (int)std::min<UINT64>((it->second.size - off) / 4, 3072);
        nf = known ? std::min(availF, 640) : availF;
        if (nf < 64) return;
        memcpy(tmp, it->second.map + off, (size_t)nf * 4);
        it->second.lastUse.store(fr, std::memory_order_relaxed);
    }
    bool ok = ParseViewBuffer(tmp, nf, off);
    if (ok) { if (!known) AddKnownVA(va); }
    else if (known) DropKnownVA(va);
    else { unsigned h = HashVA(va); g_negVA[h].store(va); g_negFrame[h].store(fr); }
}
static std::atomic<long> g_probeN{0}, g_probeNs{0}, g_cbvPerFrame{0};
static inline long long NowNs() { static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER n; QueryPerformanceCounter(&n); return (long long)((double)n.QuadPart * 1e9 / (double)f.QuadPart); }
static void TrackCamera(UINT64 va) {
    g_cbvCalls++;
    int fr = g_frame.load(std::memory_order_relaxed);
    TcTls& t = t_tls;
    bool known = IsKnownVA(va);
    if (known) {
        if (t.readFrame != fr) { t.readFrame = fr; t.readCnt = 0; }
        if (t.readCnt >= 3) return;
        t.readCnt++;
    } else {
        unsigned h = HashVA(va);
        TcEntry& e = t.e[h];
        if (e.va != va || e.frame != fr) { e.va = va; e.frame = fr; e.cnt = 0; }
        if (++e.cnt != 2) return;                       // only buffers bound >= 2x in one frame on this thread (the view buffer is bound by every pass)
        // v16: while a BUSY 3D scene is rendering but no camera was found for >2 frames, search harder (more probes per frame, short negative cache:
        // the allocator re-uses addresses, so an address rejected earlier may hold the view buffer now). Menus/loading (few CBV calls) are not boosted.
        bool lost = (fr - g_camFrame.load(std::memory_order_relaxed)) > 2 && g_cbvPerFrame.load(std::memory_order_relaxed) > 1000;
        if (g_negVA[h].load(std::memory_order_relaxed) == va && fr - g_negFrame[h].load(std::memory_order_relaxed) < (lost ? 2 : 30)) return;
        if (g_probeFrame.load(std::memory_order_relaxed) != fr) { g_probeFrame.store(fr); g_probeCnt.store(0); }
        if (g_probeCnt.fetch_add(1) >= (lost ? 96 : 24)) return;
    }
    long long t0 = NowNs(); ProbeView(va, known, fr); g_probeNs += NowNs() - t0; g_probeN++;
}

// ---- hooks on the native D3D12 vtables
using PFN_CreateRTV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateDSV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateSRV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CreateCommitted = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreatePlaced = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_OMSetRT = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_ClearRTV = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using PFN_CopyTex = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using PFN_SetRootCbv = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_DrawInst = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIdxInst = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_Exec = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_CreateRTV oCreateRTV; static PFN_CreateDSV oCreateDSV; static PFN_CreateSRV oCreateSRV; static PFN_CopyDescSimple oCopyDescSimple;
static PFN_CreateCommitted oCreateCommitted; static PFN_CreatePlaced oCreatePlaced; static PFN_OMSetRT oOMSetRT; static PFN_ClearRTV oClearRTV; static PFN_ClearDSV oClearDSV;
static PFN_CopyTex oCopyTex; static PFN_SetRootCbv oSetGCbv, oSetCCbv; static PFN_DrawInst oDrawInst; static PFN_DrawIdxInst oDrawIdxInst; static PFN_Exec oExec;

// hudless capture
struct RtTrack { ID3D12GraphicsCommandList* cl = nullptr; void* rt0 = nullptr; };
static thread_local RtTrack t_rt;
static std::atomic<int> g_bbDraws{0}, g_hudStamp{-1};
static std::unordered_map<void*, bool> g_bbCache;
static UINT g_dispW = 0, g_dispH = 0;
static ID3D12Resource* g_hudless = nullptr; static D3D12_RESOURCE_STATES g_hudCur = D3D12_RESOURCE_STATE_COPY_DEST; static UINT g_hudW = 0, g_hudH = 0; static DXGI_FORMAT g_hudFmt = DXGI_FORMAT_UNKNOWN;
static std::mutex g_hudMx;

static void STDMETHODCALLTYPE hkCreateRTV(ID3D12Device* dev, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (!g_device) g_device = dev;
    RegisterRes(res);
    if (res) { std::lock_guard<std::shared_mutex> lk(g_resMx); g_rtvToRes[dest.ptr] = (void*)res; }
    oCreateRTV(dev, res, desc, dest);
}
static void STDMETHODCALLTYPE hkCreateDSV(ID3D12Device* dev, ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (!g_device) g_device = dev;
    RegisterRes(res);
    if (res) { std::lock_guard<std::shared_mutex> lk(g_resMx); g_dsvToRes[dest.ptr] = (void*)res; }
    oCreateDSV(dev, res, desc, dest);
}
static void STDMETHODCALLTYPE hkCreateSRV(ID3D12Device* dev, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (res) {
        D3D12_RESOURCE_DESC d = ResDesc(res);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && IsInternal((UINT)d.Width, d.Height)) { RegisterRes(res); NoteUse((void*)res, 4); }
    }
    oCreateSRV(dev, res, desc, dest);
}
static void STDMETHODCALLTYPE hkCopyDescSimple(ID3D12Device* dev, UINT num, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    oCopyDescSimple(dev, num, dest, src, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV && g_rtvInc) {
        std::lock_guard<std::shared_mutex> lk(g_resMx);
        for (UINT i = 0; i < num; i++) { auto it = g_rtvToRes.find(src.ptr + i * g_rtvInc); if (it != g_rtvToRes.end()) g_rtvToRes[dest.ptr + i * g_rtvInc] = it->second; }
    }
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV && g_dsvInc) {
        std::lock_guard<std::shared_mutex> lk(g_resMx);
        for (UINT i = 0; i < num; i++) { auto it = g_dsvToRes.find(src.ptr + i * g_dsvInc); if (it != g_dsvToRes.end()) g_dsvToRes[dest.ptr + i * g_dsvInc] = it->second; }
    }
}
static HRESULT STDMETHODCALLTYPE hkCreateCommitted(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv) {
    HRESULT hr = oCreateCommitted(d, hp, hf, rd, st, cv, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && hp && hp->Type == D3D12_HEAP_TYPE_UPLOAD && rd && rd->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        ID3D12Resource* r = nullptr; if (SUCCEEDED(((IUnknown*)*ppv)->QueryInterface(__uuidof(ID3D12Resource), (void**)&r)) && r) { RegisterUploadBuf(r); r->Release(); } }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkCreatePlaced(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv) {
    HRESULT hr = oCreatePlaced(d, h, off, rd, st, cv, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && rd && rd->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        ID3D12Resource* r = nullptr; if (SUCCEEDED(((IUnknown*)*ppv)->QueryInterface(__uuidof(ID3D12Resource), (void**)&r)) && r) { RegisterUploadBuf(r); r->Release(); } }
    return hr;
}
static void STDMETHODCALLTYPE hkSetGCbv(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { TrackCamera(va); oSetGCbv(cl, i, va); }
static void STDMETHODCALLTYPE hkSetCCbv(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { TrackCamera(va); oSetCCbv(cl, i, va); }

static void STDMETHODCALLTYPE hkOMSetRT(ID3D12GraphicsCommandList* cl, UINT num, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    oOMSetRT(cl, num, rts, single, dsv);
    t_rt.cl = cl; t_rt.rt0 = nullptr;
    if (rts) for (UINT i = 0; i < num && i < 8; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = single ? rts[0] : rts[i]; if (single && i > 0) h.ptr += i * g_rtvInc;
        void* r = LookupRTV(h); if (i == 0) t_rt.rt0 = r; NoteUse(r, 1);
    }
    if (dsv) { void* r = nullptr; { std::shared_lock<std::shared_mutex> lk(g_resMx); auto it = g_dsvToRes.find(dsv->ptr); if (it != g_dsvToRes.end()) r = it->second; } NoteUse(r, 1); }
}
static void STDMETHODCALLTYPE hkClearRTV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT* c, UINT nr, const D3D12_RECT* r) { oClearRTV(cl, rtv, c, nr, r); NoteUse(LookupRTV(rtv), 0); }
static void STDMETHODCALLTYPE hkClearDSV(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT nr, const D3D12_RECT* r) {
    oClearDSV(cl, dsv, f, d, s, nr, r);
    void* rr = nullptr; { std::shared_lock<std::shared_mutex> lk(g_resMx); auto it = g_dsvToRes.find(dsv.ptr); if (it != g_dsvToRes.end()) rr = it->second; } NoteUse(rr, 0);
}
static void STDMETHODCALLTYPE hkCopyTex(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (src && src->pResource) { RegisterRes(src->pResource); NoteUse((void*)src->pResource, 2); }
    if (dst && dst->pResource) { RegisterRes(dst->pResource); NoteUse((void*)dst->pResource, 3); }
    oCopyTex(cl, dst, x, y, z, src, box);
}

// ---- HUD-less capture: copy of the back buffer right before the Nth draw that targets it
static bool IsBackbufferRes(void* r) {
    if (!r || !g_dispW) return false;
    { std::lock_guard<std::shared_mutex> lk(g_resMx); auto it = g_bbCache.find(r); if (it != g_bbCache.end()) return it->second; }
    D3D12_RESOURCE_DESC d = ResDesc((ID3D12Resource*)r);
    bool bb = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_dispW && d.Height == g_dispH && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && d.MipLevels == 1 && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1;
    std::lock_guard<std::shared_mutex> lk(g_resMx); g_bbCache[r] = bb; return bb;
}
static void TransitionOne(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a; x.Transition.StateAfter = b; oResBarrier(cl, 1, &x);
}
static void CaptureHudless(ID3D12GraphicsCommandList* cl, ID3D12Resource* bb) {
    std::lock_guard<std::mutex> lk(g_hudMx);
    D3D12_RESOURCE_DESC d = ResDesc(bb);
    if (!g_hudless || g_hudW != d.Width || g_hudH != d.Height || g_hudFmt != d.Format) {
        if (g_hudless) { g_hudless->Release(); g_hudless = nullptr; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = d.Width; rd.Height = d.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = d.Format; rd.SampleDesc.Count = 1;
        HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_hudless);
        Log("hudless texture %llux%u fmt=%d -> hr=0x%08X", (unsigned long long)d.Width, d.Height, (int)d.Format, (unsigned)hr);
        if (FAILED(hr)) { g_hudless = nullptr; return; }
        g_hudW = (UINT)d.Width; g_hudH = d.Height; g_hudFmt = d.Format; g_hudCur = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (g_hudCur != D3D12_RESOURCE_STATE_COPY_DEST) TransitionOne(cl, g_hudless, g_hudCur, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionOne(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(g_hudless, bb);
    TransitionOne(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    TransitionOne(cl, g_hudless, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_hudCur = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
static void NoteDraw(ID3D12GraphicsCommandList* cl, const char* kind, UINT count) {
    if (t_rt.cl != cl || !IsBackbufferRes(t_rt.rt0)) return;
    int n = ++g_bbDraws;
    static std::atomic<int> logged{0};
    if (logged.load() < 160 && g_frame.load() > 200 && g_frame.load() < 215) { logged++; Log("bbdraw frame=%d #%d %s count=%u", g_frame.load(), n, kind, count); }
    if (g_cfg.hudless && n == g_cfg.hudlessDraw && g_hudStamp.load() != g_frame.load() && g_device) { CaptureHudless(cl, (ID3D12Resource*)t_rt.rt0); g_hudStamp = g_frame.load(); }
}
static void STDMETHODCALLTYPE hkDrawInst(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, UINT d) { NoteDraw(cl, "DrawInstanced", a); oDrawInst(cl, a, b, c, d); }
static void STDMETHODCALLTYPE hkDrawIdxInst(ID3D12GraphicsCommandList* cl, UINT a, UINT b, UINT c, INT d, UINT e) { NoteDraw(cl, "DrawIndexedInstanced", a); oDrawIdxInst(cl, a, b, c, d, e); }

static void STDMETHODCALLTYPE hkExec(ID3D12CommandQueue* q, UINT num, ID3D12CommandList* const* lists) {
    static int nq = 0;
    if (nq < 8) { D3D12_COMMAND_QUEUE_DESC qd = QDesc(q); Log("ExecuteCommandLists queue=%p type=%d (game queue=%p)", (void*)q, (int)qd.Type, (void*)g_queue); nq++; }
    oExec(q, num, lists);
}

// ------------------------------------------------------------------ deferred release of private command lists
struct DeferredCl { ID3D12GraphicsCommandList* cl; ID3D12CommandAllocator* al; int frame; };
static std::vector<DeferredCl> g_deferred; static std::mutex g_deferredMx;
static void DeferRelease(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* al) {
    std::lock_guard<std::mutex> lk(g_deferredMx); int now = g_frame.load();
    for (size_t i = 0; i < g_deferred.size();) { if (now - g_deferred[i].frame >= 8) { g_deferred[i].cl->Release(); g_deferred[i].al->Release(); g_deferred.erase(g_deferred.begin() + i); } else i++; }
    g_deferred.push_back({ cl, al, now });
}

// ------------------------------------------------------------------ MV compute pass: UE4 velocity (R16G16_UNORM, 0 = static) + depth + ClipToPrevClip -> pixel MVs (R16G16_FLOAT) + depth copy (R32_FLOAT)
static const char* kMvHlsl =
"cbuffer C : register(b0) { row_major float4x4 M; uint2 dim; float2 sgn; };\n"
"Texture2D<float2> V : register(t0);\n"
"Texture2D<float>  D : register(t1);\n"
"RWTexture2D<float2> O : register(u0);\n"
"RWTexture2D<float>  OD : register(u1);\n"
"[numthreads(8,8,1)]\n"
"void main(uint3 t : SV_DispatchThreadID)\n"
"{\n"
"    if (t.x >= dim.x || t.y >= dim.y) return;\n"
"    float2 v = V.Load(int3(t.xy, 0));\n"
"    float z = D.Load(int3(t.xy, 0));\n"
"    float2 back;\n"
"    if (v.x > 0.0) {\n"
"        back = (v - 32767.0/65535.0) / (0.499*0.5);\n"
"    } else {\n"
"        float2 sp = float2((t.x + 0.5)/dim.x*2.0 - 1.0, 1.0 - (t.y + 0.5)/dim.y*2.0);\n"
"        float4 pc = mul(float4(sp, z, 1.0), M);\n"
"        back = sp - pc.xy / pc.w;\n"
"    }\n"
"    O[t.xy] = float2(-back.x * 0.5 * dim.x * sgn.x, back.y * 0.5 * dim.y * sgn.y);\n"
"    OD[t.xy] = z;\n"
"}\n";
static ID3D12RootSignature* g_mvRS = nullptr; static ID3D12PipelineState* g_mvPSO = nullptr; static ID3D12DescriptorHeap* g_mvHeap = nullptr;
static ID3D12Resource* g_mvOut = nullptr; static ID3D12Resource* g_depthOut = nullptr; static UINT g_mvInc = 0; static bool g_mvFailed = false; static int g_mvSlot = 0;
// the MV / depth outputs are rotated over MV_BUFSETS sets: with frame generation the GPU may still be reading frame N's inputs while frame N+1 already writes its own
// (that is exactly what happens in GPU-bound busy scenes) -> one shared set = corrupted inputs = ghosting
static const int MV_BUFSETS = 3;
static ID3D12Resource* g_mvOutA[MV_BUFSETS] = {}; static ID3D12Resource* g_depthOutA[MV_BUFSETS] = {}; static bool g_setFirst[MV_BUFSETS] = {}; static int g_setPos = 0;
// ring of pre-created command allocators/lists + fence (v13 created and destroyed an allocator + list EVERY frame)
static const int MV_RING = 8;
static ID3D12CommandAllocator* g_ringAl[MV_RING] = {}; static ID3D12GraphicsCommandList* g_ringCl[MV_RING] = {}; static UINT64 g_ringVal[MV_RING] = {};
static ID3D12Fence* g_ringFence = nullptr; static UINT64 g_fenceNext = 0; static int g_ringPos = 0;
static std::vector<std::pair<ID3D12Resource*, int>> g_deferredRes; static std::mutex g_deferredResMx;
static void DeferResRelease(ID3D12Resource* r) { if (!r) return; std::lock_guard<std::mutex> lk(g_deferredResMx); g_deferredRes.push_back({ r, g_frame.load() }); }
static void FlushDeferredRes() {
    std::lock_guard<std::mutex> lk(g_deferredResMx); int now = g_frame.load();
    for (size_t i = 0; i < g_deferredRes.size();) { if (now - g_deferredRes[i].second >= 24) { g_deferredRes[i].first->Release(); g_deferredRes.erase(g_deferredRes.begin() + i); } else i++; }
}
static UINT g_mvW = 0, g_mvH = 0;
static const int MV_SETS = 16;
static const D3D12_RESOURCE_STATES kOutState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

#include "shadercache.inc"   // SNSC shader cache, pre-game warm-up + progress window, EnsureMvPipeline()

static std::mutex g_mvInitMx; static std::atomic<bool> g_mvPreIniting{false};
static bool InitMvPass(UINT W, UINT H) {
    std::lock_guard<std::mutex> imk(g_mvInitMx);
    if (g_mvPSO && g_mvOutA[0] && g_mvW == W && g_mvH == H) return true;
    if (g_mvFailed || !g_device) return false;
    g_mvFailed = true; DWORD tInit = GetTickCount();
    if (!EnsureMvPipeline(g_device)) return false;   // shaders come from the pre-built cache (compiled at run time only if there is none)
    HRESULT hr = S_OK;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4 * MV_SETS; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!g_mvHeap && FAILED(g_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_mvHeap))) { Log("MV: heap failed"); return false; }
    g_mvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* nm[MV_BUFSETS] = {}; ID3D12Resource* nd[MV_BUFSETS] = {};
    for (int i = 0; i < MV_BUFSETS; i++) {
        rd.Format = DXGI_FORMAT_R16G16_FLOAT;
        hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&nm[i]);
        if (FAILED(hr)) { Log("MV: mv texture 0x%08X", (unsigned)hr); break; }
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&nd[i]);
        if (FAILED(hr)) { Log("MV: depth texture 0x%08X", (unsigned)hr); break; }
    }
    if (FAILED(hr)) { for (int i = 0; i < MV_BUFSETS; i++) { if (nm[i]) nm[i]->Release(); if (nd[i]) nd[i]->Release(); } return false; }
    // resolution change: the old buffers may still be in use by the GPU -> released a few frames later (v13 leaked them)
    for (int i = 0; i < MV_BUFSETS; i++) { DeferResRelease(g_mvOutA[i]); DeferResRelease(g_depthOutA[i]); g_mvOutA[i] = nm[i]; g_depthOutA[i] = nd[i]; g_setFirst[i] = true; }
    g_mvOut = g_mvOutA[0]; g_depthOut = g_depthOutA[0];
    g_mvW = W; g_mvH = H; g_mvFailed = false; g_forceReset = true; Log("MV pass ready (%ux%u, MV R16G16_FLOAT + depth R32_FLOAT, %d buffer sets) in %u ms", W, H, MV_BUFSETS, (unsigned)(GetTickCount() - tInit)); return true;
}
static DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; }
}
// Builds a private command list, runs the MV pass on the game's queue. Returns true if g_mvOut/g_depthOut were refreshed.
static bool RunMvPass(UINT W, UINT H, ID3D12Resource* vel, ID3D12Resource* depth) {
    if (!g_device || !g_queue || !vel || !depth) return false;
    if (!InitMvPass(W, H)) return false;
    ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr; int ringSlot = -1;
    if (!g_ringFence) g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_ringFence);
    if (g_ringFence) {
        int s = g_ringPos % MV_RING;
        if (!g_ringAl[s]) {
            ID3D12CommandAllocator* a2 = nullptr; ID3D12GraphicsCommandList* c2 = nullptr;
            if (SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a2)) &&
                SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a2, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&c2))) { c2->Close(); g_ringAl[s] = a2; g_ringCl[s] = c2; g_ringVal[s] = 0; }
            else { if (c2) c2->Release(); if (a2) a2->Release(); }
        }
        if (g_ringAl[s] && g_ringFence->GetCompletedValue() >= g_ringVal[s] && SUCCEEDED(g_ringAl[s]->Reset()) && SUCCEEDED(g_ringCl[s]->Reset(g_ringAl[s], nullptr))) { al = g_ringAl[s]; cl = g_ringCl[s]; ringSlot = s; g_ringPos++; }
    }
    if (!cl) {   // ring slot still busy on the GPU (or ring creation failed): fall back to a one-off list
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al))) return false;
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl))) { al->Release(); return false; }
    }
    int set = (g_setPos++) % MV_BUFSETS; g_mvOut = g_mvOutA[set]; g_depthOut = g_depthOutA[set]; bool firstUse = g_setFirst[set]; g_setFirst[set] = false;
    // input transitions (tracked states of the game's resources) -> compute-readable, restored afterwards
    D3D12_RESOURCE_BARRIER pre[8], post[8]; UINT nb = 0;
    struct In { ID3D12Resource* r; D3D12_RESOURCE_STATES want; const char* n; UINT planes; } ins[2] = {
        { vel, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "velocity", 1 },
        { depth, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), "depth", 2 } };
    auto add = [&](ID3D12Resource* res, UINT sub, UINT from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = res; b.Transition.Subresource = sub;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)from; b.Transition.StateAfter = to; pre[nb] = b;
        b.Transition.StateBefore = to; b.Transition.StateAfter = (D3D12_RESOURCE_STATES)from; post[nb] = b; nb++; };
    static int s_log = 0;
    for (auto& in : ins) {
        UINT st[2] = { 0, 0 }; bool k[2] = { false, false };
        for (UINT p = 0; p < in.planes; p++) k[p] = GetSubState(in.r, p, &st[p]);
        if (s_log < 3) Log("barrier: %s tracked state plane0=%s0x%X plane1=%s0x%X", in.n, k[0] ? "" : "?", st[0], k[1] ? "" : "?", st[1]);
        if (in.planes == 1) {
            if (!k[0]) continue;
            if ((st[0] & in.want) == (UINT)in.want) continue;
            add(in.r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st[0], in.want);
        } else {
            if (!k[0] && !k[1]) continue;
            if (k[0] && k[1] && st[0] == st[1]) { if ((st[0] & in.want) != (UINT)in.want) add(in.r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st[0], in.want); }
            else for (UINT p = 0; p < 2; p++) if (k[p] && (st[p] & in.want) != (UINT)in.want) add(in.r, p, st[p], in.want);
        }
    }
    if (nb) oResBarrier(cl, nb, pre);
    if (!firstUse) { D3D12_RESOURCE_BARRIER u[2] = {}; for (int i = 0; i < 2; i++) { u[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; u[i].Transition.pResource = i ? g_depthOut : g_mvOut; u[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; u[i].Transition.StateBefore = kOutState; u[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; } oResBarrier(cl, 2, u); }
    int slot = (g_mvSlot++) % MV_SETS; UINT64 base = (UINT64)slot * 4 * g_mvInc;
    D3D12_CPU_DESCRIPTOR_HANDLE c = CpuStart(g_mvHeap); c.ptr += (SIZE_T)base;
    D3D12_GPU_DESCRIPTOR_HANDLE g = GpuStart(g_mvHeap); g.ptr += base;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R16G16_UNORM; g_device->CreateShaderResourceView(vel, &sv, c);
    c.ptr += g_mvInc; sv.Format = DepthSrvFormat(ResDesc(depth).Format); g_device->CreateShaderResourceView(depth, &sv, c);
    c.ptr += g_mvInc; D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = DXGI_FORMAT_R16G16_FLOAT; g_device->CreateUnorderedAccessView(g_mvOut, nullptr, &uv, c);
    c.ptr += g_mvInc; uv.Format = DXGI_FORMAT_R32_FLOAT; g_device->CreateUnorderedAccessView(g_depthOut, nullptr, &uv, c);
    struct { float m[16]; UINT w, h; float sx, sy; } k; { std::lock_guard<std::mutex> lk(g_camMx); memcpy(k.m, g_c2p, sizeof k.m); }
    k.w = W; k.h = H; k.sx = (float)g_cfg.mvSX; k.sy = (float)g_cfg.mvSY;
    ID3D12DescriptorHeap* hh[1] = { g_mvHeap }; cl->SetDescriptorHeaps(1, hh);
    cl->SetComputeRootSignature(g_mvRS); cl->SetPipelineState(g_mvPSO);
    cl->SetComputeRoot32BitConstants(0, 20, &k, 0); cl->SetComputeRootDescriptorTable(1, g);
    cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER o[2] = {}; for (int i = 0; i < 2; i++) { o[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; o[i].Transition.pResource = i ? g_depthOut : g_mvOut; o[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; o[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; o[i].Transition.StateAfter = kOutState; }
    oResBarrier(cl, 2, o);
    if (nb) oResBarrier(cl, nb, post);
    cl->Close();
    ID3D12CommandList* lists[1] = { cl }; oExec(g_queue, 1, lists);
    if (ringSlot >= 0) { g_ringVal[ringSlot] = ++g_fenceNext; g_queue->Signal(g_ringFence, g_ringVal[ringSlot]); } else DeferRelease(cl, al);
    if (s_log++ < 3) Log("MV pass submitted: frame=%d slot=%d %ux%u transitions=%u c2p diag=%.5f %.5f %.5f %.5f", g_frame.load(), slot, W, H, nb, k.m[0], k.m[5], k.m[10], k.m[15]);
    return true;
}

// Drops candidate resources the game has already destroyed (only our own reference is left). v13 kept them forever (VRAM leak) and,
// because the use-counters only ever grew, an old dead depth/velocity buffer could stay "the best" forever after a level load / settings change -> stale MVs = ghosting.
static void PruneRes() {
    std::unique_lock<std::shared_mutex> lk(g_resMx);
    for (auto it = g_res.begin(); it != g_res.end();) {
        auto& m = it->second;
        if (m.held && m.res) {
            m.res->AddRef(); ULONG c = m.res->Release();
            if (c <= 1) {
                void* key = it->first;
                if (g_pinVel == m.res) g_pinVel = nullptr;
                if (g_pinDepth == m.res) g_pinDepth = nullptr;
                for (auto r = g_rtvToRes.begin(); r != g_rtvToRes.end();) { if (r->second == key) r = g_rtvToRes.erase(r); else ++r; }
                for (auto r = g_dsvToRes.begin(); r != g_dsvToRes.end();) { if (r->second == key) r = g_dsvToRes.erase(r); else ++r; }
                { std::lock_guard<std::mutex> sl(g_stateMx); g_state.erase(key); }
                m.res->Release();
                it = g_res.erase(it); continue;
            }
        }
        ++it;
    }
}
static void UpdatePins() {
    int fr = g_frame.load();
    if ((fr % 30) == 0) PruneRes();
    if (g_pinVel && g_pinDepth && (fr % 8) != 0) return;      // pins are stable: re-evaluate every 8 frames only
    ID3D12Resource* bestVel = nullptr, * bestDep = nullptr; int bv = -1, bd = -1, curV = -1, curD = -1;
    { std::unique_lock<std::shared_mutex> lk(g_resMx);
      for (auto& kv : g_res) { auto& m = kv.second; if (!m.cand || !IsInternal(m.w, m.h)) continue;
        int window = m.useClear.exchange(0) + m.useSetRT.exchange(0) * 2 + m.useCopySrc.exchange(0) + m.useCopyDst.exchange(0) + m.useSRV.exchange(0);
        m.score = (m.score * 3) / 4 + window;                  // recent activity counts, old activity fades (v13: counters only ever grew)
        if (m.res == g_pinDepth) curD = m.score;
        if (m.res == g_pinVel) curV = m.score;
        if ((m.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && m.score > bd) { bd = m.score; bestDep = m.res; }
        if (m.fmt == DXGI_FORMAT_R16G16_UNORM && m.score > bv) { bv = m.score; bestVel = m.res; } } }
    // hysteresis: keep the current pin unless another resource is clearly (1.5x) busier
    ID3D12Resource* nv = (g_pinVel && curV >= 0 && curV * 3 >= bv * 2) ? g_pinVel : bestVel;
    ID3D12Resource* nd = (g_pinDepth && curD >= 0 && curD * 3 >= bd * 2) ? g_pinDepth : bestDep;
    if (nv != g_pinVel || nd != g_pinDepth) { if (g_pinVel || g_pinDepth) Log("pins changed at frame %d: vel %p -> %p, depth %p -> %p (frame generation is reset)", fr, (void*)g_pinVel, (void*)nv, (void*)g_pinDepth, (void*)nd); g_forceReset = true; }
    g_pinVel = nv; g_pinDepth = nd;
    if (nd) { D3D12_RESOURCE_DESC dd = ResDesc(nd); if ((int)dd.Width != g_effW.load() || (int)dd.Height != g_effH.load()) { g_effW = (int)dd.Width; g_effH = (int)dd.Height; Log("render size from pinned depth buffer: %dx%d", g_effW.load(), g_effH.load()); } }
}

// ------------------------------------------------------------------ 4x4 helpers + constants
static bool Inv4(const float* m, float* out) {
    double a[16], inv[16]; for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabs(det) < 1e-30) return false;
    det = 1.0 / det; for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] * det); return true;
}
static void ToSl(const float* m, sl::float4x4& o) { for (int r = 0; r < 4; r++) o.row[r] = sl::float4(m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]); }
static bool BuildConstants(sl::Constants& c, bool reset) {
    float vtc[16], c2p[16], j[2]; { std::lock_guard<std::mutex> lk(g_camMx); memcpy(vtc, g_vtcNoAA, sizeof vtc); memcpy(c2p, g_c2p, sizeof c2p); j[0] = g_jitPX; j[1] = g_jitPY; }
    float inv1[16], inv2[16];
    if (!Inv4(vtc, inv1) || !Inv4(c2p, inv2)) return false;
    ToSl(vtc, c.cameraViewToClip); ToSl(inv1, c.clipToCameraView); ToSl(c2p, c.clipToPrevClip); ToSl(inv2, c.prevClipToClip);
    c.jitterOffset = sl::float2(j[0], j[1]);
    c.mvecScale = sl::float2(1.0f / (float)EffW(), 1.0f / (float)EffH());
    c.cameraPinholeOffset = sl::float2(0.f, 0.f);
    c.cameraPos = sl::float3(0.f, 0.f, 0.f); c.cameraUp = sl::float3(0.f, 1.f, 0.f); c.cameraRight = sl::float3(1.f, 0.f, 0.f); c.cameraFwd = sl::float3(0.f, 0.f, 1.f);
    float nearP = vtc[14]; if (!(nearP > 0.f) || !std::isfinite(nearP)) nearP = 10.f;
    c.cameraNear = nearP; c.cameraFar = (float)g_cfg.farPlane;
    c.cameraFOV = 2.0f * atanf(1.0f / vtc[5]); c.cameraAspectRatio = vtc[5] / vtc[0];
    c.depthInverted = sl::Boolean::eTrue; c.cameraMotionIncluded = sl::Boolean::eTrue; c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse; c.orthographicProjection = sl::Boolean::eFalse; c.motionVectorsDilated = sl::Boolean::eFalse; c.motionVectorsJittered = sl::Boolean::eFalse;
    return true;
}

// ------------------------------------------------------------------ per-frame logic (runs inside the Present hook)
struct FrameCtx { sl::FrameToken* tok = nullptr; };
static sl::FrameToken* g_nextTok = nullptr;   // reflexplace=1: token that was already used for slReflexSleep/SimulationStart at the end of the previous Present
static std::atomic<bool> g_deviceLost{false};
static int g_lastOnFrame = -1; static bool g_wasOn = false; static int g_stateLog = 0; static bool g_loggedSupport = false;
static UINT g_scW = 0, g_scH = 0;

static void LogSupport() {
    if (g_loggedSupport || !p_slIsFeatureSupported) return; g_loggedSupport = true;
    HMODULE mdx = GetModuleHandleW(L"dxgi.dll"); if (!mdx) return;
    auto pCF = (F_CreateDXGIFactory)GetProcAddress(mdx, "CreateDXGIFactory1"); if (!pCF) return;
    // note: this goes through our hook -> Streamline's proxy factory, which is fine for EnumAdapters/GetDesc
    IDXGIFactory1* f = nullptr; if (FAILED(pCF(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return;
    IDXGIAdapter* ad = nullptr;
    for (UINT i = 0; f->EnumAdapters(i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC d = {}; ad->GetDesc(&d);
        sl::AdapterInfo ai{}; ai.deviceLUID = (uint8_t*)&d.AdapterLuid; ai.deviceLUIDSizeInBytes = sizeof(LUID);
        sl::Result r = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
        char nm[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, nullptr, nullptr);
        Log("DLSS-G supported on adapter %u '%s' -> %d %s", i, nm, (int)r, SlStr(r));
        ad->Release();
    }
    f->Release();
}
// Streamline docs: slGetFeatureFunction needs the device to be set first. In the log the interposer never got it
// (UE4 creates + destroys probe devices), so we hand it the live (proxy) device from the swap chain explicitly, once.
static bool g_slDevSet = false; static int g_slDevTries = 0;
static void EnsureSlDevice(IDXGISwapChain* sc) {
    if (g_slDevSet || g_slDevTries >= 5 || !g_sl || !sc) return; g_slDevTries++;
    typedef sl::Result(*F_slSetD3DDevice)(void*);
    auto fn = (F_slSetD3DDevice)GetProcAddress(g_sl, "slSetD3DDevice");
    if (!fn) { Log("slSetD3DDevice export missing"); g_slDevTries = 99; return; }
    ID3D12Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) { Log("swapchain->GetDevice failed"); return; }
    sl::Result r = fn((void*)dev);
    Log("slSetD3DDevice(%p) -> %d %s", (void*)dev, (int)r, SlStr(r));
    dev->Release();
    if (r == sl::Result::eOk) g_slDevSet = true;
}
// Plugins (incl. DLSS-G) only start once Streamline knows the device, and DLSS-G must see CreateSwapChain to take over the swap chain.
// So hand the device to Streamline from the queue that is passed to CreateSwapChain, BEFORE the swap chain is created.
static void EnsureSlDeviceFromQueue(IUnknown* queueUnk) {
    if (g_slDevSet || !g_sl || !queueUnk) return;
    typedef sl::Result(*F_slSetD3DDevice)(void*);
    auto fn = (F_slSetD3DDevice)GetProcAddress(g_sl, "slSetD3DDevice");
    if (!fn) { Log("slSetD3DDevice export missing"); return; }
    ID3D12CommandQueue* cq = nullptr;
    if (FAILED(queueUnk->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&cq)) || !cq) { Log("EnsureSlDeviceFromQueue: not a D3D12 queue"); return; }
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(cq->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) && dev) {
        void* nat = nullptr; ID3D12Device* use = dev;
        if (p_slGetNativeInterface && p_slGetNativeInterface((void*)dev, &nat) == sl::Result::eOk && nat) use = (ID3D12Device*)nat;
        sl::Result r = fn((void*)use);
        Log("slSetD3DDevice(%p) BEFORE CreateSwapChain -> %d %s", (void*)use, (int)r, SlStr(r));
        if (r == sl::Result::eOk) g_slDevSet = true;
        dev->Release();
    }
    cq->Release();
}
static bool InitSlRuntime() {
    if (g_slRuntimeOk) return true;
    static int tries = 0; if (tries++ > 5) return false;
    auto get = [&](sl::Feature f, const char* n, void*& fn) { sl::Result r = p_slGetFeatureFunction(f, n, fn); if (r != sl::Result::eOk) Log("slGetFeatureFunction(%s) -> %d %s", n, (int)r, SlStr(r)); return r == sl::Result::eOk; };
    bool ok = true;
    ok &= get(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)p_DLSSGSetOptions);
    get(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)p_DLSSGGetState);
    ok &= get(sl::kFeaturePCL, "slPCLSetMarker", (void*&)p_PCLSetMarker);
    ok &= get(sl::kFeatureReflex, "slReflexSetOptions", (void*&)p_ReflexSetOptions);
    get(sl::kFeatureReflex, "slReflexSleep", (void*&)p_ReflexSleep);
    LogSupport();
    if (!ok) { Log("Streamline runtime init incomplete (try %d)", tries); return false; }
    sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatency; ro.useMarkersToOptimize = true;
    if (g_cfg.baseFpsLimit > 0) ro.frameLimitUs = (uint32_t)(1000000 / g_cfg.baseFpsLimit);
    sl::Result r = p_ReflexSetOptions(ro); Log("slReflexSetOptions(LowLatency) -> %d %s", (int)r, SlStr(r));
    g_slRuntimeOk = true; return true;
}
static void Marker(sl::PCLMarker m, const FrameCtx& c) {
    if (!(p_PCLSetMarker && c.tok)) return;
    sl::Result r = p_PCLSetMarker(m, *c.tok);
    static int bad = 0; if (r != sl::Result::eOk && bad++ < 10) Log("slPCLSetMarker(%d) -> %d %s", (int)m, (int)r, SlStr(r));
}

static LARGE_INTEGER g_qf = {};
static inline double NowMs() { if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf); LARGE_INTEGER n; QueryPerformanceCounter(&n); return (double)n.QuadPart * 1000.0 / (double)g_qf.QuadPart; }
struct PreInitArg { UINT w, h; };
static DWORD WINAPI MvPreInitThread(LPVOID p) { PreInitArg a = *(PreInitArg*)p; delete (PreInitArg*)p; t_prewarm = true; try { InitMvPass(a.w, a.h); } catch (...) {} t_prewarm = false; g_mvPreIniting = false; return 0; }
static double g_obStart = 0, g_obMs = 0, g_tSleep = 0, g_tPins = 0, g_tMv = 0, g_tPrune = 0;   // time spent in OnPresentBegin (= our own per-frame overhead) of the last frame
static FrameCtx OnPresentBegin(IDXGISwapChain* sc) {
    g_obStart = NowMs(); g_obMs = 0; g_tSleep = g_tPins = g_tMv = g_tPrune = 0;
    { static long s_lastCbv = 0; long c = g_cbvCalls.load(); g_cbvPerFrame = c - s_lastCbv; s_lastCbv = c; }
    FrameCtx ctx;
    int cur = g_frame.load(); bool hudOk = g_hudStamp.load() == cur && cur > 0;
    int fr = ++g_frame; int bbd = g_bbDraws.exchange(0);
    // frame time (base fps) + maintenance
    static LARGE_INTEGER s_qf = {}, s_last = {}; static double s_ema = 16.7;
    if (!s_qf.QuadPart) QueryPerformanceFrequency(&s_qf);
    { LARGE_INTEGER n; QueryPerformanceCounter(&n); double dtMs = s_last.QuadPart ? (double)(n.QuadPart - s_last.QuadPart) * 1000.0 / (double)s_qf.QuadPart : 0.0; s_last = n;
      if (dtMs > 0.0 && dtMs < 1000.0) s_ema += (dtMs - s_ema) * 0.05;
      if (dtMs > 250.0) { g_forceReset = true; static int nh = 0; if (nh++ < 20) Log("hitch %.0f ms at frame %d -> frame generation reset", dtMs, fr); } }   // after a long stall the history is useless
    FlushDeferredRes();
    if ((fr % 120) == 0) { double tp0 = NowMs(); { std::unique_lock<std::shared_mutex> lkb(g_bufMx); PruneBuffersLocked(); } g_tPrune = NowMs() - tp0; }
    if (fr == 1 || (fr % 600) == 0) {
        DXGI_SWAP_CHAIN_DESC d = {}; if (SUCCEEDED(sc->GetDesc(&d))) { g_dispW = d.BufferDesc.Width; g_dispH = d.BufferDesc.Height; g_scW = g_dispW; g_scH = g_dispH; }
        size_t nb_; { std::shared_lock<std::shared_mutex> lkb(g_bufMx); nb_ = g_bufs.size(); }
        Log("Present: frame=%d swapchain %ux%u fmt=%d buffers=%u | pins vel=%p depth=%p | camFrame=%d | bbdraws(last)=%d", fr, g_dispW, g_dispH, (int)d.BufferDesc.Format, d.BufferCount, (void*)g_pinVel, (void*)g_pinDepth, g_camFrame.load(), bbd);
        static long s_pn = 0, s_pns = 0; long pn = g_probeN.load(), pns = g_probeNs.load();
        Log("cam-diag: probes +%ld (%.2f ms total, %.1f us each) cbv/frame=%ld", pn - s_pn, (pns - s_pns) / 1e6, pn > s_pn ? (pns - s_pns) / 1e3 / (double)(pn - s_pn) : 0.0, g_cbvPerFrame.load()); s_pn = pn; s_pns = pns;
        Log("cam-diag: upload bufs registered=%zu rootCBV calls=%ld (no-buffer misses=%ld) projection-like matrices seen=%ld scans=%ld locked=%d offsets proj=%d noaa=%d mv=%d render=%dx%d", nb_, g_cbvCalls.load(), g_cbvMiss.load(), g_projSeen.load(), g_scanRuns.load(), g_camLocked.load(), g_cfg.projOff, g_cfg.noAAOff, g_cfg.mvOff, EffW(), EffH());
    }
    { double tp0 = NowMs(); UpdatePins(); g_tPins = NowMs() - tp0; }
    // Build the MV pipeline + buffers as soon as the pins are known (long before warm-up ends), on a worker thread: the first frame with frame generation
    // then does not have to create a PSO, 6 textures, a descriptor heap and the command-list ring on the render thread.
    if (g_cfg.stage >= 2 && g_cfg.fg && g_pinVel && g_pinDepth && g_device && !g_mvPreIniting.load()) {
        D3D12_RESOURCE_DESC vd = ResDesc(g_pinVel), dd = ResDesc(g_pinDepth);
        if (vd.Width == dd.Width && vd.Height == dd.Height && !(g_mvPSO && g_mvOutA[0] && g_mvW == (UINT)vd.Width && g_mvH == vd.Height) && !g_mvFailed) {
            g_mvPreIniting = true; auto* a = new PreInitArg{ (UINT)vd.Width, vd.Height };
            HANDLE th = CreateThread(nullptr, 0, MvPreInitThread, a, 0, nullptr); if (th) CloseHandle(th); else { delete a; g_mvPreIniting = false; }
        }
    }
    if (!g_slInitOk || g_cfg.stage < 1) return ctx;
    if (!g_slRuntimeOk) { EnsureSlDevice(sc); if (!(g_device && g_slDevSet && InitSlRuntime())) return ctx; }
    if (g_cfg.reflexPlace && g_nextTok) {
        ctx.tok = g_nextTok; g_nextTok = nullptr;   // sleep + SimulationStart were issued at the end of the previous Present = the real start of this game frame
        Marker(sl::PCLMarker::eSimulationEnd, ctx); Marker(sl::PCLMarker::eRenderSubmitStart, ctx); Marker(sl::PCLMarker::eRenderSubmitEnd, ctx);
    } else {
        if (p_slGetNewFrameToken(ctx.tok, nullptr) != sl::Result::eOk || !ctx.tok) { ctx.tok = nullptr; return ctx; }
        if (g_cfg.reflexSleep && p_ReflexSleep) { double ts0 = NowMs(); p_ReflexSleep(*ctx.tok); g_tSleep = NowMs() - ts0; }
        Marker(sl::PCLMarker::eSimulationStart, ctx); Marker(sl::PCLMarker::eSimulationEnd, ctx);
        Marker(sl::PCLMarker::eRenderSubmitStart, ctx); Marker(sl::PCLMarker::eRenderSubmitEnd, ctx);
    }
    sl::ViewportHandle vp(0u);
    // DLSS-G requires IDXGISwapChain3::GetCurrentBackBufferIndex to be called through the SL swap chain EVERY frame
    // (UE4 tracks the back buffer index itself and never calls it -> status 0x10 / eFailGetCurrentBackBufferIndexNotCalled).
    { IDXGISwapChain3* s3 = nullptr;
      if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&s3)) && s3) { (void)s3->GetCurrentBackBufferIndex(); s3->Release(); }
      else { static int w = 0; if (w++ < 3) Log("swap chain has no IDXGISwapChain3 - cannot call GetCurrentBackBufferIndex"); } }

    // busy-scene protection: frame generation on top of a very low base fps looks bad (ghosting) and costs GPU time itself -> pause it until the base fps recovers
    static bool s_susp = false; static int s_lowN = 0, s_highN = 0;
    if (g_cfg.fgMinFps > 0 && fr > g_cfg.warmup) {
        double fps = 1000.0 / std::max(s_ema, 1.0);
        if (!s_susp) { if (fps < (double)g_cfg.fgMinFps) { if (++s_lowN >= 45) { s_susp = true; s_highN = 0; Log("FG paused: base fps %.1f < %d (frame %d)", fps, g_cfg.fgMinFps, fr); } } else s_lowN = 0; }
        else { if (fps >= (double)(g_cfg.fgMinFps + 8)) { if (++s_highN >= 120) { s_susp = false; s_lowN = 0; g_forceReset = true; Log("FG resumed: base fps %.1f (frame %d)", fps, fr); } } else s_highN = 0; }
    }
    bool wantOn = g_cfg.fg && g_cfg.stage >= 2 && fr >= g_cfg.warmup && !g_deviceLost.load() && !s_susp;
    int camAge = fr - g_camFrame.load(); bool camOk = g_camFrame.load() >= 0 && camAge <= g_cfg.camStale;
    static int s_camStreak = 0; if (camOk) s_camStreak++; else s_camStreak = 0;
    bool delayOk = g_wasOn || s_camStreak >= g_cfg.fgDelay;       // already running: keep going; (re)starting: wait until the view has been stable for fgdelay frames
    bool inputs = false; sl::Constants consts{};
    if (wantOn && camOk && delayOk && g_pinVel && g_pinDepth && g_queue && !g_mvPreIniting.load()) {
        D3D12_RESOURCE_DESC vd = ResDesc(g_pinVel), dd = ResDesc(g_pinDepth);
        if (vd.Width == dd.Width && vd.Height == dd.Height) {
            double tm0 = NowMs(); bool mvok = RunMvPass((UINT)vd.Width, vd.Height, g_pinVel, g_pinDepth); g_tMv = NowMs() - tm0;
            if (mvok) {
                bool reset = g_forceReset.exchange(false) || !g_wasOn || (fr - g_lastOnFrame) > 1;
                if (BuildConstants(consts, reset)) inputs = true;
            }
        }
    }
    static int s_why = 0;
    if (wantOn && !inputs && (s_why++ % 300) == 0) Log("FG not ready: camOk=%d(age %d) vel=%p depth=%p queue=%p mv=%p", (int)camOk, camAge, (void*)g_pinVel, (void*)g_pinDepth, (void*)g_queue, (void*)g_mvOut);

    { static int prev = -1; int now = inputs ? 1 : 0;
      if (now != prev) { Log("FG %s at frame=%d (wantOn=%d camOk=%d age=%d vel=%d depth=%d queue=%d)", now ? "ON" : "OFF", fr, (int)wantOn, (int)camOk, camAge, g_pinVel != nullptr, g_pinDepth != nullptr, g_queue != nullptr); prev = now; } }
    sl::DLSSGOptions opt{};
    if (inputs) {
        sl::Extent ext{ 0, 0, (uint32_t)g_cfg.renderW, (uint32_t)g_cfg.renderH };
        D3D12_RESOURCE_DESC vd = ResDesc(g_pinVel); ext.width = (uint32_t)vd.Width; ext.height = vd.Height;
        sl::Resource rDepth(sl::ResourceType::eTex2d, g_depthOut, (uint32_t)kOutState);
        sl::Resource rMv(sl::ResourceType::eTex2d, g_mvOut, (uint32_t)kOutState);
        sl::Resource rHud(sl::ResourceType::eTex2d, g_hudless, (uint32_t)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        sl::ResourceTag tags[3] = {
            sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext),
            sl::ResourceTag(&rMv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext),
            sl::ResourceTag(&rHud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, nullptr) };
        uint32_t nt = 2; if (g_cfg.hudless && hudOk && g_hudless) nt = 3;
        sl::Result r1 = g_useFrameTags ? p_slSetTagForFrame(*ctx.tok, vp, tags, nt, nullptr) : p_slSetTag(vp, tags, nt, nullptr);
        sl::Result r2 = p_slSetConstants(consts, *ctx.tok, vp);
        static int s_r = 0; if (s_r < 4 || r1 != sl::Result::eOk || r2 != sl::Result::eOk) { if (s_r < 40) Log("frame=%d tags(%u)=%d %s constants=%d %s hud=%d", fr, nt, (int)r1, SlStr(r1), (int)r2, SlStr(r2), (int)(nt == 3)); s_r++; }
        opt.mode = sl::DLSSGMode::eOn; opt.numFramesToGenerate = (uint32_t)(g_cfg.mult - 1);
        opt.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        { DXGI_SWAP_CHAIN_DESC sd = {}; if (SUCCEEDED(sc->GetDesc(&sd))) { opt.numBackBuffers = sd.BufferCount; opt.colorWidth = sd.BufferDesc.Width; opt.colorHeight = sd.BufferDesc.Height; opt.colorBufferFormat = (uint32_t)sd.BufferDesc.Format; } }
        opt.mvecDepthWidth = ext.width; opt.mvecDepthHeight = ext.height; opt.mvecBufferFormat = (uint32_t)DXGI_FORMAT_R16G16_FLOAT; opt.depthBufferFormat = (uint32_t)DXGI_FORMAT_R32_FLOAT;
        g_wasOn = true; g_lastOnFrame = fr;
    } else { opt.mode = sl::DLSSGMode::eOff; opt.flags = sl::DLSSGFlags::eRetainResourcesWhenOff; g_wasOn = false; }
    // clamp the multiplier to what the GPU supports
    static uint32_t s_maxGen = 0;
    if (s_maxGen && opt.numFramesToGenerate > s_maxGen) opt.numFramesToGenerate = s_maxGen;
    sl::Result ro = p_DLSSGSetOptions(vp, opt);
    static int s_o = 0; if (ro != sl::Result::eOk && s_o++ < 20) Log("slDLSSGSetOptions(mode=%d gen=%u) -> %d %s", (int)opt.mode, opt.numFramesToGenerate, (int)ro, SlStr(ro));
    if (p_DLSSGGetState && (g_stateLog < 6 || (fr % 30) == 0) && fr > g_cfg.warmup) {
        sl::DLSSGState st{}; sl::Result rs = p_DLSSGGetState(vp, st, nullptr);
        static unsigned s_lastStatus = 0xFFFFFFFFu; bool chg = rs == sl::Result::eOk && (unsigned)st.status != s_lastStatus; if (rs == sl::Result::eOk) s_lastStatus = (unsigned)st.status;
        if (rs == sl::Result::eOk) s_maxGen = st.numFramesToGenerateMax;
        if (rs == sl::Result::eOk && !(g_stateLog < 6 || chg || (fr % 600) == 0)) { /* nothing new */ }
        else if (rs == sl::Result::eOk) { s_maxGen = st.numFramesToGenerateMax; Log("DLSS-G state: frame=%d mode=%d status=0x%X (0=ok) maxGen=%u presented=%u minWH=%u vram=%lluMB", fr, (int)opt.mode, (unsigned)st.status, st.numFramesToGenerateMax, st.numFramesActuallyPresented, st.minWidthOrHeight, (unsigned long long)(st.estimatedVRAMUsageInBytes >> 20)); }
        else Log("slDLSSGGetState -> %d %s", (int)rs, SlStr(rs));
        g_stateLog++;
    }
    // Best-effort proof that frames are really being generated: compare the game's Present() rate with the swap chain's own
    // present counter (generated frames are presented by Streamline on the same swap chain). ratio ~2.0 => frame generation works.
    {
        static LARGE_INTEGER s_f = {}, s_t0 = {}; static int s_fr0 = 0; static UINT s_pc0 = 0; static bool s_have = false;
        if (!s_f.QuadPart) QueryPerformanceFrequency(&s_f);
        if (fr > g_cfg.warmup && (fr % 300) == 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            DXGI_FRAME_STATISTICS fs = {}; HRESULT hs = sc->GetFrameStatistics(&fs);
            if (s_have && SUCCEEDED(hs)) {
                double dt = (double)(now.QuadPart - s_t0.QuadPart) / (double)s_f.QuadPart; int dFr = fr - s_fr0; UINT dPc = fs.PresentCount - s_pc0;
                Log("RATE: game present %.1f/s | swapchain PresentCount %.1f/s | ratio %.2f (about 2.0 = frame generation active, 1.0 = not generating)", dFr / dt, dPc / dt, dFr ? (double)dPc / dFr : 0.0);
            } else if (FAILED(hs)) Log("RATE: GetFrameStatistics failed 0x%08X", (unsigned)hs);
            s_t0 = now; s_fr0 = fr; s_pc0 = SUCCEEDED(hs) ? fs.PresentCount : 0; s_have = SUCCEEDED(hs);
        }
    }
    Marker(sl::PCLMarker::ePresentStart, ctx);
    g_obMs = NowMs() - g_obStart;
    return ctx;
}
static void OnPresentEnd(const FrameCtx& ctx) {
    Marker(sl::PCLMarker::ePresentEnd, ctx);
    if (g_cfg.reflexPlace && ctx.tok && p_slGetNewFrameToken) {   // the next game frame starts here: sleep BEFORE simulating, not before presenting
        sl::FrameToken* t = nullptr;
        if (p_slGetNewFrameToken(t, nullptr) == sl::Result::eOk && t) {
            if (g_cfg.reflexSleep && p_ReflexSleep) p_ReflexSleep(*t);
            FrameCtx nc; nc.tok = t; Marker(sl::PCLMarker::eSimulationStart, nc);
            g_nextTok = t;
        }
    }
}

static void CheckDeviceRemoved() {
    if (g_deviceLost.load() || !g_device) return;
    HRESULT hr = g_device->GetDeviceRemovedReason();
    if (FAILED(hr)) { g_deviceLost = true; Log("DEVICE REMOVED reason=0x%08X (frame=%d)", (unsigned)hr, g_frame.load()); }
}

static size_t bufCount() { std::shared_lock<std::shared_mutex> lk(g_bufMx); return g_bufs.size(); }
static VHook g_hPresent, g_hPresent1, g_hCSC, g_hCSCH;
static thread_local int t_depth = 0;
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT s, UINT f) {
    auto o = (HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT))OrigOf(g_hPresent, sc);
    if (!o) return E_FAIL;
    bool outer = (t_depth++ == 0); FrameCtx ctx;
    if (outer) { CheckDeviceRemoved(); try { ctx = OnPresentBegin(sc); } catch (...) { Log("exception in OnPresentBegin"); } }
    double tp = NowMs();
    HRESULT r = o(sc, s, f);
    if (outer) { double dp = NowMs() - tp; if ((dp > 120.0 || (g_obMs - g_tSleep) > 8.0) && g_frame.load() > 60) { static int ns = 0; if (ns++ < 60) Log("slow frame %d: our work before Present %.1f ms [ReflexSleep %.1f (intentional wait) | UpdatePins %.1f | MV pass %.1f | buffer prune %.1f] | inside Present (DLSS-G/driver/GPU wait) %.1f ms | upload bufs held=%zu (%llu MB, pruned %ld) cbv/frame=%ld", g_frame.load(), g_obMs, g_tSleep, g_tPins, g_tMv, g_tPrune, dp, bufCount(), (unsigned long long)(g_bufBytes >> 20), g_prunedTotal, g_cbvPerFrame.load()); } }
    if (outer) { try { OnPresentEnd(ctx); } catch (...) {} }
    t_depth--; return r;
}
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p) {
    auto o = (HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*))OrigOf(g_hPresent1, sc);
    if (!o) return E_FAIL;
    bool outer = (t_depth++ == 0); FrameCtx ctx;
    if (outer) { CheckDeviceRemoved(); try { ctx = OnPresentBegin(sc); } catch (...) { Log("exception in OnPresentBegin"); } }
    HRESULT r = o(sc, s, f, p);
    if (outer) { try { OnPresentEnd(ctx); } catch (...) {} }
    t_depth--; return r;
}

// ------------------------------------------------------------------ swap chain / factory / device hooks
static ID3D12CommandQueue* NativeQueueOf(IUnknown* q) {
    if (!q) return nullptr;
    ID3D12CommandQueue* cq = nullptr; if (FAILED(q->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&cq)) || !cq) return nullptr;
    void* nat = nullptr;
    if (p_slGetNativeInterface && p_slGetNativeInterface((void*)cq, &nat) == sl::Result::eOk && nat) { cq->Release(); return (ID3D12CommandQueue*)nat; }
    return cq;   // already native (reference intentionally kept)
}
static void OnSwapChainCreated(IUnknown* queueUnk, IDXGISwapChain* sc) {
    if (!sc) return;
    ID3D12CommandQueue* nq = NativeQueueOf(queueUnk);
    if (nq) { D3D12_COMMAND_QUEUE_DESC qd = QDesc(nq); Log("swap chain created: queue proxy/native=%p native=%p type=%d", (void*)queueUnk, (void*)nq, (int)qd.Type); if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_queue = nq; }
    else Log("swap chain created with a non-D3D12 queue (D3D11?) - frame generation will not work");
    DXGI_SWAP_CHAIN_DESC d = {}; if (SUCCEEDED(sc->GetDesc(&d))) { g_dispW = d.BufferDesc.Width; g_dispH = d.BufferDesc.Height; Log("swap chain %ux%u fmt=%d buffers=%u effect=%d", g_dispW, g_dispH, (int)d.BufferDesc.Format, d.BufferCount, (int)d.SwapEffect); }
    void** vt = *(void***)sc; bool a = HookVt(g_hPresent, vt, 8, (void*)hkPresent);
    IDXGISwapChain1* s1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&s1)) && s1) { HookVt(g_hPresent1, *(void***)s1, 22, (void*)hkPresent1); s1->Release(); }
    Log("Present hooked (new vtable=%d)", (int)a);
}
static HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    auto o = (HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**))OrigOf(g_hCSC, f);
    if (!o) return E_FAIL;
    EnsureSlDeviceFromQueue(dev);
    HRESULT hr = o(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainCreated(dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hw, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* out, IDXGISwapChain1** sc) {
    auto o = (HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**))OrigOf(g_hCSCH, f);
    if (!o) return E_FAIL;
    EnsureSlDeviceFromQueue(dev);
    HRESULT hr = o(f, dev, hw, d, fd, out, sc);
    if (SUCCEEDED(hr) && sc && *sc) OnSwapChainCreated(dev, (IDXGISwapChain*)*sc);
    return hr;
}
static void PatchFactory(IUnknown* fac) {
    if (!fac) return;
    IDXGIFactory* f1 = nullptr;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory), (void**)&f1)) && f1) { if (HookVt(g_hCSC, *(void***)f1, 10, (void*)hkCreateSwapChain)) Log("factory CreateSwapChain hooked"); f1->Release(); }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) { if (HookVt(g_hCSCH, *(void***)f2, 15, (void*)hkCreateSwapChainForHwnd)) Log("factory CreateSwapChainForHwnd hooked"); f2->Release(); }
}

static thread_local bool t_inSL = false;
// Streamline must be initialised BEFORE the game creates its D3D12 device. Doing slInit on a background thread raced with the game
// (the device was created first -> no SL proxy device/queue -> no frame generation). Now the hooks are installed immediately and
// Streamline is initialised lazily, synchronously, inside the first hooked D3D12/DXGI call (outside the loader lock).
static std::once_flag g_slOnce;
static bool EnsureSL() {
    if (t_inSL) return g_slInitOk;   // re-entrant call coming from Streamline itself
    std::call_once(g_slOnce, [] {
        t_inSL = true;
        bool ok = false; try { ok = InitStreamline(); } catch (...) { Log("exception in InitStreamline"); }
        t_inSL = false;
        if (!ok) Log("Streamline init failed -> game runs unmodified (hooks pass through)");
    });
    return g_slInitOk;
}
// true = call the original function (our own warm-up thread, or Streamline calling back into us). Otherwise the game waits here until the shader cache is ready.
static bool PassThrough() { if (t_prewarm) return true; WaitPrewarm(); return t_inSL || !EnsureSL(); }
// ---- v17: PSO creation diagnostics + one retry (game crash: UE4 D3D12RHI "CreatePipelineState failed with E_INVALIDARG")
// A PSO that succeeds is never touched. Only when the GAME's call returns E_INVALIDARG: (1) log the desc + device state,
// (2) if the desc carries a CachedPSO blob, retry once without it (a stale/corrupt blob is a known cause of E_INVALIDARG).
using PFN_CreateGPSO = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateCPSO = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
static PFN_CreateGPSO oCreateGPSO; static PFN_CreateCPSO oCreateCPSO;
static std::atomic<int> g_psoFail{0}, g_psoRescued{0};
static void LogGPsoFail(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, int n) {
    HRESULT rem = dev ? dev->GetDeviceRemovedReason() : E_POINTER;
    Log("PSO FAIL #%d (graphics, E_INVALIDARG) tid=%lu | deviceRemoved=0x%08X | rootSig=%p VS=%zu PS=%zu GS=%zu HS=%zu DS=%zu cached=%zu | inputElems=%u rtCount=%u rtv=[%d %d %d %d %d %d %d %d] dsv=%d | samples=%u/%u mask=0x%X topo=%d cut=%d flags=0x%X | blend: alpha2cov=%d indep=%d rt0(en=%d logic=%d wm=0x%X) | raster: fill=%d cull=%d depthClip=%d msaa=%d forced=%u | ds: z=%d s=%d func=%d",
        n, (unsigned long)GetCurrentThreadId(), (unsigned)rem, (void*)d->pRootSignature, d->VS.BytecodeLength, d->PS.BytecodeLength, d->GS.BytecodeLength, d->HS.BytecodeLength, d->DS.BytecodeLength, d->CachedPSO.CachedBlobSizeInBytes,
        d->InputLayout.NumElements, d->NumRenderTargets, (int)d->RTVFormats[0], (int)d->RTVFormats[1], (int)d->RTVFormats[2], (int)d->RTVFormats[3], (int)d->RTVFormats[4], (int)d->RTVFormats[5], (int)d->RTVFormats[6], (int)d->RTVFormats[7], (int)d->DSVFormat,
        d->SampleDesc.Count, d->SampleDesc.Quality, (unsigned)d->SampleMask, (int)d->PrimitiveTopologyType, (int)d->IBStripCutValue, (unsigned)d->Flags,
        (int)d->BlendState.AlphaToCoverageEnable, (int)d->BlendState.IndependentBlendEnable, (int)d->BlendState.RenderTarget[0].BlendEnable, (int)d->BlendState.RenderTarget[0].LogicOpEnable, (unsigned)d->BlendState.RenderTarget[0].RenderTargetWriteMask,
        (int)d->RasterizerState.FillMode, (int)d->RasterizerState.CullMode, (int)d->RasterizerState.DepthClipEnable, (int)d->RasterizerState.MultisampleEnable, (unsigned)d->RasterizerState.ForcedSampleCount,
        (int)d->DepthStencilState.DepthEnable, (int)d->DepthStencilState.StencilEnable, (int)d->DepthStencilState.DepthFunc);
    if (g_log) fflush(g_log);                              // the game is about to die: do not lose these lines
}
static HRESULT STDMETHODCALLTYPE hkCreateGPSO(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    HRESULT hr = oCreateGPSO(dev, desc, riid, ppv);
    if (hr != E_INVALIDARG || !desc || !g_cfg.psoRetry) return hr;
    int n = ++g_psoFail;
    if (n <= 16) LogGPsoFail(dev, desc, n);
    if (desc->CachedPSO.CachedBlobSizeInBytes) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC cp = *desc; cp.CachedPSO.pCachedBlob = nullptr; cp.CachedPSO.CachedBlobSizeInBytes = 0;
        HRESULT hr2 = oCreateGPSO(dev, &cp, riid, ppv);
        if (n <= 16) Log("PSO FAIL #%d: retry without CachedPSO -> hr=0x%08X", n, (unsigned)hr2);
        if (SUCCEEDED(hr2)) { g_psoRescued++; Log("PSO rescued (total rescued=%d of %d failures)", g_psoRescued.load(), n); if (g_log) fflush(g_log); return hr2; }
    }
    if (g_log) fflush(g_log);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkCreateCPSO(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    HRESULT hr = oCreateCPSO(dev, desc, riid, ppv);
    if (hr != E_INVALIDARG || !desc || !g_cfg.psoRetry) return hr;
    int n = ++g_psoFail;
    if (n <= 16) { Log("PSO FAIL #%d (compute, E_INVALIDARG) tid=%lu | deviceRemoved=0x%08X | rootSig=%p CS=%zu cached=%zu", n, (unsigned long)GetCurrentThreadId(), (unsigned)dev->GetDeviceRemovedReason(), (void*)desc->pRootSignature, desc->CS.BytecodeLength, desc->CachedPSO.CachedBlobSizeInBytes); }
    if (desc->CachedPSO.CachedBlobSizeInBytes) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cp = *desc; cp.CachedPSO.pCachedBlob = nullptr; cp.CachedPSO.CachedBlobSizeInBytes = 0;
        HRESULT hr2 = oCreateCPSO(dev, &cp, riid, ppv);
        if (n <= 16) Log("PSO FAIL #%d: retry without CachedPSO -> hr=0x%08X", n, (unsigned)hr2);
        if (SUCCEEDED(hr2)) { g_psoRescued++; return hr2; }
    }
    if (g_log) fflush(g_log);
    return hr;
}

static bool g_devHooked = false; static std::mutex g_devHookMx;

static void InstallNativeHooks(ID3D12Device* nat) {
    std::lock_guard<std::mutex> lk(g_devHookMx);
    if (g_devHooked) return;
    void** vtDev = *(void***)nat;
    g_rtvInc = nat->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); g_dsvInc = nat->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    // dummy queue + command list on the native device to read their (shared) vtables
    ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(nat->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q)) || FAILED(nat->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&al)) ||
        FAILED(nat->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&cl))) { Log("InstallNativeHooks: dummy objects failed"); return; }
    void** vtQ = *(void***)q; void** vtCL = *(void***)cl; cl->Close();
    oCreateCBVStub:;
    if (g_cfg.psoRetry) { oCreateGPSO = (PFN_CreateGPSO)Patch(vtDev, 10, (void*)hkCreateGPSO); oCreateCPSO = (PFN_CreateCPSO)Patch(vtDev, 11, (void*)hkCreateCPSO); }
    oCreateSRV = (PFN_CreateSRV)Patch(vtDev, 18, (void*)hkCreateSRV);
    oCreateRTV = (PFN_CreateRTV)Patch(vtDev, 20, (void*)hkCreateRTV);
    oCreateDSV = (PFN_CreateDSV)Patch(vtDev, 21, (void*)hkCreateDSV);
    oCopyDescSimple = (PFN_CopyDescSimple)Patch(vtDev, 24, (void*)hkCopyDescSimple);
    oCreateCommitted = (PFN_CreateCommitted)Patch(vtDev, 27, (void*)hkCreateCommitted);
    oCreatePlaced = (PFN_CreatePlaced)Patch(vtDev, 29, (void*)hkCreatePlaced);
    oExec = (PFN_Exec)Patch(vtQ, 10, (void*)hkExec);
    oCopyTex = (PFN_CopyTex)Patch(vtCL, 16, (void*)hkCopyTex);
    oResBarrier = (PFN_ResBarrier)Patch(vtCL, 26, (void*)hkResBarrier);
    oOMSetRT = (PFN_OMSetRT)Patch(vtCL, 46, (void*)hkOMSetRT);
    oClearDSV = (PFN_ClearDSV)Patch(vtCL, 47, (void*)hkClearDSV);
    oClearRTV = (PFN_ClearRTV)Patch(vtCL, 48, (void*)hkClearRTV);
    oSetCCbv = (PFN_SetRootCbv)Patch(vtCL, 37, (void*)hkSetCCbv);
    oSetGCbv = (PFN_SetRootCbv)Patch(vtCL, 38, (void*)hkSetGCbv);
    if (g_cfg.hudless) { oDrawInst = (PFN_DrawInst)Patch(vtCL, 12, (void*)hkDrawInst); oDrawIdxInst = (PFN_DrawIdxInst)Patch(vtCL, 13, (void*)hkDrawIdxInst); }
    cl->Release(); al->Release(); q->Release();
    g_devHooked = true; Log("native D3D12 hooks installed (device vtable=%p queue vtable=%p cl vtable=%p)", (void*)vtDev, (void*)vtQ, (void*)vtCL);
}
static void OnDeviceCreated(IUnknown* dev) {
    if (!dev) return;
    ID3D12Device* d = nullptr; if (FAILED(dev->QueryInterface(__uuidof(ID3D12Device), (void**)&d)) || !d) return;
    void* nat = nullptr; ID3D12Device* use = d;
    if (p_slGetNativeInterface && p_slGetNativeInterface((void*)d, &nat) == sl::Result::eOk && nat) use = (ID3D12Device*)nat;
    Log("D3D12 device created: returned=%p native=%p", (void*)d, (void*)use);
    InstallNativeHooks(use);
    d->Release();
}
static HRESULT WINAPI hkD3D12CreateDevice(IUnknown* ad, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppDev) {
    if (PassThrough()) return oD3D12CreateDevice(ad, fl, riid, ppDev);
    t_inSL = true; HRESULT hr = pSL_D3D12CreateDevice(ad, fl, riid, ppDev); t_inSL = false;
    Log("D3D12CreateDevice -> 0x%08X (ppDev=%p)", (unsigned)hr, (void*)ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev) OnDeviceCreated((IUnknown*)*ppDev);
    return hr;
}
static HRESULT WINAPI hkCreateDXGIFactory(REFIID riid, void** pp) {
    if (PassThrough()) return oCreateDXGIFactory(riid, pp);
    t_inSL = true; HRESULT hr = pSL_CreateDXGIFactory(riid, pp); t_inSL = false;
    if (SUCCEEDED(hr) && pp && *pp) PatchFactory((IUnknown*)*pp); return hr;
}
static HRESULT WINAPI hkCreateDXGIFactory1(REFIID riid, void** pp) {
    if (PassThrough()) return oCreateDXGIFactory1(riid, pp);
    t_inSL = true; HRESULT hr = pSL_CreateDXGIFactory1(riid, pp); t_inSL = false;
    if (SUCCEEDED(hr) && pp && *pp) PatchFactory((IUnknown*)*pp); return hr;
}
static HRESULT WINAPI hkCreateDXGIFactory2(UINT fl, REFIID riid, void** pp) {
    if (PassThrough() || !pSL_CreateDXGIFactory2) return oCreateDXGIFactory2(fl, riid, pp);
    t_inSL = true; HRESULT hr = pSL_CreateDXGIFactory2(fl, riid, pp); t_inSL = false;
    if (SUCCEEDED(hr) && pp && *pp) PatchFactory((IUnknown*)*pp); return hr;
}

static DWORD WINAPI InitThread(LPVOID) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = exe; g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\'));
    LoadCfg();
    if (g_cfg.log) AddVectoredExceptionHandler(1, DiagVEH);
    if (g_cfg.stage < 1 || !g_cfg.fg) { Log("stage=%d fg=%d -> pass-through, nothing hooked", g_cfg.stage, g_cfg.fg); return 0; }
    // make sure the system D3D12/DXGI are loaded before we hook their exports (the game loads the same modules later)
    HMODULE m12 = LoadLibraryW(L"d3d12.dll"), mdx = LoadLibraryW(L"dxgi.dll");
    if (!m12 || !mdx) { Log("d3d12/dxgi load failed"); return 0; }
    // Streamline itself is initialised lazily from the first hooked call (see EnsureSL)
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }
    void* tDev = (void*)GetProcAddress(m12, "D3D12CreateDevice"); void* tF = (void*)GetProcAddress(mdx, "CreateDXGIFactory");
    void* tF1 = (void*)GetProcAddress(mdx, "CreateDXGIFactory1"); void* tF2 = (void*)GetProcAddress(mdx, "CreateDXGIFactory2");
    int ok = 0;
    if (tDev) ok += MH_CreateHook(tDev, (void*)hkD3D12CreateDevice, (void**)&oD3D12CreateDevice) == MH_OK;
    if (tF) ok += MH_CreateHook(tF, (void*)hkCreateDXGIFactory, (void**)&oCreateDXGIFactory) == MH_OK;
    if (tF1) ok += MH_CreateHook(tF1, (void*)hkCreateDXGIFactory1, (void**)&oCreateDXGIFactory1) == MH_OK;
    if (tF2) ok += MH_CreateHook(tF2, (void*)hkCreateDXGIFactory2, (void**)&oCreateDXGIFactory2) == MH_OK;
    PrewarmInit();   // game's D3D12/DXGI creation calls will wait for the shader warm-up from the moment the hooks are live
    MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
    Log("MinHook: %d hooks created, enable -> %d (D3D12CreateDevice=%p CreateDXGIFactory=%p/1=%p/2=%p)", ok, (int)es, tDev, tF, tF1, tF2);
    PrewarmStart();
    // if the game already created its device before we got here, we cannot recover (log it so we know)
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); LoadRealWinmm(); CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr); }
    return TRUE;
}
