// holo.h - "system hologram" (v28): a small rotating neural-network picture of the injector's subsystems.
//   * pure C++ (no Windows / D3D) so tests/test_holo.cpp can render it to a PNG on any machine
//   * 12 subsystem nodes on the outer shell, a few mid / inner neurons, curved synapses, a few faint glass shards, travelling pulses
//   * healthy = calm blue, warning = amber, error = red (node + its path to the core, held >= 1.5 s so it can be seen), off = gray
//   * LIGHT by design: no blur passes, no full-screen buffers; ~100 curved edges drawn as short anti-aliased segments straight into the overlay's
//     panel bitmap (premultiplied BGRA). Only runs while the hologram page of the menu is open. Glow is a single faint halo on nodes only.
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

namespace holo {

enum Status { ST_OK = 0, ST_WARN = 1, ST_ERR = 2, ST_OFF = 3 };
enum Node { N_GPU, N_DRIVER, N_SL, N_D3D12, N_SWAP, N_REFLEX, N_CAMERA, N_DEPTH, N_VEL, N_MV, N_DLSSG, N_OVERLAY, N_COUNT };
static const char* const kNodeName[N_COUNT] = { "GPU", "Driver", "Streamline", "D3D12", "Swap chain", "Reflex", "Camera", "Depth", "Velocity", "MV pass", "DLSS-G", "Overlay" };

struct Canvas { uint32_t* px; int stride; int w, h; };           // premultiplied BGRA, (0,0) = top-left of the hologram area (already offset by the caller)
struct NodePos { float x, y, z, r; };                             // projected centre (pixels inside the area), depth (-1 back .. +1 front), radius

struct Vec3 { float x, y, z; };
struct Edge { int a, b; int owner; float bend; float ox, oy, oz; };   // owner = subsystem whose path this is (-1 = background synapse)
struct Model {
    std::vector<Vec3> p; std::vector<int> layer;                  // points: 0..11 subsystem nodes, 12 = core, then mid (layer 1) / inner (layer 2) neurons
    std::vector<Edge> e; std::vector<std::vector<int>> shards;    // shards = index lists into p2 (extra points)
    std::vector<Vec3> sp;                                         // shard vertices
    int quality = -1;
} g_model;

struct State { int st[N_COUNT] = {}; double errUntil[N_COUNT] = {}; int shown[N_COUNT] = {}; double t0 = -1; float brain = 0.f; int brainMode = 0; } g_state;   // brain: 0..1 activity, brainMode: 0 off / 1 adapting / 2 safe (the core is the brain)

static uint32_t Rng(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static float Rf(uint32_t& s) { return (float)(Rng(s) & 0xFFFFFF) / 16777216.f; }
static float Len(Vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static Vec3 Norm(Vec3 a) { float l = Len(a); if (l < 1e-6f) return { 0, 0, 1 }; return { a.x / l, a.y / l, a.z / l }; }
static float D2(Vec3 a, Vec3 b) { float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return x * x + y * y + z * z; }

// quality 0 = low (about 40 edges), 1 = medium, 2 = high
static void Build(int quality) {
    Model& M = g_model; M = Model(); M.quality = quality; uint32_t s = 0x9E3779B1u;
    const int nMid = quality == 0 ? 10 : quality == 1 ? 18 : 30, nIn = quality == 0 ? 4 : 6;
    // 12 subsystem nodes: fibonacci sphere, slightly jittered, radius ~0.88 (the order of the enum is mixed on the sphere so that neighbours are unrelated)
    static const int order[N_COUNT] = { 0, 7, 3, 10, 5, 1, 8, 4, 11, 2, 9, 6 };
    for (int i = 0; i < N_COUNT; i++) {
        float k = (float)order[i] + 0.5f, y = 1.f - 2.f * k / (float)N_COUNT, r = sqrtf(std::max(0.f, 1.f - y * y)), a = k * 2.399963f;
        float j = 0.86f + 0.10f * Rf(s); M.p.push_back({ cosf(a) * r * j, y * j * 0.92f, sinf(a) * r * j }); M.layer.push_back(0);
    }
    M.p.push_back({ 0, 0, 0 }); M.layer.push_back(2);                                  // 12 = core
    for (int i = 0; i < nMid + nIn; i++) {                                              // mid / inner neurons
        bool in = i >= nMid; float R = in ? 0.14f + 0.14f * Rf(s) : 0.38f + 0.26f * Rf(s);
        Vec3 d = Norm({ Rf(s) * 2.f - 1.f, (Rf(s) * 2.f - 1.f) * 0.9f, Rf(s) * 2.f - 1.f }); M.p.push_back({ d.x * R, d.y * R, d.z * R }); M.layer.push_back(in ? 2 : 1);
    }
    const int core = N_COUNT, first = N_COUNT + 1, total = (int)M.p.size();
    auto nearest = [&](int from, int layer, int skip) { int best = -1; float bd = 1e9f; for (int j = first; j < total; j++) { if (M.layer[j] != layer || j == skip) continue; float d = D2(M.p[from], M.p[j]); if (d < bd) { bd = d; best = j; } } return best; };
    auto add = [&](int a, int b, int owner, float bend) { if (a < 0 || b < 0 || a == b) return; M.e.push_back({ a, b, owner, bend, Rf(s) - .5f, Rf(s) - .5f, Rf(s) - .5f }); };
    for (int i = 0; i < N_COUNT; i++) {                                                 // the path of every subsystem: node -> mid -> inner -> core
        int m = nearest(i, 1, -1), n = nearest(m >= 0 ? m : i, 2, -1);
        add(i, m, i, 0.30f); add(m, n, i, 0.22f); add(n, core, i, 0.12f);
        int m2 = nearest(i, 1, m); if (quality >= 1) add(i, m2, -1, 0.30f);             // a second, decorative synapse
    }
    for (int j = first; j < total; j++) if (M.layer[j] == 1 && quality >= 1) {   // mid <-> mid: the two nearest neighbours (medium), plus one link to the shell (high)
        int o1 = nearest(j, 1, j), o2 = nearest(j, 1, o1 >= 0 ? o1 : j); if (o1 > j) add(j, o1, -1, 0.25f); if (o2 > j) add(j, o2, -1, 0.25f);
        if (quality >= 2) { int sn = (int)(Rf(s) * N_COUNT) % N_COUNT; add(j, sn, -1, 0.30f); } }
    for (int j = first; j < total; j++) if (M.layer[j] == 2) { int o = nearest(j, 2, j); if (o > j) add(j, o, -1, 0.18f); add(j, core, -1, 0.12f); }
    if (quality >= 1) for (int i = 0; i < N_COUNT; i += 3) add(i, (i + 5) % N_COUNT, -1, 0.40f);   // a few long arcs between subsystems on the shell
    // faint glass shards (outline only): 4 / 5 / 7 small polygons just outside the shell
    int nsh = quality == 0 ? 3 : quality == 1 ? 5 : 7;
    for (int i = 0; i < nsh; i++) {
        Vec3 c = Norm({ Rf(s) * 2.f - 1.f, Rf(s) * 2.f - 1.f, Rf(s) * 2.f - 1.f }); float R = 0.95f + 0.2f * Rf(s); std::vector<int> idx;
        Vec3 u = Norm({ -c.z, 0.f, c.x }), v = { c.y * u.z - c.z * u.y, c.z * u.x - c.x * u.z, c.x * u.y - c.y * u.x }; int nv = 3 + (int)(Rf(s) * 2.f); float rot = Rf(s) * 6.28f;
        for (int k = 0; k < nv; k++) { float a = rot + k * 6.2832f / nv, rr = (0.10f + 0.12f * Rf(s)) * (1.f + 0.4f * Rf(s)); M.sp.push_back({ c.x * R + (u.x * cosf(a) + v.x * sinf(a)) * rr, c.y * R + (u.y * cosf(a) + v.y * sinf(a)) * rr, c.z * R + (u.z * cosf(a) + v.z * sinf(a)) * rr }); idx.push_back((int)M.sp.size() - 1); }
        M.shards.push_back(idx);
    }
}

// --------------------------------------------------------------- status handling
// in[]: current status per subsystem (ST_*). An error is kept visible for 1.6 s after it was last seen so a one-frame glitch can be noticed.
static void Update(const int in[N_COUNT], double nowMs) {
    State& S = g_state; if (S.t0 < 0) S.t0 = nowMs;
    for (int i = 0; i < N_COUNT; i++) { S.st[i] = in[i]; if (in[i] == ST_ERR) S.errUntil[i] = nowMs + 1600.0; S.shown[i] = (in[i] == ST_ERR || S.errUntil[i] > nowMs) ? ST_ERR : in[i]; }
}

// --------------------------------------------------------------- tiny software rasteriser (premultiplied BGRA "over")
static inline void Put(const Canvas& c, int x, int y, int r, int g, int b, float a) {
    if (x < 0 || y < 0 || x >= c.w || y >= c.h || a <= 0.004f) return; if (a > 1.f) a = 1.f;
    uint32_t* d = &c.px[(size_t)y * c.stride + x]; uint32_t o = *d; float ia = 1.f - a;
    int na = (int)(((o >> 24) & 255) * ia + 255.f * a + .5f), nr = (int)(((o >> 16) & 255) * ia + r * a + .5f), ng = (int)(((o >> 8) & 255) * ia + g * a + .5f), nb = (int)((o & 255) * ia + b * a + .5f);
    *d = ((uint32_t)std::min(na, 255) << 24) | ((uint32_t)std::min(nr, 255) << 16) | ((uint32_t)std::min(ng, 255) << 8) | (uint32_t)std::min(nb, 255);
}
static void Line(const Canvas& c, float x0, float y0, float x1, float y1, float th, int r, int g, int b, float a) {
    int bx0 = (int)floorf(std::min(x0, x1) - th) - 1, by0 = (int)floorf(std::min(y0, y1) - th) - 1, bx1 = (int)ceilf(std::max(x0, x1) + th) + 1, by1 = (int)ceilf(std::max(y0, y1) + th) + 1;
    bx0 = std::max(bx0, 0); by0 = std::max(by0, 0); bx1 = std::min(bx1, c.w); by1 = std::min(by1, c.h);
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float px = x + .5f - x0, py = y + .5f - y0, t = l2 > 0.f ? std::min(1.f, std::max(0.f, (px * dx + py * dy) / l2)) : 0.f, ex = px - t * dx, ey = py - t * dy;
        float cv = th * .5f + .5f - sqrtf(ex * ex + ey * ey); if (cv <= 0.f) continue; Put(c, x, y, r, g, b, a * std::min(1.f, cv));
    }
}
static void Disc(const Canvas& c, float cx, float cy, float rad, int r, int g, int b, float a) {
    int x0 = std::max(0, (int)floorf(cx - rad) - 1), y0 = std::max(0, (int)floorf(cy - rad) - 1), x1 = std::min(c.w, (int)ceilf(cx + rad) + 1), y1 = std::min(c.h, (int)ceilf(cy + rad) + 1);
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { float dx = x + .5f - cx, dy = y + .5f - cy, cv = rad + .5f - sqrtf(dx * dx + dy * dy); if (cv > 0.f) Put(c, x, y, r, g, b, a * std::min(1.f, cv)); }
}
static void Halo(const Canvas& c, float cx, float cy, float rad, int r, int g, int b, float a) {   // one faint soft disc (the only "glow" in the whole picture)
    int x0 = std::max(0, (int)floorf(cx - rad)), y0 = std::max(0, (int)floorf(cy - rad)), x1 = std::min(c.w, (int)ceilf(cx + rad) + 1), y1 = std::min(c.h, (int)ceilf(cy + rad) + 1);
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { float dx = x + .5f - cx, dy = y + .5f - cy, k = 1.f - sqrtf(dx * dx + dy * dy) / rad; if (k > 0.f) Put(c, x, y, r, g, b, a * k * k); }
}

static void Col(int st, int& r, int& g, int& b) {
    switch (st) { case ST_ERR: r = 255; g = 96; b = 92; break; case ST_WARN: r = 244; g = 190; b = 70; break; case ST_OFF: r = 128; g = 136; b = 154; break; default: r = 104; g = 172; b = 255; }
}

// --------------------------------------------------------------- render
// Draws into the whole canvas (the caller clears / provides the glass background). 'sel' = selected subsystem (-1 none). 'pos' receives the projected node centres for the labels.
// The picture is a unit sphere scaled to fit min(w, h); it turns slowly around the vertical axis and tilts a little.
static void Render(const Canvas& c, double nowMs, int sel, int quality, NodePos pos[N_COUNT]) {
    if (g_model.quality != quality) Build(quality);
    const Model& M = g_model; const State& S = g_state;
    const double sec = (nowMs - (S.t0 < 0 ? nowMs : S.t0)) * 0.001;
    const float ang = (float)(sec * 0.22), ca = cosf(ang), sa = sinf(ang), tilt = 0.30f, ct = cosf(tilt), st_ = sinf(tilt);
    const float R = std::min(c.w, c.h) * 0.40f, cx = c.w * 0.5f, cy = c.h * 0.5f, persp = 3.2f;
    auto proj = [&](Vec3 v, float& x, float& y, float& z) {
        float rx = v.x * ca + v.z * sa, rz = -v.x * sa + v.z * ca, ry = v.y; float y2 = ry * ct - rz * st_, z2 = ry * st_ + rz * ct;
        float k = persp / (persp - z2 * 0.9f); x = cx + rx * R * k; y = cy + y2 * R * k; z = z2; };
    const int total = (int)M.p.size(); std::vector<float> X(total), Y(total), Z(total);
    for (int i = 0; i < total; i++) proj(M.p[i], X[i], Y[i], Z[i]);
    auto depthA = [](float z) { return 0.45f + 0.55f * std::min(1.f, std::max(0.f, (z + 1.1f) / 2.2f)); };
    // glass shards (outline only, very faint)
    { size_t base = 0; for (auto& sh : M.shards) { size_t n = sh.size(); std::vector<float> sx(n), sy(n), sz(n); for (size_t k = 0; k < n; k++) proj(M.sp[sh[k]], sx[k], sy[k], sz[k]);
        for (size_t k = 0; k < n; k++) { size_t k2 = (k + 1) % n; Line(c, sx[k], sy[k], sx[k2], sy[k2], 1.f, 120, 176, 240, 0.20f * depthA((sz[k] + sz[k2]) * .5f)); } base += n; } (void)base; }
    // synapses: quadratic curves, 7 segments each
    const int SEG = M.quality == 0 ? 5 : 7;
    for (size_t ei = 0; ei < M.e.size(); ei++) {
        const Edge& e = M.e[ei]; Vec3 a = M.p[e.a], b = M.p[e.b]; Vec3 m = { (a.x + b.x) * .5f, (a.y + b.y) * .5f, (a.z + b.z) * .5f }; float L = sqrtf(D2(a, b));
        Vec3 ctrl = { m.x + e.ox * e.bend * L * 2.f, m.y + e.oy * e.bend * L * 2.f, m.z + e.oz * e.bend * L * 2.f };
        int stt = e.owner >= 0 ? S.shown[e.owner] : ST_OK; bool err = stt == ST_ERR, sel_ = e.owner >= 0 && e.owner == sel; int cr, cg, cb; Col(err ? ST_ERR : stt == ST_WARN ? ST_WARN : ST_OK, cr, cg, cb);
        if (e.owner >= 0 && stt == ST_OFF) Col(ST_OFF, cr, cg, cb);
        float px = 0, py = 0, pz = 0; bool have = false;
        for (int k = 0; k <= SEG; k++) {
            float t = (float)k / SEG, u = 1.f - t; Vec3 q = { u * u * a.x + 2 * u * t * ctrl.x + t * t * b.x, u * u * a.y + 2 * u * t * ctrl.y + t * t * b.y, u * u * a.z + 2 * u * t * ctrl.z + t * t * b.z };
            float x, y, z; proj(q, x, y, z);
            if (have) { float al = (e.owner >= 0 ? 0.70f : 0.38f) * depthA((z + pz) * .5f); if (err) al = 0.95f; else if (sel_) al = 0.9f; Line(c, px, py, x, y, (err || sel_) ? 1.7f : 1.f, cr, cg, cb, al); }
            px = x; py = y; pz = z; have = true;
        }
    }
    // pulses along each subsystem path (a small bright dot; faster + red on error, none when off)
    for (int i = 0; i < N_COUNT; i++) {
        if (S.shown[i] == ST_OFF) continue; bool err = S.shown[i] == ST_ERR; int cr, cg, cb; Col(S.shown[i], cr, cg, cb);
        // find the first edge of this subsystem path (node -> mid) and the others in order of creation
        float speed = err ? 1.1f : 0.28f; float ph = (float)fmod(sec * speed + i * 0.173, 1.0) * 3.f; int seg = (int)ph; float t = ph - seg; int cnt = 0;
        for (const Edge& e : M.e) { if (e.owner != i) continue; if (cnt++ != seg) continue;
            Vec3 a = M.p[e.a], b = M.p[e.b]; Vec3 m = { (a.x + b.x) * .5f, (a.y + b.y) * .5f, (a.z + b.z) * .5f }; float L = sqrtf(D2(a, b)); Vec3 ctrl = { m.x + e.ox * e.bend * L * 2.f, m.y + e.oy * e.bend * L * 2.f, m.z + e.oz * e.bend * L * 2.f }; float u = 1.f - t;
            Vec3 q = { u * u * a.x + 2 * u * t * ctrl.x + t * t * b.x, u * u * a.y + 2 * u * t * ctrl.y + t * t * b.y, u * u * a.z + 2 * u * t * ctrl.z + t * t * b.z }; float x, y, z; proj(q, x, y, z);
            Disc(c, x, y, 1.9f, std::min(255, cr + 60), std::min(255, cg + 50), std::min(255, cb + 40), 0.95f * depthA(z)); break; }
    }
    // neurons (small dots) then the core, then the subsystem nodes on top
    for (int i = N_COUNT + 1; i < total; i++) Disc(c, X[i], Y[i], M.layer[i] == 1 ? 1.6f : 1.3f, 150, 200, 255, 0.55f * depthA(Z[i]));
    { const float ba = std::min(1.f, std::max(0.f, S.brain)), bp = 0.5f + 0.5f * sinf((float)sec * (2.0f + 5.0f * ba));   // the core breathes calmly; faster and wider while the brain is actively adapting
      const float haA = S.brainMode == 0 ? 0.07f : S.brainMode == 2 ? 0.12f : 0.16f + 0.14f * ba * bp, haR = R * (0.16f + (S.brainMode == 1 ? 0.05f * ba * bp : 0.f));
      Halo(c, X[N_COUNT], Y[N_COUNT], haR, S.brainMode == 2 ? 244 : 110, S.brainMode == 2 ? 190 : 175, S.brainMode == 2 ? 70 : 255, haA); Disc(c, X[N_COUNT], Y[N_COUNT], 3.2f, 205, 228, 255, 0.95f); Disc(c, X[N_COUNT], Y[N_COUNT], 1.6f, 255, 255, 255, 0.95f); }
    for (int i = 0; i < N_COUNT; i++) {
        int stt = S.shown[i]; int cr, cg, cb; Col(stt, cr, cg, cb); float rad = (i == sel ? 4.6f : 3.6f) * (0.85f + 0.2f * depthA(Z[i])), al = depthA(Z[i]);
        if (stt == ST_ERR) { float pulse = 0.5f + 0.5f * sinf((float)sec * 7.f); Halo(c, X[i], Y[i], rad * 4.0f, cr, cg, cb, 0.22f + 0.10f * pulse); rad *= 1.15f; }
        else if (stt != ST_OFF) Halo(c, X[i], Y[i], rad * 2.6f, cr, cg, cb, 0.10f);
        Disc(c, X[i], Y[i], rad, cr, cg, cb, std::min(1.f, al + 0.1f));
        if (stt != ST_ERR) Disc(c, X[i], Y[i], rad * 0.45f, 235, 244, 255, 0.85f * al);
        if (i == sel) { for (int k = 0; k < 24; k++) { float a0 = k * 0.2618f, a1 = (k + 1) * 0.2618f, rr = rad + 4.5f; if (k & 1) continue; Line(c, X[i] + cosf(a0) * rr, Y[i] + sinf(a0) * rr, X[i] + cosf(a1) * rr, Y[i] + sinf(a1) * rr, 1.2f, 235, 244, 255, 0.9f); } }
        if (pos) pos[i] = { X[i], Y[i], Z[i], rad };
    }
}

}   // namespace holo
