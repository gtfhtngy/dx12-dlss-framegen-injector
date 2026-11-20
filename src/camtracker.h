// camtracker.h  (step 5: the tracker's declarations + state; the bodies of Parse / Frame and their helpers live in main.cpp between the SN_EXTRACT markers)
//
// The camera tracker today = two marked regions of main.cpp (SN_EXTRACT camera: ScanViewBuffer / TryLegacyLayout / C2pMonitor / CamCutCheck / ParseViewBuffer,
// and SN_EXTRACT camhold: the per-frame bookkeeping) plus ~50 globals / function-local statics. Step 5 moves that code into CamTracker::Parse / CamTracker::Frame
// WITHOUT changing decisions. This header fixes the target shape first, so the move is mechanical:
//
//   * ALL mutable tracker state is a member (STATE-MAP below; tests/camera_seq/check_state_inventory.py fails if a static of either region is not listed).
//   * The tracker never touches host globals. What it used to do to the outside world is reported in a result struct and applied by the HOST (CamHostApply below):
//       g_forceReset = true          -> CamParseResult.forceReset / CamFrameResult.forceReset
//       g_cfg.projOff/noAAOff/mvOff  -> CamParseResult.offsets (+ offsetsChanged); the tracker owns the working offsets in `off`, g_cfg keeps a MIRROR for readers outside the tracker
//       CfgSetInt("projoff"...)      -> CamParseResult.persist bits (set only when cfg->autoLearn)
//       Log(...)                     -> still env.log (text unchanged: tests pin it) AND a typed event (CamEvent) per decision
//   * Configuration is read LIVE through env.cfg, never copied: camhold / cutdetect are toggled from the overlay menu and camstale by presets while the game runs
//     (overlay.inc:177/178, 148); a snapshot taken at Init() would silently freeze those.
//
// Contract with the host (compiler-checked): a type `Cfg` with the int members listed in CamEnv below, plus host symbols g_cfg, g_forceReset, CfgSetInt (CamHostApply).
// main.cpp has them from config.inc / cfgsave.inc; tests/camera_seq/cam_shim.h defines the same names.
#pragma once
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstddef>
#include "logic.h"      // SnProjHist
#include "camcore.h"    // Cam* pure helpers (used by the step-5 code)

// ------------------------------------------------------------------------------------------------ events
// Numbers 0..10 == tests/camtrace.h snct::EV_* (EXPECT records of existing .snct scenarios stay valid). 11+ are tracker-only for now (camtrace.h gets them if a scenario needs an EXPECT).
// a0 / a1 per kind (as the replay tests already pin them):
//   LOCKED a0=projOff a1=mvOff | SWITCHED a0=mvOff before a1=mvOff after | LOST a0=camera age | HOLD a0=frames held a1=1 if history reset | RESCAN a0=camera age
//   RESCAN_FOUND a0=0 | RESCAN_GIVEUP a0=frames searched | HOLD_ENDED a0=age | CUT_SUSPENDED a0=0 | C2P_BANNED a0=mvOff float index
enum CamEvKind : uint16_t {
    CAMEV_NONE = 0, CAMEV_LOCKED = 1, CAMEV_SWITCHED = 2, CAMEV_CUT = 3, CAMEV_LOST = 4, CAMEV_CAND_IGNORED = 5,
    CAMEV_ALT_VIEW = 6, CAMEV_HOLD = 7, CAMEV_RESCAN = 8, CAMEV_GARBAGE = 9, CAMEV_GARBAGE_CLEARED = 10,
    CAMEV_RESCAN_FOUND = 11, CAMEV_RESCAN_GIVEUP = 12, CAMEV_HOLD_ENDED = 13, CAMEV_CUT_SUSPENDED = 14, CAMEV_C2P_BANNED = 15
};
struct CamEvent { uint16_t kind = CAMEV_NONE; int32_t frame = 0, a0 = 0, a1 = 0; };
// Fixed capacity (no allocation on the probe path). By reading the code, one Parse() call can produce at most 5 events (BANNED, SWITCHED, GARBAGE*, one of ALT_VIEW/CUT/CUT_SUSPENDED, RESCAN_FOUND)
// and one Frame() call at most 4 (RESCAN, RESCAN_GIVEUP, LOST, HOLD/HOLD_ENDED); CAP is 8. `dropped` counts overflow: step 5 asserts it is 0 in every scenario (that turns this reading into a test).
struct CamEventList {
    static const int CAP = 8; CamEvent ev[CAP]; uint8_t n = 0, dropped = 0;
    void Add(uint16_t kind, int frame, int a0 = 0, int a1 = 0) { if (n < CAP) { ev[n].kind = kind; ev[n].frame = frame; ev[n].a0 = a0; ev[n].a1 = a1; n++; } else if (dropped < 255) dropped++; }
    bool Has(uint16_t kind) const { for (int i = 0; i < n; i++) if (ev[i].kind == kind) return true; return false; }
};

