// tests/camera_seq/test_camtracker_layout.cpp  (step 4, adapted in step 5) - the CamTracker LAYOUT.
//  1. DEFAULTS: a default-constructed CamTracker starts in exactly the state the 1.1.7 globals / function-local statics started in (pinned literals since step 5).
//  2. CamEventList: fixed capacity, overflow is counted, Has().
//  3. CamHostApply / CamHostApplyFrame: what the host does with a result == what the region used to do inline (g_forceReset, g_cfg offsets, CfgSetInt order and values).
//  4. The header compiles against the shim's Cfg / g_cfg / g_forceReset / CfgSetInt (the compile IS the contract check).
// usage: test_camtracker_layout            (no scenario selection: nothing here depends on one-per-process state; the legacy globals are only read)
#include "cam_shim.h"
#include "../../src/camtracker.h"
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// CamTracker::lim is private (step 6); CamLayoutProbe is its declared friend: the one place outside the tracker that may look at the log limiters
struct CamLayoutProbe {
    static bool LimitsDefault(const CamTracker& t) {
        const CamLogLimits& l = t.lim;
        for (int i = 0; i < 16; i++) if (l.seen[i] != 0) return false;
        return l.nseen == 0 && l.nlog == 0 && !l.listed && l.scanN == 0 && l.scanIgnoredN == 0 && l.legacyN == 0 && l.parseN == 0
            && l.monN == 0 && l.monNoUsableN == 0 && l.cutAltN == 0 && l.cutExplainedN == 0;
    }
};

