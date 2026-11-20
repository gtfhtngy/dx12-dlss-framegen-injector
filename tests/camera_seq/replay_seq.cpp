// tests/camera_seq/replay_seq.cpp - STEP 3: replays SNCT call sequences through the REAL camera region of src/main.cpp (camera_extract.inc, see extract_camera_code.py).
//
//   replay_seq <scenario>                       run one behaviour scenario (ONE PER PROCESS: function-local statics of the region cannot be reset)
//   replay_seq --file <trace.snct> [-v]         replay a captured / generated trace; EXPECT records are checked
//   replay_seq --write <scenario> <out.snct>    write a scenario as SNCT (+ <out>.txt text dump) for review
//   replay_seq --gate [datadir]                 sparse-vs-full gate (see below), spawns itself once per variant
//   replay_seq --list
//
// What is REAL and what is a MODEL:
//   REAL  : everything inside ParseViewBuffer (scan/lock, strict path, legacy layout, c2p monitor, cut check) - the text of main.cpp, compiled unchanged.
//   REAL  : DriverFrame() = the per-frame bookkeeping (camAge/camStale, rescan arming, camhold, g_cutHold decay), extracted from main.cpp between the camhold markers (step 3b).
#include "cam_shim.h"
#include "cam_compat.h"             // step 4: the call surface shared with the step-5 tracker mode
#include "../camtrace.h"
#include <functional>
#include <set>
#include <map>
#include <string>
#include <cstdlib>
#include <sstream>
using namespace snct;

// ---------------------------------------------------------------- events observed during a replay
struct Ev { int frame; uint16_t ev; int a0, a1; };
static int g_tevOverflow = 0;
static std::vector<Ev> g_ev; static int g_forceResets = 0; static std::string g_sig;
static const char* EvName(int e) { static const char* n[] = { "NONE","LOCKED","SWITCHED","CUT","LOST","CAND_IGNORED","ALT_VIEW","HOLD","RESCAN","GARBAGE","GARBAGE_CLEARED" }; return (e >= 0 && e < 11) ? n[e] : "?"; }
// step 5: the TYPED events (CamParseResult / CamFrameResult .events) of every call, collected next to the log-derived g_ev above and cross-checked in CrossCheckTyped()
static std::vector<Ev> g_tev; static int g_maxParseEv = 0, g_maxFrameEv = 0;
static void CollectTyped(int fr, const CamEventList& l, bool isParse) { (void)fr; for (int i = 0; i < l.n; i++) g_tev.push_back({ l.ev[i].frame, l.ev[i].kind, l.ev[i].a0, l.ev[i].a1 }); if (isParse) g_maxParseEv = std::max(g_maxParseEv, (int)l.n); else g_maxFrameEv = std::max(g_maxFrameEv, (int)l.n);
    if (l.dropped) { printf("FAIL typed event list overflowed at frame %d (dropped %d)\n", fr, (int)l.dropped); g_tevOverflow++; } }
static void Emit(int fr, uint16_t e, int a0 = 0, int a1 = 0) { g_ev.push_back({ fr, e, a0, a1 }); char b[160]; snprintf(b, sizeof b, "E f=%d %s %d %d\n", fr, EvName(e), a0, a1); g_sig += b; }

// REAL (step 3b): the per-frame camera bookkeeping of main.cpp (camAge / camStale, rescan arming, camhold, cut-hold decay) is extracted between
// SN_EXTRACT_BEGIN/END camhold and compiled unchanged as the body of CamFrameCall() (cam_compat.h, legacy mode); DriverFrame() calls it. Its statics (s_held, s_rescanStart, ...) are function-local = one scenario per process.
// The only test-side code is the event mapping from the region's own log lines. Not covered: the FG pacing logic around it (wantOn, stall hold, fgminfps).
static void DriverFrame(int fr) {
    size_t lb = g_shimLog.size();
    CamFrameOut fo = CamFrameCall(fr);   // = CamTracker::Frame (the real camhold region of main.cpp, extracted)
    (void)fo; CollectTyped(fr, g_lastFrame.events, false);
    for (size_t i = lb; i < g_shimLog.size(); i++) { const std::string& s = g_shimLog[i].text; g_sig += "L f" + std::to_string(g_shimLog[i].frame) + " " + s + "\n"; int a = 0, b = 0;
        if (sscanf(s.c_str(), "camera lost for %d frames", &a) == 1) Emit(fr, EV_RESCAN, a);
        else if (sscanf(s.c_str(), "camera lost (age %d) while FG", &a) == 1) Emit(fr, EV_LOST, a);
        else if (sscanf(s.c_str(), "camera found again (frame %d) -> camhold released after %d frames", &a, &b) == 2) Emit(fr, EV_HOLD, b, s.find("history reset") != std::string::npos ? 1 : 0); }
}

