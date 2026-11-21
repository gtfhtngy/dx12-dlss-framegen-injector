// mv_shader.inc - motion-vector compute shader (included from main.cpp; its text is part of the SNSC cache key, see MvSourceKey)
static const char* kMvHlsl = R"HLSL(
// SN_MV_CS v3 - engine velocity / camera reprojection -> pixel MVs (R16G16_FLOAT) + depth copy (R32_FLOAT)
// Bindings unchanged: b0 = 24 root dwords, t0 velocity (UNORM, 0 = static), t1 depth, u0 MV, u1 depth copy. Compiles as cs_5_1 (FXC) and cs_6_x.
cbuffer C : register(b0)
{
    row_major float4x4 M;   // ClipToPrevClip
    uint   dimP;            // view rect  : x | y << 16   (was uint2 dim)
    uint   fdimP;           // buffer size: x | y << 16   (was uint2 fdim)
    float2 sgn;             // mvSX / mvSY
    float  zA, zB;          // depth = zA + zB / viewZ   (clip.z = zA*z + zB, clip.w = z; = ViewToClip[2][2], [3][2])
    float  fall0, fall1;    // 1.1.8 (brain): soft-falloff window in NDC (1.1.7 had these as compile-time constants 0.35 / 1.00); 24 root dwords
};                          // reverse-Z <=> zB > 0.  UE (infinite far, reverse-Z): zA = 0, zB = near.
Texture2D<float2>   V  : register(t0);
Texture2D<float>    D  : register(t1);
RWTexture2D<float2> O  : register(u0);
RWTexture2D<float>  OD : register(u1);

#ifndef SN_EDGE_REL
#define SN_EDGE_REL 0.05  // relative linear-depth gap that counts as a silhouette (planarity-gated)
#endif
#ifndef SN_FILL_REL
#define SN_FILL_REL 0.08  // relative depth range inside which a neighbour may donate its MV to a hole
#endif
#ifndef SN_DILATE
#define SN_DILATE  1
#endif

#define GX 16
#define GY 8
#define TW (GX + 2)
#define TH (GY + 2)
#define TN (TW * TH)      // 180 texels, 2 per thread at most

// x,y = back (NDC, cur - prev, UE sign) | z = e = |zB| / viewZ (0 = infinitely far) | w = 1 valid, 0 hole
groupshared float4 gT[TN];

float Fade(float2 b) { return 1.0 - smoothstep(fall0, fall1, length(b)); }

[numthreads(GX, GY, 1)]
void main(uint3 t : SV_DispatchThreadID, uint3 gt : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint2  dim  = uint2(dimP & 0xFFFFu, dimP >> 16);
    const uint2  fdim = uint2(fdimP & 0xFFFFu, fdimP >> 16);
    const int2   vmax = int2(max(dim, uint2(1, 1))) - 1;
    const float2 inv2 = 2.0 / float2(max(dim, uint2(1, 1)));
    const float  sB   = (zB < 0.0) ? -1.0 : 1.0;
    const float  farD = (zB > 0.0) ? 0.0 : 1.0;
    const int2   g0   = int2(gid.xy) * int2(GX, GY);
    const uint   gi   = gt.y * GX + gt.x;

    // ---- phase 1: every tile texel (16x8 + 1-texel halo) is decoded ONCE; no thread returns before the barrier
    [unroll] for (uint k = 0; k < 2; k++)
    {
        const uint i = gi + k * (GX * GY);
        if (i < TN)
        {
            const int2 lt = int2(i % TW, i / TW);
            const int2 gp = g0 + lt - 1;
            const int2 q  = clamp(gp, int2(0, 0), vmax);          // halo never reads outside the real view rect (stale data with dynamic resolution)

            float d = D.Load(int3(q, 0));
            d = isfinite(d) ? saturate(d) : farD;
            const float e = max((d - zA) * sB, 0.0);               // = |zB| / viewZ : linear depth for BOTH conventions, no divide

            float2 b = float2(0.0, 0.0);
            float  valid = 0.0;

            const float2 v = V.Load(int3(q, 0));
            if (any(v > 0.0))                                      // engine velocity written (UE: 0 = static pixel)
            {
                const float2 bE = (v - 32767.0 / 65535.0) * (1.0 / (0.499 * 0.5));
                if (all(isfinite(bE)) && length(bE) < fall1) { b = bE; valid = 1.0; }
            }
            if (valid == 0.0)                                      // static pixel / unusable velocity -> camera reprojection (clip z = raw depth, no linearization needed)
            {
                const float2 sp = float2((q.x + 0.5) * inv2.x - 1.0, 1.0 - (q.y + 0.5) * inv2.y);
                const float4 pc = mul(float4(sp, d, 1.0), M);
                if (pc.w > 1e-6 && all(isfinite(pc.xyw)))
                {
                    const float2 br = sp - pc.xy / pc.w;
                    if (all(isfinite(br))) { b = br; valid = 1.0; }
                }
            }
            gT[i] = float4(b, e, valid);

            // depth copy: written by the thread that decoded the texel, interior texels only
            if (all(lt >= 1) && lt.x <= GX && lt.y <= GY && all(uint2(gp) < fdim))
                OD[gp] = all(uint2(gp) < dim) ? d : 0.0;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (any(t.xy >= fdim)) return;
    if (any(t.xy >= dim)) { O[t.xy] = float2(0.0, 0.0); return; }  // outside the view rect: never leave stale data

    // ---- phase 2: hole fill + silhouette dilation from LDS (no more global reads)
    const int    ci   = (gt.y + 1) * TW + gt.x + 1;
    const float4 c    = gT[ci];
    const float  eSky = abs(zB) * 1e-7;                            // viewZ > 1e7 = sky
    const float  eRef = max(c.z, eSky);
    float2 b = c.xy;

    float2 fs = float2(0.0, 0.0); float fw = 0.0;                  // depth-weighted average of valid neighbours
    float  eN = c.z; float2 bN = b; int2 oN = int2(0, 0);          // nearest valid neighbour
    [unroll] for (int dy = -1; dy <= 1; dy++)
    [unroll] for (int dx = -1; dx <= 1; dx++)
    {
        if (dx == 0 && dy == 0) continue;
        const float4 n = gT[ci + dy * TW + dx];
        const float  w = n.w * saturate(1.0 - abs(n.z - c.z) / (SN_FILL_REL * eRef));
        fs += w * n.xy; fw += w;
        if (n.w > 0.5 && n.z > eN) { eN = n.z; bN = n.xy; oN = int2(dx, dy); }
    }

    if (c.w < 0.5)                                                 // hole (w <= 0 behind previous camera, NaN, ...): borrow, fade out if little support
        b = (fw > 1e-4) ? (fs / fw) * saturate(fw * 0.5) : float2(0.0, 0.0);

#if SN_DILATE
    if (eN > c.z)                                                  // a nearer valid neighbour: foreground silhouette?
    {
        const float eO  = gT[ci - oN.y * TW - oN.x].z;             // opposite neighbour: plane through (eO, c) predicts 2c - eO at the candidate
        const float thr = SN_EDGE_REL * eRef;
        if ((eN - c.z) > thr && (eN - (2.0 * c.z - eO)) > thr) b = bN;   // real step, not just a slanted surface
    }
#endif

    b *= Fade(b);                                                  // soft falloff (C1), replaces the hard 0.5 cut
    O[t.xy] = float2(-b.x * 0.5 * dim.x * sgn.x, b.y * 0.5 * dim.y * sgn.y);
}
)HLSL";
