// brain.h - "Brain v1" (v0 rule + online-learned camera predictor + learned load governor). Session-only, safe limits, pure C++.
//   v1 additions (1.1.8+):
//   * CamPredictor: an online ridge regression (RLS, 4 weights) that predicts next frame's camera motion from the last 3. The label is the camera motion
//     that really happens one frame later = free self-supervised training data, no GPU readback. fall0 then opens one frame EARLIER when the camera accelerates and the
//     margin grows with the measured prediction error (erratic camera = wider window). Same hard limits as v0, only ever raises the v0 target, v0 rule is the fallback.
//   * Governor: learns what the injector's own periodic jobs (buffer prune, DLSS-G state poll, refresh-rate query, frame statistics) really cost on THIS machine
//     (online RLS per job on [1, pressure, jitter], measured with the clock around the job) and spends a small per-frame time credit on them.
//     Jobs wait for calm frames and for credit, but never longer than a hard maximum. Only the CADENCE of internal housekeeping changes - no user setting, no FG multiplier.
//   * Honest limit: neither part has a signal for the final picture quality (that needs the GPU statistics buffer, not in this version).
// "Brain v0": a small, safe, session-only controller for the MV pass quality parameters.
//   * pure C++ (no Windows / D3D) so tests/test_brain.cpp can drive it with synthetic camera sequences on any machine
//   * SENSOR : how far the camera alone moves the screen this frame (computed on the CPU from the same ClipToPrevClip matrix the MV shader uses; no GPU readback, no extra GPU work)
//   * ACTUATOR: the soft-falloff window of the MV shader (fall0 = where vectors start to fade, fall1 = where they are gone). Nothing else. It never touches the FG multiplier, the pause limit or any user setting.
//   * RULE   : vectors that the camera motion itself explains must not be faded. So fall0 follows the measured camera motion (fast attack, slow release), inside hard limits.
//              Trouble (camera cut, stall, FG paused, camera lost, MV pass failed, NaN) -> back to the defaults immediately and held there for a while.
//   * Nothing is stored on disk. Every start begins with the defaults. brain=0 in the cfg (or the menu switch) = the defaults forever (identical to 1.1.7 behaviour).
//   * v0 falloff rule = a physical argument, not a trained model (no quality signal). v1 adds the learned predictor on top of it. The log lines make it measurable (A/B: brain=0, brainlearn=0, brainload=0).
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace brain {

struct Limits {
    float f0Def = 0.35f, f1Def = 1.00f;      // the 1.1.7 compile-time values (SN_FALL0 / SN_FALL1)
    float f0Max = 0.60f;                     // fall0 never goes above this
    float f1Min = 0.85f, f1Max = 1.40f;      // fall1 stays inside this
    float gain = 1.15f, margin = 0.06f;      // fall0 target = gain * cameraMotion + margin
    float window = 0.55f;                    // fall1 target = fall0 + window
    float attack = 0.50f, release = 0.02f;   // smoothing of the camera motion (per frame): rising is fast, falling is slow
    float slewUp = 0.05f, slewDown = 0.004f; // max change of fall0 per frame (a falling threshold must not make vectors pop out of the fade)
    int   holdFrames = 90;                   // frames the defaults are held after trouble
    float marginMax = 0.12f, kErr = 1.5f;    // v1: learned extra margin = kErr * predictor RMS error, never above marginMax (the f0Max limit still applies on top)
    int   predReady = 90;                    // v1: predictor samples needed before it may influence anything
};

enum Mode { MODE_OFF = 0, MODE_ADAPT = 1, MODE_SAFE = 2 };

struct Sense {                               // everything the brain looks at, gathered once per frame by the host (main.cpp)
    int   frame = 0;
    double emaMs = 0;                        // smoothed base frame time
    bool  cut = false, stall = false, susp = false, camOk = true, mvFailed = false;
    float c2p[16] = {};                      // ClipToPrevClip (row-vector convention, as sent to the MV shader)
    float zA = 0, zB = 1;                    // depth = zA + zB / viewZ
};

struct Out { float f0, f1; };

struct Snapshot {                            // read by the overlay (hologram page) from another thread; plain values, a torn read only costs one wrong digit for one frame
    volatile int mode = MODE_OFF;
    volatile float cam = 0, f0 = 0.35f, f1 = 1.0f, activity = 0;   // activity 0..1: how far the brain is from the defaults right now
    volatile int adaptedPct = 0, troubleHolds = 0, frames = 0;
    volatile float predErr = 0; volatile int predReady = 0, learnOn = 0;   // v1: predictor RMS error (NDC), ready flag
};

// How far the camera alone moves the picture (NDC units, = the 'back' vector length of the MV shader for a static pixel).
// 5 screen points x 2 depths (a middle distance and a far one). Returns -1 if the matrix is unusable.
static inline float CamMotion(const float* m, float zA, float zB) {
    for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return -1.f;
    const float sB = zB < 0.f ? -1.f : 1.f;
    const float eMid = 0.02f, eFar = 0.001f;                                   // e = |zB| / viewZ (same meaning as in the shader)
    const float pts[5][2] = { { 0.f, 0.f }, { 0.9f, 0.9f }, { -0.9f, 0.9f }, { 0.9f, -0.9f }, { -0.9f, -0.9f } };
    float best = 0.f;
    for (int k = 0; k < 2; k++) {
        const float d = std::min(1.f, std::max(0.f, zA + sB * (k ? eFar : eMid))), wgt = k ? 1.0f : 0.7f;   // the middle distance counts a bit less (a lot of parallax from a single close object must not dominate)
        for (auto& p : pts) {
            const float v[4] = { p[0], p[1], d, 1.f }; float r[4];
            for (int j = 0; j < 4; j++) r[j] = v[0] * m[0 + j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];   // mul(float4, row_major float4x4)
            if (!(r[3] > 1e-6f) || !std::isfinite(r[3])) continue;               // behind the previous camera: the shader treats it as a hole, so it says nothing about camera motion
            const float bx = p[0] - r[0] / r[3], by = p[1] - r[1] / r[3];
            if (!std::isfinite(bx) || !std::isfinite(by)) continue;
            best = std::max(best, wgt * sqrtf(bx * bx + by * by));
        }
    }
    return best;
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// v1: online ridge regression (recursive least squares with forgetting). Double precision inside, finite-checked, covariance capped (no wind-up when the input does not move).
template <int N> struct Rls {
    double w[N], P[N][N]; double lambda = 0.998, pmax = 1e3; uint32_t n = 0;
    Rls() { Reset(); }
    void Reset(double p0 = 100.0) { for (int i = 0; i < N; i++) { w[i] = 0; for (int j = 0; j < N; j++) P[i][j] = i == j ? p0 : 0; } n = 0; }
    double Predict(const float* x) const { double r = 0; for (int i = 0; i < N; i++) r += w[i] * x[i]; return r; }
    double Quad(const float* x) const { double r = 0; for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) r += x[i] * P[i][j] * x[j]; return r; }   // x'Px = how unsure the model is for this input
    bool Update(const float* x, double y) {
        if (!std::isfinite(y)) return false; for (int i = 0; i < N; i++) if (!std::isfinite(x[i])) return false;
        double Px[N]; for (int i = 0; i < N; i++) { Px[i] = 0; for (int j = 0; j < N; j++) Px[i] += P[i][j] * x[j]; }
        double den = lambda; for (int i = 0; i < N; i++) den += x[i] * Px[i];
        if (!(den > 1e-9) || !std::isfinite(den)) return false;
        const double e = y - Predict(x); double k[N]; for (int i = 0; i < N; i++) k[i] = Px[i] / den;
        for (int i = 0; i < N; i++) w[i] += k[i] * e;
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) P[i][j] = (P[i][j] - k[i] * Px[j]) / lambda;
        for (int i = 0; i < N; i++) for (int j = i + 1; j < N; j++) { const double a = 0.5 * (P[i][j] + P[j][i]); P[i][j] = P[j][i] = a; }
        for (int i = 0; i < N; i++) { if (!(P[i][i] > 1e-9)) P[i][i] = 1e-9; if (P[i][i] > pmax) P[i][i] = pmax; }
        bool bad = false; for (int i = 0; i < N; i++) if (!std::isfinite(w[i])) bad = true;
        if (bad) { Reset(); return false; }
        n++; return true;
    }
};