// ---------------------------------------------------------------- replay engine
static bool Has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
static void ApplyHeader(const Header& h) {
    CamSetOffsets(h.projOff, h.noAAOff, h.mvOff); g_cfg.c2pIdx = h.c2pIdx; g_cfg.camAuto = h.camAuto; g_cfg.c2pAuto = h.c2pAuto;
    g_cfg.autoLearn = h.autoLearn; g_cfg.cutDetect = h.cutDetect; g_cfg.cutFov = h.cutFov; g_cfg.jitSX = h.jitSX; g_cfg.jitSY = h.jitSY;
}
static void ObserveCall(const Call& c, bool ret, int mvBefore, bool garbBefore, int cutBefore, size_t logBefore) {
    int fr = c.frame; char b[400];
    snprintf(b, sizeof b, "C f=%d buf=%08x ret=%d camFrame=%d locked=%d off=%d/%d/%d jit=%.6g,%.6g c2p=%.6g,%.6g,%.6g,%.6g cut=%d hold=%d scans=%ld\n", fr, c.bufId, (int)ret, g_camFrame.load(), g_camLocked.load(),
        g_cfg.projOff, g_cfg.noAAOff, g_cfg.mvOff, g_jitPX, g_jitPY, g_c2p[0], g_c2p[5], g_c2p[10], g_c2p[15], g_cutTotal, g_cutHold.load(), g_scanRuns.load()); g_sig += b;
    for (size_t i = logBefore; i < g_shimLog.size(); i++) {
        const std::string& s = g_shimLog[i].text; g_sig += "L f" + std::to_string(g_shimLog[i].frame) + " " + s + "\n";
        if (Has(s, "cam-scan: LOCKED")) Emit(fr, EV_LOCKED, g_cfg.projOff, g_cfg.mvOff);
        else if (Has(s, "c2p-monitor: SWITCH")) Emit(fr, EV_SWITCHED, mvBefore, g_cfg.mvOff);
        else if (Has(s, "cam-scan: candidate") && Has(s, "ignored")) Emit(fr, EV_CAND_IGNORED);
        else if (Has(s, "alternating views")) Emit(fr, EV_ALT_VIEW);
    }
    if (g_cutTotal > cutBefore) Emit(fr, EV_CUT);
    if (g_c2pGarbage.load() != garbBefore) Emit(fr, g_c2pGarbage.load() ? EV_GARBAGE : EV_GARBAGE_CLEARED);
    if (g_forceReset.exchange(false)) { g_forceResets++; g_sig += "R f=" + std::to_string(fr) + "\n"; }   // the injector's frame loop consumes it
}
static void Replay(const Trace& t) {
    ApplyHeader(t.h); std::vector<float> buf; size_t i = 0, n = t.items.size();
    int fr0 = t.h.startFrame, last = fr0; for (const Item& it : t.items) if (it.type == REC_CALL) last = std::max(last, (int)it.call.frame);
    int first = last; for (const Item& it : t.items) if (it.type == REC_CALL) first = std::min(first, (int)it.call.frame);
    for (int fr = first; fr <= last; fr++) {
        for (size_t k = 0; k < n; k++) { const Item& it = t.items[k]; if (it.type != REC_CALL || it.call.frame != fr) continue;
            const Call& c = it.call; g_frame = fr; g_shimW = c.effW; g_shimH = c.effH; Materialize(c, buf);
            int mvB = g_cfg.mvOff; bool gB = g_c2pGarbage.load(); int cutB = g_cutTotal; size_t lB = g_shimLog.size();
            bool r = CamParseCall(buf.data(), (int)buf.size(), (UINT64)c.bufId); CollectTyped(fr, g_lastParse.events, true);
            ObserveCall(c, r, mvB, gB, cutB, lB); }
        g_frame = fr; DriverFrame(fr);
    }
    (void)i;
}
// EXPECT records: for every event KIND that appears in an EXPECT, the observed events of that kind must match exactly (frame; a0/a1 unless -1). Kinds without EXPECT are unconstrained.
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static void CompareExpect(const Trace& t) {
    std::set<uint16_t> kinds; for (const Item& it : t.items) if (it.type == REC_EXPECT) kinds.insert(it.ex.ev);
    for (uint16_t k : kinds) {
        std::vector<Expect> ex; for (const Item& it : t.items) if (it.type == REC_EXPECT && it.ex.ev == k) ex.push_back(it.ex);
        std::vector<Ev> ob; for (const Ev& e : g_ev) if (e.ev == k) ob.push_back(e);
        bool ok = ex.size() == ob.size(); for (size_t i = 0; ok && i < ex.size(); i++) ok = ex[i].frame == ob[i].frame && (ex[i].a0 == -1 || ex[i].a0 == ob[i].a0) && (ex[i].a1 == -1 || ex[i].a1 == ob[i].a1);
        if (!ok) { g_fail++; printf("FAIL event %s: expected %zu, observed %zu\n  expected:", EvName(k), ex.size(), ob.size()); for (auto& e : ex) printf(" f%d(%d,%d)", e.frame, e.a0, e.a1);
            printf("\n  observed:"); for (auto& e : ob) printf(" f%d(%d,%d)", e.frame, e.a0, e.a1); printf("\n"); }
    }
}

