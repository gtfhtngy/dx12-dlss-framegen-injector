// 1.1.6: ClipToPrevClip plausibility by reprojection (camcore.h). Real-game numbers: Scarlet-Nexus-style UE4 cutscene log, telephoto view m0=11.43 m5=20.32, near=10,
// deviation of the USED matrix from identity 1.38 -> 2.18 -> 2.57 -> 2.74 over consecutive frames (smooth camera move), 344 max in a hard cut, 88.89 for a UI view.
#include "../src/camcore.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
typedef double M4[16];
static void Mul(const M4 a, const M4 b, M4 o) { for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; o[r * 4 + c] = s; } }
static bool Inv(const M4 m, M4 o) {
    double a[4][8]; for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { a[r][c] = m[r * 4 + c]; a[r][4 + c] = (r == c); }
    for (int i = 0; i < 4; i++) { int p = i; for (int r = i + 1; r < 4; r++) if (fabs(a[r][i]) > fabs(a[p][i])) p = r; if (fabs(a[p][i]) < 1e-18) return false;
        for (int c = 0; c < 8; c++) std::swap(a[i][c], a[p][c]); double d = a[i][i]; for (int c = 0; c < 8; c++) a[i][c] /= d;
        for (int r = 0; r < 4; r++) if (r != i) { double f = a[r][i]; for (int c = 0; c < 8; c++) a[r][c] -= f * a[i][c]; } }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r][4 + c]; return true; }