static void TestDefaults() {
    // Step 5: the legacy globals no longer exist, so the 1.1.7 initial state is pinned as LITERALS (they were machine-compared against the real globals' initialisers in step 4).
    CamTracker* t = new CamTracker();
    CHECK(t->camFrame.load() == -1 && t->camRescan.load() == 0 && t->locked.load() == 0 && t->projSeen.load() == 0 && t->scanRuns.load() == 0);
    CHECK(t->cutHold.load() == 0 && !t->c2pGarbage.load() && t->c2pBadTotal.load() == 0 && t->lastJitFrame.load() == -1000 && t->lastRealViewFrame.load() == -100000);
    for (int i = 0; i < 4; i++) { CHECK(t->c2pCand[i] == -1); const C2pStat& a = t->c2pSt[i]; CHECK(a.active == 0 && a.maxDev == 0.0 && a.minDev == 1e9 && a.sumDev == 0.0 && a.bad == 0); }
    CHECK(t->c2pCandN == 0 && t->c2pStFrames == 0 && t->c2pLastFrame == -1 && t->c2pWinStart == 0 && t->c2pSwitches == 0);
    for (int i = 0; i < 8; i++) CHECK(t->c2pBanned[i] == -1 && t->c2pBanFrame[i] == 0);
    CHECK(t->c2pBannedN == 0);
    CHECK(t->prevM0 == 0.f && t->prevM5 == 0.f && t->prevCamFrame == -1);
    CHECK(t->cutWinStart == 0 && t->cutWinCount == 0 && t->cutSuppressUntil == 0 && t->cutTotal == 0);
    { SnProjHist ref; CHECK(t->projHist.n == ref.n); for (int i = 0; i < SnProjHist::N; i++) CHECK(t->projHist.m0[i] == ref.m0[i] && t->projHist.m5[i] == ref.m5[i] && t->projHist.fr[i] == ref.fr[i]); }
    for (int i = 0; i < 16; i++) CHECK(t->vtcNoAA[i] == 0.f && t->c2p[i] == 0.f);
    CHECK(t->jitPX == 0.f && t->jitPY == 0.f && t->jitNX == 0.f && t->jitNY == 0.f);
    // former function-local statics
    CHECK(t->li == -1 && t->lj == -1 && t->lk == -1 && t->hits == 0 && !t->prevBad);
    CHECK(!t->held && t->rescanStart == -1 && t->rescanCool == 0 && t->heldSince == 0 && t->camStreak == 0);
    CHECK(CamLayoutProbe::LimitsDefault(*t));
    // the working offsets default to the injector's config defaults (the shim's Cfg copies src/config.inc 1.1.7)
    Cfg c; CHECK(t->off.proj == c.projOff && t->off.noaa == c.noAAOff && t->off.mv == c.mvOff);
    t->Init({ 112, 128, 544 }); CHECK(t->off.proj == 112 && t->off.noaa == 128 && t->off.mv == 544);
    printf("  sizeof(CamTracker) = %zu bytes (one static instance in the DLL)\n", sizeof(CamTracker));
    delete t;
}
static void TestEventList() {
    CamEventList l; CHECK(l.n == 0 && l.dropped == 0 && !l.Has(CAMEV_CUT));
    for (int i = 0; i < CamEventList::CAP; i++) l.Add(CAMEV_CUT, 100 + i, i, -i);
    CHECK(l.n == CamEventList::CAP && l.dropped == 0 && l.Has(CAMEV_CUT) && !l.Has(CAMEV_LOCKED));
    CHECK(l.ev[0].frame == 100 && l.ev[7].frame == 107 && l.ev[7].a0 == 7 && l.ev[7].a1 == -7);
    l.Add(CAMEV_LOCKED, 1); l.Add(CAMEV_LOCKED, 2);
    CHECK(l.n == CamEventList::CAP && l.dropped == 2 && !l.Has(CAMEV_LOCKED));       // overflow is dropped AND counted, never overwrites
    CHECK(CAMEV_LOCKED == 1 && CAMEV_SWITCHED == 2 && CAMEV_CUT == 3 && CAMEV_LOST == 4 && CAMEV_CAND_IGNORED == 5 && CAMEV_ALT_VIEW == 6
          && CAMEV_HOLD == 7 && CAMEV_RESCAN == 8 && CAMEV_GARBAGE == 9 && CAMEV_GARBAGE_CLEARED == 10);   // == tests/camtrace.h snct::EV_*
}
static void TestHostApply() {
    g_shimCfgWrites.clear(); g_forceReset = false; g_cfg.projOff = 112; g_cfg.noAAOff = 128; g_cfg.mvOff = 544;
    { CamParseResult r; CamHostApply(r); CHECK(!g_forceReset.load() && g_shimCfgWrites.empty() && g_cfg.projOff == 112 && g_cfg.mvOff == 544); }   // an empty result touches nothing
    { CamParseResult r; r.offsetsChanged = true; r.offsets = { 116, 132, 492 }; r.persist = CAMP_PROJOFF | CAMP_NOAAOFF | CAMP_MVOFF;   // = the scan LOCK
      CamHostApply(r); CHECK(g_cfg.projOff == 116 && g_cfg.noAAOff == 132 && g_cfg.mvOff == 492 && !g_forceReset.load());
      CHECK(g_shimCfgWrites.size() == 3 && g_shimCfgWrites[0] == std::make_pair(std::string("projoff"), 116) && g_shimCfgWrites[1] == std::make_pair(std::string("noaaoff"), 132) && g_shimCfgWrites[2] == std::make_pair(std::string("mvoff"), 492)); }
    g_shimCfgWrites.clear();
    { CamParseResult r; r.offsetsChanged = true; r.offsets = { 116, 132, 420 }; r.persist = CAMP_MVOFF; r.forceReset = true;     // = the c2p SWITCH
      CamHostApply(r); CHECK(g_cfg.mvOff == 420 && g_cfg.projOff == 116 && g_forceReset.exchange(false));
      CHECK(g_shimCfgWrites.size() == 1 && g_shimCfgWrites[0] == std::make_pair(std::string("mvoff"), 420)); }
    g_shimCfgWrites.clear();
    { CamParseResult r; r.offsetsChanged = true; r.offsets = { 116, 132, 420 }; r.persist = 0; CamHostApply(r); CHECK(g_shimCfgWrites.empty()); }   // autolearn=0: mirrored, not persisted
    { CamParseResult r; r.forceReset = true; CamHostApply(r); CHECK(g_forceReset.exchange(false)); }                                          // = a camera cut
    { CamFrameResult f; CamHostApplyFrame(f); CHECK(!g_forceReset.load()); f.forceReset = true; CamHostApplyFrame(f); CHECK(g_forceReset.exchange(false)); }   // = long camhold released
}
int main() {
    TestDefaults(); TestEventList(); TestHostApply();
    if (g_fail) { printf("camtracker layout FAILED (%d)\n", g_fail); return 1; }
    printf("camtracker layout ok\n"); return 0;
}
