// psocache.inc - v26 persistent cache for the GAME's own pipeline states (graphics + compute). Included from main.cpp (after shadercache.inc).
//
// The problem
//   Every shader the game uses has to be turned into GPU code by the driver (CreateGraphicsPipelineState / CreateComputePipelineState).
//   That is the "shader compilation" stutter. The injector cannot know the game's shader list in advance, so it cannot compile "everything"
//   before the first launch - but it can make sure every pipeline is compiled only ONCE, ever.
//
// How it works
//   * Every pipeline the game creates is identified by a 64-bit hash (all shader byte code + every state field + the root signature).
//   * After a pipeline was created normally, the driver's own cached blob (ID3D12PipelineState::GetCachedBlob) is appended to
//         SN_DLSSG_psocache.bin   (next to the game exe; falls back to %LOCALAPPDATA%\SN_DLSSG\ if the game folder is read-only)
//   * On the next launch the file is read BEFORE the game creates its D3D12 device (background thread, started with the shader warm-up).
//     When the game asks for a pipeline we already know, we hand the driver its cached blob (D3D12_CACHED_PIPELINE_STATE) and the driver skips the
//     shader compile. If the driver rejects a blob (driver update, corrupt data) the entry is dropped and the pipeline is compiled normally.
//   * The file is bound to the GPU + driver version. A new driver = the file is started again from scratch (blobs are driver-specific).
//   * The game's own CachedPSO (UE pipeline-library) is never touched: pipelines that already carry a blob are passed through unchanged.
//
// File layout (little endian, append-only, every record has its own CRC; a torn last record after a crash is simply cut off)
//   PsoFileHeader (32 bytes) | { PsoRecHeader (16 bytes) , blob bytes } ...
#pragma once
#include <memory>

#pragma pack(push, 1)
struct PsoFileHeader { char magic[4]; uint16_t version; uint16_t headerSize; uint64_t gpuKey; uint64_t reserved0; uint64_t reserved1; };
struct PsoRecHeader  { uint64_t hash; uint32_t size; uint32_t crc; };
#pragma pack(pop)
static_assert(sizeof(PsoFileHeader) == 32, "PsoFileHeader size");
static_assert(sizeof(PsoRecHeader) == 16, "PsoRecHeader size");

typedef std::shared_ptr<const std::vector<uint8_t>> PsoBlobPtr;
static std::unordered_map<uint64_t, PsoBlobPtr> g_psoMap; static std::shared_mutex g_psoMx;
static std::vector<std::pair<uint64_t, PsoBlobPtr>> g_psoPending; static std::mutex g_psoQMx;     // new blobs waiting to be appended to the file
static std::atomic<bool> g_psoReady{ false };                  // the cache file has been read (or there is none)
static HANDLE g_psoReadyEv = nullptr;
static std::atomic<size_t> g_psoBytes{ 0 };
static std::atomic<long> g_psoHit{ 0 }, g_psoNew{ 0 }, g_psoRejected{ 0 }, g_psoSkipFull{ 0 }, g_psoSeen{ 0 };
static std::wstring g_psoPath; static uint64_t g_psoGpuKey = 0; static bool g_psoFileOk = false;

// root signature identity: hash of the serialized blob, remembered per object (updated on every creation, so a recycled address never keeps a stale hash)
static std::unordered_map<void*, uint64_t> g_rsHash; static std::shared_mutex g_rsMx;
static void NoteRootSig(void* rs, const void* blob, SIZE_T len) {
    if (!rs || !blob || !len) return;
    uint64_t h = Fnv64(blob, (size_t)len);
    std::unique_lock<std::shared_mutex> lk(g_rsMx); g_rsHash[rs] = h;
}
static uint64_t RsHashOf(void* rs) {
    if (!rs) return 0; std::shared_lock<std::shared_mutex> lk(g_rsMx);
    auto it = g_rsHash.find(rs); return it != g_rsHash.end() ? it->second : 0x9E3779B97F4A7C15ull;   // unknown object: still deterministic per run, but never matches a known one by accident
}

