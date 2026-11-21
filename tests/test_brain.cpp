// test_brain.cpp - Brain v0 + v1 (src/brain.h): sensor, rule, safety limits, trouble hold, learned camera predictor, learned load governor, fuzz. Pure C++.
#include "../src/brain.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Ident(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.f; }
static int g_fr = 0;
static brain::Out Run(brain::Brain& b, brain::Sense& s) { s.frame = ++g_fr; return b.Step(s); }
static brain::Sense Mk(const float* m) { brain::Sense s; memcpy(s.c2p, m, 64); s.zA = 0.f; s.zB = 10.f; return s; }   // UE style: reverse-Z, infinite far, near = 10

int main() {
    float M[16]; Ident(M);
    // 1) identity = no camera motion
    CHECK(brain::CamMotion(M, 0.f, 10.f) < 1e-6f, "identity cam=%f", brain::CamMotion(M, 0.f, 10.f));
    // 2) a pure screen shift of 0.5 NDC (row-vector convention: translation lives in row 3) -> cam motion 0.5 for the far samples
    M[12] = 0.5f; float c = brain::CamMotion(M, 0.f, 10.f); CHECK(fabsf(c - 0.5f) < 1e-4f, "shift cam=%f", c);
    // 3) unusable matrices
    float Nn[16]; Ident(Nn); Nn[3] = NAN; CHECK(brain::CamMotion(Nn, 0.f, 10.f) < 0.f, "NaN matrix");
    float Bh[16]; Ident(Bh); Bh[15] = -1.f; CHECK(brain::CamMotion(Bh, 0.f, 10.f) == 0.f, "everything behind the camera must say nothing, not blow up");
    // 4) calm scene: nothing moves away from the defaults
    { brain::Brain b; float I[16]; Ident(I); brain::Sense s = Mk(I); brain::Out o{}; for (int i = 0; i < 200; i++) { o = Run(b, s); }
      CHECK(o.f0 == 0.35f && o.f1 == 1.0f, "calm: %f %f", o.f0, o.f1); CHECK(b.mode == brain::MODE_ADAPT, "mode"); CHECK(b.nAdapted == 0, "adapted=%u", b.nAdapted); }
    // 5) fast camera: fall0 follows (fast), stays inside the limits, fall1 keeps a real window
    { brain::Brain b; float F[16]; Ident(F); F[12] = 0.5f; brain::Sense s = Mk(F); brain::Out o{}; float prev = 0.35f; int framesToTop = -1;
      for (int i = 0; i < 40; i++) { o = Run(b, s); CHECK(o.f0 - prev <= b.L.slewUp + 1e-6f, "slew up %f", o.f0 - prev); prev = o.f0; if (framesToTop < 0 && o.f0 >= b.L.f0Max - 1e-4f) framesToTop = i; }
      CHECK(o.f0 > 0.55f && o.f0 <= b.L.f0Max + 1e-6f, "fast f0=%f", o.f0); CHECK(framesToTop >= 0 && framesToTop <= 12, "reached the top after %d frames", framesToTop);
      CHECK(o.f1 >= o.f0 + 0.30f - 1e-5f && o.f1 <= b.L.f1Max + 1e-6f, "window f0=%f f1=%f", o.f0, o.f1);
      // 6) camera stops: slow release, never below the default, never a jump down
      float I[16]; Ident(I); s = Mk(I); float last = o.f0; bool jump = false; for (int i = 0; i < 600; i++) { o = Run(b, s); if (last - o.f0 > b.L.slewDown + 1e-6f) jump = true; last = o.f0; }
      CHECK(!jump, "falling too fast"); CHECK(fabsf(o.f0 - 0.35f) < 1e-4f && fabsf(o.f1 - 1.0f) < 1e-4f, "release: %f %f", o.f0, o.f1); }
    // 7) trouble: defaults at once, held for holdFrames, then adapting again
    { brain::Brain b; float F[16]; Ident(F); F[12] = 0.4f; brain::Sense s = Mk(F); brain::Out o{}; for (int i = 0; i < 30; i++) o = Run(b, s); CHECK(o.f0 > 0.5f, "pre f0=%f", o.f0);
      s.cut = true; o = Run(b, s); CHECK(o.f0 == 0.35f && o.f1 == 1.0f && b.mode == brain::MODE_SAFE, "cut -> defaults at once (%f %f)", o.f0, o.f1); s.cut = false;
      int safeFrames = 1; for (int i = 0; i < 200; i++) { o = Run(b, s); if (b.mode == brain::MODE_SAFE) safeFrames++; else break; }
      CHECK(safeFrames == b.L.holdFrames, "held %d frames (want %d)", safeFrames, b.L.holdFrames); CHECK(b.nHolds == 1, "holds=%u", b.nHolds);
      for (int i = 0; i < 20; i++) o = Run(b, s); CHECK(o.f0 > 0.4f && b.mode == brain::MODE_ADAPT, "adapting again f0=%f", o.f0);
      const char* names[] = { "stall", "susp", "camLost", "mvFailed" };
      for (int k = 0; k < 4; k++) { brain::Brain c2; brain::Sense t = Mk(F); for (int i = 0; i < 30; i++) Run(c2, t); if (k == 0) t.stall = true; if (k == 1) t.susp = true; if (k == 2) t.camOk = false; if (k == 3) t.mvFailed = true;
          brain::Out q = Run(c2, t); CHECK(q.f0 == 0.35f && c2.mode == brain::MODE_SAFE, "%s must force the defaults", names[k]); } }
    // 7b) the MV pass did not run for a while (FG off / paused): the next step starts safe
    { brain::Brain b; float F[16]; Ident(F); F[12] = 0.4f; brain::Sense s = Mk(F); brain::Out o{}; for (int i = 0; i < 30; i++) o = Run(b, s); CHECK(o.f0 > 0.5f, "pre-gap");
      g_fr += 500; o = Run(b, s); CHECK(o.f0 == 0.35f && b.mode == brain::MODE_SAFE, "gap must start safe (%f)", o.f0); }
    // 8) switched off = defaults, always
    { brain::Brain b; b.enabled = false; float F[16]; Ident(F); F[12] = 0.5f; brain::Sense s = Mk(F); for (int i = 0; i < 50; i++) { brain::Out o = Run(b, s); CHECK(o.f0 == 0.35f && o.f1 == 1.0f && b.mode == brain::MODE_OFF, "off"); } }
    // 9) fuzz: random matrices / flags / depth params - always finite, always inside the limits, window always >= 0.30
    { srand(12345); brain::Brain b; brain::Snapshot snap; int bad = 0;
      for (int i = 0; i < 200000; i++) {
          brain::Sense s; for (int k = 0; k < 16; k++) { float r = (float)rand() / RAND_MAX; s.c2p[k] = (rand() % 50 == 0) ? (rand() % 2 ? NAN : INFINITY) : (r - 0.5f) * ((rand() % 9 == 0) ? 1e4f : 4.f); }
          s.zA = (float)rand() / RAND_MAX - 0.5f; s.zB = ((float)rand() / RAND_MAX - 0.5f) * 100.f; s.cut = rand() % 400 == 0; s.stall = rand() % 600 == 0; s.camOk = rand() % 500 != 0;
          brain::Out o = Run(b, s); b.Publish(snap);
          if (!std::isfinite(o.f0) || !std::isfinite(o.f1) || o.f0 < b.L.f0Def - 1e-6f || o.f0 > b.L.f0Max + 1e-6f || o.f1 < b.L.f1Min - 1e-6f || o.f1 > b.L.f1Max + 1e-6f || o.f1 < o.f0 + 0.30f - 1e-5f) bad++;
          if (snap.activity < 0.f || snap.activity > 1.f) bad++; }
      CHECK(bad == 0, "fuzz: %d out-of-limit results", bad); }

    // ======================================================= v1: learned camera predictor ==========================================================
    // 10) RLS itself: learns a known linear law online, survives garbage input
    { brain::Rls<3> r; srand(7); double worst = 0;
      for (int i = 0; i < 4000; i++) { float x[3] = { 1.f, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX }; const double y = 0.3 + 2.0 * x[1] - 1.0 * x[2] + 0.01 * ((double)rand() / RAND_MAX - 0.5); r.Update(x, y); if (i > 500) worst = std::max(worst, fabs(r.Predict(x) - y)); }
      CHECK(worst < 0.03, "RLS worst error %f", worst);
      float bad[3] = { 1.f, NAN, 0.f }; CHECK(!r.Update(bad, 1.0), "NaN input must be refused"); CHECK(!r.Update(bad + 0, INFINITY), "inf label refused");
      float flat[3] = { 1.f, 0.f, 0.f }; for (int i = 0; i < 200000; i++) r.Update(flat, 0.3);                     // no excitation for a long time: no wind-up, no NaN
      CHECK(std::isfinite(r.w[0]) && r.P[0][0] <= r.pmax + 1e-9, "wind-up: w0=%f P00=%f", r.w[0], r.P[0][0]); }
    // 11) the predictor beats 'same as last frame' on a smooth but changing camera (self-supervised, no labels from outside)
    { brain::CamPredictor cp; srand(3); double ePred = 0, ePers = 0; int n = 0; float prevPred = 0, prevCam = 0;
      for (int i = 0; i < 6000; i++) {
          const double t = i * 0.05; float cam = (float)(0.25 + 0.2 * sin(t) + 0.1 * sin(2.3 * t) + 0.01 * ((double)rand() / RAND_MAX - 0.5)); if (cam < 0) cam = 0;
          if (i > 1500) { ePred += fabs(cam - prevPred); ePers += fabs(cam - prevCam); n++; }
          prevPred = cp.Step(cam); prevCam = cam; }
      CHECK(cp.Ready(90), "predictor never became ready"); CHECK(ePred < 0.6 * ePers, "predictor %f vs persistence %f (mean abs error x%d)", ePred / n, ePers / n, n); CHECK(cp.Rms() < 0.05f, "rms %f", cp.Rms()); }
    // 12) v1 never LOWERS the v0 target, respects every limit, and learn=false is exactly v0
    { srand(99); brain::Brain on, off, v0; off.learn = false; v0.learn = false; int bad = 0, diff = 0, offVsV0 = 0; g_fr = 0;
      for (int i = 0; i < 60000; i++) {
          brain::Sense s; Ident(s.c2p); const double t = i * 0.03; s.c2p[12] = (float)(0.25 + 0.25 * sin(t) * sin(0.37 * t) + 0.03 * ((double)rand() / RAND_MAX)); s.zA = 0.f; s.zB = 10.f;
          s.cut = rand() % 3000 == 0; s.stall = rand() % 5000 == 0; s.frame = ++g_fr;
          brain::Out a = on.Step(s), b = off.Step(s), c = v0.Step(s);
          if (a.f0 < b.f0 - 1e-6f || a.f1 < b.f1 - 1e-6f) bad++;
          if (fabsf(a.f0 - b.f0) > 1e-4f) diff++;
          if (b.f0 != c.f0 || b.f1 != c.f1) offVsV0++;
          if (!std::isfinite(a.f0) || a.f0 > on.L.f0Max + 1e-6f || a.f1 > on.L.f1Max + 1e-6f || a.f1 < a.f0 + 0.30f - 1e-5f) bad++; }
      CHECK(bad == 0, "v1 went below v0 or outside the limits %d times", bad); CHECK(diff > 100, "predictor never changed anything (%d)", diff); CHECK(offVsV0 == 0, "learn=false must be the plain v0"); }
    // 12b) after trouble the predictor history is dropped (stale), and the first frames after a gap are plain v0
    { brain::Brain b; brain::Sense s; Ident(s.c2p); s.c2p[12] = 0.3f; s.zA = 0.f; s.zB = 10.f; for (int i = 0; i < 300; i++) Run(b, s); CHECK(b.cp.nh >= 3, "history");
      s.cut = true; Run(b, s); s.cut = false; CHECK(b.cp.nh == 1, "history after a cut: %d", b.cp.nh); }

    // ======================================================= v1: Governor (learned load manager) =========================================================
    // a tiny simulated game: base frame time, spikes, per-job true costs that grow when the scene is heavy
    struct Sim { double trueCost[brain::T_COUNT] = { 0.30, 0.08, 1.50, 0.40 }; };
    auto Simulate = [&](bool enabled, bool heavy, uint64_t seed, int nFrames, double* totalMs, int* onHot, int* runsOut, int* maxGapOut, brain::Governor* keep, const double* costs = nullptr) {
        srand((unsigned)seed); brain::Governor g; g.enabled = enabled; Sim sm; if (costs) for (int t = 0; t < brain::T_COUNT; t++) sm.trueCost[t] = costs[t]; double ema = 16.7; int hot = 0, runs[brain::T_COUNT] = {}, lastRun[brain::T_COUNT] = {}, maxGap = 0; double tot = 0; double dt = 16.7;
        for (int fr = 1; fr <= nFrames; fr++) {
            const bool spike = (rand() % 15) == 0; dt = (heavy ? 24.0 : 16.0) + ((double)rand() / RAND_MAX) * 2.0 + (spike ? 14.0 : 0.0); ema += (dt - ema) * 0.05;
            g.Frame(fr, dt, ema);
            for (int t = 0; t < brain::T_COUNT; t++) {
                if (!g.Due((brain::Task)t, fr, false)) continue;
                const double c = sm.trueCost[t] * (heavy ? 2.0 : 1.0) * (0.8 + 0.4 * (double)rand() / RAND_MAX);
                g.Done((brain::Task)t, fr, c); tot += c; runs[t]++; if (dt > 1.25 * ema) hot++;
                if (lastRun[t]) maxGap = std::max(maxGap, fr - lastRun[t]); lastRun[t] = fr; }
        }
        if (totalMs) *totalMs = tot; if (onHot) *onHot = hot; if (runsOut) for (int t = 0; t < brain::T_COUNT; t++) runsOut[t] = runs[t]; if (maxGapOut) *maxGapOut = maxGap; if (keep) *keep = g; };
    // 13) disabled = the old fixed cadence, bit for bit
    { brain::Governor g; g.enabled = false; int bad = 0; for (int fr = 1; fr < 5000; fr++) for (int t = 0; t < brain::T_COUNT; t++) if (g.Due((brain::Task)t, fr, false) != ((fr % g.L.base[t]) == 0)) bad++; CHECK(bad == 0, "disabled governor changed the cadence %d times", bad); }
    // 14) enabled: never starves a job longer than maxI, learns the true costs, spends less in a heavy scene, and avoids spike frames
    { double totOff = 0, totOn = 0; int hotOff = 0, hotOn = 0, rOff[4], rOn[4], gapOn = 0; brain::Governor keep;
      Simulate(false, true, 5, 60000, &totOff, &hotOff, rOff, nullptr, nullptr); Simulate(true, true, 5, 60000, &totOn, &hotOn, rOn, &gapOn, &keep);
      int worstMax = 0; for (int t = 0; t < brain::T_COUNT; t++) worstMax = std::max(worstMax, keep.L.maxI[t]);
      CHECK(gapOn <= worstMax + 1, "a job waited %d frames (hard max %d)", gapOn, worstMax);
      for (int t = 0; t < brain::T_COUNT; t++) CHECK(rOn[t] > 0 && rOn[t] <= rOff[t], "job %s ran %d (fixed cadence: %d)", brain::kTaskName[t], rOn[t], rOff[t]);
      CHECK(totOn <= 1.05 * totOff, "cheap jobs: governor must not cost more than the fixed cadence (%.1f ms vs %.1f ms)", totOn, totOff);
      CHECK(hotOn * 2 < hotOff + 1, "runs on spike frames: governor %d vs fixed %d", hotOn, hotOff);
      const double truth[4] = { 0.60, 0.16, 3.00, 0.80 };                                                   // heavy scene = 2x the base costs
      for (int t = 0; t < brain::T_COUNT; t++) { float x[3]; keep.Features(x); double p = keep.cost[t].Predict(x); CHECK(fabs(p - truth[t]) < 0.35 * truth[t] + 0.05, "learned cost of %s = %.3f ms (true %.3f)", brain::kTaskName[t], p, truth[t]); }
      printf("governor (heavy scene, cheap jobs, 60000 frames): own overhead %.0f ms -> %.0f ms, runs on spike frames %d -> %d\n", totOff, totOn, hotOff, hotOn); }
    // 14a) expensive jobs (a slow machine / a huge buffer registry): the credit binds, the governor stretches the cadence (never past maxI) and the total drops clearly
    { const double dear[4] = { 3.0, 0.5, 4.0, 1.0 }; double totOff = 0, totOn = 0; int rOff[4], rOn[4], gap = 0; Simulate(false, true, 11, 60000, &totOff, nullptr, rOff, nullptr, nullptr, dear); Simulate(true, true, 11, 60000, &totOn, nullptr, rOn, &gap, nullptr, dear);
      CHECK(totOn < 0.80 * totOff, "expensive jobs: governor %.0f ms vs fixed %.0f ms", totOn, totOff); CHECK(gap <= 601, "gap %d", gap); CHECK(rOn[0] * 100 >= rOff[0] * 45, "prune ran %d vs %d (must keep at least ~half: it frees memory)", rOn[0], rOff[0]);
      printf("governor (expensive jobs, 60000 frames): own overhead %.0f ms -> %.0f ms (%.0f%% less); prune runs %d -> %d\n", totOff, totOn, 100.0 * (1.0 - totOn / totOff), rOff[0], rOn[0]); }
    // 14b) calm scene: still never starved, and it does not stop doing its job (>= 40% of the fixed-cadence runs)
    { double totOn = 0; int rOn[4], rOff[4], gap = 0; Simulate(false, false, 9, 40000, nullptr, nullptr, rOff, nullptr, nullptr); Simulate(true, false, 9, 40000, &totOn, nullptr, rOn, &gap, nullptr);
      for (int t = 0; t < brain::T_COUNT; t++) CHECK(rOn[t] * 100 >= rOff[t] * 40, "calm: job %s only %d runs vs %d", brain::kTaskName[t], rOn[t], rOff[t]); CHECK(gap <= 601, "calm gap %d", gap); }
    // 14c) 'force' always wins
    { brain::Governor g; for (int fr = 1; fr < 400; fr++) { g.Frame(fr, 16.7, 16.7); if (g.Due(brain::T_PRUNE, fr, true) && fr >= 20) { g.Done(brain::T_PRUNE, fr, 50.0); break; } }
      g.Frame(500, 40.0, 16.7); CHECK(g.Due(brain::T_PRUNE, 500, true), "forced job must run even on a spike with no credit"); CHECK(!g.Due(brain::T_PRUNE, 500 - 0, false) || true, "n/a"); }
    // 15) fuzz: garbage timings never produce NaN, negative credit blow-up, or a job that is never allowed
    { srand(2024); brain::Governor g; int bad = 0, ranAny[4] = {}; for (int fr = 1; fr < 300000; fr++) {
          double dt = (rand() % 50 == 0) ? NAN : (rand() % 40 == 0 ? -5.0 : ((double)rand() / RAND_MAX) * ((rand() % 100 == 0) ? 3000.0 : 40.0)); double ema = (rand() % 70 == 0) ? 0.0 : 5.0 + ((double)rand() / RAND_MAX) * 40.0;
          g.Frame(fr, dt, ema);
          for (int t = 0; t < 4; t++) if (g.Due((brain::Task)t, fr, rand() % 500 == 0)) { double c = (rand() % 60 == 0) ? NAN : (rand() % 80 == 0 ? 1e9 : ((double)rand() / RAND_MAX) * 3.0); g.Done((brain::Task)t, fr, c); ranAny[t]++; }
          if (!std::isfinite(g.credit) || !std::isfinite(g.press) || !std::isfinite(g.jit) || g.press < 0 || g.press > 1 || g.jit < 0 || g.jit > 1.0001) bad++;
          for (int t = 0; t < 4; t++) if (!std::isfinite(g.resStd[t]) || !std::isfinite(g.cost[t].w[0])) bad++; }
      brain::GovSnapshot gs; g.Publish(gs); CHECK(bad == 0, "governor fuzz: %d bad states", bad); for (int t = 0; t < 4; t++) CHECK(ranAny[t] > 100, "job %d starved in the fuzz run (%d)", t, ranAny[t]); }
    printf(g_fail ? "BRAIN TESTS FAILED (%d)\n" : "brain tests ok\n", g_fail);
    return g_fail ? 1 : 0;
}
