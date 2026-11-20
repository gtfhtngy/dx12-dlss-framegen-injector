// tests/camera_seq/test_extract_smoke.cpp - proves that the camera region extracted from the REAL src/main.cpp builds and runs on Linux, and pins facts of 1.1.7 (two of them deliberately changed in step 6: see scan_no_budget / hits_cleared_on_lock).
// Scenarios run one per process (kept from step 3; since step 5 nothing is function-local static any more, the tracker could be re-created):  test_extract_smoke <strict|lock5|scan_no_budget|hits_cleared_on_lock>   (function-local statics in the region cannot be reset - see STEP1 doc section 4).
// These are CHARACTERIZATION tests: they assert what 1.1.7 does today. A deliberate behaviour change must flip the matching assertion in the same commit.
#include "cam_shim.h"
#include "cam_compat.h"             // step 4: the call surface (CamParseCall / CamSetOffsets / ...) shared with the step-5 tracker mode
#include "../camtrace.h"
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static void Put(std::vector<float>& b, int at, std::initializer_list<float> v) { int i = at; for (float x : v) b[i++] = x; }
// Scarlet Nexus-like UE4 view buffer: ViewToClip(jittered) @po, ViewToClipNoAA @no, ClipToPrevClip @mo (identity-like, tiny camera move)
static std::vector<float> Ue4(int po, int no, int mo, float jx = 0.0007f, float jy = -0.0012f) {
    std::vector<float> b(700, 0.f); const float m0 = 0.99999f, m5 = 1.7778f;
    Put(b, po, { m0, 0, 0, 0, 0, m5, 0, 0, jx, jy, 0, 1, 0, 0, 0.1f, 0 });
    Put(b, no, { m0, 0, 0, 0, 0, m5, 0, 0, 0, 0, 0, 1, 0, 0, 0.1f, 0 });
    Put(b, mo, { 1, 0.0001f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0002f, -0.0001f, 0, 1 });
    return b;
}
int main(int argc, char** argv) {
    const std::string sc = argc > 1 ? argv[1] : "";
    g_shimW = 1920; g_shimH = 1080;
    if (sc == "strict") {            // offsets already right: accepted on the first call, jitter converted to pixels, no scan needed
        auto b = Ue4(116, 132, 492); g_frame = 1000;
        CHECK(CamParseCall(b.data(), (int)b.size(), 0));
        CHECK(g_camFrame.load() == 1000 && g_scanRuns.load() == 0);
        CHECK(fabsf(g_jitPX - 0.0007f * 0.5f * 1920.f) < 1e-3f);
    } else if (sc == "lock5") {      // 1.1.6 log offsets 112/128/544 are wrong for this buffer: the scan needs the SAME candidate 5 times, then locks and saves 3 cfg values
        CamSetOffsets(112, 128, 544);
        for (int f = 1100; f < 1105; f++) {
            auto b = Ue4(116, 132, 492, 0.0007f * (float)(f % 3)); g_frame = f;
            CHECK(!CamParseCall(b.data(), (int)b.size(), 7));                          // not accepted while scanning
            CHECK(g_camLocked.load() == (f == 1104 ? 1 : 0));                             // locked by the 5th call
        }
        CHECK(g_cfg.projOff == 116 && g_cfg.noAAOff == 132 && g_cfg.mvOff == 492);
        CHECK(g_shimCfgWrites.size() == 3);                                               // autolearn: projoff, noaaoff, mvoff
        { auto b = Ue4(116, 132, 492); g_frame = 1105; CHECK(CamParseCall(b.data(), (int)b.size(), 7) && g_camFrame.load() == 1105); }   // next call takes the strict path
    } else if (sc == "scan_no_budget") {   // step 6: the v16 per-frame budget (it sat inside a // comment, so it never ran) was DELETED, not repaired. Probes are capped upstream in TrackCamera (24/frame, 96 while the camera is lost).
        CamSetOffsets(112, 128, 544);                      // behaviour is unchanged since 1.1.7: 40 scans in one frame all run. (Introducing a budget here would be a new decision: change the number AND the release notes.)
        std::vector<float> zeros(700, 0.f); g_frame = 2000;
        for (int i = 0; i < 40; i++) CamParseCall(zeros.data(), (int)zeros.size(), (UINT64)i);
        CHECK(g_scanRuns.load() == 40);
    } else if (sc == "hits_cleared_on_lock") {   // step 6 (behaviour change vs 1.1.7, release note): a successful lock clears li/lj/lk/hits, so a later re-scan on the SAME candidate needs 5 calls again (1.1.7 re-locked on the first)
        CamSetOffsets(112, 128, 544);
        for (int f = 1100; f < 1105; f++) { auto b = Ue4(116, 132, 492, 0.0007f * (float)(f % 3)); g_frame = f; CamParseCall(b.data(), (int)b.size(), 7); }
        CHECK(g_camLocked.load() == 1 && g_cfg.projOff == 116 && g_shimCfgWrites.size() == 3);                 // first lock: needed 5 equal candidates
        // same synthetic trigger as before: the offsets in force are wrong again (e.g. cfg reloaded) and rescan mode is armed, as the frame loop does after camStale/camRescan frames without a camera
        CamSetOffsets(112, 128, 544); g_camRescan = 1; g_shimCfgWrites.clear(); size_t lg = g_shimLog.size();
        for (int f = 1400; f < 1405; f++) {                                                                    // SAME candidate (116/132/492), five calls
            auto b = Ue4(116, 132, 492, 0.0007f * (float)(f % 3)); g_frame = f; CHECK(!CamParseCall(b.data(), (int)b.size(), 7));
            if (f < 1404) { CHECK(g_cfg.projOff == 112 && g_shimCfgWrites.empty() && g_camRescan.load() == 1); }   // calls 1-4: NOT re-locked (1.1.7: re-locked on call 1)
        }
        CHECK(g_cfg.projOff == 116 && g_cfg.noAAOff == 132 && g_cfg.mvOff == 492);                             // call 5 locks
        CHECK(g_shimCfgWrites.size() == 3 && g_shimCfgWrites[0].first == "projoff" && g_shimCfgWrites[2].first == "mvoff");
        CHECK(g_camRescan.load() == 0);                                                                         // rescan flag consumed by the re-lock
        { int cand = 0; bool found = false, locked = false; for (size_t i = lg; i < g_shimLog.size(); i++) { const std::string& t = g_shimLog[i].text;
              if (t.find("cam-scan: candidate projoff=116") != std::string::npos) cand++;                        // the cleared li/lj/lk make the candidate "new" again: its (existing) log line shows once more
              if (t.find("camera re-scan: view buffer found again at projoff=116") != std::string::npos) found = true;
              if (t.find("cam-scan: LOCKED projoff=116") != std::string::npos) locked = true; }
          CHECK(cand == 1 && found && locked); }
        // control (unchanged): a DIFFERENT candidate (120/136/496) needs 5 calls as well
        CamSetOffsets(112, 128, 544); g_camRescan = 1; g_shimCfgWrites.clear();
        for (int f = 1500; f < 1505; f++) { auto b = Ue4(120, 136, 496, 0.0007f * (float)(f % 3)); g_frame = f; CamParseCall(b.data(), (int)b.size(), 7);
            if (f < 1504) { CHECK(g_cfg.projOff == 112 && g_shimCfgWrites.empty() && g_camRescan.load() == 1); } }
        CHECK(g_cfg.projOff == 120 && g_cfg.noAAOff == 136 && g_cfg.mvOff == 496 && g_shimCfgWrites.size() == 3 && g_camRescan.load() == 0);   // locked by the 5th call
    } else { printf("usage: test_extract_smoke <strict|lock5|scan_no_budget|hits_cleared_on_lock>\n"); return 2; }
    if (g_fail) { printf("camera smoke '%s' FAILED (%d)\n", sc.c_str(), g_fail); return 1; }
    printf("camera smoke '%s' ok\n", sc.c_str()); return 0;
}