// step 5: the typed events must equal the events derived from the log lines (and from the counters, for CUT / GARBAGE), per KIND and in order.
//  * kinds with a log-derived a0/a1 (LOCKED SWITCHED LOST RESCAN HOLD): frame + a0 + a1 must match
//  * kinds whose log-derived event carries no arguments (CUT CAND_IGNORED ALT_VIEW GARBAGE GARBAGE_CLEARED): frame must match
//  * tracker-only kinds (RESCAN_FOUND RESCAN_GIVEUP HOLD_ENDED CUT_SUSPENDED C2P_BANNED): derived here straight from their log lines (frame must match)
//  * CAND_IGNORED / ALT_VIEW: the LOG is capped (5 / 10 lines), the typed event is not: the logged ones must be the first typed ones, and a longer typed list is only allowed once the cap is reached
static void CrossCheckTyped(bool verbose) {
    struct K { uint16_t k; const char* name; bool args; const char* logKey; int cap; };
    static const K ks[] = { { EV_LOCKED, "LOCKED", true, nullptr, 0 }, { EV_SWITCHED, "SWITCHED", true, nullptr, 0 }, { EV_CUT, "CUT", false, nullptr, 0 }, { EV_LOST, "LOST", true, nullptr, 0 },
        { EV_CAND_IGNORED, "CAND_IGNORED", false, nullptr, 5 }, { EV_ALT_VIEW, "ALT_VIEW", false, nullptr, 10 }, { EV_HOLD, "HOLD", true, nullptr, 0 }, { EV_RESCAN, "RESCAN", true, nullptr, 0 },
        { EV_GARBAGE, "GARBAGE", false, nullptr, 0 }, { EV_GARBAGE_CLEARED, "GARBAGE_CLEARED", false, nullptr, 0 },
        { CAMEV_RESCAN_FOUND, "RESCAN_FOUND", false, "camera re-scan: view buffer found again", 0 }, { CAMEV_RESCAN_GIVEUP, "RESCAN_GIVEUP", false, "camera re-scan: giving up", 0 },
        { CAMEV_HOLD_ENDED, "HOLD_ENDED", false, "camhold ended", 0 }, { CAMEV_CUT_SUSPENDED, "CUT_SUSPENDED", false, "camera-cut guard: fires too often", 0 }, { CAMEV_C2P_BANNED, "C2P_BANNED", false, "is INVALID - not selected", 0 } };
    int total = 0;
    for (const K& k : ks) {
        std::vector<Ev> lg, ty;
        if (k.logKey) { for (auto& l : g_shimLog) if (Has(l.text, k.logKey)) lg.push_back({ l.frame, k.k, 0, 0 }); } else for (const Ev& e : g_ev) if (e.ev == k.k) lg.push_back(e);
        for (const Ev& e : g_tev) if (e.ev == k.k) ty.push_back(e);
        total += (int)ty.size();
        bool ok = true; size_t n = std::min(lg.size(), ty.size());
        if (k.cap) { ok = ty.size() >= lg.size() && (ty.size() == lg.size() || (int)lg.size() == k.cap); }   // capped log
        else ok = lg.size() == ty.size();
        for (size_t i = 0; ok && i < n; i++) ok = lg[i].frame == ty[i].frame && (!k.args || (lg[i].a0 == ty[i].a0 && lg[i].a1 == ty[i].a1));
        if (verbose && !ty.empty()) printf("  typed %-16s %zu (log-derived %zu)%s\n", k.name, ty.size(), lg.size(), ok ? "" : "  MISMATCH");
        if (!ok) { g_fail++; printf("FAIL typed-vs-log events %s: typed %zu, log-derived %zu\n  typed:", k.name, ty.size(), lg.size()); for (auto& e : ty) printf(" f%d(%d,%d)", e.frame, e.a0, e.a1);
            printf("\n  log  :"); for (auto& e : lg) printf(" f%d(%d,%d)", e.frame, e.a0, e.a1); printf("\n"); }
    }
    // every typed event must belong to a kind that was cross-checked above (no silent extras), and the capacity claims of camtracker.h must hold
    size_t all = g_tev.size(); if ((int)all != total) { g_fail++; printf("FAIL typed events of an unchecked kind: %zu total, %d checked\n", all, total); }
    CHECK(g_tevOverflow == 0); CHECK(g_maxParseEv <= 5); CHECK(g_maxFrameEv <= 4);
    if (verbose) printf("  typed events: %zu total, max %d per Parse (claim <= 5), max %d per Frame (claim <= 4), overflow %d\n", all, g_maxParseEv, g_maxFrameEv, g_tevOverflow);
}

