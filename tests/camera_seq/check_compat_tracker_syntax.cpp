// tests/camera_seq/check_compat_tracker_syntax.cpp (step 4) - compiled with -DSN_CAM_USE_TRACKER -fsyntax-only (NOT linked: CamTracker::Parse/Frame get their bodies in step 5).
// Proves that cam_compat.h's TRACKER mode is well-formed C++ against src/camtracker.h and that every name the tests use exists there, so step 5 starts from a compiling shim.
#ifndef SN_CAM_USE_TRACKER
#error compile with -DSN_CAM_USE_TRACKER
#endif
#include "cam_shim.h"
#include "cam_compat.h"
int main() {
    CamNewScenario(); CamSetOffsets(112, 128, 544);
    std::vector<float> b(700, 0.f); bool ok = CamParseCall(b.data(), (int)b.size(), 7); CamFrameOut fo = CamFrameCall(1);
    g_camRescan = 1; g_camFrame = 3; g_c2p[0] = 1.f; g_jitPX = 0.f;
    return (int)ok + (int)fo.camOk + g_camLocked.load() + (int)g_scanRuns.load() + g_cutHold.load() + g_cutTotal + (int)g_c2pGarbage.load() + g_c2pBadTotal.load() + (int)g_lastParse.events.n + (int)g_lastFrame.events.n + (int)g_cfg.mvOff + (int)g_vtcNoAA[0] + (int)g_projSeen.load() + (int)g_jitNX + (int)g_jitNY + (int)g_jitPY;
}
