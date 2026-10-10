// pso_hooks.inc (v27, split out of main.cpp unchanged): device hooks for CreateGraphicsPipelineState / CreateComputePipelineState / state streams / CreateRootSignature
// (PSO failure diagnostics + retry without CachedPSO, and the v26 persistent pipeline cache glue). Needs psocache.inc (included right before it).
#pragma once
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
// v26: CreateRootSignature hook = identity of the root signature for the pipeline cache key
using PFN_CreateRS = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static PFN_CreateRS oCreateRS;
static HRESULT STDMETHODCALLTYPE hkCreateRS(ID3D12Device* dev, UINT nm, const void* blob, SIZE_T len, REFIID riid, void** ppv) {
    HRESULT hr = oCreateRS(dev, nm, blob, len, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) NoteRootSig(*ppv, blob, len);
    return hr;
}
// errors a stale / foreign cached blob can cause: E_INVALIDARG, D3D12_ERROR_ADAPTER_NOT_FOUND (0x887E0001), D3D12_ERROR_DRIVER_VERSION_MISMATCH (0x887E0002)
static inline bool PsoRetryable(HRESULT hr) { return hr == E_INVALIDARG || hr == (HRESULT)0x887E0001 || hr == (HRESULT)0x887E0002; }
static inline bool PsoCacheable(REFIID riid, void** ppv) { return ppv && IsEqualGUID(riid, __uuidof(ID3D12PipelineState)); }

static HRESULT STDMETHODCALLTYPE hkCreateGPSO(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    if (!desc) return oCreateGPSO(dev, desc, riid, ppv);
    // v26 persistent pipeline cache: known pipeline -> give the driver its cached blob (no shader compile); unknown -> compile as usual, then remember it
    const bool cache = g_cfg.psoCache && PsoCacheable(riid, ppv) && !desc->CachedPSO.CachedBlobSizeInBytes;   // pipelines that already carry the game's own blob are left alone
    uint64_t key = 0; bool hit = false;
    if (cache) {
        PsoWaitReady(); g_psoSeen++; key = HashGfxDesc(desc); PsoBlobPtr blob = PsoFind(key);
        if (blob) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cp = *desc; cp.CachedPSO.pCachedBlob = blob->data(); cp.CachedPSO.CachedBlobSizeInBytes = blob->size();
            HRESULT h2 = oCreateGPSO(dev, &cp, riid, ppv);
            if (SUCCEEDED(h2)) { g_psoHit++; return h2; }
            long nr = ++g_psoRejected; PsoErase(key); if (nr <= 8) Log("pso-cache: driver rejected a cached graphics pipeline (hr=0x%08X) -> entry dropped, compiling normally", (unsigned)h2);
        }
    }
    HRESULT hr = oCreateGPSO(dev, desc, riid, ppv);
    if (SUCCEEDED(hr)) { if (cache && !hit && ppv && *ppv) PsoStore(key, (ID3D12PipelineState*)*ppv); return hr; }
    if (!PsoRetryable(hr) || !g_cfg.psoRetry) return hr;
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
    if (!desc) return oCreateCPSO(dev, desc, riid, ppv);
    const bool cache = g_cfg.psoCache && PsoCacheable(riid, ppv) && !desc->CachedPSO.CachedBlobSizeInBytes;
    uint64_t key = 0;
    if (cache) {
        PsoWaitReady(); g_psoSeen++; key = HashCompDesc(desc); PsoBlobPtr blob = PsoFind(key);
        if (blob) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC cp = *desc; cp.CachedPSO.pCachedBlob = blob->data(); cp.CachedPSO.CachedBlobSizeInBytes = blob->size();
            HRESULT h2 = oCreateCPSO(dev, &cp, riid, ppv);
            if (SUCCEEDED(h2)) { g_psoHit++; return h2; }
            long nr = ++g_psoRejected; PsoErase(key); if (nr <= 8) Log("pso-cache: driver rejected a cached compute pipeline (hr=0x%08X) -> entry dropped, compiling normally", (unsigned)h2);
        }
    }
    HRESULT hr = oCreateCPSO(dev, desc, riid, ppv);
    if (SUCCEEDED(hr)) { if (cache && ppv && *ppv) PsoStore(key, (ID3D12PipelineState*)*ppv); return hr; }
    if (!PsoRetryable(hr) || !g_cfg.psoRetry) return hr;
    int n = ++g_psoFail;
    if (n <= 16) { Log("PSO FAIL #%d (compute, hr=0x%08X) tid=%lu | deviceRemoved=0x%08X | rootSig=%p CS=%zu cached=%zu", n, (unsigned)hr, (unsigned long)GetCurrentThreadId(), (unsigned)dev->GetDeviceRemovedReason(), (void*)desc->pRootSignature, desc->CS.BytecodeLength, desc->CachedPSO.CachedBlobSizeInBytes); }
    if (desc->CachedPSO.CachedBlobSizeInBytes) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cp = *desc; cp.CachedPSO.pCachedBlob = nullptr; cp.CachedPSO.CachedBlobSizeInBytes = 0;
        HRESULT hr2 = oCreateCPSO(dev, &cp, riid, ppv);
        if (n <= 16) Log("PSO FAIL #%d: retry without CachedPSO -> hr=0x%08X", n, (unsigned)hr2);
        if (SUCCEEDED(hr2)) { g_psoRescued++; return hr2; }
    }
    if (g_log) fflush(g_log);
    return hr;
}

// ---- v26: ID3D12Device2::CreatePipelineState (pipeline state STREAM) - same cache, stream flavour
using PFN_CreatePSS = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const SnStreamDesc*, REFIID, void**);
static PFN_CreatePSS oCreatePSS;
static HRESULT STDMETHODCALLTYPE hkCreatePSS(ID3D12Device* dev, const SnStreamDesc* desc, REFIID riid, void** ppv) {
    if (!desc || !g_cfg.psoCache || !PsoCacheable(riid, ppv)) return oCreatePSS(dev, desc, riid, ppv);
    const uint8_t* base = (const uint8_t*)desc->pPipelineStateSubobjectStream; size_t total = (size_t)desc->SizeInBytes;
    std::vector<SubInfo> subs;
    long nc = ++g_psoStreamCalls; if (nc == 1) Log("pso-cache: this game creates pipelines with CreatePipelineState (state stream) - cached as well");
    if (!ParseStream(base, total, subs) || StreamHasBlob(base, subs)) { long u = ++g_psoStreamUnknown; if (u <= 3) Log("pso-cache: a pipeline stream was not cacheable (unknown sub-object type, or it already carries the game's own blob) - passed through unchanged"); return oCreatePSS(dev, desc, riid, ppv); }
    PsoWaitReady(); g_psoSeen++;
    uint64_t key = HashStream(base, subs); PsoBlobPtr blob = PsoFind(key);
    if (blob) {
        std::vector<uint64_t> buf; size_t nb = 0;
        if (StreamWithBlob(base, total, subs, blob, buf, nb)) {
            SnStreamDesc d2 = { nb, buf.data() };
            HRESULT h2 = oCreatePSS(dev, &d2, riid, ppv);
            if (SUCCEEDED(h2)) { g_psoHit++; return h2; }
            long nr = ++g_psoRejected; PsoErase(key); if (nr <= 8) Log("pso-cache: driver rejected a cached stream pipeline (hr=0x%08X) -> entry dropped, compiling normally", (unsigned)h2);
        }
    }
    HRESULT hr = oCreatePSS(dev, desc, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) PsoStore(key, (ID3D12PipelineState*)*ppv);
    return hr;
}