// ------------------------------------------------------------------------------------------------ results
struct CamOffsets { int proj = 116, noaa = 132, mv = 492; };     // float indices into the view uniform buffer (defaults = src/config.inc)
enum CamPersist : uint8_t { CAMP_PROJOFF = 1, CAMP_NOAAOFF = 2, CAMP_MVOFF = 4 };

// What one ParseViewBuffer call did to the world. Replaces: return value, g_forceReset writes, g_cfg offset writes, CfgSetInt calls.
struct CamParseResult {
    bool recognised = false;          // the old bool return: "this buffer was recognised as a camera view buffer"
    bool forceReset = false;          // host: g_forceReset = true  (camera cut, ClipToPrevClip SWITCH)
    bool offsetsChanged = false;      // scan LOCK or c2p SWITCH changed the working offsets -> host mirrors `offsets` into g_cfg
    CamOffsets offsets;               //   valid only when offsetsChanged (always the COMPLETE current triple, so the host mirror is one assignment)
    uint8_t persist = 0;              // CamPersist bits: host CfgSetInt()s these offsets. Set only when cfg->autoLearn (decided by the tracker, as today)
    int  cutHold = 0;                 // snapshot after the call (== cutHold member)
    bool c2pGarbage = false;          // snapshot after the call (== c2pGarbage member)
    CamEventList events;
};
// What one frame of per-frame bookkeeping did (the camhold region). Inputs come from the frame loop, outputs replace its locals camOk / delayOk / cutNow.
struct CamFrameIn {
    int  frame = 0;
    bool wasOn = false;               // FG was running last frame (g_wasOn)
    long regMB = 0, evicted = 0, regSkipped = 0;   // only used as Log() arguments of the "camera lost for N frames" line
};
struct CamFrameResult {
    bool camOk = false;               // camera usable this frame (a camhold counts as ok)
    bool delayOk = false;             // FG may (re)start: already running, or the view was stable for fgdelay frames
    bool cutNow = false;              // a camera cut is being held out this frame -> the frame is sent with reset
    int  camAge = 0;                  // frame - camFrame, computed at the top of Frame() (the host logs it)
    bool forceReset = false;          // long camhold released -> host: g_forceReset = true
    CamEventList events;
};

// ------------------------------------------------------------------------------------------------ env (everything the tracker may read from / call into the host)
struct CamEnv {
    const Cfg* cfg = nullptr;                          // LIVE config. Fields read: c2pIdx c2pAuto camAuto autoLearn cutDetect cutFov jitSX jitSY camStale camRescan camHold camHoldMax fgDelay
                                                       // (projOff/noAAOff/mvOff are NOT read from here after Init(): the tracker owns the working offsets)
    const std::atomic<int>* frame = nullptr;           // g_frame, read live (a mid-call frame advance behaves exactly as today; snapshotting it would be a deliberate step-6 change)
    int  (*effW)() = nullptr; int (*effH)() = nullptr; // EffW() / EffH()
    void (*log)(const char* fmt, ...) = nullptr;       // Log()
    void (*dumpViewBuf)(const float* p, int availF, uint64_t off, const char* why) = nullptr;   // DumpViewBuf (file I/O: stays in the host, the extractor skips it)
};

