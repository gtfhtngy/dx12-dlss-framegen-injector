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
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/test_holo tests/test_holo.cpp
echo "== system hologram (v28): geometry, error hold, render cost"
tests/bin/test_holo tests/bin
$CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -o tests/bin/test_brain tests/test_brain.cpp
echo "== brain v0 + v1 (1.1.8): camera-motion sensor, safe limits, trouble hold, learned camera predictor, learned load governor (simulated game), fuzz"
tests/bin/test_brain
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
echo "== camtrace format (SNCT v1)"
$CXX -std=c++17 -O1 -Wall -Wextra -o tests/bin/test_camtrace tests/test_camtrace.cpp
tests/bin/test_camtrace
if command -v python3 >/dev/null 2>&1; then
  echo "== camera tracker (step 5): CamTracker::Parse / Frame = the REAL text of src/main.cpp (SN_EXTRACT markers), built in TRACKER mode (-DSN_CAM_USE_TRACKER) on a Linux shim"
  python3 tests/camera_seq/extract_camera_code.py src tests/bin && python3 tests/camera_seq/extract_camera_code.py src tests/bin camhold
  $CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -DSN_CAM_USE_TRACKER -Itests/bin -o tests/bin/test_extract_smoke tests/camera_seq/test_extract_smoke.cpp
  for sc in strict lock5 scan_no_budget hits_cleared_on_lock; do tests/bin/test_extract_smoke $sc; done
  echo "== camera tracker: SNCT sequence replay (REAL Parse + Frame), typed events cross-checked against the log-derived events"
  $CXX -std=c++17 -O1 -Wall -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -DSN_CAM_USE_TRACKER -Itests/bin -o tests/bin/replay_seq tests/camera_seq/replay_seq.cpp
  for sc in mislock12752 alt_views c2p_never_moved c2p_monitor_needs_lock hard_cut hold_then_return hold_long_reset rescan_giveup; do tests/bin/replay_seq $sc; done
  echo "== sparse-vs-full gate (recorder rule vs full buffers, on generated sequences + every tests/data dump)"
  tests/bin/replay_seq --gate tests/data
  echo "== camtracker layout: no mutable static left in the regions, members declared, no legacy global in the host; default state; host-apply of results"
  python3 tests/camera_seq/check_state_inventory.py tests/bin src/camtracker.h tests/camera_seq/cam_shim.h src
  $CXX -std=c++17 -O1 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Itests/bin -o tests/bin/test_camtracker_layout tests/camera_seq/test_camtracker_layout.cpp
  tests/bin/test_camtracker_layout
  $CXX -std=c++17 -Wall -Wextra -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -fsyntax-only -DSN_CAM_USE_TRACKER -Itests/bin tests/camera_seq/check_compat_tracker_syntax.cpp
else echo "(python3 not found - camera tracker extraction tests skipped)"; fi
echo "ALL TESTS PASSED"
