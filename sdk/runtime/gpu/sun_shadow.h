#pragma once
// The sun-shadow feature (render_profile.h kSunShadow): directional shadow from the title's environment light, one
// cascade over the first range metres of the camera's view and a coarser one out to far. Casters are the previous frame's streamed scene geometry
// (vehicle_shadow.h) put back in the world; the player's car moves with it (or is left to the vehicle shadow). What
// the title did not draw last frame, everything it culled to the camera, casts nothing.

namespace gpu {

// b1: m0-m3 the previous frame's inverse view x projection rows; p0-p2, p3 the previous car's axes and position; c0-c2,
// c3 this frame's; l0-l3 the light's view x projection rows; z min depth, max depth, car mode (0 none, 1 moved with
// the car, 2 dropped); box the car's half width, top, half length, bottom
inline constexpr int kSunCasterConstants = 72;
inline const char* kSunCasterHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, p0, p1, p2, p3, c0, c1, c2, c3, l0, l1, l2, l3, z, box; };
struct I { float4 c : POSITION; };
float4 vs(I i) : SV_Position {
    float4 cull = asfloat(0x7FC00000).xxxx;
    if (!(abs(i.c.w) > 1e-6)) return cull;
    float3 n = i.c.xyz / i.c.w;
    float4 w = n.x * m0 + n.y * m1 + ((n.z - z.x) / max(z.y - z.x, 1e-6)) * m2 + m3;
    float3 wp = w.xyz / w.w;
    if (z.z > 0.5) {
        float3 d = wp - p3.xyz;
        float3 l = float3(dot(d, p0.xyz), dot(d, p1.xyz), dot(d, p2.xyz));
        if (abs(l.x) < box.x && abs(l.z) < box.z && l.y < box.y && l.y > box.w) {
            if (z.z > 1.5) return cull;
            wp = c3.xyz + l.x * c0.xyz + l.y * c1.xyz + l.z * c2.xyz;
        }
    }
    return wp.x * l0 + wp.y * l1 + wp.z * l2 + l3;
}
)";

// b1: m0-m3 this frame's inverse view x projection rows; a0-a2 the near cascade's light x, y and depth as columns
// (dot with the world position and 1), b0-b2 the far cascade's; eye the camera and w the strength; vp the viewport in
// target pixels; z min depth, max depth, debug, one cascade's side in texels; e the near cascade's constant and slope
// bias in its depth units, pcf radius, range; f the light's direction and where the near cascade starts to hand over;
// g the far cascade's biases, how far it reaches (no further than range: one cascade) and where it starts to fade.
// The map holds the cascades side by side, the near one on the left.
inline constexpr int kSunReceiverConstants = 64;
inline const char* kSunReceiverHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, a0, a1, a2, b0, b1, b2, eye, vp, z, e, f, g; };
Texture2D<float> depth : register(t0);
Texture2D<float> smap : register(t1);
// the share of the taps round wp that something nearer the light covers, in cascade i; 0 outside it
float Occ(float3 wp, float4 c0, float4 c1, float4 c2, float cbias, float sbias, float ndl, float i) {
    float4 p = float4(wp, 1);
    float3 s = float3(dot(p, c0), dot(p, c1), dot(p, c2));
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0) || any(uv > 1) || s.z < 0 || s.z > 1) return 0;
    float bias = cbias + sbias * sqrt(saturate(1 - ndl * ndl)) / max(ndl, 0.05);
    int k = (int)e.z;
    float lit = 0, n = 0;
    for (int y = -k; y <= k; y++)
        for (int x = -k; x <= k; x++) {
            float2 t = clamp(uv * z.w + float2(x, y), 0, z.w - 1) + float2(i * z.w, 0);
            lit += s.z - bias <= smap.Load(int3(t, 0)) ? 1 : 0;
            n += 1;
        }
    return 1 - lit / n;
}
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
    // the face as the camera sees it, from the neighbouring pixels (taken before any of them returns); one turned from
    // the light is already dark and is left alone
    float3 nrm = cross(ddy(wp), ddx(wp));
    nrm = nrm / max(length(nrm), 1e-9);
    if (dot(nrm, eye.xyz - wp) < 0) nrm = -nrm;
    bool two = g.z > e.w + 1e-3;
    float sides = two ? 2 : 1;
    if (z.z > 1.5 && z.z < 2.5 && i.p.x < 256 * sides && i.p.y < 256)
        return float4(0, 0, 0, smap.Load(int3(i.p.xy * z.w / 256, 0)) < 0.9999 ? 0.9 : 0);
    if (d >= z.y) return 0;
    float dist = length(wp - eye.xyz);
    float reach = two ? g.z : e.w;
    if (dist > reach) return 0;
    // the far cascade's share: none before f.w, all past range
    float w1 = two ? smoothstep(f.w, e.w, dist) : 0;
    float fade = 1 - smoothstep(two ? g.w : f.w, reach, dist);
    if (z.z > 2.5) return float4(0, 0, 0, (0.25 + 0.4 * w1) * fade);  // debug 3: the near cascade light, the far dark
    float ndl = dot(nrm, -f.xyz);
    if (ndl <= 0.05) return 0;
    float occ0 = w1 < 1 ? Occ(wp, a0, a1, a2, e.x, e.y, ndl, 0) : 0;
    float occ1 = w1 > 0 ? Occ(wp, b0, b1, b2, g.x, g.y, ndl, 1) : 0;
    float occ = lerp(occ0, occ1, w1);
    return float4(0, 0, 0, eye.w * occ * (z.z > 0.5 ? 1 : fade));
}
)";

}  // namespace gpu