// ------------------------------------------------------------------------------------------------ the tracker
// STATE-MAP-BEGIN   (machine-checked by tests/camera_seq/check_state_inventory.py against the extracted regions and cam_shim.h)
//   camera ::g_projSeen                  -> projSeen
//   camera ::g_scanRuns                  -> scanRuns
//   camera ::g_camLocked                 -> locked
//   camera ::g_lastJitFrame              -> lastJitFrame
//   camera ::g_c2pCand                   -> c2pCand
//   camera ::g_c2pCandN                  -> c2pCandN
//   camera ::g_lastRealViewFrame         -> lastRealViewFrame
//   camera ::g_c2pSt                     -> c2pSt
//   camera ::g_c2pStFrames               -> c2pStFrames
//   camera ::g_c2pLastFrame              -> c2pLastFrame
//   camera ::g_c2pWinStart               -> c2pWinStart
//   camera ::g_c2pGarbage                -> c2pGarbage
//   camera ::g_c2pSwitches               -> c2pSwitches
//   camera ::g_c2pBadTotal               -> c2pBadTotal
//   camera ::g_c2pBanned                 -> c2pBanned
//   camera ::g_c2pBanFrame               -> c2pBanFrame
//   camera ::g_c2pBannedN                -> c2pBannedN
//   camera ::g_cutHold                   -> cutHold
//   camera ::g_prevM0                    -> prevM0
//   camera ::g_prevM5                    -> prevM5
//   camera ::g_prevCamFrame              -> prevCamFrame
//   camera ::g_cutWinStart               -> cutWinStart
//   camera ::g_cutWinCount               -> cutWinCount
//   camera ::g_cutSuppressUntil          -> cutSuppressUntil
//   camera ::g_cutTotal                  -> cutTotal
//   camera ::g_projHist                  -> projHist
//   (dropped in step 6, never ran since v16: ScanViewBuffer::s_frame / s_budget - the per-frame scan budget; probes are capped in TrackCamera)
//   camera ScanViewBuffer::li            -> li
//   camera ScanViewBuffer::lj            -> lj
//   camera ScanViewBuffer::lk            -> lk
//   camera ScanViewBuffer::hits          -> hits
//   camera CamCutCheck::s_prevBad        -> prevBad
//   camera ScanViewBuffer::seen          -> lim.seen
//   camera ScanViewBuffer::nseen         -> lim.nseen
//   camera ScanViewBuffer::nlog          -> lim.nlog
//   camera ScanViewBuffer::s_listed      -> lim.listed
//   camera ScanViewBuffer::n             -> lim.scanN
//   camera ScanViewBuffer::s_nl          -> lim.scanIgnoredN
//   camera TryLegacyLayout::n            -> lim.legacyN
//   camera C2pMonitor::s_nlog            -> lim.monN
//   camera C2pMonitor::w                 -> lim.monNoUsableN
//   camera CamCutCheck::s_alt            -> lim.cutAltN
//   camera CamCutCheck::s_nz             -> lim.cutExplainedN
//   camera ParseViewBuffer::n            -> lim.parseN
//   stay   ::g_cbvCalls                  -> (host global: ProbeView statistics; shares a declaration line with g_projSeen today - split it in step 5)
//   stay   ::g_cbvMiss                   -> (host global: ProbeView statistics)
//   camhold Frame::s_held                -> held
//   camhold Frame::s_rescanStart         -> rescanStart
//   camhold Frame::s_rescanCool          -> rescanCool
//   camhold Frame::s_heldSince           -> heldSince
//   camhold Frame::s_camStreak           -> camStreak
//   ext    ::g_camFrame                  -> camFrame
//   ext    ::g_camRescan                 -> camRescan
//   ext    ::g_camMx                     -> camMx
//   ext    ::g_vtcNoAA                   -> vtcNoAA
//   ext    ::g_c2p                       -> c2p
//   ext    ::g_jitPX                     -> jitPX
//   ext    ::g_jitPY                     -> jitPY
//   ext    ::g_jitNX                     -> jitNX
//   ext    ::g_jitNY                     -> jitNY
// (not tracker state, stays as is: g_frame, g_cfg, g_forceReset (many other writers: hitch, MV re-init, FG resume), g_wasOn (frame-loop input -> CamFrameIn), g_bufBytes/g_evicted/g_regSkipped (log arguments -> CamFrameIn))
// STATE-MAP-END
struct C2pStat { int active = 0; double maxDev = 0.0; double minDev = 1e9; double sumDev = 0.0; int bad = 0; };   // moved here unchanged from main.cpp (step 5)