// v1: predicts the camera motion of the NEXT frame from the last three. Trained on what really happened (self-supervised).
struct CamPredictor {
    Rls<4> rls; float h[3] = { 0, 0, 0 }; int nh = 0; double e2 = 0.0; float pred = 0.f; uint32_t nTrain = 0;
    void Reset() { nh = 0; h[0] = h[1] = h[2] = 0.f; pred = 0.f; }                      // after trouble the history is stale; the learned weights stay
    bool Ready(int need) const { return nTrain >= (uint32_t)need && nh >= 3; }
    float Rms() const { return (float)std::sqrt(std::max(0.0, e2)); }
    // cam = camera motion measured this frame (>= 0). Returns the prediction for the next frame.
    float Step(float cam) {
        if (!std::isfinite(cam) || cam < 0.f) { Reset(); return 0.f; }
        if (nh >= 3) {                                                                    // the label of the previous features is the camera motion we see now
            const float f[4] = { 1.f, h[0], h[1], h[2] };
            const double err = (double)cam - rls.Predict(f); e2 += 0.02 * (err * err - e2);
            if (rls.Update(f, (double)cam)) nTrain++;
        }
        h[2] = h[1]; h[1] = h[0]; h[0] = cam; if (nh < 3) nh++;
        if (nh >= 3) { const float f[4] = { 1.f, h[0], h[1], h[2] }; pred = (float)std::min(2.0 * std::max((double)cam, 0.05) + 0.05, std::max(0.0, rls.Predict(f))); } else pred = cam;
        return pred;
    }
};