// ------------------------------------------------------------------ hashing of pipeline descriptions (field by field: padding bytes of the D3D12 structs are NOT stable)
struct PsoHasher {
    uint64_t h = 1469598103934665603ull;
    void Bytes(const void* d, size_t n) { const uint8_t* p = (const uint8_t*)d; while (n--) { h ^= *p++; h *= 1099511628211ull; } }
    template <class T> void V(const T& v) { Bytes(&v, sizeof v); }
    void Shader(const D3D12_SHADER_BYTECODE& s) { uint64_t n = s.pShaderBytecode ? s.BytecodeLength : 0; V(n); if (n) Bytes(s.pShaderBytecode, (size_t)n); }
    void Blend(const D3D12_BLEND_DESC& b) {
        V((int)b.AlphaToCoverageEnable); V((int)b.IndependentBlendEnable);
        for (int i = 0; i < 8; i++) { const D3D12_RENDER_TARGET_BLEND_DESC& r = b.RenderTarget[i]; V((int)r.BlendEnable); V((int)r.LogicOpEnable); V((int)r.SrcBlend); V((int)r.DestBlend); V((int)r.BlendOp); V((int)r.SrcBlendAlpha); V((int)r.DestBlendAlpha); V((int)r.BlendOpAlpha); V((int)r.LogicOp); V((int)r.RenderTargetWriteMask); }
    }
    void Depth(const D3D12_DEPTH_STENCIL_DESC& d) {
        V((int)d.DepthEnable); V((int)d.DepthWriteMask); V((int)d.DepthFunc); V((int)d.StencilEnable); V((int)d.StencilReadMask); V((int)d.StencilWriteMask);
        const D3D12_DEPTH_STENCILOP_DESC* f[2] = { &d.FrontFace, &d.BackFace }; for (int i = 0; i < 2; i++) { V((int)f[i]->StencilFailOp); V((int)f[i]->StencilDepthFailOp); V((int)f[i]->StencilPassOp); V((int)f[i]->StencilFunc); }
    }
    void Raster(const D3D12_RASTERIZER_DESC& r) {
        V((int)r.FillMode); V((int)r.CullMode); V((int)r.FrontCounterClockwise); V((int)r.DepthBias); V(r.DepthBiasClamp); V(r.SlopeScaledDepthBias); V((int)r.DepthClipEnable); V((int)r.MultisampleEnable); V((int)r.AntialiasedLineEnable); V((int)r.ForcedSampleCount); V((int)r.ConservativeRaster);
    }
};
static uint64_t HashGfxDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    PsoHasher H; H.V((int)1); H.V(RsHashOf(d->pRootSignature));
    H.Shader(d->VS); H.Shader(d->PS); H.Shader(d->DS); H.Shader(d->HS); H.Shader(d->GS);
    H.V((UINT)d->StreamOutput.NumEntries); H.V((UINT)d->StreamOutput.NumStrides); H.V((UINT)d->StreamOutput.RasterizedStream);
    if (d->StreamOutput.pSODeclaration) for (UINT i = 0; i < d->StreamOutput.NumEntries && i < 64; i++) { const D3D12_SO_DECLARATION_ENTRY& e = d->StreamOutput.pSODeclaration[i]; H.V((UINT)e.Stream); if (e.SemanticName) H.Bytes(e.SemanticName, strlen(e.SemanticName)); H.V((UINT)e.SemanticIndex); H.V((int)e.StartComponent); H.V((int)e.ComponentCount); H.V((int)e.OutputSlot); }
    if (d->StreamOutput.pBufferStrides) for (UINT i = 0; i < d->StreamOutput.NumStrides && i < 4; i++) H.V((UINT)d->StreamOutput.pBufferStrides[i]);
    H.Blend(d->BlendState); H.V((UINT)d->SampleMask); H.Raster(d->RasterizerState); H.Depth(d->DepthStencilState);
    H.V((UINT)d->InputLayout.NumElements);
    if (d->InputLayout.pInputElementDescs) for (UINT i = 0; i < d->InputLayout.NumElements && i < 64; i++) {
        const D3D12_INPUT_ELEMENT_DESC& e = d->InputLayout.pInputElementDescs[i];
        if (e.SemanticName) H.Bytes(e.SemanticName, strlen(e.SemanticName)); H.V((UINT)e.SemanticIndex); H.V((int)e.Format); H.V((UINT)e.InputSlot); H.V((UINT)e.AlignedByteOffset); H.V((int)e.InputSlotClass); H.V((UINT)e.InstanceDataStepRate);
    }
    H.V((int)d->IBStripCutValue); H.V((int)d->PrimitiveTopologyType); H.V((UINT)d->NumRenderTargets);
    for (int i = 0; i < 8; i++) H.V((int)d->RTVFormats[i]);
    H.V((int)d->DSVFormat); H.V((UINT)d->SampleDesc.Count); H.V((UINT)d->SampleDesc.Quality); H.V((UINT)d->NodeMask); H.V((int)d->Flags);
    return H.h ? H.h : 1;
}
static uint64_t HashCompDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d) {
    PsoHasher H; H.V((int)2); H.V(RsHashOf(d->pRootSignature)); H.Shader(d->CS); H.V((UINT)d->NodeMask); H.V((int)d->Flags);
    return H.h ? H.h : 1;
}

