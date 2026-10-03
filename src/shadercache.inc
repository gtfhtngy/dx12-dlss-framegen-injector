// shadercache.inc - SN_DLSSG shader cache + start-up progress window. Included from main.cpp (after kMvHlsl).
//
// What it does
//   * The only shader this DLL owns is the motion-vector compute shader (kMvHlsl). Compiling it at run time (loading d3dcompiler_47.dll,
//     HLSL -> DXBC, then the GPU driver compiling DXBC -> GPU code) used to happen in the middle of the game on the render thread = hitch.
//   * Now everything is built BEFORE the game creates its D3D12 device, behind a small progress window, and stored in
//         SN_DLSSG_shaders.snsc      (next to the game exe; falls back to %LOCALAPPDATA%\SN_DLSSG\ if the game folder is read-only)
//   * On the next launches the cache is validated (checksums + shader-source hash + GPU/driver key) and the game just loads it:
//     no HLSL compile, no window, no delay.  If the GPU driver is updated only the GPU part is rebuilt.
//
// SNSC v1 file layout (little endian, our own container - NOT a DXBC/PSO dump you can feed to other tools):
//   SnscHeader | SnscEntry[entryCount] | payloads... | u32 CRC32 of everything before it
//   entry type 1 = DXBC bytecode of the MV compute shader, type 2 = serialized root signature,
//   type 3 = D3D12 pipeline library (driver-native, valid only for the adapter/driver recorded in the entry).
#pragma once

// mingw-w64's d3d12.h declares ID3D12Device1::CreatePipelineLibrary but not the ID3D12PipelineLibrary interface itself -> declare it here (official vtable order, IID c64226a8-9201-46af-b4cc-53fb9ff7414f)
#ifndef SN_HAVE_PIPELINELIB
struct ID3D12PipelineLibrary : public ID3D12DeviceChild {
    virtual HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState* pso) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pp) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** pp) = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetSerializedSize() = 0;
    virtual HRESULT STDMETHODCALLTYPE Serialize(void* data, SIZE_T size) = 0;
};
#endif
static const GUID IID_SN_PipelineLibrary = { 0xc64226a8, 0x9201, 0x46af, { 0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f } };

