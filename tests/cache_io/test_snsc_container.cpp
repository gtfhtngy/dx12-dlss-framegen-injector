// test_snsc_container.cpp - SNSC shader-cache container (src/shadercache.inc: BuildCacheFile / ParseCache) incl. packed entries and 1.1.6 (version 1) files.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>
#include "../../src/snpack.h"
typedef unsigned int DWORD; typedef void* HANDLE;
struct Cfg { int cacheCompress = 1; int shaderCache = 1; }; static Cfg g_cfg;
static uint64_t MvSourceKey() { return 0x1122334455667788ull; }
#include "snsc_extract.inc"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static ShaderCache Sample() {
    ShaderCache c; for (int i = 0; i < 3000; i++) c.dxbc.push_back((uint8_t)((i * 7) % 23)); for (int i = 0; i < 120; i++) c.rs.push_back((uint8_t)i);
    uint32_t x = 12345; LibBlob a; a.vendor = 0x10DE; a.device = 0x2684; a.revision = 161; a.driver = 0x123456789ull; for (int i = 0; i < 50000; i++) a.data.push_back((uint8_t)((i * 13) % 31)); c.libs.push_back(a);
    LibBlob b; b.vendor = 0x10DE; b.device = 0x2704; b.revision = 161; b.driver = 0x99ull; for (int i = 0; i < 20000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b.data.push_back((uint8_t)x); } c.libs.push_back(b);     // incompressible
    return c;
}
int main() {
    ShaderCache c = Sample();
    std::vector<uint8_t> f, f0; g_cfg.cacheCompress = 1; BuildCacheFile(c, f); g_cfg.cacheCompress = 0; BuildCacheFile(c, f0);
    printf("packed %zu bytes, raw %zu bytes\n", f.size(), f0.size()); CHECK(f.size() < f0.size() * 6 / 10);
    for (int pass = 0; pass < 2; pass++) {
        ShaderCache r; const char* why = ""; bool ok = ParseCache(pass ? f0 : f, r, &why); CHECK(ok);
        CHECK(r.dxbc == c.dxbc); CHECK(r.rs == c.rs); CHECK(r.libs.size() == 2 && r.libs[0].data == c.libs[0].data && r.libs[1].data == c.libs[1].data && r.libs[1].device == 0x2704 && r.libs[0].driver == 0x123456789ull);
    }
    // a 1.1.6 file = version 1 header, no packed entries (the layout of the unpacked build, header version 1, whole-file CRC recomputed)
    { std::vector<uint8_t> v1 = f0; SnscHeader h; memcpy(&h, v1.data(), sizeof h); h.version = 1; memcpy(v1.data(), &h, sizeof h); uint32_t fc = Crc32(v1.data(), v1.size() - 4); memcpy(&v1[v1.size() - 4], &fc, 4);
      ShaderCache r; const char* why = ""; bool ok = ParseCache(v1, r, &why); printf("version 1 file: %s\n", ok ? "accepted" : why); CHECK(ok && r.dxbc == c.dxbc && r.libs.size() == 2);
      // a packed entry is not allowed in a version 1 file
      std::vector<uint8_t> bad = f; memcpy(&h, bad.data(), sizeof h); h.version = 1; memcpy(bad.data(), &h, sizeof h); fc = Crc32(bad.data(), bad.size() - 4); memcpy(&bad[bad.size() - 4], &fc, 4); ShaderCache r2; CHECK(!ParseCache(bad, r2, &why)); }
    int accepted = 0; for (size_t i = 0; i < f.size(); i += 7) { auto g = f; g[i] ^= 0x40; ShaderCache r; const char* why; if (ParseCache(g, r, &why)) accepted++; } CHECK(accepted == 0);   // whole-file CRC: every flipped byte is caught
    { auto g = f; SnscEntry e; memcpy(&e, &g[sizeof(SnscHeader)], sizeof e); CHECK(e.type & SNSC_PACKED);      // garbage packed data WITH valid CRCs: must be rejected or decode to something else, never crash
      for (size_t k = 0; k < 40; k++) g[e.offset + 4 + 3 + k] ^= 0xA5; e.crc = Crc32(&g[e.offset], e.size); memcpy(&g[sizeof(SnscHeader)], &e, sizeof e); uint32_t fc = Crc32(g.data(), g.size() - 4); memcpy(&g[g.size() - 4], &fc, 4);
      ShaderCache r; const char* why = ""; bool ok = ParseCache(g, r, &why); CHECK(!ok || r.dxbc != c.dxbc); }
    printf("snsc container: %s\n", fails ? "FAILED" : "all ok"); return fails ? 1 : 0;
}
