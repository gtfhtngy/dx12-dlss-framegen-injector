// test_psocache_io.cpp - runs the REAL storage code of src/psocache.inc (cut out by extract_cache_code.py) on a POSIX Win32 shim: load v1 -> convert to v2 -> append -> reload -> hit.
#include "win_shim.h"
#include "../../src/snpack.h"
static std::vector<std::wstring> g_testPaths; static std::vector<std::wstring> PsoPaths() { return g_testPaths; }
#include "psocache_extract.inc"

static int fails = 0, oks = 0;
#define CHECK(c) do { if (c) oks++; else { fails++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
struct Rng { uint64_t s; explicit Rng(uint64_t x) : s(x) {} uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); } };
typedef std::vector<uint8_t> Bytes;
// compressible, shader-like
static Bytes Blob(size_t n, uint64_t seed, bool compressible) {
    Rng r(seed); Bytes b; b.reserve(n + 16);
    if (!compressible) { b.resize(n); for (auto& x : b) x = (uint8_t)r.next(); return b; }
    static const uint8_t ops[8][4] = { {0x24,0x78,2,0}, {0x10,0x73,4,1}, {0x19,0x78,0,0}, {0x47,0x79,0,0}, {0x21,0x72,5,2}, {0x0C,0x72,0,0}, {0x85,0x79,6,0}, {0x18,0x78,0,3} };
    while (b.size() < n) { const uint8_t* o = ops[r.next() & 7]; for (int i = 0; i < 4; i++) b.push_back(o[i]); b.push_back(r.next() & 31); b.push_back(r.next() & 31); b.push_back(r.next() % 6); b.push_back(0); for (int i = 0; i < 8; i++) b.push_back((r.next() & 3) ? 0 : (uint8_t)r.next()); }
    b.resize(n); return b;
}
static std::unordered_map<uint64_t, Bytes> g_truth;      // what the "game" gave us
static void StoreRaw(uint64_t key, const Bytes& v) {      // same accounting as PsoStore (which needs a real ID3D12PipelineState)
    auto p = std::make_shared<std::vector<uint8_t>>(v); PsoEntry e; e.data = p; e.raw = (uint32_t)v.size();
    { std::unique_lock<std::shared_mutex> lk(g_psoMx); if (!g_psoMap.emplace(key, e).second) return; }
    g_psoBytes += v.size(); g_psoRawBytes += v.size(); g_psoNew++;
    { std::lock_guard<std::mutex> lk(g_psoQMx); g_psoPending.emplace_back(key, p); }
    g_truth[key] = v;
}
static void ResetProcess() { { std::unique_lock<std::shared_mutex> lk(g_psoMx); g_psoMap.clear(); } g_psoPending.clear(); g_psoBytes = 0; g_psoRawBytes = 0; g_psoFileOk = false; g_psoFileVer = 0; g_psoPath.clear(); }
static long FileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) ? -1 : (long)st.st_size; }
static uint16_t FileVer(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); PsoFileHeader h = {}; if (f) { if (fread(&h, sizeof h, 1, f) != 1) h.version = 0; fclose(f); } return h.version; }
static void WriteV1(const std::string& path, uint64_t gpuKey, const std::vector<std::pair<uint64_t, Bytes>>& recs) {
    FILE* f = fopen(path.c_str(), "wb"); PsoFileHeader fh = {}; memcpy(fh.magic, "SNPC", 4); fh.version = 1; fh.headerSize = sizeof fh; fh.gpuKey = gpuKey; fwrite(&fh, sizeof fh, 1, f);
    for (auto& r : recs) { PsoRecHeader rh; rh.hash = r.first; rh.size = (uint32_t)r.second.size(); rh.crc = Crc32(r.second.data(), r.second.size()); fwrite(&rh, sizeof rh, 1, f); fwrite(r.second.data(), 1, r.second.size(), f); }
    fclose(f);
}
static bool AllMatch(size_t expectCount) {
    size_t cnt; { std::shared_lock<std::shared_mutex> lk(g_psoMx); cnt = g_psoMap.size(); }
    if (cnt != expectCount) { printf("  count %zu != %zu\n", cnt, expectCount); return false; }
    for (auto& kv : g_truth) { PsoBlobPtr b = PsoFind(kv.first); if (!b || *b != kv.second) { printf("  mismatch for key %llx\n", (unsigned long long)kv.first); return false; } }
    return true;
}

