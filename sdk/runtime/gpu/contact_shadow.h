#pragma once
// The contact-shadow feature's pass (render_profile.h kContactShadow): a decal under the player's car, made from the
// game's own depth. Each pixel's depth goes back to a world position, then into the car's frame; on the contact
// plane inside the ellipse the colour is darkened. A surface well above the plane is the car itself and a surface
// far below it is the road under a jump, and neither is touched; the alpha channel never is.

namespace gpu {

// b1: m0-m3 inverse view x projection rows; r0-r2 the car's axes, r3 its position and w the strength after the
// distance fade; vp the viewport in target pixels; z min depth, max depth, debug; e half width, half length,
// plane height, top; f fade, soft
inline constexpr int kShadowConstants = 48;
inline const char* kShadowHlsl = R"(
cbuffer B : register(b1) { float4 m0, m1, m2, m3, r0, r1, r2, r3, vp, z, e, f; };
Texture2D<float> depth : register(t0);
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
    if (d >= z.y) return 0;  // nothing drawn here: sky, or the clear
    float2 ndc = float2((i.p.x - vp.x) / vp.z * 2 - 1, 1 - (i.p.y - vp.y) / vp.w * 2);
    float4 w = ndc.x * m0 + ndc.y * m1 + ((d - z.x) / (z.y - z.x)) * m2 + m3;
    float3 p = w.xyz / w.w - r3.xyz;
    float3 l = float3(dot(p, r0.xyz), dot(p, r1.xyz), dot(p, r2.xyz));
    float up = l.y - e.z;
    if (z.z > 0) {
        // calibration: bands of 0.25 m in the car's frame over a footprint twice as wide
        float2 q = l.xz / (e.xy * 2);
        if (dot(q, q) > 1) return 0;
        float band = frac(up * 2) < 0.5 ? 0.75 : 0.25;
        return float4(0, 0, 0, abs(up) < 0.05 ? 0.95 : band);
    }
    float r = length(l.xz / e.xy);
    float foot = 1 - smoothstep(1 - f.y, 1, r);
    float height = up > 0 ? 1 - smoothstep(e.w * 0.6, e.w, up) : saturate(1 + up / f.x);
    return float4(0, 0, 0, r3.w * foot * height);
}
)";

}  // namespace gpu