struct Brain {
    Limits L; bool enabled = true, learn = true;   // learn=false: exactly the v0 rule (A/B switch: brainlearn=0)
    CamPredictor cp;
    float m = 0.f;                           // smoothed camera motion
    float f0 = 0.35f, f1 = 1.0f;
    int hold = 0; Mode mode = MODE_OFF; float lastCam = 0.f; int lastFrame = -1000000;
    uint32_t nFrames = 0, nAdapted = 0, nHolds = 0;

    void Reset() { m = 0.f; f0 = L.f0Def; f1 = L.f1Def; hold = 0; }

    // One call per MV pass. Returns the two values for the shader constants.
    Out Step(const Sense& s) {
        nFrames++;
        if (!enabled) { mode = MODE_OFF; f0 = L.f0Def; f1 = L.f1Def; m = 0.f; return { f0, f1 }; }
        const float cam = CamMotion(s.c2p, s.zA, s.zB); lastCam = std::max(cam, 0.f);
        const bool gap = nFrames > 1 && s.frame - lastFrame > 2; lastFrame = s.frame;      // the MV pass did not run for a while (FG was off / paused): the old smoothing is stale, start safe
        const bool trouble = gap || s.cut || s.stall || s.susp || !s.camOk || s.mvFailed || cam < 0.f;
        if (trouble) { if (hold == 0) nHolds++; hold = L.holdFrames; m = 0.f; cp.Reset(); }
        const float pr = learn ? cp.Step(cam) : 0.f;                                          // v1: always trains (also in SAFE), only used when ready and not in trouble
        const bool useP = learn && cp.Ready(L.predReady);
        float t0, t1;
        if (hold > 0) { hold--; mode = MODE_SAFE; t0 = L.f0Def; t1 = L.f1Def; }
        else {
            mode = MODE_ADAPT; m += (cam > m ? L.attack : L.release) * (cam - m);
            const float mEff = useP ? std::max(m, pr) : m;                                       // v1: anticipate acceleration (never lowers the v0 target)
            const float mar = L.margin + (useP ? std::min(L.marginMax - L.margin, std::max(0.f, L.kErr * cp.Rms())) : 0.f);   // v1: erratic camera (big prediction error) = wider window
            t0 = std::min(L.f0Max, std::max(L.f0Def, L.gain * mEff + mar));
            t1 = std::min(L.f1Max, std::max(L.f1Min, std::max(L.f1Def, t0 + L.window)));
        }
        if (mode == MODE_SAFE) { f0 = t0; f1 = t1; }                                       // trouble: defaults at once, no slewing
        else { f0 += std::min(L.slewUp, std::max(-L.slewDown, t0 - f0)); f1 += std::min(L.slewUp, std::max(-L.slewDown, t1 - f1)); }
        f0 = std::min(L.f0Max, std::max(L.f0Def, f0)); f1 = std::min(L.f1Max, std::max(L.f1Min, f1));
        if (f1 < f0 + 0.30f) f1 = std::min(L.f1Max, f0 + 0.30f);                           // the window must keep a real width (smoothstep needs fall1 > fall0)
        if (f0 > L.f0Def + 0.01f) nAdapted++;
        return { f0, f1 };
    }