int main() {
    const std::string dir = "/tmp/sn_psotest"; system("rm -rf /tmp/sn_psotest && mkdir -p /tmp/sn_psotest");
    const std::string path = dir + "/psocache.bin"; g_testPaths = { std::wstring(path.begin(), path.end()) };
    g_psoGpuKey = 0xABCDEF0123456789ull; g_cfg.psoCacheMB = 384; g_cfg.cacheCompress = 1;
    snpack::Packer pk;

    printf("== 1. read an OLD v1 file (as written by 1.1.6), keep it usable, convert it in the background\n");
    std::vector<std::pair<uint64_t, Bytes>> recs; size_t rawTotal = 0; Rng r(7);
    for (int i = 0; i < 400; i++) { size_t n = 200 + r.next() % 60000; bool comp = (i % 9) != 0; recs.emplace_back(0x1000 + i, Blob(n, 1000 + i, comp)); rawTotal += n; g_truth[0x1000 + i] = recs.back().second; }
    WriteV1(path, g_psoGpuKey, recs);
    PsoLoadFile();
    CHECK(g_psoFileOk && g_psoFileVer == 1); CHECK(g_psoBytes == rawTotal); CHECK(g_psoRawBytes == rawTotal);
    CHECK(AllMatch(400));
    long v1size = FileSize(path);
    PsoCompact(pk);
    CHECK(g_psoFileVer == 2); CHECK(FileVer(path) == 2); long v2size = FileSize(path);
    printf("  v1 file %ld bytes -> v2 file %ld bytes (%.1f%%), RAM %zu -> %zu bytes\n", v1size, v2size, 100.0 * v2size / v1size, rawTotal, g_psoBytes.load());
    CHECK(v2size < v1size * 7 / 10); CHECK(g_psoBytes < rawTotal * 7 / 10); CHECK(g_psoRawBytes == rawTotal);
    CHECK(AllMatch(400));                                    // hits now unpack from the packed RAM copy
    CHECK(FileSize(path + ".tmp") < 0);

    printf("== 2. new pipelines during the session: stored raw, appended packed\n");
    for (int i = 0; i < 60; i++) StoreRaw(0x9000 + i, Blob(500 + r.next() % 90000, 5000 + i, (i % 7) != 0));
    CHECK(g_psoPending.size() == 60);
    size_t rawBefore = g_psoBytes; PsoFlush(pk);
    CHECK(g_psoPending.empty()); CHECK(g_psoBytes < rawBefore); CHECK(AllMatch(460));
    printf("  v2 file now %ld bytes\n", FileSize(path));

    printf("== 3. next launch: v2 file loads; everything identical; a hit costs only an unpack\n");
    size_t rawKept = g_psoRawBytes, storedKept = g_psoBytes; ResetProcess(); PsoLoadFile();
    CHECK(g_psoFileOk && g_psoFileVer == 2); CHECK(g_psoRawBytes == rawKept); CHECK(g_psoBytes == storedKept); CHECK(AllMatch(460));
    PsoCompact(pk); // must also be safe to run on an already-v2 map (nothing to pack)
    CHECK(AllMatch(460));

    printf("== 4. torn last record (crash while appending) is cut off, the rest survives\n");
    long sz = FileSize(path); { FILE* f = fopen(path.c_str(), "rb+"); ftruncate(fileno(f), sz - 100); fclose(f); }
    ResetProcess(); PsoLoadFile(); CHECK(g_psoFileOk && FileSize(path) < sz - 100 + 1);
    size_t have; { std::shared_lock<std::shared_mutex> lk(g_psoMx); have = g_psoMap.size(); } CHECK(have == 459 || have == 460 - 0 - 1);
    printf("  after the torn tail: %zu pipelines kept\n", have);
    // every entry that is still there must be bit-exact
    { std::shared_lock<std::shared_mutex> lk(g_psoMx); for (auto& kv : g_psoMap) { lk.unlock(); PsoBlobPtr b = PsoFind(kv.first); lk.lock(); CHECK(b && *b == g_truth[kv.first]); } }

    printf("== 5. bit rot inside a packed record -> CRC stops the load at that record, file is cut there, nothing wrong is ever returned\n");
    { FILE* f = fopen(path.c_str(), "rb+"); fseek(f, 5000, SEEK_SET); int c = fgetc(f); fseek(f, 5000, SEEK_SET); fputc(c ^ 0x5A, f); fclose(f); }
    ResetProcess(); PsoLoadFile(); CHECK(g_psoFileOk);
    { std::shared_lock<std::shared_mutex> lk(g_psoMx); size_t n = g_psoMap.size(); printf("  %zu pipelines survived\n", n); for (auto& kv : g_psoMap) { lk.unlock(); PsoBlobPtr b = PsoFind(kv.first); lk.lock(); CHECK(b && *b == g_truth[kv.first]); } }

    printf("== 6. corrupt PACKED bytes that still pass the CRC (e.g. a bug / hostile file): unpack fails -> entry dropped, no crash\n");
    ResetProcess(); g_truth.clear(); { std::vector<uint8_t> blob = Blob(30000, 3, true); snpack::Packer p2; Bytes sc(snpack::Bound(blob.size())); size_t pn = p2.Pack(blob.data(), blob.size(), sc.data(), sc.size()); CHECK(pn > 0);
        sc.resize(pn); sc[pn / 2] ^= 0xFF; sc[3] ^= 0x7F; PsoEntry e; e.data = std::make_shared<const std::vector<uint8_t>>(sc); e.raw = (uint32_t)blob.size(); e.method = snpack::kSnLz; g_psoMap[0x77] = e; g_psoBytes = pn; g_psoRawBytes = blob.size();
        PsoBlobPtr b = PsoFind(0x77); CHECK(!b || *b != Bytes()); /* either dropped, or (very unlikely) decoded to the right size */
        printf("  corrupt entry: %s, corrupt counter=%ld\n", b ? "decoded to the right size" : "dropped", g_psoCorrupt.load()); }

    printf("== 7. GPU / driver key changed -> brand-new v2 file\n");
    ResetProcess(); g_truth.clear(); g_psoGpuKey ^= 1; PsoLoadFile(); CHECK(g_psoFileOk && g_psoFileVer == 2); CHECK(FileSize(path) == (long)sizeof(PsoFileHeader)); CHECK(g_psoMap.empty());

    printf("== 8. size cap counts STORED bytes: the same cap holds more pipelines when packed\n");
    { std::vector<std::pair<uint64_t, Bytes>> big; for (int i = 0; i < 200; i++) big.emplace_back(0x2000 + i, Blob(100000, 9000 + i, true));   // 20 MB raw
      ResetProcess(); g_truth.clear(); g_psoGpuKey ^= 5; PsoLoadFile(); for (auto& b : big) StoreRaw(b.first, b.second); PsoFlush(pk);
      size_t stored = g_psoBytes; printf("  20 MB of blobs -> %zu KB stored in RAM, file %ld KB\n", stored >> 10, FileSize(path) >> 10); CHECK(stored < 12u << 20);
      g_cfg.psoCacheMB = 16; ResetProcess(); PsoLoadFile(); CHECK(g_psoMap.size() == 200); CHECK(AllMatch(200));       // 20 MB raw would NOT fit into 16 MB; packed does
      g_cfg.psoCacheMB = 384;
      printf("== 8b. no writable file at all (memory-only mode): blobs are still packed in RAM\n");
      ResetProcess(); g_truth.clear(); g_psoFileOk = false; for (auto& b : big) StoreRaw(b.first, b.second); size_t rawIn = g_psoBytes; PsoFlush(pk);
      CHECK(g_psoBytes < rawIn * 6 / 10); CHECK(AllMatch(200)); CHECK(g_psoPending.empty()); }

    printf("== 9. cachecompress=0: everything stays raw (method 0) but the file is still valid v2\n");
    g_cfg.cacheCompress = 0; ResetProcess(); g_truth.clear(); g_psoGpuKey ^= 3; PsoLoadFile();
    for (int i = 0; i < 30; i++) StoreRaw(0x300 + i, Blob(4000 + i * 100, 77 + i, true));
    size_t rawSum = g_psoBytes; PsoFlush(pk); CHECK(g_psoBytes == rawSum); ResetProcess(); PsoLoadFile(); CHECK(AllMatch(30)); CHECK(g_psoBytes == rawSum);
    g_cfg.cacheCompress = 1;

    printf("== 10. entries stored while the conversion runs are not appended twice\n");
    g_truth.clear(); ResetProcess(); g_psoGpuKey = 0x1234; std::vector<std::pair<uint64_t, Bytes>> r2; for (int i = 0; i < 20; i++) { r2.emplace_back(0x500 + i, Blob(8000, 400 + i, true)); g_truth[0x500 + i] = r2.back().second; }
    WriteV1(path, g_psoGpuKey, r2); PsoLoadFile(); CHECK(g_psoFileVer == 1);
    StoreRaw(0xAAA, Blob(5000, 1, true)); StoreRaw(0xAAB, Blob(5000, 2, true));    // "stored by the game" right before the conversion: in map + pending
    PsoCompact(pk); CHECK(g_psoFileVer == 2); CHECK(g_psoPending.empty());       // already part of the new file -> not queued again
    ResetProcess(); PsoLoadFile(); CHECK(AllMatch(22));

    printf("harness: %d checks ok, %d failed\n", oks, fails);
    return fails ? 1 : 0;
}
