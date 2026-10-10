// hudcal.inc (v27) - hudless=2 : find 'hudlessdraw' automatically (and save it)
//
// The HUD-less copy of the back buffer has to be taken right after the last full-screen scene pass (tonemap / upscale / post process) and before the first UI draw.
// Which draw that is differs per game, so instead of asking the user to try 2,3,4,... the injector measures it once:
//   1. in a few 3D frames it copies one pixel of a coarse 16x9 grid out of the back buffer BEFORE each of the first 12 draws that target it (1x1 copies into a read-back buffer),
//   2. after the GPU is done (own fence on the game's queue) it compares neighbouring snapshots: a scene pass changes (nearly) the whole grid, UI draws only a few points,
//   3. the frame votes for N = "first draw after the full-screen run"; when >= 6 frames agree (>= 75 %) the result is applied, written to the config (hudless=1, hudlessdraw=N)
//      and the normal HUD-less capture takes over. If the frames disagree, nothing is changed (hudless stays off) and the table is in the log (lines 'hudless-auto:').
// Costs ~150 tiny copies on up to ~60 frames in the first minute, nothing afterwards. Needs: hudless=2, a 3D scene (camera found), back buffer format RGBA8 / BGRA8 / R10G10B10A2 / FP16.
#pragma once
static void DeferResRelease(ID3D12Resource* r);   // defined further down in main.cpp
static const int HC_M = 12, HC_GX = 16, HC_GY = 9, HC_K = HC_GX * HC_GY; static const UINT64 HC_SLOT = 512;   // 512 = D3D12 placement alignment: one sample per slot
struct HudCal {
    std::atomic<int> phase{ 0 };        // 0 idle, 1 sampling the current frame, 2 waiting for the GPU, 3 done (applied), 4 done (gave up / unsupported)
    std::atomic<int> frame{ -1 };       // g_frame value while the sampled draws are recorded
    std::atomic<unsigned> got{ 0 };     // bit i = slot i recorded
    ID3D12Resource* rb = nullptr; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; int cls = 0; UINT bw = 0, bh = 0; int kind[HC_M] = {}; UINT count[HC_M] = {};
    ID3D12Fence* fence = nullptr; UINT64 fv = 0; int armedAt = 0, waitSince = 0;
    std::vector<int> votes; int attempts = 0, inconclusive = 0;
    std::mutex mx;
} g_hc;
static void HudCalReset() { std::lock_guard<std::mutex> lk(g_hc.mx); if (g_hc.rb) { DeferResRelease(g_hc.rb); g_hc.rb = nullptr; } g_hc.phase = 0; g_hc.votes.clear(); g_hc.attempts = 0; g_hc.inconclusive = 0; g_hc.armedAt = 0; g_hc.got = 0; }
static int HudCalVotes() { return (int)g_hc.votes.size(); }
static bool HudCalFormat(DXGI_FORMAT f, DXGI_FORMAT* typed, int* cls) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: *typed = DXGI_FORMAT_R8G8B8A8_UNORM; *cls = SN_PIX_U8x4; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: *typed = DXGI_FORMAT_B8G8R8A8_UNORM; *cls = SN_PIX_U8x4; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: *typed = DXGI_FORMAT_R10G10B10A2_UNORM; *cls = SN_PIX_R10G10B10A2; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: *typed = DXGI_FORMAT_R16G16B16A16_FLOAT; *cls = SN_PIX_F16x4; return true;
    default: return false; }
}
// camera + 3D scene present right now?
static bool HudCalSceneOk(int fr) { int cf = g_camFrame.load(); return g_pinDepth && cf >= 0 && fr - cf <= 5; }
static void HudCalFinish(int phase, int n, int agree) {
    g_hc.phase = phase;
    if (phase == 3) {
        g_cfg.hudlessDraw = n; g_cfg.hudless = 1; CfgSetInt("hudless", 1); CfgSetInt("hudlessdraw", n);
        Log("hudless-auto: RESULT hudlessdraw=%d (%d of %d frames agree, %d frames sampled) -> HUD-less capture ON and saved to the config", n, agree, HudCalVotes(), g_hc.attempts);
    } else Log("hudless-auto: no clear result after %d sampled frames (%d conclusive, votes %d) -> hudless stays OFF. Set hudless=1 and hudlessdraw=N by hand (see the table lines above) and send the log if you want this game supported.", g_hc.attempts, HudCalVotes(), HudCalVotes());
}
static void HudCalEvaluate() {
    std::lock_guard<std::mutex> lk(g_hc.mx); if (!g_hc.rb) return;
    unsigned got = g_hc.got.load(); int m = 0; while (m < HC_M && (got & (1u << m))) m++;
    g_hc.attempts++;
    if (m >= 5) {
        D3D12_RANGE rr = { 0, (SIZE_T)(HC_SLOT * HC_K * HC_M) }; uint8_t* p = nullptr;
        if (SUCCEEDED(g_hc.rb->Map(0, &rr, (void**)&p)) && p) {
            double ch[HC_M] = {}; for (int i = 0; i + 1 < m; i++) ch[i] = SnChangedFraction(g_hc.cls, p + (size_t)i * HC_K * HC_SLOT, p + (size_t)(i + 1) * HC_K * HC_SLOT, HC_K, (size_t)HC_SLOT);
            D3D12_RANGE none = { 0, 0 }; g_hc.rb->Unmap(0, &none);
            int n = SnHudDecideFrame(ch, m - 1); std::string t; for (int i = 0; i + 1 < m; i++) t += Fmt(" %d:%.2f%s", i + 1, ch[i], g_hc.kind[i] == 0 ? "" : "");
            std::string dr; for (int i = 0; i < m; i++) dr += Fmt(" #%d=%s(%u)", i + 1, g_hc.kind[i] == 1 ? "DI" : "DII", g_hc.count[i]);
            Log("hudless-auto: frame sample %d: changed fraction per draw [%s ]  draws[%s ] -> N=%d%s", g_hc.attempts, t.c_str(), dr.c_str(), n, n ? "" : " (inconclusive)");
            if (n > 0) g_hc.votes.push_back(n); else g_hc.inconclusive++;
        } else { Log("hudless-auto: Map failed"); g_hc.inconclusive++; }
    } else { g_hc.inconclusive++; if (g_hc.attempts <= 3) Log("hudless-auto: frame sample %d: only %d back-buffer draws recorded (need 5+)", g_hc.attempts, m); }
    DeferResRelease(g_hc.rb); g_hc.rb = nullptr; g_hc.got = 0;
    int agree = 0; int res = SnHudVote(g_hc.votes, 6, 0.75, &agree);
    if (res > 0) { HudCalFinish(3, res, agree); return; }
    if (g_hc.attempts >= 60 || (int)g_hc.votes.size() >= 16) { HudCalFinish(4, 0, 0); return; }
    g_hc.phase = 0;
}
// called once per Present (after the draw counter was reset)
static void HudCalOnPresent(int fr) {
    if (g_cfg.hudless != 2 || g_hc.phase.load() >= 3 || !g_device || !g_queue || !g_dispW) return;
    int ph = g_hc.phase.load();
    if (ph == 1 && fr == g_hc.frame.load() + 1) {   // the sampled frame is over: ask the queue to signal when everything recorded so far has finished
        if (!g_hc.fence && FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_hc.fence))) { g_hc.phase = 4; Log("hudless-auto: CreateFence failed -> off"); return; }
        if (!g_hc.got.load() || !g_hc.rb) { std::lock_guard<std::mutex> lk(g_hc.mx); if (g_hc.rb) { DeferResRelease(g_hc.rb); g_hc.rb = nullptr; } g_hc.attempts++; g_hc.inconclusive++; g_hc.phase = 0; return; }
        g_hc.fv++; g_queue->Signal(g_hc.fence, g_hc.fv); g_hc.phase = 2; g_hc.waitSince = fr; return;
    }
    if (ph == 2) { if (g_hc.fence->GetCompletedValue() >= g_hc.fv) HudCalEvaluate(); else if (fr - g_hc.waitSince > 180) { Log("hudless-auto: GPU fence timeout"); HudCalReset(); g_hc.phase = 4; } return; }
    if (ph == 0 && fr > g_cfg.warmup + 30 && fr - g_hc.armedAt >= 7 && HudCalSceneOk(fr)) { g_hc.armedAt = fr; g_hc.got = 0; g_hc.frame = fr; g_hc.phase = 1; }
}
// called from NoteDraw for every draw that targets the back buffer (n = 1-based number of the draw in this frame)
static void HudCalOnDraw(ID3D12GraphicsCommandList* cl, ID3D12Resource* bb, int n, int kind, UINT count) {
    if (g_hc.phase.load() != 1 || n < 1 || n > HC_M || g_frame.load() != g_hc.frame.load() || !g_device) return;
    { std::lock_guard<std::mutex> lk(g_hc.mx);
      if (!g_hc.rb) {
          D3D12_RESOURCE_DESC d = ResDesc(bb); DXGI_FORMAT tf; int cls;
          if (!HudCalFormat(d.Format, &tf, &cls)) { Log("hudless-auto: back buffer format %d is not supported -> off", (int)d.Format); g_hc.phase = 4; return; }
          D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = HC_SLOT * HC_K * HC_M; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
          ID3D12Resource* r = nullptr; HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&r);
          if (FAILED(hr) || !r) { Log("hudless-auto: read-back buffer failed 0x%08X -> off", (unsigned)hr); g_hc.phase = 4; return; }
          g_hc.rb = r; g_hc.fmt = tf; g_hc.cls = cls; g_hc.bw = (UINT)d.Width; g_hc.bh = d.Height;
      } }
    int slot = n - 1; g_hc.kind[slot] = kind; g_hc.count[slot] = count;
    TransitionOne(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (int gy = 0; gy < HC_GY; gy++) for (int gx = 0; gx < HC_GX; gx++) {
        UINT x = (UINT)(((UINT64)(2 * gx + 1) * g_hc.bw) / (2 * HC_GX)), y = (UINT)(((UINT64)(2 * gy + 1) * g_hc.bh) / (2 * HC_GY));
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {}; dst.pResource = g_hc.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = ((UINT64)slot * HC_K + (UINT64)(gy * HC_GX + gx)) * HC_SLOT; dst.PlacedFootprint.Footprint.Format = g_hc.fmt; dst.PlacedFootprint.Footprint.Width = 1; dst.PlacedFootprint.Footprint.Height = 1; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
        src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0; D3D12_BOX bx = { x, y, 0, x + 1, y + 1, 1 };
        oCopyTex(cl, &dst, 0, 0, 0, &src, &bx);
    }
    TransitionOne(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_hc.got.fetch_or(1u << slot);
}