    void Publish(Snapshot& o) const {
        o.mode = (int)mode; o.cam = lastCam; o.f0 = f0; o.f1 = f1;
        o.activity = std::min(1.f, std::max(0.f, (f0 - L.f0Def) / std::max(1e-3f, L.f0Max - L.f0Def)));
        o.adaptedPct = nFrames ? (int)(100.0 * nAdapted / nFrames + 0.5) : 0; o.troubleHolds = (int)nHolds; o.frames = (int)nFrames;
        o.predErr = cp.Rms(); o.predReady = (learn && cp.Ready(L.predReady)) ? 1 : 0; o.learnOn = learn ? 1 : 0;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// v1 Governor: schedules the injector's own periodic housekeeping so it costs the real game frames as little as possible.
//   * It learns the real cost (ms, measured around the job by the host) of each job per machine / game: online RLS on [1, pressure, jitter].
//   * Each frame it earns a small time credit (share of the frame time, less under pressure / jitter). A due job runs when its pessimistic cost (prediction + 1 sigma) fits
//     into the credit AND the last frame was not a spike. Otherwise it waits - but never longer than maxI (hard limit) and never when the host says 'force'.
//   * disabled (brainload=0 or brain=0) = exactly the old fixed cadence (frame % base == 0).
//   It changes WHEN internal jobs run. It never touches user settings, the FG multiplier, the pause limit or any MV / DLSS-G parameter.
enum Task { T_PRUNE = 0, T_STATE, T_REFRESH, T_RATE, T_COUNT };
static const char* const kTaskName[T_COUNT] = { "prune", "state", "refresh", "rate" };

struct GovLimits {
    int   base[T_COUNT] = { 20, 30, 300, 300 };      // the fixed 1.1.8 cadence (frames)
    int   maxI[T_COUNT] = { 40, 90, 600, 600 };      // hard maximum wait (frames): a job is never starved longer than this
    float share = 0.004f;                            // credit earned per frame = share * frame time (0.4 %)
    float capFrames = 40.f;                          // credit cap = capFrames * share * frame time
    float hotRatio = 1.25f;                          // last frame longer than hotRatio * average = spike: do not pile work on it
    int   minSamples = 4;                            // jobs run on the fixed schedule until the cost model has seen this many runs
    float kStd = 1.0f;                               // pessimism: cost estimate = prediction + kStd * residual std
};

struct GovSnapshot {                                 // read by the overlay from another thread; plain values
    volatile int on = 0, frames = 0;
    volatile float share = 0, pressure = 0, jitter = 0, credit = 0, spentMsPerSec = 0;
    volatile float cost[T_COUNT] = {}; volatile int runs[T_COUNT] = {}, defers[T_COUNT] = {}, forced[T_COUNT] = {};
};

struct Governor {
    GovLimits L; bool enabled = true;
    double credit = 0, ema = 16.7, best = 0, jit = 0, press = 0, spentMs = 0, spentWin = 0; bool hot = false; int frames = 0, lastFrame = -1;
    Rls<3> cost[T_COUNT]; double resStd[T_COUNT] = {}; int last[T_COUNT]; bool seen[T_COUNT] = {}, deferred[T_COUNT] = {};
    uint32_t nRun[T_COUNT] = {}, nDefer[T_COUNT] = {}, nForce[T_COUNT] = {};
    Governor() { for (int i = 0; i < T_COUNT; i++) { last[i] = 0; cost[i].lambda = 0.9995; cost[i].pmax = 50.0; cost[i].Reset(10.0); } }

    void Features(float* x) const { x[0] = 1.f; x[1] = (float)press; x[2] = (float)jit; }

    // once per game frame. dtMs = time since the previous Present began, emaMs = the smoothed value.
    void Frame(int frame, double dtMs, double emaMs) {
        frames++; lastFrame = frame;
        if (!(emaMs > 0.5) || !std::isfinite(emaMs)) return;
        ema = emaMs; best = (best <= 0.0) ? ema : std::min(ema, best * 1.0002);          // slowly rising floor = the best (fastest) average seen
        press = std::min(1.0, std::max(0.0, ema / std::max(best, 0.5) - 1.0));
        if (dtMs > 0.0 && std::isfinite(dtMs)) { jit += 0.02 * (std::min(1.0, std::fabs(dtMs - ema) / ema) - jit); hot = dtMs > (double)L.hotRatio * ema; } else hot = false;
        if (!enabled) return;
        const double sh = (double)L.share * (1.0 - 0.5 * press) * (1.0 - 0.5 * jit);
        credit = std::min(credit + sh * ema, (double)L.capFrames * (double)L.share * ema);
        spentWin *= 0.9995;                                                              // slow decay: ~ms spent in the last ~2000 frames
    }
    double UpperCost(int t) const {
        float x[3]; Features(x); const double p = std::max(0.0, cost[t].Predict(x));
        return p + (double)L.kStd * resStd[t];
    }
    // Should job t run in this frame? 'force' = the host knows it is urgent (e.g. the buffer registry is nearly full).
    bool Due(Task t, int frame, bool force = false) {
        if (!enabled) return (frame % L.base[t]) == 0;                                  // exactly the old behaviour
        const int age = seen[t] ? frame - last[t] : (frame % L.base[t] == 0 ? L.base[t] : 0);
        if (age < L.base[t]) return false;
        if (force || age >= L.maxI[t] || (int)cost[t].n < L.minSamples) return true;
        if (!hot && UpperCost(t) <= credit) return true;
        if (!deferred[t]) { deferred[t] = true; nDefer[t]++; }
        return false;
    }
    // the host calls this after the job ran (costMs = clock around it, including any lock wait)
    void Done(Task t, int frame, double costMs) {
        if (!std::isfinite(costMs) || costMs < 0.0) costMs = 0.0; costMs = std::min(costMs, 1000.0);
        if (seen[t] && deferred[t] && frame - last[t] >= L.maxI[t]) nForce[t]++;
        deferred[t] = false; last[t] = frame; seen[t] = true; nRun[t]++;
        credit = std::max(credit - costMs, -(double)L.capFrames * (double)L.share * ema); spentMs += costMs; spentWin += costMs;
        float x[3]; Features(x); const double err = costMs - cost[t].Predict(x);
        if (cost[t].Update(x, costMs)) resStd[t] += 0.05 * (std::fabs(err) - resStd[t]);
    }
    void Publish(GovSnapshot& o) const {
        o.on = enabled ? 1 : 0; o.frames = frames; o.share = L.share; o.pressure = (float)press; o.jitter = (float)jit; o.credit = (float)credit;
        o.spentMsPerSec = (float)(spentWin / 2000.0 * 1000.0 / std::max(ema, 1.0));      // ms per second of game time (window ~2000 frames)
        for (int i = 0; i < T_COUNT; i++) { float x[3]; Features(x); o.cost[i] = (float)std::max(0.0, cost[i].Predict(x)); o.runs[i] = (int)nRun[i]; o.defers[i] = (int)nDefer[i]; o.forced[i] = (int)nForce[i]; }
    }
};

}   // namespace brain
