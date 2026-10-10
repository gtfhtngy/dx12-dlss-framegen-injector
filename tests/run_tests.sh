#!/bin/sh
# Unit tests + view-buffer replay. Needs only g++ (and unzip for the zip check). Runs on Linux / CI / Windows with MSYS2 - no game, no GPU.
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/bin
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/test_logic tests/test_logic.cpp
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/replay_viewbuf tests/replay_viewbuf.cpp
echo "== logic.h unit tests"
tests/bin/test_logic tests/bin/test.zip
if command -v unzip >/dev/null 2>&1; then unzip -tq tests/bin/test.zip; fi
echo "== camera scan self test"
tests/bin/replay_viewbuf --selftest
echo "== replay of recorded dumps (tests/data/*.txt, first line '# expect: PROJ NOAA MV')"
for f in tests/data/*.txt; do [ -f "$f" ] && tests/bin/replay_viewbuf "$f"; done
echo "ALL TESTS PASSED"