// ------------------------------------------------------------------ lookup / store
static PsoBlobPtr PsoFind(uint64_t key) { std::shared_lock<std::shared_mutex> lk(g_psoMx); auto it = g_psoMap.find(key); return it != g_psoMap.end() ? it->second : PsoBlobPtr(); }
static void PsoErase(uint64_t key) { std::unique_lock<std::shared_mutex> lk(g_psoMx); auto it = g_psoMap.find(key); if (it != g_psoMap.end()) { g_psoBytes -= std::min(g_psoBytes.load(), it->second->size()); g_psoMap.erase(it); } }
static void PsoStore(uint64_t key, ID3D12PipelineState* pso) {
    if (!pso || !key) return;
    const size_t cap = (size_t)g_cfg.psoCacheMB << 20;
    if (g_psoBytes.load() >= cap) { long n = ++g_psoSkipFull; if (n == 1) Log("pso-cache: size limit (%d MB) reached - new pipelines are no longer stored (raise psocachemb or delete SN_DLSSG_psocache.bin)", g_cfg.psoCacheMB); return; }
    ID3DBlob* b = nullptr; if (FAILED(pso->GetCachedBlob(&b)) || !b) return;
    size_t n = b->GetBufferSize();
    if (n == 0 || n > (32u << 20)) { b->Release(); return; }
    auto v = std::make_shared<std::vector<uint8_t>>((uint8_t*)b->GetBufferPointer(), (uint8_t*)b->GetBufferPointer() + n); b->Release();
    PsoBlobPtr p = v;
    { std::unique_lock<std::shared_mutex> lk(g_psoMx); if (!g_psoMap.emplace(key, p).second) return; }
    g_psoBytes += n; g_psoNew++;
    { std::lock_guard<std::mutex> lk(g_psoQMx); g_psoPending.emplace_back(key, p); }
}