// Log rate limiters. They only gate Log() lines (audited in step 3, finding 2: nothing reads them for a decision), grouped so the logic members stay readable.
struct CamLogLimits {
    unsigned seen[16] = {}; int nseen = 0, nlog = 0;   // ScanViewBuffer: de-dup of "no jittered/NoAA pair" lines
    bool listed = false;                               // ScanViewBuffer: the one-time candidate list
    int scanN = 0, scanIgnoredN = 0;                   // ScanViewBuffer: "candidate"/"no c2p" lines (shared counter, as today), "ignored" lines
    int legacyN = 0, parseN = 0;                       // TryLegacyLayout / ParseViewBuffer first-N lines
    int monN = 0, monNoUsableN = 0;                    // C2pMonitor: window lines, "no usable candidate" lines
    int cutAltN = 0, cutExplainedN = 0;                // CamCutCheck: "alternating views" / "FOV change explained" lines
};

struct CamTracker {
    // ---- A. published: written by the tracker (worker threads / frame loop), read by OTHER threads (ProbeView, overlay, diag, hudcal, MV pass)
    std::atomic<int>  camFrame{ -1 };            // frame of the last accepted view buffer (-1 = never). ONLY the strict parse and the legacy layout set it.
    std::atomic<int>  camRescan{ 0 };            // 1 = camera lost for a while: relaxed checks + scanning although offsets were locked (armed by Frame(), cleared by Parse() / Frame())
    std::atomic<int>  locked{ 0 };               // set ONLY by the scan lock (never by offsets that came from the cfg file) -> C2pMonitor does not run after a cfg-file start (step 3 finding 3, pinned by c2p_monitor_needs_lock)
    std::atomic<long> projSeen{ 0 }, scanRuns{ 0 };   // diagnostics counters (overlay / cam-diag)
    std::atomic<int>  cutHold{ 0 };              // camera-cut hold countdown; raised by CutCheck, decremented by Frame()
    std::atomic<bool> c2pGarbage{ false };       // the USED ClipToPrevClip is mostly implausible -> no cut resets, identity only
    std::atomic<int>  c2pBadTotal{ 0 };          // implausible ClipToPrevClip frames since start
    std::atomic<int>  lastJitFrame{ -1000 };     // last frame with a jittered projection
    std::atomic<int>  lastRealViewFrame{ -100000 };   // legacy layout: last frame with a non-dummy view

    // ---- B. the published camera block. Guarded by camMx (as g_camMx today); written by Parse() and by Frame() (camhold writes identity), read by the MV pass
    std::mutex camMx;
    float vtcNoAA[16] = {}, c2p[16] = {};
    float jitPX = 0.f, jitPY = 0.f, jitNX = 0.f, jitNY = 0.f;

    // ---- C. working offsets. OWNED by the tracker; g_cfg.projOff/noAAOff/mvOff is a mirror written by CamHostApply (readers: ProbeView cap, overlay, diag, cam-diag log)
    CamOffsets off;                              // Init() sets it from g_cfg after LoadCfg(); the scan LOCK and the c2p SWITCH change it

    // ---- D. scan state (ScanViewBuffer)
    int c2pCand[4] = { -1, -1, -1, -1 }; int c2pCandN = 0;   // identity-like matrices found after ViewToClipNoAA at the last scan (float offsets); the monitor evaluates every one
    // Lock needs the same candidate 5 times in a row. `hits` restarts on "ignored" (li/lj/lk stay), on a different candidate, and - since step 6 - on a successful lock, which also clears li/lj/lk:
    // a later re-scan has to see its candidate 5x again (until 1.1.7 it re-locked on its FIRST call). Pinned by hits_cleared_on_lock (was hits_carryover).
    int li = -1, lj = -1, lk = -1, hits = 0;

