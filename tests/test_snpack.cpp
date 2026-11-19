// test_snpack.cpp - unit tests for src/snpack.h (SNLZ codec used by the shader / pipeline caches). No Windows, no GPU.
// build: g++ -std=c++17 -O1 -Wall -o tests/bin/test_snpack tests/test_snpack.cpp   (CI also runs it with -fsanitize=address,undefined)
#include "../src/snpack.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_fail = 0, g_ok = 0;
#define CHECK(c) do { if (c) g_ok++; else { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Rng { uint64_t s; explicit Rng(uint64_t x) : s(x) {} uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); } };
typedef std::vector<uint8_t> Bytes;

static Bytes RandomBytes(size_t n, uint64_t seed) { Rng r(seed); Bytes b(n); for (auto& x : b) x = (uint8_t)r.next(); return b; }
// looks like a compiled shader: 16-byte "instructions" from a small opcode set, registers / immediates change, long runs of identical records, padding
static Bytes FakeShaderCode(size_t n, uint64_t seed) {
    Rng r(seed); Bytes b; b.reserve(n + 16);
    static const uint8_t ops[8][4] = { {0x24,0x78,0x02,0x00}, {0x10,0x73,0x04,0x01}, {0x19,0x78,0x00,0x00}, {0x47,0x79,0x00,0x00}, {0x21,0x72,0x05,0x02}, {0x0C,0x72,0x00,0x00}, {0x85,0x79,0x06,0x00}, {0x18,0x78,0x00,0x03} };
    while (b.size() < n) {
        const uint8_t* o = ops[r.next() & 7]; for (int i = 0; i < 4; i++) b.push_back(o[i]);
        b.push_back((uint8_t)(r.next() & 0x1F)); b.push_back((uint8_t)(r.next() & 0x1F)); b.push_back((uint8_t)(r.next() % 6)); b.push_back(0);
        for (int i = 0; i < 8; i++) b.push_back((r.next() & 3) ? 0 : (uint8_t)r.next());
        if ((r.next() & 63) == 0) b.insert(b.end(), 64 + (r.next() & 255), 0);
    }
    b.resize(n); return b;
}
static Bytes TextBytes(size_t n) { std::string s; int i = 0; while (s.size() < n) s += "float4 main(float2 uv : TEXCOORD0) : SV_Target { return tex.Sample(smp, uv) * " + std::to_string(i++ % 17) + "; }\n"; return Bytes(s.begin(), s.begin() + n); }

// pack, check the result is valid and round-trips; returns the packed size (0 = stored raw)
static size_t RoundTrip(const Bytes& in, int depth = 24) {
    snpack::Packer pk; Bytes out(snpack::Bound(in.size()) + 8);
    size_t pn = pk.Pack(in.data(), in.size(), out.data(), out.size(), depth);
    if (pn == 0) return 0;
    CHECK(pn < in.size());
    Bytes back(in.size());
    bool ok = snpack::Unpack(out.data(), pn, back.data(), back.size());
    CHECK(ok); if (ok) CHECK(back == in);
    return pn;
}

static void TestSizesAndPatterns() {
    printf("== round trip: sizes 0..600 x 4 patterns\n");
    for (size_t n = 0; n <= 600; n++) {
        RoundTrip(Bytes(n, 0)); RoundTrip(RandomBytes(n, n + 1)); RoundTrip(FakeShaderCode(n, n + 7)); RoundTrip(TextBytes(n));
    }
    printf("== zero run (1 MB), long literal / match length extensions\n");
    size_t z = RoundTrip(Bytes(1 << 20, 0)); CHECK(z > 0 && z < 8000);
    Bytes mix = RandomBytes(700, 5); Bytes big = mix; big.insert(big.end(), mix.begin(), mix.end()); RoundTrip(big);   // 700 literals then a 700-byte match
    for (int k = 0; k < 40; k++) { Bytes a = RandomBytes(1 + k * 37, 100 + k), t = a; for (int r = 0; r < 3; r++) t.insert(t.end(), a.begin(), a.end()); RoundTrip(t); }
    printf("== random data is never made bigger (Pack returns 0)\n");
    for (int k = 0; k < 20; k++) { Bytes r = RandomBytes(1000 + k * 977, 900 + k); size_t p = RoundTrip(r); CHECK(p == 0); }
    printf("== window: a copy 100 KB away cannot be referenced but must still round trip\n");
    Bytes w = RandomBytes(4000, 77); Bytes far = w; far.insert(far.end(), 100000, 0x55); far.insert(far.end(), w.begin(), w.end()); RoundTrip(far);
}

static void TestRatio() {
    printf("== ratio on shader-like data\n");
    Bytes code = FakeShaderCode(1 << 20, 42); size_t p = RoundTrip(code);
    printf("  fake shader code : %zu -> %zu bytes (%.1f%%)\n", code.size(), p, p ? 100.0 * p / code.size() : 100.0);
    CHECK(p > 0 && p < code.size() * 6 / 10);
    Bytes txt = TextBytes(200000); size_t pt = RoundTrip(txt); CHECK(pt > 0 && pt < txt.size() / 8);
    size_t fast = RoundTrip(code, 4), best = RoundTrip(code, 96);
    printf("  depth 4 / 24 / 96: %zu / %zu / %zu bytes\n", fast, p, best);
    CHECK(best <= p && p <= fast + fast / 50);
}

