// tests/camera_seq/cam_compat.h  (step 4, switched in step 5) - ONE call surface for the camera tests. Since step 5 there is only TRACKER mode (-DSN_CAM_USE_TRACKER):
// the REAL CamTracker::Parse / Frame bodies are cut out of src/main.cpp by extract_camera_code.py (camera_extract.inc / camhold_extract.inc, included below) and
// Parse() / Frame() results are applied by the real CamHostApply() exactly as the host does it. Include order in a test:  cam_shim.h -> cam_compat.h (-> ../camtrace.h)
//
// What a test uses (the whole migration surface):
//   CamParseCall(p, n, bufId) -> bool          = the old ParseViewBuffer (g_lastParse = its typed result: events, persist bits, ...)
//   CamFrameCall(fr)          -> CamFrameOut   = the old camhold region (camOk / delayOk / cutNow; g_lastFrame = its typed result)
//   CamSetOffsets(proj, noaa, mv)              = "the offsets in force are now these" (tracker `off` + the g_cfg mirror)
//   CamNewScenario()                           = fresh tracker state (new CamTracker): scenarios MAY now share a process (nothing is function-local static any more)
//   the legacy global names g_camFrame g_camLocked g_camRescan g_scanRuns g_cutHold g_cutTotal g_c2pGarbage g_c2pBadTotal g_c2p g_jitPX.. (macros onto the members)
// g_cfg.projOff/noAAOff/mvOff stay READABLE (the host mirror, updated by CamHostApply).
#pragma once
#ifndef SN_CAM_USE_TRACKER
#error build the camera tests with -DSN_CAM_USE_TRACKER (the legacy regions no longer exist in src/main.cpp since step 5)
#endif

struct CamFrameOut { bool camOk, delayOk, cutNow; };

#include "../../src/camtracker.h"   // needs Cfg / g_cfg / g_forceReset / CfgSetInt from cam_shim.h (already included)
#include "camera_extract.inc"        // generated: CamTracker::Parse + helpers, the REAL text of src/main.cpp
#include "camhold_extract.inc"       // generated: CamTracker::Frame
static CamTracker* g_camT = nullptr;
static inline CamTracker& CamT() { if (!g_camT) g_camT = new CamTracker(); return *g_camT; }
#define g_camFrame     (CamT().camFrame)
#define g_camRescan    (CamT().camRescan)
#define g_camLocked    (CamT().locked)
#define g_scanRuns     (CamT().scanRuns)
#define g_projSeen     (CamT().projSeen)
#define g_cutHold      (CamT().cutHold)
#define g_cutTotal     (CamT().cutTotal)
#define g_c2pGarbage   (CamT().c2pGarbage)
#define g_c2pBadTotal  (CamT().c2pBadTotal)
#define g_c2p          (CamT().c2p)
#define g_vtcNoAA      (CamT().vtcNoAA)
#define g_jitPX        (CamT().jitPX)
#define g_jitPY        (CamT().jitPY)
#define g_jitNX        (CamT().jitNX)
#define g_jitNY        (CamT().jitNY)
static CamEnv CamShimEnv() { CamEnv e; e.cfg = &g_cfg; e.frame = &g_frame; e.effW = EffW; e.effH = EffH; e.log = Log; e.dumpViewBuf = DumpViewBuf; return e; }
static CamParseResult g_lastParse;     // the result of the last CamParseCall (typed events: step 5 cross-checks them against the log-derived events)
static CamFrameResult g_lastFrame;
static inline bool CamParseCall(const float* p, int n, uint64_t bufId) { g_lastParse = CamT().Parse(CamShimEnv(), p, n, bufId); CamHostApply(g_lastParse); return g_lastParse.recognised; }
static inline CamFrameOut CamFrameCall(int fr) {
    CamFrameIn in; in.frame = fr; in.wasOn = g_wasOn; in.regMB = (long)(g_bufBytes >> 20); in.evicted = g_evicted.load(); in.regSkipped = g_regSkipped.load();
    g_lastFrame = CamT().Frame(CamShimEnv(), in); CamHostApplyFrame(g_lastFrame); return { g_lastFrame.camOk, g_lastFrame.delayOk, g_lastFrame.cutNow };
}
static inline void CamSetOffsets(int proj, int noaa, int mv) { g_cfg.projOff = proj; g_cfg.noAAOff = noaa; g_cfg.mvOff = mv; CamT().off.proj = proj; CamT().off.noaa = noaa; CamT().off.mv = mv; }
static inline void CamNewScenario() { delete g_camT; g_camT = nullptr; g_lastParse = CamParseResult(); g_lastFrame = CamFrameResult(); }
