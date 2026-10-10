// tests/replay_viewbuf.cpp - replays view-buffer dumps (SN_DLSSG_viewbuf.txt, written by the injector when camera detection struggles) through the SAME
// scan core the injector uses (src/camcore.h). No game, no GPU, no Windows needed.
//
//   replay_viewbuf <dump.txt> [--expect PROJ NOAA MV]   scan every dump in the file, print what was found; with --expect exit 1 if no dump matches
//   replay_viewbuf --selftest                            built-in synthetic UE4 / UE5-like buffers (runs in CI)
//   replay_viewbuf --write-synthetic <file>              writes a synthetic dump (same format as the injector writes) to use as a regression sample
//
// A real dump from a game that works can be added to tests/data/ with a first line   # expect: 116 132 492   -> run_tests.sh replays it on every build.
#include "../src/camcore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

struct Dump { int frame = 0; int avail = 0; std::vector<float> f; };
static std::vector<Dump> ParseDumps(std::istream& in) {
    std::vector<Dump> out; std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("=== dump", 0) == 0) { Dump d; int n = 0; unsigned long long off = 0; sscanf(line.c_str(), "=== dump %d frame %d bufoffset %llu floats %d", &n, &d.frame, &off, &d.avail); out.push_back(d); continue; }
        if (out.empty()) continue; int idx; float a, b, c, d;
        if (sscanf(line.c_str(), "%d: %f %f %f %f", &idx, &a, &b, &c, &d) == 5) { Dump& D = out.back(); if ((int)D.f.size() < idx) D.f.resize(idx, 0.f); D.f.push_back(a); D.f.push_back(b); D.f.push_back(c); D.f.push_back(d); }
    }
    return out;
}
struct Result { bool found = false; int proj = -1, noaa = -1, c2p = -1, nc = 0; };
static Result Scan(const Dump& d, int c2pIdx = 0) {
    Result r; int ji, nj; long ps = 0; r.found = CamFindPair(d.f.data(), (int)d.f.size(), &ji, &nj, &ps); if (!r.found) return r;
    int cand[4]; r.nc = CamCollectC2P(d.f.data(), (int)d.f.size(), nj, cand); r.proj = ji; r.noaa = nj; r.c2p = c2pIdx < r.nc ? cand[c2pIdx] : -1; return r;
}
static void Put(std::vector<float>& b, int at, std::initializer_list<float> v) { int i = at; for (float x : v) b[i++] = x; }
// Scarlet Nexus-like UE4 layout: ViewToClip(jittered) @116, ViewToClipNoAA @132, ClipToPrevClip @492
static std::vector<float> SynthUe4(float jx = 0.0007f, float jy = -0.0012f) {
    std::vector<float> b(640, 0.f); float m0 = 1.0f / 1.0f * 0.5625f * 1.7778f, m5 = 1.7778f; m0 = 0.99999f;
    Put(b, 116, { m0, 0, 0, 0, 0, m5, 0, 0, jx, jy, 0, 1, 0, 0, 0.1f, 0 });
    Put(b, 132, { m0, 0, 0, 0, 0, m5, 0, 0, 0, 0, 0, 1, 0, 0, 0.1f, 0 });
    Put(b, 492, { 1, 0.0001f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0002f, -0.0001f, 0, 1 });
    return b;
}
// UE5-like: an extra identity-like matrix in front of ClipToPrevClip, the pair at other offsets
static std::vector<float> SynthUe5() {
    std::vector<float> b(900, 0.f); float m0 = 1.2f, m5 = 2.1333f;
    Put(b, 200, { m0, 0, 0, 0, 0, m5, 0, 0, 0.0005f, 0.0003f, 0, 1, 0, 0, 0.1f, 0 });
    Put(b, 216, { m0, 0, 0, 0, 0, m5, 0, 0, 0, 0, 0, 1, 0, 0, 0.1f, 0 });
    Put(b, 400, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
    Put(b, 420, { 1, 0.0002f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0004f, 0, 0, 1 });
    return b;
}
static void WriteDump(FILE* f, const std::vector<float>& v, int n, int frame) {
    fprintf(f, "=== dump %d frame %d bufoffset %d floats %d (synthetic) ===\n", n, frame, 0, (int)v.size());
    for (int i = 0; i + 4 <= (int)v.size(); i += 4) fprintf(f, "%4d: %.8g %.8g %.8g %.8g\n", i, v[i], v[i + 1], v[i + 2], v[i + 3]);
}
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static int SelfTest() {
    { Dump d; d.f = SynthUe4(); Result r = Scan(d); CHECK(r.found && r.proj == 116 && r.noaa == 132 && r.c2p == 492); }
    { Dump d; d.f = SynthUe4(0.f, 0.f); Result r = Scan(d); CHECK(r.found && r.proj == 116 && r.noaa == 132); }      // unjittered frame: identical matrices count as a pair
    { Dump d; d.f = SynthUe5(); Result r = Scan(d); CHECK(r.found && r.proj == 200 && r.noaa == 216 && r.nc == 2 && r.c2p == 400); Result r1 = Scan(d, 1); CHECK(r1.c2p == 420); }   // c2pidx picks the candidate
    { Dump d; d.f.assign(640, 0.f); CHECK(!Scan(d).found); }                                                             // empty buffer
    { Dump d; d.f = SynthUe4(); d.f[116 + 11] = 0.f; d.f[132 + 11] = 0.f; CHECK(!Scan(d).found); }                       // not perspective matrices
    { Dump d; d.f = SynthUe4(); d.f[132] = 1.5f; CHECK(!Scan(d).found); }                                                // NoAA matrix differs from the jittered one
    { Dump d; d.f = SynthUe4(); d.f[116 + 5] = 5.0f; d.f[132 + 5] = 5.0f; CHECK(!Scan(d).found); }                       // implausible aspect (>3.8)
    { Dump d; d.f = SynthUe4(); d.f[492 + 15] = 9.0f; Result r = Scan(d); CHECK(r.found && r.c2p == -1); }                // no plausible ClipToPrevClip
    // dump file round trip (writer format == parser format)
    { std::stringstream ss; std::vector<float> a = SynthUe4(), b = SynthUe5(); FILE* tmp = tmpfile(); WriteDump(tmp, a, 1, 300); WriteDump(tmp, b, 2, 600); rewind(tmp); char buf[1 << 16]; size_t n; std::string all; while ((n = fread(buf, 1, sizeof buf, tmp)) > 0) all.append(buf, n); fclose(tmp);
      std::istringstream in(all); auto ds = ParseDumps(in); CHECK(ds.size() == 2); if (ds.size() == 2) { Result r0 = Scan(ds[0]), r1 = Scan(ds[1]); CHECK(r0.found && r0.proj == 116 && r0.c2p == 492); CHECK(r1.found && r1.proj == 200 && r1.noaa == 216); CHECK(ds[0].frame == 300 && ds[1].frame == 600); } }
    printf("replay selftest: %s\n", g_fail ? "FAILED" : "ok"); return g_fail ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--selftest")) return SelfTest();
    if (argc >= 3 && !strcmp(argv[1], "--write-synthetic")) { FILE* f = fopen(argv[2], "w"); if (!f) return 2; fprintf(f, "# expect: 116 132 492\n"); WriteDump(f, SynthUe4(), 1, 300); fclose(f); return 0; }
    if (argc < 2) { printf("usage: replay_viewbuf <dump.txt> [--expect PROJ NOAA MV] | --selftest | --write-synthetic <file>\n"); return 2; }
    std::ifstream in(argv[1]); if (!in) { printf("cannot open %s\n", argv[1]); return 2; }
    int ep = -1, en = -1, em = -1; bool haveExp = false;
    for (int i = 2; i + 3 < argc + 0 && !strcmp(argv[i], "--expect"); i += 4) { ep = atoi(argv[i + 1]); en = atoi(argv[i + 2]); em = atoi(argv[i + 3]); haveExp = true; }
    if (!haveExp) { std::string first; std::getline(in, first); if (first.rfind("# expect:", 0) == 0 && sscanf(first.c_str(), "# expect: %d %d %d", &ep, &en, &em) == 3) haveExp = true; in.clear(); in.seekg(0); }
    auto ds = ParseDumps(in); printf("%s: %zu dump(s)\n", argv[1], ds.size()); int match = 0;
    for (size_t i = 0; i < ds.size(); i++) { Result r = Scan(ds[i]); printf("  dump %zu (frame %d, %zu floats): ", i + 1, ds[i].frame, ds[i].f.size());
        if (!r.found) printf("no ViewToClip / ViewToClipNoAA pair\n"); else { printf("projoff=%d noaaoff=%d mvoff=%d (%d identity-like candidate(s))\n", r.proj, r.noaa, r.c2p, r.nc); if (haveExp && r.proj == ep && r.noaa == en && r.c2p == em) match++; } }
    if (haveExp) { printf("expected %d %d %d -> %s\n", ep, en, em, match ? "MATCH" : "NO MATCH"); return match ? 0 : 1; }
    return 0;
}