// ---------------------------------------------------------------- buffer builders
static void Put(std::vector<float>& b, int at, std::initializer_list<float> v) { int i = at; for (float x : v) b[i++] = x; }
static void Proj(std::vector<float>& b, int at, float m0, float jx, float jy) { float m5 = m0 * 1.7778f; Put(b, at, { m0, 0, 0, 0, 0, m5, 0, 0, jx, jy, 0, 1, 0, 0, 0.1f, 0 }); }
static void C2p(std::vector<float>& b, int at, float t) {   // t = camera-move phase, MUST stay in [0,20): translation <= 0.004, a plausible ClipToPrevClip (generators used to grow it with the frame number)
    if (!(t >= 0.f && t < 20.f)) { printf("generator bug: c2p phase %g out of [0,20)\n", t); exit(3); } Put(b, at, { 1, 0.0001f * t, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0002f * t, -0.0001f * t, 0, 1 }); }
static float Jx(int f) { return 0.0007f * (float)((f % 5) - 2) + 0.0001f; }
static float Jy(int f) { return -0.0012f * (float)((f % 3) - 1) - 0.0001f; }
// Scarlet-Nexus-like UE4 buffer: ViewToClip(jittered) @po, NoAA @po+16, ClipToPrevClip @mo
static std::vector<float> Ue4(int f, int po = 116, int no = 132, int mo = 492, float m0 = 1.f, float t = 1.f, int n = 700) { std::vector<float> b(n, 0.f); Proj(b, po, m0, Jx(f), Jy(f)); Proj(b, no, m0, 0, 0); C2p(b, mo, t); return b; }
// UE5-like: extra identity matrix in front of the real (moving) ClipToPrevClip.  cand #0 @400 is a constant identity placeholder, #1 @420 the real one
static std::vector<float> Ue5(int f, bool moving) { std::vector<float> b(900, 0.f); Proj(b, 200, 1.2f, Jx(f), Jy(f)); Proj(b, 216, 1.2f, 0, 0);
    Put(b, 400, { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }); float s = moving ? 0.02f * sinf((float)f / 7.f) : 0.f; Put(b, 420, { 1, 0.0002f, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, s, 0.5f * s, 0, 1 }); return b; }
// CODE-VEIN-like "legacy" layout: 16 WorldToClip(jit) | 32 pre-view | 96 ViewToClip(jit) | 252 PrevViewProj | 456 TemporalAAJitter | 472 ViewSize
static std::vector<float> Legacy(int f, bool dummy) {
    std::vector<float> b(700, 0.f); float jx = Jx(f), jy = Jy(f); Proj(b, 96, 1.f, jx, jy);
    static const double Wn[16] = { 1.2, 0.1, 0, 0.3, 0, 1.5, 0.2, 0.1, 0.1, 0, 1, 1, 0, 0, -0.5, 2 };
    double W[16], Wp[16]; for (int i = 0; i < 16; i++) { W[i] = Wn[i]; Wp[i] = Wn[i] + ((i == 12) ? 0.001 : 0.0) * (double)(f % 4); }
    for (int r = 0; r < 4; r++) { b[16 + r * 4 + 0] = (float)(W[r * 4] + W[r * 4 + 3] * jx); b[16 + r * 4 + 1] = (float)(W[r * 4 + 1] + W[r * 4 + 3] * jy); b[16 + r * 4 + 2] = (float)W[r * 4 + 2]; b[16 + r * 4 + 3] = (float)W[r * 4 + 3];
                                  b[252 + r * 4 + 0] = (float)(Wp[r * 4] + Wp[r * 4 + 3] * 0.0003); b[252 + r * 4 + 1] = (float)(Wp[r * 4 + 1] + Wp[r * 4 + 3] * 0.0002); b[252 + r * 4 + 2] = (float)Wp[r * 4 + 2]; b[252 + r * 4 + 3] = (float)Wp[r * 4 + 3]; }
    b[456] = jx; b[457] = jy; b[458] = 0.0003f; b[459] = 0.0002f; b[472] = 1920.f; b[473] = 1080.f;
    if (dummy) { static const float kRot[16] = { 0,0,1,0, 1,0,0,0, 0,1,0,0, 0,0,0,1 }; memcpy(&b[32], kRot, sizeof kRot); memcpy(&b[0], &b[16], 64); }
    return b;
}
static void FullBlocks(Call& c, const std::vector<float>& v) {   // lossless: 16-float windows, stride 16, the last one aligned to the end
    c.availF = (uint16_t)v.size(); c.blocks.clear(); size_t i = 0;
    for (; i + 16 <= v.size(); i += 16) { Block k; k.idx = (uint16_t)i; memcpy(k.m, &v[i], 64); c.blocks.push_back(k); }
    if (i < v.size() && v.size() >= 16) { Block k; k.idx = (uint16_t)(v.size() - 16); memcpy(k.m, &v[v.size() - 16], 64); c.blocks.push_back(k); }
}
static Header Hdr(int po, int no, int mo, const char* tag) { Header h; h.projOff = po; h.noAAOff = no; h.mvOff = mo; strncpy(h.tag, tag, 23); return h; }
static void AddCall(Trace& t, int f, uint32_t buf, const std::vector<float>& v) { Item it; it.type = REC_CALL; it.call.frame = f; it.call.effW = 1920; it.call.effH = 1080; it.call.bufId = buf; FullBlocks(it.call, v); t.items.push_back(it); }
static void AddEx(Trace& t, int f, uint16_t ev, int a0 = -1, int a1 = -1) { Item it; it.type = REC_EXPECT; it.ex.frame = f; it.ex.ev = ev; it.ex.a0 = a0; it.ex.a1 = a1; t.items.push_back(it); }