// ------------------------------------------------------------------ file
static std::vector<std::wstring> PsoPaths() {
    std::vector<std::wstring> v; v.push_back(g_dir + L"\\SN_DLSSG_psocache.bin");
    wchar_t la[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n && n < MAX_PATH) v.push_back(std::wstring(la) + L"\\SN_DLSSG\\SN_DLSSG_psocache.bin");
    return v;
}
static bool ReadExact(HANDLE h, void* d, size_t n) { uint8_t* p = (uint8_t*)d; while (n) { DWORD r = 0; if (!ReadFile(h, p, (DWORD)std::min<size_t>(n, 1u << 24), &r, nullptr) || !r) return false; p += r; n -= r; } return true; }
static bool WriteFileSync(HANDLE h, const void* d, size_t n) { const uint8_t* p = (const uint8_t*)d; while (n) { DWORD w = 0; if (!WriteFile(h, p, (DWORD)std::min<size_t>(n, 1u << 24), &w, nullptr) || !w) return false; p += w; n -= w; } return true; }
static bool WriteNewPsoFile(const std::wstring& p) {
    size_t sl = p.find_last_of(L'\\'); if (sl != std::wstring::npos) CreateDirectoryW(p.substr(0, sl).c_str(), nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (h == INVALID_HANDLE_VALUE) return false;
    PsoFileHeader fh = {}; memcpy(fh.magic, "SNPC", 4); fh.version = 1; fh.headerSize = sizeof fh; fh.gpuKey = g_psoGpuKey;
    bool ok = WriteFileSync(h, &fh, sizeof fh); CloseHandle(h); return ok;
}
// returns: 1 = loaded (or empty but valid), 0 = not usable (a new file was started or none can be written)
static void PsoLoadFile() {
    const size_t cap = (size_t)g_cfg.psoCacheMB << 20; DWORD t0 = GetTickCount();
    for (auto& p : PsoPaths()) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool usable = false; LONGLONG validLen = 0, fileLen = 0; size_t count = 0, bytes = 0;
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz = {}; GetFileSizeEx(h, &sz); fileLen = sz.QuadPart;
            PsoFileHeader fh; const char* why = "";
            if (!ReadExact(h, &fh, sizeof fh)) why = "file too small";
            else if (memcmp(fh.magic, "SNPC", 4) != 0 || fh.version != 1 || fh.headerSize != sizeof fh) why = "unknown format";
            else if (fh.gpuKey != g_psoGpuKey) why = "GPU or driver changed (cached blobs are driver specific)";
            else usable = true;
            if (!usable) Log("pso-cache: %s -> starting a new file", why);
            else {
                validLen = sizeof fh; std::unique_lock<std::shared_mutex> lk(g_psoMx);
                for (;;) {
                    PsoRecHeader rh; if (!ReadExact(h, &rh, sizeof rh)) break;
                    if (rh.size == 0 || rh.size > (32u << 20) || (LONGLONG)rh.size > fileLen - validLen - (LONGLONG)sizeof rh) break;
                    std::vector<uint8_t> d(rh.size); if (!ReadExact(h, d.data(), d.size())) break;
                    if (Crc32(d.data(), d.size()) != rh.crc) break;
                    validLen += sizeof rh + rh.size;
                    if (bytes + d.size() > cap) { g_psoSkipFull++; continue; }
                    bytes += d.size(); count++; g_psoMap[rh.hash] = std::make_shared<const std::vector<uint8_t>>(std::move(d));
                }
            }
            CloseHandle(h);
        }
        if (usable) {
            if (validLen < fileLen) { HANDLE w = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); if (w != INVALID_HANDLE_VALUE) { LARGE_INTEGER pos; pos.QuadPart = validLen; SetFilePointerEx(w, pos, nullptr, FILE_BEGIN); SetEndOfFile(w); CloseHandle(w); Log("pso-cache: cut %lld bytes of damaged data from the end of the file", fileLen - validLen); } }
            g_psoBytes = bytes; g_psoPath = p; g_psoFileOk = true;
            LogW((L"pso-cache loaded: " + p).c_str()); Log("pso-cache: %u pipelines, %u MB, read in %u ms (they will be handed to the driver instead of being compiled)", (unsigned)count, (unsigned)(bytes >> 20), (unsigned)(GetTickCount() - t0));
            return;
        }
        if (WriteNewPsoFile(p)) { g_psoPath = p; g_psoFileOk = true; { std::unique_lock<std::shared_mutex> lk(g_psoMx); g_psoMap.clear(); } g_psoBytes = 0; LogW((L"pso-cache: new file " + p).c_str()); return; }
    }
    Log("pso-cache: no writable location (game folder read-only and no LOCALAPPDATA?) - the pipeline cache is kept in memory only");
}
static void PsoFlush() {
    std::vector<std::pair<uint64_t, PsoBlobPtr>> q;
    { std::lock_guard<std::mutex> lk(g_psoQMx); q.swap(g_psoPending); }
    if (q.empty() || !g_psoFileOk) return;
    HANDLE h = CreateFileW(g_psoPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("pso-cache: cannot append to the cache file (0x%08X)", (unsigned)GetLastError()); return; }
    std::vector<uint8_t> buf; bool ok = true; size_t total = 0;
    for (auto& e : q) {
        PsoRecHeader rh; rh.hash = e.first; rh.size = (uint32_t)e.second->size(); rh.crc = Crc32(e.second->data(), e.second->size());
        buf.insert(buf.end(), (uint8_t*)&rh, (uint8_t*)&rh + sizeof rh); buf.insert(buf.end(), e.second->begin(), e.second->end());
        if (buf.size() >= (8u << 20)) { ok = ok && WriteFileSync(h, buf.data(), buf.size()); total += buf.size(); buf.clear(); }
    }
    if (!buf.empty()) { ok = ok && WriteFileSync(h, buf.data(), buf.size()); total += buf.size(); }
    CloseHandle(h);
    Log("pso-cache: +%u pipelines appended (%u KB) | total %u MB | hits %ld new %ld rejected %ld of %ld created%s", (unsigned)q.size(), (unsigned)(total >> 10), (unsigned)(g_psoBytes.load() >> 20), g_psoHit.load(), g_psoNew.load(), g_psoRejected.load(), g_psoSeen.load(), ok ? "" : " (WRITE ERROR)");
}

