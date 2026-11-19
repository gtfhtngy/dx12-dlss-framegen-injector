#!/bin/sh
# Unit tests + view-buffer replay. Needs only g++ (and unzip for the zip check). Runs on Linux / CI / Windows with MSYS2 - no game, no GPU.
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/bin
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/test_logic tests/test_logic.cpp
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/replay_viewbuf tests/replay_viewbuf.cpp
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/test_c2p tests/test_c2p.cpp
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/test_snpack tests/test_snpack.cpp
echo "== ClipToPrevClip reprojection tests (1.1.6)"
tests/bin/test_c2p
echo "== SNLZ shader-cache codec (1.1.7)"
tests/bin/test_snpack
if command -v python3 >/dev/null 2>&1; then
  echo "== cache file formats (1.1.7): the real psocache.inc / shadercache.inc storage code on a Win32 file shim"
  python3 tests/cache_io/extract_cache_code.py src tests/bin
  $CXX -std=c++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-result -Wno-misleading-indentation -Itests/bin -o tests/bin/test_psocache_io tests/cache_io/test_psocache_io.cpp
  $CXX -std=c++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Itests/bin -o tests/bin/test_snsc_container tests/cache_io/test_snsc_container.cpp
  tests/bin/test_psocache_io
  tests/bin/test_snsc_container
else echo "(python3 not found - cache file format tests skipped)"; fi
echo "== logic.h unit tests"
tests/bin/test_logic tests/bin/test.zip
if command -v unzip >/dev/null 2>&1; then unzip -tq tests/bin/test.zip; fi
echo "== camera scan self test"
tests/bin/replay_viewbuf --selftest
echo "== replay of recorded dumps (tests/data/*.txt, first line '# expect: PROJ NOAA MV')"
for f in tests/data/*.txt; do [ -f "$f" ] && tests/bin/replay_viewbuf "$f"; done
echo "ALL TESTS PASSED"