// ---------------------------------------------------------------- behaviour scenarios
struct Scn { Trace t; std::function<void()> extra; const char* what; int badC2p = 0; };   // badC2p = number of implausible-c2p frames the scenario deliberately contains
static Scn MkMislock() {   // 1.1.6 locked projoff=2800 at frame 12752 while 112/128/544 still delivered the camera. 1.1.7: SnScanMayLock must refuse
    Scn s; s.what = "mislock @12752: working offsets deliver the camera, a 2nd buffer repeats 5x -> candidate ignored, no lock, no cfg write"; s.t.h = Hdr(112, 128, 544, "mislock-12752");
    for (int f = 12740; f <= 12760; f++) {
        AddCall(s.t, f, 1, Ue4(f, 112, 128, 544, 1.f, (float)(f % 20)));
        std::vector<float> b(3000, 0.f); Proj(b, 2800, 1.f, Jx(f), Jy(f)); Proj(b, 2816, 1.f, 0, 0); C2p(b, 2840, 0.f); AddCall(s.t, f, 2, b); }
    for (int f : { 12744, 12749, 12754, 12759 }) AddEx(s.t, f, EV_CAND_IGNORED);
    s.extra = [] { CHECK(g_camLocked.load() == 0); CHECK(g_cfg.projOff == 112 && g_cfg.noAAOff == 128 && g_cfg.mvOff == 544); CHECK(g_shimCfgWrites.empty()); for (auto& e : g_ev) CHECK(e.ev != EV_LOCKED); CHECK(g_camFrame.load() == 12760); };
    return s; }
static Scn MkAlt() {
    Scn s; s.what = "alternating views m0 1.0 <-> 1.6406 from frame 3011: the first flip is a cut (never seen), later flips are 'alternating views' (10 logged), no more cuts"; s.t.h = Hdr(116, 132, 492, "alt-views");
    for (int f = 3000; f <= 3039; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, (f >= 3010 && (f & 1)) ? 1.6406f : 1.f, (float)(f % 20)));
    AddEx(s.t, 3011, EV_CUT); for (int f = 3012; f <= 3021; f++) AddEx(s.t, f, EV_ALT_VIEW);
    s.extra = [] { CHECK(g_cutTotal == 1); CHECK(g_forceResets == 1); CHECK(g_camFrame.load() == 3039); };
    return s; }
static Scn MkC2pSwitch(bool viaScan) {
    Scn s; s.what = viaScan ? "c2p never moved: scan locks UE5 buffer on cand #0 (constant identity), monitor verdict after 120 frames switches to #1" : "[pins a finding] SAME buffers but offsets come from the cfg file (no scan lock): g_camLocked stays 0 -> C2pMonitor never runs, NO switch";
    s.t.h = viaScan ? Hdr(116, 132, 492, "c2p-dead-scan") : Hdr(200, 216, 400, "c2p-dead-cfg");
    for (int f = 1000; f <= 1200; f++) AddCall(s.t, f, 1, Ue5(f, true));
    if (viaScan) { AddEx(s.t, 1004, EV_LOCKED, 200, 400); AddEx(s.t, 1124, EV_SWITCHED, 400, 420); }
    s.extra = viaScan ? std::function<void()>([] { CHECK(g_cfg.mvOff == 420); CHECK(g_forceResets == 1); CHECK(g_shimCfgWrites.size() == 4 && g_shimCfgWrites[3].first == "mvoff" && g_shimCfgWrites[3].second == 420); })
                      : std::function<void()>([] { CHECK(g_camLocked.load() == 0); CHECK(g_cfg.mvOff == 400); CHECK(g_forceResets == 0); for (auto& e : g_ev) CHECK(e.ev != EV_SWITCHED); });
    return s; }
static Scn MkCut() {
    Scn s; s.what = "hard cut: 4 consecutive implausible c2p frames = ONE cut (1.1.6b); later FOV jump 1.0 -> 1.3 with identity c2p = 2nd cut; cutHold decays to 0"; s.t.h = Hdr(116, 132, 492, "hard-cut");
    for (int f = 2000; f <= 2060; f++) { auto b = Ue4(f, 116, 132, 492, f >= 2045 ? 1.3f : 1.f, (float)(f % 20)); if (f >= 2030 && f <= 2033) Put(b, 492, { 40,0,0,0, 0,40,0,0, 0,0,40,0, 0,0,0,1 }); AddCall(s.t, f, 1, b); }
    AddEx(s.t, 2030, EV_CUT); AddEx(s.t, 2045, EV_CUT);
    s.badC2p = 4; s.extra = [] { CHECK(g_cutTotal == 2); CHECK(g_forceResets == 2); CHECK(g_cutHold.load() == 0); CHECK(g_c2pBadTotal.load() == 4); };
    return s; }