// ------------------------------------------------------------------ GPU/driver key (same adapter pick as the shader warm-up)
static DWORD WINAPI PsoCacheThread(LPVOID) {
    t_prewarm = true;                                   // our own thread: the D3D12 / DXGI hooks must pass straight through
    try {
        AdapterPick ap; bool have = PickAdapter(ap);
        std::string s = have ? "gpu|" : "nogpu|"; if (have) { char b[96]; snprintf(b, sizeof b, "%04X|%04X|%u|%016llX", ap.vendor, ap.device, ap.revision, (unsigned long long)ap.driver); s += b; }
        s += "|PSO1"; g_psoGpuKey = Fnv64(s.data(), s.size()); if (ap.ad) ap.ad->Release();
        PsoLoadFile();
    } catch (...) { Log("pso-cache: exception while loading (ignored)"); }
    g_psoReady = true; SetEvent(g_psoReadyEv);
    DWORD lastLog = GetTickCount();
    for (;;) {                                          // appender: new pipelines are written in the background, never on a render / RHI thread
        Sleep(15000);
        try { size_t pend; { std::lock_guard<std::mutex> lk(g_psoQMx); pend = g_psoPending.size(); } if (pend) PsoFlush(); } catch (...) {}
        (void)lastLog;
    }
    return 0;
}
static void PsoCacheStart() {
    if (!g_cfg.psoCache || g_psoReadyEv) return;
    g_psoReadyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!g_psoReadyEv) return;
    HANDLE t = CreateThread(nullptr, 0, PsoCacheThread, nullptr, 0, nullptr);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); } else { g_psoReady = true; SetEvent(g_psoReadyEv); }
}
static void PsoWaitReady() {                            // the first pipeline the game creates waits for the cache file to be read (normally a few hundred ms)
    if (g_psoReady.load(std::memory_order_acquire) || !g_psoReadyEv) return;
    if (WaitForSingleObject(g_psoReadyEv, 15000) == WAIT_TIMEOUT) { static std::atomic<bool> once{ false }; if (!once.exchange(true)) Log("pso-cache: reading the cache took > 15 s, continuing without it for now"); }
}

