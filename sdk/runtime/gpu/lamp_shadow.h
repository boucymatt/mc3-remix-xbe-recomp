#pragma once
// The street lamps' shadows (render_profile.h kLightShadow, MC3-0012). MC3 bakes its lamps' light into the city's vertex
// colours and draws a glow at each lamp head; the glows give the lamps' places, and a lamp's shadow takes back part of the
// baked light where something stands between the lamp and the ground.

namespace gpu {

// b1: m0-m3 this frame's inverse view x projection rows; l0-l3 the lamp's view x projection rows; lamp its position and
// the radius it lights; vp the viewport in target pixels; z min depth, max depth, one tile's side, strength; e constant
// and slope bias in metres, pcf radius, near; f the tile's corner in the atlas (texels), far, debug
inline constexpr int kLampReceiverConstants = 52;
inline const char* kLampReceiverHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, l0, l1, l2, l3, lamp, vp, z, e, f; };
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
    float3 tl = lamp.xyz - wp;
    if (dot(nrm, tl) < 0) nrm = -nrm;
    if (d >= z.y) return 0;
    float dist = length(tl);
    if (dist > lamp.w) return 0;
    float ndl = dot(nrm, tl / max(dist, 1e-4));
    if (ndl <= 0.05) return 0;
    float4 s = wp.x * l0 + wp.y * l1 + wp.z * l2 + l3;
    if (s.w <= e.w) return 0;
    float3 n = s.xyz / s.w;
    float2 uv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
    if (any(uv < 0) || any(uv > 1) || n.z > 1) return 0;
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
    float occ = 1 - lit / cnt;
    if (f.w > 2.5) return float4(0, 0, 0, occ);
    float fall = saturate(1 - dist / lamp.w);
    return float4(0, 0, 0, z.w * occ * fall * saturate(ndl * 2));
}
)";

}  // namespace gpu