static Scn MkHold() {
    Scn s; s.what = "hold-then-return: camera lost after frame 4040 (age>30 at 4071 -> LOST+camhold), rescan armed at age 91, camera back at 4136 = camhold released after 65 frames (short hold, history kept)"; s.t.h = Hdr(116, 132, 492, "hold-return");
    for (int f = 4000; f <= 4040; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20)));
    for (int f = 4136; f <= 4150; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20)));
    AddEx(s.t, 4071, EV_LOST, 31); AddEx(s.t, 4131, EV_RESCAN, 91); AddEx(s.t, 4136, EV_HOLD, 65, 0);
    s.extra = [] { CHECK(g_camRescan.load() == 0); CHECK(g_forceResets == 0); CHECK(g_camFrame.load() == 4150); bool found = false; for (auto& l : g_shimLog) if (Has(l.text, "camera re-scan: view buffer found again")) found = true; CHECK(found); };
    return s; }
static Scn MkHoldLong() {
    Scn s; s.what = "long hold (300 frames, > 240): release resets the DLSS-G history (forceReset, a1=1)"; s.t.h = Hdr(116, 132, 492, "hold-long");
    for (int f = 4000; f <= 4040; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20)));
    for (int f = 4371; f <= 4380; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20)));
    AddEx(s.t, 4071, EV_LOST, 31); AddEx(s.t, 4131, EV_RESCAN, 91); AddEx(s.t, 4371, EV_HOLD, 300, 1);
    s.extra = [] { CHECK(g_forceResets == 1); CHECK(g_camFrame.load() == 4380); };
    return s; }
static Scn MkRescanGiveup() {
    Scn s; s.what = "camera gone ~1960 frames: rescan armed at age 91, gives up after 1200 frames, cools down 600, re-arms at 5932; camhold keeps FG alive (camholdmax 7200); long release"; s.t.h = Hdr(116, 132, 492, "rescan-giveup");
    for (int f = 4000; f <= 4040; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20)));
    for (int f = 6000; f <= 6005; f++) AddCall(s.t, f, 1, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20)));
    AddEx(s.t, 4071, EV_LOST, 31); AddEx(s.t, 4131, EV_RESCAN, 91); AddEx(s.t, 5932, EV_RESCAN, 1892); AddEx(s.t, 6000, EV_HOLD, 1929, 1);
    s.extra = [] { CHECK(g_forceResets == 1); CHECK(g_camRescan.load() == 0); };
    return s; }
// sequences used by the gate (no EXPECTs needed: full and sparse must simply agree).  All start with unknown offsets so the scan / lock path is part of the signature.
static Trace GateTrace(const std::string& n, std::string* err) {
    Trace t; t.h = Hdr(116, 132, 492, n.c_str());
    if (n == "ue4") { t.h = Hdr(112, 128, 544, "gate-ue4"); for (int f = 1100; f < 1160; f++) AddCall(t, f, 7, Ue4(f, 116, 132, 492, 1.f, (float)(f % 20))); }
    else if (n == "ue5") { t.h = Hdr(112, 128, 544, "gate-ue5"); for (int f = 1000; f <= 1200; f++) AddCall(t, f, 1, Ue5(f, true)); }
    else if (n == "legacy") { t.h = Hdr(112, 128, 544, "gate-legacy"); for (int f = 1000; f < 1060; f++) { AddCall(t, f, 1, Legacy(f, false)); AddCall(t, f, 2, Legacy(f, true)); } }
    else { *err = "unknown gate trace " + n; }
    return t; }

// ---------------------------------------------------------------- recorder rules under test (the "sparse" hypothesis of STEP1 section 5)
// v1 = rule as written in step 1: every 4-aligned 16-float window that is projection-like (loose) or c2p-like, plus the windows at the offsets in force.
// v2 = v1 + the fixed windows TryLegacyLayout reads (floats 0, 16, 32, 96, 252, 456, 472).
// nomv = v1 WITHOUT the c2p-like / in-force windows: a deliberately broken recorder, the gate MUST flag it (proves the gate has teeth).
static Trace Sparsify(const Trace& full, const std::string& variant) {
    Trace out = full; if (variant == "full") return out; out.h.flags |= FLAG_SPARSE;
    std::vector<float> v;
    for (Item& it : out.items) { if (it.type != REC_CALL) continue; Call& c = it.call; Materialize(c, v); std::set<int> idx; int n = (int)v.size();
        for (int i = 0; i + 16 <= n; i += 4) { if (CamIsProj(&v[i], true, 0.f)) idx.insert(i); if (variant != "nomv" && CamLooksLikeC2P(&v[i])) idx.insert(i); }
        if (variant != "nomv") for (int o : { 116, 132, 492, 112, 128, 544, 200, 216, 400, 420, full.h.projOff, full.h.noAAOff, full.h.mvOff }) if (o >= 0 && o + 16 <= n) idx.insert(o);   // offsets "in force": the live recorder knows them
        if (variant == "v2") for (int o : { 0, 16, 32, 96, 252, 456, 472 }) if (o + 16 <= n) idx.insert(o);
        c.blocks.clear(); for (int i : idx) { Block k; k.idx = (uint16_t)i; memcpy(k.m, &v[i], 64); c.blocks.push_back(k); } }
    return out; }