// ------------------------------------------------------------------ pipeline STATE STREAM (ID3D12Device2::CreatePipelineState) - the API most UE5 builds use
// The stream is a packed list of sub-objects: each one starts at an 8-byte boundary with a 4-byte type, followed by the payload (aligned to 8 if it contains a pointer, else to 4).
// The parser below mirrors the runtime's own rules; anything it does not know makes the pipeline pass through UNTOUCHED (never a guess).
struct SnStreamDesc { SIZE_T SizeInBytes; void* pPipelineStateSubobjectStream; };      // = D3D12_PIPELINE_STATE_STREAM_DESC (not declared by mingw's d3d12.h)
struct SubInfo { int type; size_t off; size_t size; };
enum { SUB_RS = 0, SUB_VS, SUB_PS, SUB_DS, SUB_HS, SUB_GS, SUB_CS, SUB_SO, SUB_BLEND, SUB_SAMPLEMASK, SUB_RASTER, SUB_DEPTH, SUB_INPUT, SUB_IBCUT, SUB_TOPO, SUB_RTFMT, SUB_DSFMT, SUB_SAMPLE, SUB_NODE, SUB_CACHED, SUB_FLAGS, SUB_DEPTH1, SUB_VIEWINST, SUB_AS = 24, SUB_MS, SUB_DEPTH2, SUB_RASTER1, SUB_RASTER2 };
static_assert(sizeof(D3D12_SHADER_BYTECODE) == 16 && sizeof(D3D12_BLEND_DESC) == 328 && sizeof(D3D12_DEPTH_STENCIL_DESC) == 52 && sizeof(D3D12_STREAM_OUTPUT_DESC) == 32 && sizeof(D3D12_INPUT_LAYOUT_DESC) == 16 && sizeof(D3D12_CACHED_PIPELINE_STATE) == 16, "D3D12 struct sizes (x64) differ from the pipeline-stream layout assumptions");
static bool StreamSubInfo(int type, size_t& size, size_t& align) {
    align = 4;
    switch (type) {
    case SUB_RS: size = 8; align = 8; return true;
    case SUB_VS: case SUB_PS: case SUB_DS: case SUB_HS: case SUB_GS: case SUB_CS: case SUB_AS: case SUB_MS: size = 16; align = 8; return true;
    case SUB_SO: size = 32; align = 8; return true;
    case SUB_BLEND: size = 328; return true;
    case SUB_SAMPLEMASK: size = 4; return true;
    case SUB_RASTER: case SUB_RASTER1: size = 44; return true;
    case SUB_RASTER2: size = 32; return true;
    case SUB_DEPTH: size = 52; return true;
    case SUB_DEPTH1: size = 56; return true;
    case SUB_DEPTH2: size = 60; return true;
    case SUB_INPUT: size = 16; align = 8; return true;
    case SUB_IBCUT: case SUB_TOPO: case SUB_DSFMT: case SUB_NODE: case SUB_FLAGS: size = 4; return true;
    case SUB_RTFMT: size = 36; return true;
    case SUB_SAMPLE: size = 8; return true;
    case SUB_CACHED: size = 16; align = 8; return true;
    case SUB_VIEWINST: size = 24; align = 8; return true;
    }
    return false;
}
static bool ParseStream(const uint8_t* base, size_t total, std::vector<SubInfo>& out) {
    if (!base || total < 8 || total > (1u << 20) || ((uintptr_t)base & 7)) return false;
    size_t off = 0;
    while (off + 4 <= total) {
        int type; memcpy(&type, base + off, 4); size_t sz, al; if (!StreamSubInfo(type, sz, al)) return false;
        size_t po = (4 + al - 1) & ~(al - 1); if (off + po + sz > total) return false;
        for (auto& e : out) if (e.type == type) return false;              // duplicate sub-object type = invalid stream, let the runtime report it
        out.push_back({ type, off + po, sz }); off = (off + po + sz + 7) & ~(size_t)7;
    }
    return !out.empty() && off >= total && off - total < 8;
}
template <class T> static T RdAt(const uint8_t* p, size_t o = 0) { T v; memcpy(&v, p + o, sizeof v); return v; }
static uint64_t HashStream(const uint8_t* base, const std::vector<SubInfo>& subs) {
    PsoHasher H; H.V((int)3);
    for (auto& s : subs) {
        const uint8_t* p = base + s.off; H.V(s.type);
        switch (s.type) {
        case SUB_RS: H.V(RsHashOf(RdAt<void*>(p))); break;
        case SUB_VS: case SUB_PS: case SUB_DS: case SUB_HS: case SUB_GS: case SUB_CS: case SUB_AS: case SUB_MS: H.Shader(RdAt<D3D12_SHADER_BYTECODE>(p)); break;
        case SUB_SO: { D3D12_STREAM_OUTPUT_DESC so = RdAt<D3D12_STREAM_OUTPUT_DESC>(p); H.V((UINT)so.NumEntries); H.V((UINT)so.NumStrides); H.V((UINT)so.RasterizedStream);
            if (so.pSODeclaration) for (UINT i = 0; i < so.NumEntries && i < 64; i++) { const D3D12_SO_DECLARATION_ENTRY& e = so.pSODeclaration[i]; H.V((UINT)e.Stream); if (e.SemanticName) H.Bytes(e.SemanticName, strlen(e.SemanticName)); H.V((UINT)e.SemanticIndex); H.V((int)e.StartComponent); H.V((int)e.ComponentCount); H.V((int)e.OutputSlot); }
            if (so.pBufferStrides) for (UINT i = 0; i < so.NumStrides && i < 4; i++) H.V((UINT)so.pBufferStrides[i]); break; }
        case SUB_BLEND: H.Blend(RdAt<D3D12_BLEND_DESC>(p)); break;
        case SUB_DEPTH: H.Depth(RdAt<D3D12_DEPTH_STENCIL_DESC>(p)); break;
        case SUB_DEPTH1: H.Depth(RdAt<D3D12_DEPTH_STENCIL_DESC>(p)); H.V(RdAt<int>(p, 52)); break;
        case SUB_DEPTH2: { H.V(RdAt<int>(p, 0)); H.V(RdAt<int>(p, 4)); H.V(RdAt<int>(p, 8)); H.V(RdAt<int>(p, 12));
            for (int f = 0; f < 2; f++) { size_t b = 16 + (size_t)f * 20; for (int k = 0; k < 4; k++) H.V(RdAt<int>(p, b + k * 4)); H.V((int)RdAt<uint8_t>(p, b + 16)); H.V((int)RdAt<uint8_t>(p, b + 17)); }
            H.V(RdAt<int>(p, 56)); break; }
        case SUB_INPUT: { D3D12_INPUT_LAYOUT_DESC il = RdAt<D3D12_INPUT_LAYOUT_DESC>(p); H.V((UINT)il.NumElements);
            if (il.pInputElementDescs) for (UINT i = 0; i < il.NumElements && i < 64; i++) { const D3D12_INPUT_ELEMENT_DESC& e = il.pInputElementDescs[i]; if (e.SemanticName) H.Bytes(e.SemanticName, strlen(e.SemanticName)); H.V((UINT)e.SemanticIndex); H.V((int)e.Format); H.V((UINT)e.InputSlot); H.V((UINT)e.AlignedByteOffset); H.V((int)e.InputSlotClass); H.V((UINT)e.InstanceDataStepRate); } break; }
        case SUB_VIEWINST: { UINT n = RdAt<UINT>(p, 0); const uint8_t* loc = RdAt<const uint8_t*>(p, 8); H.V(n); H.V(RdAt<int>(p, 16)); if (loc && n <= 4) H.Bytes(loc, (size_t)n * 8); break; }
        case SUB_CACHED: break;                                            // the blob itself is never part of the identity
        default: H.Bytes(p, s.size); break;
        }
    }
    return H.h ? H.h : 1;
}
// builds the stream with a cached blob: replaces an EMPTY cached sub-object or appends one. Returns false when the stream already carries a real blob.
static bool StreamWithBlob(const uint8_t* base, size_t total, const std::vector<SubInfo>& subs, const PsoBlobPtr& blob, std::vector<uint64_t>& out, size_t& outBytes) {
    const SubInfo* cached = nullptr; for (auto& s : subs) if (s.type == SUB_CACHED) cached = &s;
    if (cached && RdAt<SIZE_T>(base + cached->off, 8) != 0) return false;
    size_t aligned = (total + 7) & ~(size_t)7, add = cached ? 0 : 24;
    out.assign((aligned + add) / 8, 0); memcpy(out.data(), base, total);
    uint8_t* b = (uint8_t*)out.data(); size_t pOff;
    if (cached) pOff = cached->off; else { int t = SUB_CACHED; memcpy(b + aligned, &t, 4); pOff = aligned + 8; }
    const void* bp = blob->data(); SIZE_T bs = blob->size(); memcpy(b + pOff, &bp, 8); memcpy(b + pOff + 8, &bs, 8);
    outBytes = aligned + add; return true;
}
static bool StreamHasBlob(const uint8_t* base, const std::vector<SubInfo>& subs) { for (auto& s : subs) if (s.type == SUB_CACHED && RdAt<SIZE_T>(base + s.off, 8) != 0) return true; return false; }
static std::atomic<long> g_psoStreamCalls{ 0 }, g_psoStreamUnknown{ 0 };
