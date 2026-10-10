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