// existing real dumps (SN_DLSSG_viewbuf.txt format): one dump repeated for 8 frames with unknown offsets, so the scan must find them
static bool DumpTrace(const char* path, Trace& t, int* ep, int* en, int* em) {
    FILE* f = fopen(path, "r"); if (!f) return false; char line[512]; std::vector<float> v; bool have = false; *ep = *en = *em = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "# expect:", 9)) { sscanf(line, "# expect: %d %d %d", ep, en, em); continue; }
        if (!strncmp(line, "=== dump", 8)) { if (have) break; have = true; continue; }
        int idx; float a, b, c, d; if (have && sscanf(line, "%d: %f %f %f %f", &idx, &a, &b, &c, &d) == 5) { if ((int)v.size() < idx) v.resize(idx, 0.f); v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d); } }
    fclose(f); if (v.empty()) return false;
    t = Trace(); t.h = Hdr(-1, -1, -1, "dump"); for (int i = 0; i < 8; i++) AddCall(t, 300 + i, 1, v); return true; }

static int RunTrace(Trace& t, bool verbose) {
    g_shimEcho = verbose; Replay(t); CompareExpect(t); CrossCheckTyped(verbose);
    if (const char* so = getenv("SN_SIGOUT")) { FILE* sf = fopen(so, "w"); if (sf) { fputs(g_sig.c_str(), sf); fclose(sf); } }   // debug hook: full per-call signature (return, camFrame, offsets, jitter, c2p, logs, events) for old-vs-new comparisons
    if (verbose) for (auto& e : g_ev) printf("  event f%d %s a0=%d a1=%d\n", e.frame, EvName(e.ev), e.a0, e.a1);
    return 0;
}
static bool BuildScenario(const std::string& n, Scn& s) {
    if (n == "mislock12752") s = MkMislock(); else if (n == "alt_views") s = MkAlt(); else if (n == "c2p_never_moved") s = MkC2pSwitch(true); else if (n == "c2p_monitor_needs_lock") s = MkC2pSwitch(false);
    else if (n == "hard_cut") s = MkCut(); else if (n == "hold_then_return") s = MkHold(); else if (n == "hold_long_reset") s = MkHoldLong(); else if (n == "rescan_giveup") s = MkRescanGiveup(); else return false;
    return true; }
static const char* kScenarios[] = { "mislock12752", "alt_views", "c2p_never_moved", "c2p_monitor_needs_lock", "hard_cut", "hold_then_return", "hold_long_reset", "rescan_giveup" };
static std::string Slurp(const std::string& p) { FILE* f = fopen(p.c_str(), "rb"); std::string s; if (!f) return s; char b[65536]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) s.append(b, k); fclose(f); return s; }
static std::string Q(const std::string& s) { return "'" + s + "'"; }
static int Sh(const std::string& cmd) { return system(cmd.c_str()); }

