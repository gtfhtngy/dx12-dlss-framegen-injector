// tests/camera_seq/cam_shim.h - the minimum of main.cpp's world that the camera region (camera_extract.inc) needs, on plain Linux / MSYS2.
// Everything here is a STAND-IN for the injector's own globals / helpers; the logic under test is the real code extracted from src/main.cpp.
// (list of symbols: STEP1_camtrace_and_shim.md section 2/3 - the compiler checks it: a missing symbol is a build error)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>
#include "../../src/camcore.h"
#include "../../src/logic.h"
typedef uint64_t UINT64;

// ---- g_cfg: only the fields the camera region reads / writes (defaults = src/config.inc 1.1.7)
struct Cfg {
    int mvOff = 492, noAAOff = 132, projOff = 116;
    int jitSX = 1, jitSY = 1, c2pIdx = 0, c2pAuto = 1, camAuto = 1, autoLearn = 1, cutDetect = 1, cutFov = 8;
    int camStale = 30, camRescan = 90, camHold = 1, camHoldMax = 7200, fgDelay = 30;   // read by the camhold region (src/config.inc 1.1.7 defaults)
};
static Cfg g_cfg;

// ---- globals defined OUTSIDE the camera region in main.cpp
static std::atomic<int>  g_frame{0};          // the replayer sets this from every call's frame BEFORE the call (ScanViewBuffer's scan budget is keyed on it)
static std::atomic<bool> g_forceReset{false};
static std::mutex        g_camMx;
static std::atomic<int>  g_camFrame{-1};
static std::atomic<int>  g_camRescan{0};
static float g_vtcNoAA[16] = {}, g_c2p[16] = {};
static float g_jitPX = 0.f, g_jitPY = 0.f, g_jitNX = 0.f, g_jitNY = 0.f;

// ---- view size: from the trace record (EffW()/EffH() in main.cpp)
// ---- globals read by the camhold region (main.cpp ~1563-1578)
static bool g_wasOn = true;                      // FG was running (camhold only engages then)
static uint64_t g_bufBytes = 0; static std::atomic<long> g_evicted{0}, g_regSkipped{0};   // only used as Log() arguments

static int g_shimW = 1920, g_shimH = 1080;
static inline int EffW() { return g_shimW; }
static inline int EffH() { return g_shimH; }

// ---- observability: every Log() line and every config write-back is captured, tests assert on them / on the event sequence
struct ShimLogLine { int frame; std::string text; };
static std::vector<ShimLogLine> g_shimLog;
static std::vector<std::pair<std::string, int>> g_shimCfgWrites;
static int g_shimDumpCalls = 0;
static bool g_shimEcho = false;                 // set by --verbose in the replayer
static void Log(const char* fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    g_shimLog.push_back({ g_frame.load(), b }); if (g_shimEcho) printf("  [log f%d] %s\n", g_frame.load(), b);
}
static void CfgSetInt(const char* key, int v) { g_shimCfgWrites.push_back({ key, v }); }
static void DumpViewBuf(const float*, int, UINT64, const char*) { g_shimDumpCalls++; }   // stand-in for the (skipped) file-writing function
