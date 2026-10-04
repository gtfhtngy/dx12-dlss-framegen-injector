#!/bin/sh
# needs: apt install g++-mingw-w64-x86-64-posix git
# deps : MinHook + Streamline v2.14.1 headers (fetched into ./deps on first run)
set -e
[ -f winmm_stubs.inc ] || sh ./gen_stubs.sh
[ -d deps/mh ] || git clone -q --depth 1 https://github.com/TsudaKageyu/minhook.git deps/mh
[ -d deps/sl ] || git clone -q --depth 1 --branch v2.14.1 https://github.com/NVIDIAGameWorks/Streamline.git deps/sl
CC=x86_64-w64-mingw32-gcc-posix; CXX=x86_64-w64-mingw32-g++-posix
mkdir -p obj
for f in hook buffer trampoline hde/hde64; do $CC -O2 -c -Ideps/mh/include deps/mh/src/$f.c -o obj/$(basename $f).o; done
$CXX -std=c++17 -O2 -shared -Ideps/mh/include -Ideps/sl/include -o winmm.dll main.cpp obj/*.o winmm.def \
  -static -static-libgcc -static-libstdc++ -luser32 -lgdi32 -ladvapi32 -lversion -ldxguid -Wl,--kill-at
echo built winmm.dll