static void TestRepeatOffset() {
    printf("== repeat offset: fixed-size records with one changing field\n");
    Bytes b; for (int i = 0; i < 20000; i++) { uint8_t rec[16] = { 1,2,3,4,5,6,7,8, (uint8_t)i, (uint8_t)(i >> 8), 11,12,13,14,15,16 }; b.insert(b.end(), rec, rec + 16); }
    size_t p = RoundTrip(b); printf("  %zu -> %zu bytes\n", b.size(), p); CHECK(p > 0 && p < b.size() / 4);
}

static void TestStrictDecoder() {
    printf("== decoder is strict: wrong sizes, truncation, corruption\n");
    Bytes in = FakeShaderCode(50000, 9); snpack::Packer pk; Bytes out(snpack::Bound(in.size())); size_t pn = pk.Pack(in.data(), in.size(), out.data(), out.size());
    CHECK(pn > 0); if (!pn) return;
    Bytes back(in.size() + 100);
    CHECK(!snpack::Unpack(out.data(), pn, back.data(), in.size() - 1));       // too small a destination
    CHECK(!snpack::Unpack(out.data(), pn, back.data(), in.size() + 1));       // stream ends early
    CHECK(!snpack::Unpack(out.data(), pn - 1, back.data(), in.size()));       // truncated
    CHECK(!snpack::Unpack(out.data(), 0, back.data(), in.size()));
    CHECK(!snpack::Unpack(nullptr, 10, back.data(), 10));
    uint8_t bad[4] = { 0x01, 0x41, 0x05, 0x00 }; uint8_t d[64];              // 1 literal, then a match whose offset (5) points before the start
    CHECK(!snpack::Unpack(bad, sizeof bad, d, 20));
    uint8_t rep0[4] = { 0x01, 0x41, 0x00, 0x00 };                            // offset 0 (= repeat) with no previous match
    CHECK(!snpack::Unpack(rep0, sizeof rep0, d, 20));
    // fuzz: random byte flips / truncations / extensions must never crash (run under ASan/UBSan in CI); a "true" result must have written exactly rawLen bytes
    Rng r(1234); int accepted = 0; long total = 3000;
    for (long it = 0; it < total; it++) {
        Bytes c(out.begin(), out.begin() + pn);
        int mode = (int)(r.next() % 4);
        if (mode == 0) { for (int k = 0, m = 1 + (int)(r.next() % 8); k < m; k++) c[r.next() % c.size()] ^= (uint8_t)(1u << (r.next() % 8)); }
        else if (mode == 1) c.resize(1 + r.next() % c.size());
        else if (mode == 2) { for (int k = 0, m = 1 + (int)(r.next() % 4); k < m; k++) c.push_back((uint8_t)r.next()); }
        else { for (auto& x : c) if ((r.next() & 15) == 0) x = (uint8_t)r.next(); }
        Bytes dst(in.size()); if (snpack::Unpack(c.data(), c.size(), dst.data(), dst.size())) accepted++;
    }
    printf("  %ld corrupted streams: no crash, %d happened to decode to the right size\n", total, accepted);
    // pure garbage with every raw size
    for (int it = 0; it < 2000; it++) { Bytes g = RandomBytes(1 + r.next() % 300, it + 5000); Bytes dst(1 + r.next() % 2000); snpack::Unpack(g.data(), g.size(), dst.data(), dst.size()); }
    CHECK(true);
}

static void TestCapAndTiny() {
    printf("== cap handling / tiny inputs\n");
    Bytes in = FakeShaderCode(20000, 3); snpack::Packer pk; Bytes out(snpack::Bound(in.size()));
    size_t full = pk.Pack(in.data(), in.size(), out.data(), out.size()); CHECK(full > 0);
    Bytes small(full / 2); CHECK(pk.Pack(in.data(), in.size(), small.data(), small.size()) == 0);   // does not fit -> 0, no overrun (ASan)
    Bytes exact(full); size_t p2 = pk.Pack(in.data(), in.size(), exact.data(), exact.size()); CHECK(p2 == 0 || p2 == full);
    uint8_t t[8] = { 1, 1, 1, 1, 1, 1, 1, 1 }; uint8_t o[64]; CHECK(pk.Pack(t, sizeof t, o, sizeof o) == 0);
    CHECK(pk.Pack(nullptr, 100, o, sizeof o) == 0);
    // Packer is reusable: different inputs after each other give the same result as a fresh one
    Bytes a = FakeShaderCode(30000, 11), b = TextBytes(30000); snpack::Packer p1; Bytes oa(snpack::Bound(a.size())), ob(snpack::Bound(b.size())), ob2(snpack::Bound(b.size()));
    size_t na = p1.Pack(a.data(), a.size(), oa.data(), oa.size()); size_t nb = p1.Pack(b.data(), b.size(), ob.data(), ob.size());
    snpack::Packer fresh; size_t nb2 = fresh.Pack(b.data(), b.size(), ob2.data(), ob2.size());
    CHECK(na > 0 && nb == nb2 && memcmp(ob.data(), ob2.data(), nb) == 0);
}

int main() {
    TestSizesAndPatterns(); TestRatio(); TestRepeatOffset(); TestStrictDecoder(); TestCapAndTiny();
    printf("snpack: %d checks passed, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
