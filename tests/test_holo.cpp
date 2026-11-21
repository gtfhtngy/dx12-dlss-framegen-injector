// test_holo.cpp - renders the hologram (src/holo.h) to PPM files and checks basic invariants. usage: test_holo [outdir]
#include "../src/holo.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
using namespace holo;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
static void Dump(const char* path, const std::vector<uint32_t>& px, int w, int h) {
    FILE* f = fopen(path, "wb"); if (!f) return; fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint32_t p = px[i]; unsigned a = p >> 24; unsigned bg[3] = { 12, 16, 26 };   // composite over a dark glass colour
        unsigned r = ((p >> 16) & 255) + bg[0] * (255 - a) / 255, g = ((p >> 8) & 255) + bg[1] * (255 - a) / 255, b = (p & 255) + bg[2] * (255 - a) / 255; fputc(std::min(r, 255u), f); fputc(std::min(g, 255u), f); fputc(std::min(b, 255u), f); }
    fclose(f);
}
int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "."; const int W = 520, H = 400;
    for (int q = 0; q < 3; q++) {
        int in[N_COUNT]; for (int i = 0; i < N_COUNT; i++) in[i] = ST_OK; if (q == 1) in[N_MV] = ST_ERR; if (q == 2) { in[N_REFLEX] = ST_WARN; in[N_OVERLAY] = ST_OFF; }
        g_state = State(); Update(in, 1000.0); std::vector<uint32_t> px((size_t)W * H, 0u); Canvas c{ px.data(), W, W, H }; NodePos pos[N_COUNT];
        auto t0 = std::chrono::steady_clock::now(); const int reps = 50; for (int r = 0; r < reps; r++) { std::fill(px.begin(), px.end(), 0u); Render(c, 1000.0 + 2500.0 + r * 80.0, q == 1 ? N_MV : -1, q, pos); }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps; printf("quality %d: %zu edges, %.2f ms / frame (%dx%d, 1 core)\n", q, g_model.e.size(), ms, W, H);
        char b[256]; snprintf(b, sizeof b, "%s/holo_q%d.ppm", dir.c_str(), q); Dump(b, px, W, H);
        for (int i = 0; i < N_COUNT; i++) CHECK(pos[i].x >= 0 && pos[i].x < W && pos[i].y >= 0 && pos[i].y < H, "node inside the canvas");
        CHECK(g_model.e.size() >= 30, "enough edges");
    }
    // error hold: an error seen once stays 'shown' for 1.6 s, then falls back
    { g_state = State(); int in[N_COUNT] = {}; in[N_MV] = ST_ERR; Update(in, 5000.0); in[N_MV] = ST_OK; Update(in, 5500.0); CHECK(g_state.shown[N_MV] == ST_ERR, "error held at +0.5 s"); Update(in, 6700.0); CHECK(g_state.shown[N_MV] == ST_OK, "error released at +1.7 s"); }
    // a subsystem path always exists for every node (node -> core)
    { Build(1); for (int i = 0; i < N_COUNT; i++) { int n = 0; for (auto& e : g_model.e) if (e.owner == i) n++; CHECK(n == 3, "3 path edges per subsystem"); } }
    printf(fails ? "holo tests FAILED\n" : "holo tests passed\n"); return fails ? 1 : 0;
}