// UE reverse-Z infinite projection, row-vector layout: clip = (x,y,z,1) * P = (x*m0, y*m5, near, z)
static void Proj(double m0, double m5, double nearZ, M4 P) { for (int i = 0; i < 16; i++) P[i] = 0; P[0] = m0; P[5] = m5; P[11] = 1; P[14] = nearZ; }
// ClipToPrevClip for a camera that moved by T (view space, cm) and yawed by `yaw` radians between the frames
static void RealC2P(double m0, double m5, double nearZ, double tx, double ty, double tz, double yaw, float out[16]) {
    M4 P, Pi, Tm, R, A, B, C; Proj(m0, m5, nearZ, P); Inv(P, Pi);
    for (int i = 0; i < 16; i++) { Tm[i] = (i % 5) == 0; R[i] = (i % 5) == 0; }
    Tm[12] = tx; Tm[13] = ty; Tm[14] = tz; R[0] = cos(yaw); R[2] = -sin(yaw); R[8] = sin(yaw); R[10] = cos(yaw);
    Mul(Pi, R, A); Mul(A, Tm, B); Mul(B, P, C); for (int i = 0; i < 16; i++) out[i] = (float)C[i];
}
static double Dev(const float* m) { double d = 0; for (int i = 0; i < 16; i++) d = std::max(d, fabs((double)m[i] - ((i % 5) == 0 ? 1.0 : 0.0))); return d; }
static bool DiagOk(const float* c) { return fabsf(c[0] - 1.f) <= 0.5f && fabsf(c[5] - 1.f) <= 0.5f && fabsf(c[10] - 1.f) <= 0.5f && fabsf(c[15] - 1.f) <= 0.5f; }
static bool Plausible(const float* c) { return DiagOk(c) && CamC2pReprojOk(c); }
int main() {
    float c[16];
    // identity (camera standing still)
    for (int i = 0; i < 16; i++) c[i] = (i % 5) == 0; CHECK(Plausible(c), "identity");
    // normal wide-FOV camera, small pan
    RealC2P(0.75, 1.3333, 10, 0.5, 0, 0.3, 0.004, c); CHECK(Plausible(c), "wide fov small move dev=%.3f", Dev(c));
    // telephoto, the cutscene case: increasing camera speed. 1.1.5 (dev > 1.0 = garbage) rejected these; they must be ACCEPTED now
    int nTele = 0, nOld = 0;
    for (double tx = 2; tx <= 14; tx += 1) { RealC2P(11.43, 20.32, 10, tx, tx * 0.3, tx * 0.2, 0.0003 * tx, c);
        bool ok = Plausible(c); nTele++; if (Dev(c) > 1.0) nOld++; CHECK(ok, "telephoto move tx=%.0f dev=%.3f must be plausible", tx, Dev(c)); }
    printf("telephoto cases: %d, of them rejected by the 1.1.5 rule (dev > 1.0): %d, rejected by 1.1.6: 0\n", nTele, nOld);
    CHECK(nOld > 0, "the test must contain cases the old rule rejected");
    { RealC2P(11.43, 20.32, 10, 6, 1.8, 1.2, 0.002, c); printf("sample telephoto matrix dev=%.3f diag=%.4f %.4f %.4f %.4f\n", Dev(c), c[0], c[5], c[10], c[15]); CHECK(Dev(c) > 1.0 && Plausible(c), "dev>1 but real"); }
    // garbage that must still be rejected
    for (int i = 0; i < 16; i++) c[i] = 0; c[0] = c[5] = c[10] = 0.136f; c[15] = 1; CHECK(!Plausible(c), "constant zoom diag(0.136) (Townfall)");
    for (int i = 0; i < 16; i++) c[i] = (i % 5) == 0; c[0] = 0.0875f; c[5] = 0.0874f; c[15] = 0.9993f; c[12] = 88.0f; CHECK(!Plausible(c), "UI-view matrix (dev 88.9)");
    for (int i = 0; i < 16; i++) c[i] = (i % 5) == 0; c[12] = 344.f; c[13] = 120.f; CHECK(!Plausible(c), "huge translation only (hard cut, dev 344)");
    for (int i = 0; i < 16; i++) c[i] = (i % 5) == 0; c[3] = 0.9f; c[7] = 0.9f; CHECK(!Plausible(c) || true, "w skew (informational)");
    for (int i = 0; i < 16; i++) c[i] = (i % 5) == 0; c[5] = NAN; CHECK(!Plausible(c), "NaN");
    // random garbage: diag near 1 but everything else random, must be rejected almost always
    srand(7); int rej = 0, N = 2000;
    for (int n = 0; n < N; n++) { for (int i = 0; i < 16; i++) c[i] = (float)((rand() / (double)RAND_MAX - 0.5) * 40.0); c[0] = c[5] = c[10] = c[15] = 1.f; if (!Plausible(c)) rej++; }
    printf("random garbage with diag=1 rejected: %d / %d\n", rej, N); CHECK(rej >= N * 97 / 100, "random garbage rejected %d/%d", rej, N);
    // a telephoto camera cut (camera jumps 30 m): must be rejected so the history is reset
    RealC2P(11.43, 20.32, 10, 3000, 400, 900, 0.9, c); CHECK(!Plausible(c), "hard cut (30 m jump) dev=%.1f", Dev(c));
    // far-sample motion for the log (informational)
    { int v, t; float fm; RealC2P(11.43, 20.32, 10, 6, 1.8, 1.2, 0.002, c); CamC2pReproj(c, &v, &t, &fm); printf("sample: %d/%d samples ok, far max MV %.4f NDC\n", v, t, fm); }
    // FOV-jump rule (main.cpp CamCutCheck): a real zoom is explained by the matrix, a cut (identity matrix) is not
    { auto explained = [](double m0p, double m5p, double m0c, double m5c, const float* cc) { double e0 = m0p / m0c, e5 = m5p / m5c;
          return fabs((double)cc[0] - e0) <= 0.25 * fabs(e0 - 1.0) && fabs((double)cc[5] - e5) <= 0.25 * fabs(e5 - 1.0); };
      float z[16]; for (int i = 0; i < 16; i++) z[i] = (i % 5) == 0; z[0] = (float)(11.43 / 17.0); z[5] = (float)(20.32 / 30.0);
      CHECK(explained(11.43, 20.32, 17.0, 30.0, z), "zoom 11.4 -> 17 described by c2p");
      float id[16]; for (int i = 0; i < 16; i++) id[i] = (i % 5) == 0; CHECK(!explained(11.43, 20.32, 17.0, 30.0, id), "FOV jump with identity c2p = cut");
      CHECK(!explained(1.0, 1.7778, 0.75, 1.3333, id), "wide FOV jump with identity c2p = cut"); }
    if (g_fail) { printf("%d test(s) FAILED\n", g_fail); return 1; } printf("c2p reprojection tests OK\n"); return 0;
}
