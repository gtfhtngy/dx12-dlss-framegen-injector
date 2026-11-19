// snpack_bench.cpp - measure the SNLZ codec on real files (e.g. a dumped SN_DLSSG_psocache.bin payload, DXBC files, any binary).
// build: g++ -std=c++17 -O2 -o snpack_bench tools/snpack_bench.cpp
// usage: snpack_bench [-d depth] [-c chunkKB] file...      (chunk = size of the independent blocks, default 64 KB; a pipeline cache entry is one block)
#include "../src/snpack.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    int depth = 24; size_t chunk = 64 << 10; int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) { if (!strcmp(argv[i], "-d") && i + 1 < argc) depth = atoi(argv[++i]); else if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk = (size_t)atoi(argv[++i]) << 10; }
    if (i >= argc) { printf("usage: snpack_bench [-d depth] [-c chunkKB] file...\n"); return 1; }
    size_t totIn = 0, totOut = 0; double tPack = 0, tUnpack = 0; bool allOk = true;
    for (; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb"); if (!f) { printf("%s: cannot open\n", argv[i]); continue; }
        std::vector<uint8_t> d; uint8_t tmp[1 << 16]; size_t r; while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) d.insert(d.end(), tmp, tmp + r); fclose(f);
        snpack::Packer pk; size_t in = 0, out = 0; double tp = 0, tu = 0; bool ok = true;
        for (size_t o = 0; o < d.size(); o += chunk) {
            size_t n = std::min(chunk, d.size() - o); std::vector<uint8_t> buf(snpack::Bound(n)), back(n);
            auto t0 = std::chrono::steady_clock::now(); size_t pn = pk.Pack(d.data() + o, n, buf.data(), buf.size(), depth); auto t1 = std::chrono::steady_clock::now();
            if (pn) { ok = snpack::Unpack(buf.data(), pn, back.data(), n) && !memcmp(back.data(), d.data() + o, n) && ok; } auto t2 = std::chrono::steady_clock::now();
            tp += std::chrono::duration<double>(t1 - t0).count(); tu += std::chrono::duration<double>(t2 - t1).count();
            in += n; out += pn ? pn : n;
        }
        printf("%-40s %10zu -> %10zu  (%5.1f%%)  pack %6.1f MB/s  unpack %7.1f MB/s  %s\n", argv[i], in, out, in ? 100.0 * out / in : 0.0, tp > 0 ? in / tp / 1e6 : 0.0, tu > 0 ? in / tu / 1e6 : 0.0, ok ? "OK" : "ROUND-TRIP FAILED");
        totIn += in; totOut += out; tPack += tp; tUnpack += tu; allOk = allOk && ok;
    }
    printf("TOTAL %zu -> %zu (%.1f%%), depth %d chunk %zu KB\n", totIn, totOut, totIn ? 100.0 * totOut / totIn : 0.0, depth, chunk >> 10);
    return allOk ? 0 : 2;
}
