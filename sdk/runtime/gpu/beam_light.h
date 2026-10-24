#pragma once
// The cars' headlight beams (render_profile.h kLightShadow, MC3-0012), after MCLA's nights: each car near the camera
// throws a pool of warm light on the road ahead of it, and what stands in the beam shades it. MC3's own headlights only
// light the facades far ahead; this lights what the beam reaches, multiplied into what is already there.

namespace gpu {

// b1: m0-m3 this frame's inverse view x projection rows; l0-l3 the beam's view x projection rows; lp its position and
// reach; ld its direction and the cosine of the cone's edge; col its colour and strength; vp the viewport in target
// pixels; z min depth, max depth, one tile's side, the cosine where the cone is full; e constant and slope bias in metres,
// pcf radius, near; f the tile's corner in the atlas (texels), far, debug
inline constexpr int kBeamConstants = 64;
inline const char* kBeamHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, l0, l1, l2, l3, lp, ld, col, vp, z, e, f; };
Texture2D<float> depth : register(t0);
Texture2D<float> smap : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float4 psmain(O i) : SV_Target {
    float d = depth.Load(int3(i.p.xy, 0));
    float2 ndc = float2((i.p.x - vp.x) / vp.z * 2 - 1, 1 - (i.p.y - vp.y) / vp.w * 2);
    float4 w = ndc.x * m0 + ndc.y * m1 + ((d - z.x) / max(z.y - z.x, 1e-6)) * m2 + m3;
    float3 wp = w.xyz / w.w;
    float3 nrm = cross(ddy(wp), ddx(wp));
    nrm = nrm / max(length(nrm), 1e-9);
    float3 tl = lp.xyz - wp;
    if (dot(nrm, tl) < 0) nrm = -nrm;
    if (d >= z.y) return 0;
    float dist = length(tl);
    if (dist > lp.w || dist < 1e-3) return 0;
    float3 L = tl / dist;
    float cd = dot(-L, ld.xyz);
    if (cd <= ld.w) return 0;
    float cone = smoothstep(ld.w, z.w, cd);
    // a headlight meets the road at a few degrees: its light there is the cone's, not the cosine's, or the road it is
    // aimed at would get a tenth of it
    float ndl = dot(nrm, L);
    if (ndl <= 0) return 0;
    ndl = saturate(0.2 + 3 * ndl);
    float fall = saturate(1 - dist / lp.w);
    fall *= fall;
    // the shadow: what the previous frame drew between the lamp and this point
    float vis = 1;
    float4 s = wp.x * l0 + wp.y * l1 + wp.z * l2 + l3;
    if (s.w > e.w) {
        float3 n = s.xyz / s.w;
        float2 uv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
        if (all(uv >= 0) && all(uv <= 1) && n.z <= 1) {
            float N = e.w, F = f.z, size = z.z;
            float bias = e.x + e.y * s.w;
            int k = (int)e.z;
            float lit = 0, cnt = 0;
            for (int y = -k; y <= k; y++)
                for (int x = -k; x <= k; x++) {
                    float m = smap.Load(int3(f.xy + clamp(uv * size + float2(x, y), 0, size - 1), 0));
                    float ml = N * F / max(F - m * (F - N), 1e-6);
                    lit += s.w - bias <= ml ? 1 : 0;
                    cnt += 1;
                }
            vis = lit / cnt;
        }
    }
    if (f.w > 0.5) return float4(vis < 0.5 ? float3(1, 0, 0) : float3(0, 0.6, 0), 0) * cone * fall;
    return float4(col.rgb * (col.a * cone * fall * ndl * vis), 0);
}
)";

}  // namespace gpu