// ------------------------------------------------------------------ hashing
struct CrcTable { uint32_t t[256]; CrcTable() { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); t[i] = c; } } };
static uint32_t Crc32(const void* d, size_t n) {
    static const CrcTable ct; uint32_t crc = 0xFFFFFFFFu; const uint8_t* p = (const uint8_t*)d;
    while (n--) crc = ct.t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static uint64_t Fnv64(const void* d, size_t n) { uint64_t h = 1469598103934665603ull; const uint8_t* p = (const uint8_t*)d; while (n--) { h ^= *p++; h *= 1099511628211ull; } return h; }

// ------------------------------------------------------------------ SNSC container
#pragma pack(push, 1)
struct SnscHeader { char magic[4]; uint16_t version; uint16_t headerSize; uint64_t sourceKey; uint32_t entryCount; uint32_t reserved; };
struct SnscEntry { uint32_t type; uint32_t size; uint64_t offset; uint32_t crc; uint32_t vendor; uint32_t device; uint32_t revision; uint64_t driver; };
#pragma pack(pop)
static_assert(sizeof(SnscHeader) == 24, "SnscHeader size");
static_assert(sizeof(SnscEntry) == 40, "SnscEntry size");
enum { SNSC_DXBC = 1, SNSC_ROOTSIG = 2, SNSC_PSOLIB = 3 };

struct LibBlob { uint32_t vendor = 0, device = 0, revision = 0; uint64_t driver = 0; std::vector<uint8_t> data; };
struct ShaderCache { std::vector<uint8_t> dxbc, rs; std::vector<LibBlob> libs; };
static ShaderCache g_cache;
static std::mutex g_cacheMx;
static const wchar_t* kPsoName = L"SN_MV_CS";

// F_D3D12CreateDevice / F_CreateDXGIFactory originals (set by the MinHook trampolines in InitThread; used for our private device so we never go through Streamline)
static F_D3D12CreateDevice oD3D12CreateDevice; static F_CreateDXGIFactory oCreateDXGIFactory, oCreateDXGIFactory1; static F_CreateDXGIFactory2 oCreateDXGIFactory2;
static thread_local bool t_prewarm = false;     // true on our private warm-up thread: hooks must pass straight through
static HANDLE g_prewarmDone = nullptr;          // signalled when the warm-up is finished (the game's D3D12/DXGI creation calls wait for it)

static uint64_t MvSourceKey() {
    std::string s = std::string(kMvHlsl) + "|cs_5_1|flags=0|rootsig_v1|SNSC2|c22";
    return Fnv64(s.data(), s.size());
}
static std::vector<std::wstring> CachePaths() {
    std::vector<std::wstring> v; v.push_back(g_dir + L"\\SN_DLSSG_shaders.snsc");
    wchar_t la[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n && n < MAX_PATH) v.push_back(std::wstring(la) + L"\\SN_DLSSG\\SN_DLSSG_shaders.snsc");
    return v;
}
static bool ReadFileAll(const std::wstring& p, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64ll << 20);
    if (ok) { out.resize((size_t)sz.QuadPart); size_t got = 0; while (got < out.size()) { DWORD r = 0; if (!ReadFile(h, out.data() + got, (DWORD)std::min<size_t>(out.size() - got, 1u << 24), &r, nullptr) || !r) { ok = false; break; } got += r; } }
    CloseHandle(h); return ok;
}
static bool ParseCache(const std::vector<uint8_t>& f, ShaderCache& out, const char** why) {
    *why = "";
    if (f.size() < sizeof(SnscHeader) + 4) { *why = "file too small"; return false; }
    uint32_t stored; memcpy(&stored, &f[f.size() - 4], 4);
    if (Crc32(f.data(), f.size() - 4) != stored) { *why = "checksum mismatch (corrupt file)"; return false; }
    SnscHeader h; memcpy(&h, f.data(), sizeof h);
    if (memcmp(h.magic, "SNSC", 4) != 0) { *why = "bad magic"; return false; }
    if (h.version != 1 || h.headerSize != sizeof(SnscHeader)) { *why = "unsupported cache version"; return false; }
    if (h.sourceKey != MvSourceKey()) { *why = "shader source changed"; return false; }
    if (h.entryCount == 0 || h.entryCount > 32) { *why = "bad entry count"; return false; }
    size_t tableEnd = sizeof h + (size_t)h.entryCount * sizeof(SnscEntry);
    if (tableEnd + 4 > f.size()) { *why = "truncated entry table"; return false; }
    for (uint32_t i = 0; i < h.entryCount; i++) {
        SnscEntry e; memcpy(&e, &f[sizeof h + i * sizeof(SnscEntry)], sizeof e);
        if (e.offset < tableEnd || e.offset + e.size > f.size() - 4) { *why = "entry out of range"; return false; }
        const uint8_t* p = &f[(size_t)e.offset];
        if (Crc32(p, e.size) != e.crc) { *why = "entry checksum mismatch"; return false; }
        if (e.type == SNSC_DXBC) out.dxbc.assign(p, p + e.size);
        else if (e.type == SNSC_ROOTSIG) out.rs.assign(p, p + e.size);
        else if (e.type == SNSC_PSOLIB) { LibBlob lb; lb.vendor = e.vendor; lb.device = e.device; lb.revision = e.revision; lb.driver = e.driver; lb.data.assign(p, p + e.size); out.libs.push_back(std::move(lb)); }
    }
    if (out.dxbc.empty() || out.rs.empty()) { *why = "missing shader data"; return false; }
    return true;
}
static bool LoadShaderCache() {
    for (auto& p : CachePaths()) {
        std::vector<uint8_t> f; if (!ReadFileAll(p, f)) continue;
        ShaderCache c; const char* why = "";
        if (ParseCache(f, c, &why)) { g_cache = std::move(c); LogW((L"shader cache loaded: " + p).c_str()); Log("shader cache: DXBC %u bytes, root signature %u bytes, %u GPU pipeline librar%s", (unsigned)g_cache.dxbc.size(), (unsigned)g_cache.rs.size(), (unsigned)g_cache.libs.size(), g_cache.libs.size() == 1 ? "y" : "ies"); return true; }
        LogW((L"shader cache rejected: " + p).c_str()); Log("  reason: %s -> will be rebuilt", why);
    }
    return false;
}
static void BuildCacheFile(const ShaderCache& c, std::vector<uint8_t>& out) {
    struct P { uint32_t type; const std::vector<uint8_t>* d; const LibBlob* lb; };
    std::vector<P> parts; parts.push_back({ SNSC_DXBC, &c.dxbc, nullptr }); parts.push_back({ SNSC_ROOTSIG, &c.rs, nullptr });
    for (auto& l : c.libs) if (!l.data.empty()) parts.push_back({ SNSC_PSOLIB, &l.data, &l });
    SnscHeader h = {}; memcpy(h.magic, "SNSC", 4); h.version = 1; h.headerSize = sizeof h; h.sourceKey = MvSourceKey(); h.entryCount = (uint32_t)parts.size();
    size_t off = sizeof h + parts.size() * sizeof(SnscEntry);
    out.assign(off, 0); memcpy(out.data(), &h, sizeof h);
    for (size_t i = 0; i < parts.size(); i++) {
        SnscEntry e = {}; e.type = parts[i].type; e.size = (uint32_t)parts[i].d->size(); e.offset = out.size(); e.crc = Crc32(parts[i].d->data(), parts[i].d->size());
        if (parts[i].lb) { e.vendor = parts[i].lb->vendor; e.device = parts[i].lb->device; e.revision = parts[i].lb->revision; e.driver = parts[i].lb->driver; }
        memcpy(&out[sizeof h + i * sizeof e], &e, sizeof e);
        out.insert(out.end(), parts[i].d->begin(), parts[i].d->end());
    }
    uint32_t crc = Crc32(out.data(), out.size()); out.insert(out.end(), (uint8_t*)&crc, (uint8_t*)&crc + 4);
}
static bool SaveShaderCache() {
    std::vector<uint8_t> blob; BuildCacheFile(g_cache, blob);
    auto paths = CachePaths();
    for (size_t i = 0; i < paths.size(); i++) {
        const std::wstring& p = paths[i];
        if (i > 0) { std::wstring d = p.substr(0, p.find_last_of(L'\\')); CreateDirectoryW(d.c_str(), nullptr); }
        std::wstring tmp = p + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD w = 0; bool ok = WriteFile(h, blob.data(), (DWORD)blob.size(), &w, nullptr) && w == blob.size(); if (ok) FlushFileBuffers(h); CloseHandle(h);
        if (ok && MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { LogW((L"shader cache written: " + p).c_str()); Log("  %u bytes", (unsigned)blob.size()); return true; }
        DeleteFileW(tmp.c_str());
    }
    Log("shader cache: could not write the cache file anywhere (game folder read-only?) - shaders will be compiled again next launch");
    return false;
}

// ------------------------------------------------------------------ start-up progress window (own thread + own message loop, never touches the game's windows)
static std::atomic<float> g_spTarget{ 0.f }, g_spShown{ 0.f };
static std::mutex g_spMx; static std::wstring g_spStatus = L"Starting...";
static HWND g_spWnd = nullptr; static HANDLE g_spThread = nullptr; static bool g_spActive = false;

static LRESULT CALLBACK SplashProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HFONT fTitle = nullptr, fText = nullptr, fSmall = nullptr;
    switch (m) {
    case WM_CREATE:
        fTitle = CreateFontW(-21, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        fText = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        fSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        return 0;
    case WM_TIMER: {
        float t = g_spTarget.load(), s = g_spShown.load();
        if (s < t) { float d = (t - s) * 0.08f; if (d < 0.2f) d = 0.2f; s = std::min(t, s + d); g_spShown.store(s); }
        InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTCAPTION;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc); int W = rc.right, H = rc.bottom;
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, W, H); HGDIOBJ old = SelectObject(mem, bm);
        HBRUSH b = CreateSolidBrush(RGB(20, 22, 28)); FillRect(mem, &rc, b); DeleteObject(b);
        b = CreateSolidBrush(RGB(62, 66, 78)); FrameRect(mem, &rc, b); DeleteObject(b);
        SetBkMode(mem, TRANSPARENT);
        std::wstring st; { std::lock_guard<std::mutex> lk(g_spMx); st = g_spStatus; }
        RECT r = { 24, 16, W - 24, 44 }; SelectObject(mem, fTitle); SetTextColor(mem, RGB(240, 242, 246)); DrawTextW(mem, L"SN DLSS Frame Generation", -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        r = { 24, 46, W - 24, 68 }; SelectObject(mem, fSmall); SetTextColor(mem, RGB(150, 156, 170)); DrawTextW(mem, L"One-time setup - the game starts automatically.", -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        float pct = std::min(100.f, std::max(0.f, g_spShown.load()));
        r = { 24, 84, W - 90, 106 }; SelectObject(mem, fText); SetTextColor(mem, RGB(214, 218, 228)); DrawTextW(mem, st.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        wchar_t pb[16]; swprintf(pb, 16, L"%d%%", (int)pct);
        r = { W - 90, 84, W - 24, 106 }; DrawTextW(mem, pb, -1, &r, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        RECT track = { 24, 116, W - 24, 134 }; b = CreateSolidBrush(RGB(42, 46, 56)); FillRect(mem, &track, b); DeleteObject(b);
        RECT fill = track; fill.right = track.left + (LONG)((track.right - track.left) * pct / 100.f);
        if (fill.right > fill.left) { b = CreateSolidBrush(RGB(118, 185, 0)); FillRect(mem, &fill, b); DeleteObject(b); }
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps); return 0; }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: DeleteObject(fTitle); DeleteObject(fText); DeleteObject(fSmall); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static DWORD WINAPI SplashThread(LPVOID ev) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {}; wc.lpfnWndProc = SplashProc; wc.hInstance = hi; wc.lpszClassName = L"SN_DLSSG_Splash"; wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    const int W = 480, H = 156; RECT wa = { 0, 0, 1920, 1080 }; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - W) / 2, y = wa.top + ((wa.bottom - wa.top) - H) / 2;
    g_spWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"SN DLSS Frame Generation", WS_POPUP, x, y, W, H, nullptr, nullptr, hi, nullptr);
    if (g_spWnd) { ShowWindow(g_spWnd, SW_SHOWNOACTIVATE); UpdateWindow(g_spWnd); SetTimer(g_spWnd, 1, 16, nullptr); }
    SetEvent((HANDLE)ev);
    MSG msg; while (g_spWnd && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
static void SplashStart() {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_spThread = CreateThread(nullptr, 0, SplashThread, ev, 0, nullptr);
    if (g_spThread) { WaitForSingleObject(ev, 3000); g_spActive = g_spWnd != nullptr; }
    CloseHandle(ev);
}
static void Prog(float pct, const wchar_t* text) {           // no-op when no window is shown
    Log("[shader warm-up %3d%%] %ls", (int)pct, text);
    if (!g_spActive) return;
    { std::lock_guard<std::mutex> lk(g_spMx); g_spStatus = text; }
    g_spTarget.store(pct);
}
static void SplashFinish() {
    if (!g_spThread) return;
    Prog(100.f, L"Done - starting the game...");
    for (int i = 0; i < 400 && g_spShown.load() < 99.5f; i++) Sleep(10);   // let the bar reach 100% (max ~4 s)
    Sleep(450);
    if (g_spWnd) PostMessageW(g_spWnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(g_spThread, 2000); CloseHandle(g_spThread); g_spThread = nullptr; g_spActive = false;
}

// ------------------------------------------------------------------ HLSL -> DXBC + root signature (the expensive CPU part)
static bool CompileMvShader(std::vector<uint8_t>& dxbc, std::vector<uint8_t>& rs) {
    Prog(10.f, L"Loading HLSL compiler (d3dcompiler_47)...");
    HMODULE m12 = GetModuleHandleW(L"d3d12.dll"); if (!m12) m12 = LoadLibraryW(L"d3d12.dll");
    HMODULE mc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m12 || !mc) { Log("MV: d3d12/d3dcompiler missing"); return false; }
    typedef HRESULT(WINAPI* PFN_SRS)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    typedef HRESULT(WINAPI* PFN_Comp)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto pSRS = (PFN_SRS)GetProcAddress(m12, "D3D12SerializeRootSignature"); auto pComp = (PFN_Comp)GetProcAddress(mc, "D3DCompile");
    if (!pSRS || !pComp) { Log("MV: missing D3D12SerializeRootSignature/D3DCompile"); return false; }
    Prog(20.f, L"Compiling motion-vector shader (HLSL -> DXBC)...");
    ID3DBlob* cs = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = pComp(kMvHlsl, strlen(kMvHlsl), "mv.hlsl", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
    if (FAILED(hr)) { Log("MV: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    if (err) err->Release();
    Prog(45.f, L"Creating root signature...");
    D3D12_DESCRIPTOR_RANGE rg[2] = {};
    rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors = 2; rg[0].BaseShaderRegister = 0; rg[0].OffsetInDescriptorsFromTableStart = 0;
    rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rg[1].NumDescriptors = 2; rg[1].BaseShaderRegister = 0; rg[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.ShaderRegister = 0; rp[0].Constants.Num32BitValues = 22; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 2; rp[1].DescriptorTable.pDescriptorRanges = rg; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {}; rsd.NumParameters = 2; rsd.pParameters = rp;
    ID3DBlob* blob = nullptr; err = nullptr;
    if (FAILED(pSRS(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { Log("MV: serialize RS failed"); cs->Release(); return false; }
    dxbc.assign((uint8_t*)cs->GetBufferPointer(), (uint8_t*)cs->GetBufferPointer() + cs->GetBufferSize());
    rs.assign((uint8_t*)blob->GetBufferPointer(), (uint8_t*)blob->GetBufferPointer() + blob->GetBufferSize());
    cs->Release(); blob->Release(); if (err) err->Release();
    return true;
}

// ------------------------------------------------------------------ GPU side: PSO from the driver's pipeline library (fast) or from DXBC (driver compiles)
static ID3D12PipelineState* MakeMvPso(ID3D12Device* dev, ID3D12RootSignature* rs, const std::vector<uint8_t>& dxbc, bool tryLib, bool* usedLib) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = rs; pd.CS.pShaderBytecode = dxbc.data(); pd.CS.BytecodeLength = dxbc.size();
    ID3D12PipelineState* pso = nullptr; if (usedLib) *usedLib = false;
    if (tryLib && !g_cache.libs.empty()) {
        ID3D12Device1* d1 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device1), (void**)&d1)) && d1) {
            for (auto& lb : g_cache.libs) {
                if (lb.data.empty()) continue;
                ID3D12PipelineLibrary* lib = nullptr;
                if (SUCCEEDED(d1->CreatePipelineLibrary(lb.data.data(), lb.data.size(), IID_SN_PipelineLibrary, (void**)&lib)) && lib) {
                    HRESULT hr = lib->LoadComputePipeline(kPsoName, &pd, __uuidof(ID3D12PipelineState), (void**)&pso);
                    lib->Release();
                    if (SUCCEEDED(hr) && pso) { if (usedLib) *usedLib = true; break; }
                    pso = nullptr;
                }
            }
            d1->Release();
        }
    }
    if (!pso && FAILED(dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso))) pso = nullptr;
    return pso;
}
static bool BuildLibraryBlob(ID3D12Device* dev, ID3D12PipelineState* pso, std::vector<uint8_t>& out) {
    ID3D12Device1* d1 = nullptr; if (FAILED(dev->QueryInterface(__uuidof(ID3D12Device1), (void**)&d1)) || !d1) return false;
    ID3D12PipelineLibrary* lib = nullptr; bool ok = false;
    if (SUCCEEDED(d1->CreatePipelineLibrary(nullptr, 0, IID_SN_PipelineLibrary, (void**)&lib)) && lib) {
        if (SUCCEEDED(lib->StorePipeline(kPsoName, pso))) { SIZE_T n = lib->GetSerializedSize(); if (n) { out.resize(n); ok = SUCCEEDED(lib->Serialize(out.data(), n)); } }
        lib->Release();
    }
    d1->Release(); return ok;
}

// Used by the game thread (InitMvPass): builds RS + PSO from the cache. Compiles at run time only if there is no cache (and then saves one).
static ID3D12Device* g_mvDev = nullptr;
static bool EnsureMvPipeline(ID3D12Device* dev) {
    if (g_mvRS && g_mvPSO && g_mvDev == dev) return true;
    std::lock_guard<std::mutex> lk(g_cacheMx);
    if (g_mvRS && g_mvPSO && g_mvDev == dev) return true;
    DWORD t0 = GetTickCount();
    if (g_cache.dxbc.empty() || g_cache.rs.empty()) {
        Log("MV: no cached shader -> compiling now (one-time hitch, saved for the next launch)");
        std::vector<uint8_t> d, r; if (!CompileMvShader(d, r)) return false;
        g_cache.dxbc = std::move(d); g_cache.rs = std::move(r);
        if (g_cfg.shaderCache) SaveShaderCache();
    }
    ID3D12RootSignature* rs = nullptr;
    HRESULT hr = dev->CreateRootSignature(0, g_cache.rs.data(), g_cache.rs.size(), __uuidof(ID3D12RootSignature), (void**)&rs);
    if (FAILED(hr) || !rs) { Log("MV: CreateRootSignature 0x%08X", (unsigned)hr); return false; }
    bool usedLib = false; ID3D12PipelineState* pso = MakeMvPso(dev, rs, g_cache.dxbc, g_cfg.shaderCache != 0, &usedLib);
    if (!pso) { rs->Release(); Log("MV: CreateComputePipelineState failed"); return false; }
    g_mvRS = rs; g_mvPSO = pso; g_mvDev = dev;
    Log("MV: pipeline ready in %u ms (%s)", (unsigned)(GetTickCount() - t0), usedLib ? "loaded from the GPU pipeline library cache" : "built from cached DXBC by the driver");
    return true;
}

// ------------------------------------------------------------------ warm-up before the game creates its device
struct AdapterPick { IDXGIAdapter1* ad = nullptr; uint32_t vendor = 0, device = 0, revision = 0; uint64_t driver = 0; wchar_t name[128] = {}; };
static bool PickAdapter(AdapterPick& out) {
    F_CreateDXGIFactory f1 = oCreateDXGIFactory1;
    if (!f1) { HMODULE mdx = GetModuleHandleW(L"dxgi.dll"); if (mdx) f1 = (F_CreateDXGIFactory)GetProcAddress(mdx, "CreateDXGIFactory1"); }
    if (!f1) return false;
    IDXGIFactory1* fac = nullptr; if (FAILED(f1(__uuidof(IDXGIFactory1), (void**)&fac)) || !fac) return false;
    unsigned long long best = 0; bool found = false;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = nullptr; if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
        DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a->Release(); continue; }
        unsigned long long score = (d.VendorId == 0x10DE ? (1ull << 62) : 0ull) + (unsigned long long)d.DedicatedVideoMemory;   // DLSS-G only runs on NVIDIA -> prefer it
        if (!found || score > best) {
            if (out.ad) out.ad->Release();
            out.ad = a; best = score; found = true; out.vendor = d.VendorId; out.device = d.DeviceId; out.revision = d.Revision;
            wcsncpy(out.name, d.Description, 127);
            LARGE_INTEGER umd = {}; if (FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) umd.QuadPart = 0; out.driver = (uint64_t)umd.QuadPart;
        } else a->Release();
    }
    fac->Release(); return found;
}
static void PrewarmWork() {
    LoadShaderCache();
    AdapterPick ap; bool haveAd = PickAdapter(ap);
    bool haveSrc = !g_cache.dxbc.empty() && !g_cache.rs.empty(), haveLib = false;
    if (haveAd) { Log("warm-up adapter: %ls vendor=0x%04X device=0x%04X rev=%u driver=0x%016llX", ap.name, ap.vendor, ap.device, ap.revision, (unsigned long long)ap.driver);
        for (auto& l : g_cache.libs) if (l.vendor == ap.vendor && l.device == ap.device && l.revision == ap.revision && l.driver == ap.driver) haveLib = true; }
    bool needCompile = !haveSrc, needLib = haveAd && !haveLib;
    if (!needCompile && !needLib) { Log("shader cache is up to date -> nothing to build, no progress window"); if (ap.ad) ap.ad->Release(); return; }
    Log("shader cache needs work: compile=%d gpu-pipeline=%d", (int)needCompile, (int)needLib);
    if (g_cfg.splash) SplashStart();
    Prog(3.f, L"Preparing shader cache...");
    bool ok = true;
    if (needCompile) { std::vector<uint8_t> d, r; if (CompileMvShader(d, r)) { g_cache.dxbc = std::move(d); g_cache.rs = std::move(r); g_cache.libs.clear(); } else ok = false; }
    else Prog(50.f, L"Shader bytecode found in cache...");
    if (ok && haveAd) {
        wchar_t msg[200]; swprintf(msg, 200, L"Creating GPU device (%ls)...", ap.name); Prog(55.f, msg);
        F_D3D12CreateDevice cd = oD3D12CreateDevice;
        if (!cd) { HMODULE m = GetModuleHandleW(L"d3d12.dll"); if (m) cd = (F_D3D12CreateDevice)GetProcAddress(m, "D3D12CreateDevice"); }
        ID3D12Device* dev = nullptr;
        if (cd && SUCCEEDED(cd((IUnknown*)ap.ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev)) && dev) {
            Prog(65.f, L"Creating root signature on the GPU...");
            ID3D12RootSignature* rs = nullptr;
            if (SUCCEEDED(dev->CreateRootSignature(0, g_cache.rs.data(), g_cache.rs.size(), __uuidof(ID3D12RootSignature), (void**)&rs)) && rs) {
                Prog(72.f, L"Building GPU pipeline (driver shader compile)...");
                ID3D12PipelineState* pso = MakeMvPso(dev, rs, g_cache.dxbc, false, nullptr);
                if (pso) {
                    Prog(85.f, L"Serializing GPU pipeline cache...");
                    LibBlob lb; lb.vendor = ap.vendor; lb.device = ap.device; lb.revision = ap.revision; lb.driver = ap.driver;
                    if (BuildLibraryBlob(dev, pso, lb.data)) {
                        for (size_t i = 0; i < g_cache.libs.size();) { auto& o = g_cache.libs[i]; if (o.vendor == lb.vendor && o.device == lb.device) g_cache.libs.erase(g_cache.libs.begin() + i); else i++; }   // older driver of the same GPU
                        if (g_cache.libs.size() >= 4) g_cache.libs.erase(g_cache.libs.begin());
                        g_cache.libs.push_back(std::move(lb));
                        Prog(90.f, L"Verifying cache...");
                        bool used = false; ID3D12PipelineState* p2 = MakeMvPso(dev, rs, g_cache.dxbc, true, &used);
                        Log("GPU pipeline library verification: %s", used ? "OK (PSO loaded from library)" : "not loadable here (will fall back to DXBC at run time)");
                        if (p2) p2->Release();
                    } else Log("pipeline library could not be serialized on this driver (DXBC cache is still stored)");
                    pso->Release();
                } else Log("temporary PSO creation failed (DXBC cache is still stored)");
                rs->Release();
            }
            dev->Release();
        } else Log("temporary D3D12 device could not be created (DXBC cache is still stored)");
    }
    if (ok) { Prog(95.f, L"Writing shader cache..."); SaveShaderCache(); }
    else Log("shader warm-up failed -> the game will compile at run time as before");
    if (ap.ad) ap.ad->Release();
    SplashFinish();
}
static DWORD WINAPI PrewarmThread(LPVOID) {
    t_prewarm = true;
    try { PrewarmWork(); } catch (...) { Log("exception in shader warm-up (ignored)"); }
    if (g_spThread) { g_spActive = false; if (g_spWnd) PostMessageW(g_spWnd, WM_CLOSE, 0, 0); }
    SetEvent(g_prewarmDone); return 0;
}
static void PrewarmInit() { if (g_cfg.shaderCache && !g_prewarmDone) g_prewarmDone = CreateEventW(nullptr, TRUE, FALSE, nullptr); }   // before the hooks are enabled
static void PrewarmStart() {
    if (!g_prewarmDone) return;
    HANDLE t = CreateThread(nullptr, 0, PrewarmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t); else SetEvent(g_prewarmDone);
}
static void WaitPrewarm() {   // called by the hooked D3D12CreateDevice / CreateDXGIFactory*: the game does not get a device until the cache is ready
    if (t_prewarm || !g_prewarmDone) return;
    if (WaitForSingleObject(g_prewarmDone, 60000) == WAIT_TIMEOUT) { static std::atomic<bool> once{ false }; if (!once.exchange(true)) Log("shader warm-up took > 60 s, letting the game continue"); }
}
