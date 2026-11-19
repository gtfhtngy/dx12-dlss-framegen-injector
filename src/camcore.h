// camcore.h (v27) - the pure part of the UE4/UE5 view-buffer scan (no windows.h, no globals): used by main.cpp AND by tests/replay_viewbuf.cpp,
// so a recorded SN_DLSSG_viewbuf.txt dump can be replayed on any PC without running the game (regression test for the camera detection).
#pragma once
#include <cmath>
#include <cstring>

// ViewToClip-like perspective matrix (row-vector layout as stored in the UE uniform buffer). loose = only a plausible aspect range, else aspect must match refAspect
static inline bool CamIsProj(const float* m, bool loose, float refAspect) {
    float asp = m[5] / m[0];
    bool aspOk = loose ? (asp > 1.2f && asp < 3.8f) : fabsf(asp - refAspect) < 0.03f;   // 3.8 = 32:9 super-ultrawide
    return m[0] > 0.2f && m[5] > 0.2f && m[1] == 0.f && m[2] == 0.f && m[3] == 0.f && m[4] == 0.f && m[6] == 0.f && m[7] == 0.f
        && fabsf(m[11]) == 1.f && m[12] == 0.f && m[13] == 0.f && m[15] == 0.f && aspOk;
}
static inline bool CamLooksLikeC2P(const float* c) {
    for (int i = 0; i < 16; i++) if (!std::isfinite(c[i]) || fabsf(c[i]) > 4.f) return false;
    return fabsf(c[0] - 1.f) < 0.5f && fabsf(c[5] - 1.f) < 0.5f && fabsf(c[10] - 1.f) < 0.5f && fabsf(c[15] - 1.f) < 0.5f;
}
// finds ViewToClip (jittered) at float index *ji and ViewToClipNoAA (up to 128 floats later) at *nj. *projSeen += number of projection-like matrices met on the way
static inline bool CamFindPair(const float* p, int avail, int* ji, int* nj, long* projSeen) {
    *ji = -1; *nj = -1;
    for (int i = 0; i + 16 <= avail && *ji < 0; i += 4) {
        if (!CamIsProj(p + i, true, 0.f)) continue;
        if (projSeen) (*projSeen)++;
        for (int j = i + 4; j <= i + 128 && j + 16 <= avail; j += 4) {
            const float* a = p + i; const float* b = p + j;
            if (CamIsProj(b, true, 0.f) && fabsf(b[8]) < 1e-9f && fabsf(b[9]) < 1e-9f && fabsf(a[0] - b[0]) <= 1e-5f * fabsf(a[0]) && fabsf(a[5] - b[5]) <= 1e-5f * fabsf(a[5]) && ((a[8] != 0.f || a[9] != 0.f) || !memcmp(a, b, 64))) { *ji = i; *nj = j; break; }
        }
    }
    return *ji >= 0;
}
// identity-like matrices behind ViewToClipNoAA (ClipToPrevClip candidates), at most 4
static inline int CamCollectC2P(const float* p, int avail, int nj, int cand[4]) {
    int nc = 0; for (int q = nj + 16; q + 16 <= avail && nc < 4; q += 4) if (CamLooksLikeC2P(p + q)) cand[nc++] = q; return nc;
}

// ---- 1.1.6: judge a ClipToPrevClip by its EFFECT, not by the size of its entries.
// The MV shader does   pc = mul(float4(sp, z, 1), M);   back = sp - pc.xy / pc.w   (row-vector layout, sp = NDC xy, z = reverse-Z depth value).
// In a narrow-FOV (telephoto) cutscene (m0=11.4, m5=20.3) even a small, perfectly valid camera move gives translation entries of 1..300 (1.1.5 threw those away).
// So: reproject a grid of screen points at several depths. A real matrix moves every point by a bounded amount (< half the screen = 1.0 NDC);
// a garbage matrix (random / UI / other-view data) does not. z = near/d for UE's reverse-Z: 0.1 .. 0 = 10x near .. infinity.
static inline void CamC2pReproj(const float* M, int* valid, int* total, float* farMaxMv) {
    static const float kX[3] = { -0.9f, 0.f, 0.9f };
    static const float kZ[5] = { 0.1f, 0.02f, 0.004f, 0.0008f, 0.f };
    int v = 0, t = 0; float fm = 0.f;
    for (int zi = 0; zi < 5; zi++) for (int yi = 0; yi < 3; yi++) for (int xi = 0; xi < 3; xi++) {
        double sx = kX[xi], sy = kX[yi], z = kZ[zi];
        double px = sx * M[0] + sy * M[4] + z * M[8] + M[12];
        double py = sx * M[1] + sy * M[5] + z * M[9] + M[13];
        double pw = sx * M[3] + sy * M[7] + z * M[11] + M[15];
        t++;
        if (!(pw > 1e-6) || !std::isfinite(px) || !std::isfinite(py)) continue;
        double bx = sx - px / pw, by = sy - py / pw;
        if (!std::isfinite(bx) || !std::isfinite(by)) continue;
        double m = std::max(fabs(bx), fabs(by));
        if (zi >= 3 && m > fm) fm = (float)m;
        if (m <= 1.0) v++;
    }
    if (valid) *valid = v; if (total) *total = t; if (farMaxMv) *farMaxMv = fm;
}
// plausible = at least 60% of the sample points reproject to a sane place (3 of the 5 depth layers). diag / invertibility checks stay with the caller.
static inline bool CamC2pReprojOk(const float* M) {
    for (int i = 0; i < 16; i++) if (!std::isfinite(M[i])) return false;
    int v = 0, t = 0; CamC2pReproj(M, &v, &t, nullptr);
    return v * 10 >= t * 6;
}