    // ---- E. c2p monitor
    C2pStat c2pSt[4];
    int c2pStFrames = 0, c2pLastFrame = -1, c2pWinStart = 0;
    int c2pSwitches = 0;
    int c2pBanned[8] = { -1, -1, -1, -1, -1, -1, -1, -1 }; int c2pBanFrame[8] = {}; int c2pBannedN = 0;   // offsets proven invalid; a ban expires after 1800 frames

    // ---- F. camera-cut guard
    float prevM0 = 0.f, prevM5 = 0.f; int prevCamFrame = -1;
    int cutWinStart = 0, cutWinCount = 0, cutSuppressUntil = 0, cutTotal = 0;
    SnProjHist projHist;                         // the last distinct projections: alternating views are not camera cuts
    bool prevBad = false;                        // previous frame's ClipToPrevClip was implausible: only the FIRST bad frame of a run is a cut (1.1.6b)

    // ---- G. camhold / rescan bookkeeping (the second region: per-frame, runs in the frame loop)
    bool held = false; int rescanStart = -1, rescanCool = 0, heldSince = 0, camStreak = 0;

    // ---- H. log limiters (no decision reads these) - private: only the tracker's own code counts lines; the layout test reads the defaults through CamLayoutProbe
  private:
    CamLogLimits lim;
    friend struct CamLayoutProbe;
  public:

    // ---- interface (bodies: main.cpp, between the SN_EXTRACT markers)
    void Init(const CamOffsets& o) { off = o; }  // call once after LoadCfg()
    CamParseResult Parse(const CamEnv& env, const float* p, int availF, uint64_t bufOff);   // = ParseViewBuffer (+ ScanViewBuffer / TryLegacyLayout / C2pMonitor / CamCutCheck)
    CamFrameResult Frame(const CamEnv& env, const CamFrameIn& in);                          // = the camhold region
  private:
    // helpers of Parse() (former free functions ScanViewBuffer / TryLegacyLayout / C2pMonitor / CamCutCheck / C2pIsBanned / IsProj; ParseBody = the old ParseViewBuffer body)
    bool ParseBody(const CamEnv& env, CamParseResult& R, const float* p, int availF, uint64_t bufOff);
    bool IsProj(const CamEnv& env, const float* m, bool loose) const;
    void Scan(const CamEnv& env, CamParseResult& R, const float* p, int avail, uint64_t bufOff);
    bool TryLegacy(const CamEnv& env, const float* p, int availF);
    bool IsBanned(const CamEnv& env, int offF) const;
    void Monitor(const CamEnv& env, CamParseResult& R, const float* p, int availF, int fr);
    void CutCheck(const CamEnv& env, CamParseResult& R, const float* b, const float* c, int fr);
  public:

    CamTracker() = default;
    CamTracker(const CamTracker&) = delete; CamTracker& operator=(const CamTracker&) = delete;   // holds a mutex and atomics; tests make one per scenario (`new CamTracker`) instead of one process per scenario
};

// ------------------------------------------------------------------------------------------------ host side: apply a result to the world.
// ONE definition for production and tests (the shim defines the same host names), so "what the host does with a result" is itself tested.
// Threading: runs on the thread that called Parse() (a game worker thread), right after it - the same thread that used to do these writes from inside the region.
static inline void CamHostApply(const CamParseResult& r) {
    if (r.forceReset) g_forceReset = true;
    if (r.offsetsChanged) { g_cfg.projOff = r.offsets.proj; g_cfg.noAAOff = r.offsets.noaa; g_cfg.mvOff = r.offsets.mv; }
    if (r.persist & CAMP_PROJOFF) CfgSetInt("projoff", r.offsets.proj);
    if (r.persist & CAMP_NOAAOFF) CfgSetInt("noaaoff", r.offsets.noaa);
    if (r.persist & CAMP_MVOFF)   CfgSetInt("mvoff", r.offsets.mv);
}
static inline void CamHostApplyFrame(const CamFrameResult& r) { if (r.forceReset) g_forceReset = true; }
