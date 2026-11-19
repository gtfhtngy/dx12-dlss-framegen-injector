#!/usr/bin/env python3
# Cuts the storage layer (file format, load / append / convert, SNSC container) out of the REAL src/psocache.inc and src/shadercache.inc, so that the tests in this
# folder run exactly the code that goes into the DLL - without D3D12 / threads / the game.  usage: extract_cache_code.py <src dir> <output dir>
import sys, os
src, out = sys.argv[1], sys.argv[2]
pso = open(os.path.join(src, 'psocache.inc'), encoding='utf-8').read()
sc  = open(os.path.join(src, 'shadercache.inc'), encoding='utf-8').read()
def cut(s, a, b):
    i = s.index(a); j = s.index(b, i); return s[i:j]
crc = cut(sc, "struct CrcTable", "static uint64_t Fnv64")
# --- pipeline cache (psocache.inc): structs + globals, erase/find, packing helpers, file I/O.  Left out: hashing / state streams (D3D12 types), PsoStore (needs a pipeline object), PsoPaths, threads
body = "\n".join([crc,
    cut(pso, "#pragma pack(push, 1)", "// root signature identity"),
    cut(pso, "static void PsoErase", "static void PsoStore"),
    cut(pso, "// cache thread only: packs one raw entry", "static std::vector<std::wstring> PsoPaths()"),
    cut(pso, "static bool ReadExact", "// ------------------------------------------------------------------ GPU/driver key")])
open(os.path.join(out, 'psocache_extract.inc'), 'w', encoding='utf-8').write(body)
# --- SNSC container (shadercache.inc)
body = "\n".join([crc,
    cut(sc, "#pragma pack(push, 1)\nstruct SnscHeader", "// F_D3D12CreateDevice"),
    cut(sc, "static bool ParseCache(", "static bool LoadShaderCache()"),
    cut(sc, "static void BuildCacheFile(", "static bool SaveShaderCache()")])
open(os.path.join(out, 'snsc_extract.inc'), 'w', encoding='utf-8').write(body)
print("extracted storage layer of psocache.inc / shadercache.inc ->", out)
