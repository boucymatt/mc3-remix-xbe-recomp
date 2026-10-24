#pragma once
// The vehicle-shadow feature (render_profile.h kVehicleShadow): the player's car rendered from a directional light into
// a depth map, and the ground near it compared against that map.
//
// Casters come from stream output: the scene's opaque draws of the previous frame leave the clip positions their own
// vertex shaders made, whatever the title's programs compute; those go back to the world with that frame's inverse
// view x projection, into the car's frame with that frame's car matrix and out again with this frame's, so a rigid car
// casts where it is now even when the title draws it after the point where the shadow is drawn. Only what lies inside
// the car's own box casts; the road under it does not.

namespace gpu {

// b1: m0-m3 the previous frame's inverse view x projection rows; p0-p2 the previous car's axes, p3 its position;
// c0-c2, c3 this frame's; l0-l3 the light's view x projection rows; z min depth, max depth; box half width, top,
// half length, bottom in the car's frame
inline constexpr int kCasterConstants = 72;
inline const char* kCasterHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, p0, p1, p2, p3, c0, c1, c2, c3, l0, l1, l2, l3, z, box; };
struct I { float4 c : POSITION; };
float4 vs(I i) : SV_Position {
    float4 cull = asfloat(0x7FC00000).xxxx;  // a NaN corner drops the whole triangle
    if (!(abs(i.c.w) > 1e-6)) return cull;
    float3 n = i.c.xyz / i.c.w;
    float4 w = n.x * m0 + n.y * m1 + ((n.z - z.x) / max(z.y - z.x, 1e-6)) * m2 + m3;
    float3 d = w.xyz / w.w - p3.xyz;
    float3 l = float3(dot(d, p0.xyz), dot(d, p1.xyz), dot(d, p2.xyz));
    if (abs(l.x) > box.x || abs(l.z) > box.z || l.y > box.y || l.y < box.w) return cull;
    float3 cw = c3.xyz + l.x * c0.xyz + l.y * c1.xyz + l.z * c2.xyz;
    return cw.x * l0 + cw.y * l1 + cw.z * l2 + l3;
}
)";

// b1: m0-m3 this frame's inverse view x projection rows; l0-l3 the light's; r0-r2 the car's axes, r3 its position and
// w the strength after the distance fade; vp the viewport in target pixels; z min depth, max depth, debug, map size;
// e constant bias and slope bias in light depth units, pcf radius, top; f radius, light direction
inline constexpr int kReceiverConstants = 64;
inline const char* kReceiverHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, l0, l1, l2, l3, r0, r1, r2, r3, vp, z, e, f; };
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
    // debug 2: the map in the top left corner, anything drawn into it solid
    if (z.z > 1.5 && i.p.x < 256 && i.p.y < 256) return float4(0, 0, 0, smap.Load(int3(i.p.xy * z.w / 256, 0)) < 0.9999 ? 0.9 : 0);
    float d = depth.Load(int3(i.p.xy, 0));
    if (d >= z.y) return 0;
    float2 ndc = float2((i.p.x - vp.x) / vp.z * 2 - 1, 1 - (i.p.y - vp.y) / vp.w * 2);
    float4 w = ndc.x * m0 + ndc.y * m1 + ((d - z.x) / (z.y - z.x)) * m2 + m3;
    float3 wp = w.xyz / w.w;
    float3 pc = wp - r3.xyz;
    float3 lc = float3(dot(pc, r0.xyz), dot(pc, r1.xyz), dot(pc, r2.xyz));
    float rad = length(lc.xz);
    if (rad > f.x || lc.y > e.w) return 0;
    float4 s = wp.x * l0 + wp.y * l1 + wp.z * l2 + l3;
    float2 uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0) || any(uv > 1) || s.z < 0 || s.z > 1) return 0;
    // slope bias from the receiver's normal, out of the reconstructed positions of neighbouring pixels
    float3 nrm = cross(ddy(wp), ddx(wp));
    float ndl = abs(dot(normalize(nrm + 1e-9), f.yzw));
    float bias = e.x + e.y * sqrt(saturate(1 - ndl * ndl)) / max(ndl, 0.05);
    int k = (int)e.z;
    float lit = 0, n = 0;
    for (int y = -k; y <= k; y++)
        for (int x = -k; x <= k; x++) {
            float m = smap.Load(int3(clamp(uv * z.w + float2(x, y), 0, z.w - 1), 0));
            lit += s.z - bias <= m ? 1 : 0;
            n += 1;
        }
    float occ = 1 - lit / n;
    float fade = z.z > 0.5 ? 1 : 1 - smoothstep(0.7 * f.x, f.x, rad);
    return float4(0, 0, 0, r3.w * occ * fade);
}
)";

}  // namespace gpu