int main(int argc, char** argv) {
    std::string a1 = argc > 1 ? argv[1] : "";
    if (a1 == "--list") { for (const char* n : kScenarios) { Scn s; BuildScenario(n, s); printf("%-24s %s\n", n, s.what); } return 0; }
    if (a1 == "--write" && argc >= 4) {   // scenario -> SNCT file (round-trips through the Writer/Parse like a recorded capture)
        Scn s; if (!BuildScenario(argv[2], s)) { printf("unknown scenario\n"); return 2; }
        Writer w; w.Begin(s.t.h); for (const Item& it : s.t.items) { if (it.type == REC_CALL) w.AddCall(it.call); else if (it.type == REC_EXPECT) w.AddExpect(it.ex); }
        if (!w.Save(argv[3])) return 2; Trace back; std::string err; if (!Load(argv[3], back, &err)) { printf("reload failed: %s\n", err.c_str()); return 2; }
        FILE* f = fopen((std::string(argv[3]) + ".txt").c_str(), "w"); if (f) { DumpText(f, back); fclose(f); } printf("wrote %s (%zu bytes)\n", argv[3], w.Bytes().size()); return 0; }
    if (a1 == "--file" && argc >= 3) { Trace t; std::string err; if (!Load(argv[2], t, &err)) { printf("%s: %s\n", argv[2], err.c_str()); return 2; }
        bool v = argc >= 4 && !strcmp(argv[3], "-v"); RunTrace(t, v); printf("replay_seq --file %s: %s (%zu events)\n", argv[2], g_fail ? "FAILED" : "ok", g_ev.size()); return g_fail ? 1 : 0; }
    if (a1 == "--sig" && argc >= 5) {   // internal: <trace> <variant> <outfile>; trace = ue4|ue5|legacy|dump:<file>
        std::string tn = argv[2], var = argv[3]; Trace full, t; std::string err; int ep, en, em;
        if (tn.rfind("dump:", 0) == 0) { if (!DumpTrace(tn.c_str() + 5, full, &ep, &en, &em)) { printf("cannot read dump\n"); return 2; } } else { full = GateTrace(tn, &err); if (!err.empty()) { printf("%s\n", err.c_str()); return 2; } }
        t = Sparsify(full, var); Replay(t); FILE* f = fopen(argv[4], "w"); if (!f) return 2; fputs(g_sig.c_str(), f); fprintf(f, "FINAL off=%d/%d/%d writes=%zu\n", g_cfg.projOff, g_cfg.noAAOff, g_cfg.mvOff, g_shimCfgWrites.size()); fclose(f); return 0; }
    if (a1 == "--gate") {
        std::string dir = argc >= 3 ? argv[2] : "tests/data"; std::vector<std::string> traces = { "ue4", "ue5", "legacy" };
        { std::string ls = "ls " + Q(dir) + "/*.txt 2>/dev/null"; FILE* p = popen(ls.c_str(), "r"); char b[1024]; while (p && fgets(b, sizeof b, p)) { std::string s = b; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) traces.push_back("dump:" + s); } if (p) pclose(p); }
        std::string tmp = "tests/bin/gate_tmp"; Sh("mkdir -p " + Q(tmp));
        // expected verdict per (trace, variant): same = identical signature. v1 is KNOWN to diverge on the legacy layout (that is what the gate is for); nomv must diverge everywhere a c2p matters.
        struct Row { const char* v; }; int bad = 0;
        printf("%-34s %-6s %-6s %-6s %s\n", "trace", "v1", "v2", "nomv", "verdict");
        for (const std::string& tn : traces) {
            std::map<std::string, std::string> sig; for (const char* v : { "full", "v1", "v2", "nomv" }) { std::string out = tmp + "/" + std::to_string(std::hash<std::string>()(tn) % 100000) + "_" + v + ".sig";
                int rc = Sh(Q(argv[0]) + " --sig " + Q(tn) + " " + v + " " + Q(out) + " >/dev/null"); if (rc != 0) { printf("child failed for %s/%s\n", tn.c_str(), v); bad++; } sig[v] = Slurp(out); }
            auto same = [&](const char* v) { return sig["full"] == sig[v]; };
            bool legacy = tn == "legacy"; bool ok = same("v2") && (legacy ? !same("v1") : same("v1")) && !same("nomv");   // pinned expectations
            if (!ok) bad++;
            printf("%-34s %-6s %-6s %-6s %s\n", tn.size() > 33 ? tn.c_str() + tn.size() - 33 : tn.c_str(), same("v1") ? "same" : "DIFF", same("v2") ? "same" : "DIFF", same("nomv") ? "same" : "DIFF", ok ? "as pinned" : "UNEXPECTED");
            if (!same("v2") || (!legacy && !same("v1"))) { std::istringstream x(sig["full"]), y(sig[same("v2") ? "v1" : "v2"]); std::string l1, l2; int n = 0; while (std::getline(x, l1) && std::getline(y, l2)) { n++; if (l1 != l2) { printf("    first difference at line %d:\n      full  : %s\n      sparse: %s\n", n, l1.c_str(), l2.c_str()); break; } } }
        }
        printf("sparse-vs-full gate: %s\n", bad ? "FAILED" : "ok"); return bad ? 1 : 0; }
    Scn s; if (!BuildScenario(a1, s)) { printf("usage: replay_seq <scenario|--list|--file f|--write s f|--gate [dir]>\n"); return 2; }
    // round-trip through the SNCT file format first (the scenarios must survive Writer -> Parse unchanged)
    { Writer w; w.Begin(s.t.h); for (const Item& it : s.t.items) { if (it.type == REC_CALL) w.AddCall(it.call); else if (it.type == REC_EXPECT) w.AddExpect(it.ex); } Trace back; std::string err; CHECK(Parse(w.Bytes().data(), w.Bytes().size(), back, &err)); CHECK(back.items.size() == s.t.items.size()); if (back.items.size() == s.t.items.size()) s.t = back; }
    RunTrace(s.t, argc >= 3 && !strcmp(argv[2], "-v")); s.extra();
    CHECK(g_c2pBadTotal.load() == s.badC2p);   // test-data guard: a scenario may only contain implausible matrices on purpose
    if (g_fail) { printf("scenario '%s' FAILED (%d)\n", a1.c_str(), g_fail); return 1; }
    printf("scenario '%s' ok  - %s\n", a1.c_str(), s.what); return 0; }
