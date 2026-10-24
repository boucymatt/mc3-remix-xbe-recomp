#pragma once
// The post feature's full screen pass (render_profile.h kPost): edge antialiasing on the 3D scene, then exposure,
// shoulder, contrast, saturation and gamma. It runs on the scene target once its 3D is drawn and before the
// overlay goes on, so HUD text and Flash are never filtered. Alpha passes through untouched: the title blends
// with the target's alpha afterwards.

namespace gpu {

// b1: a = 1/width, 1/height, flags (1 fxaa, 2 split), threshold; b = exposure, gamma, contrast, saturation; c.x shoulder
inline constexpr int kPostConstants = 12;
inline const char* kPostHlsl = R"(
cbuffer B : register(b1) { float4 a; float4 b; float4 c; };
Texture2D img : register(t0);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 Tap(float2 uv) { return img.SampleLevel(ls, uv, 0).rgb; }
// a pixel on a contrasted edge is blended along the edge: the direction comes from the luma of the four corners,
// two taps a third of the way each side, two more at the full span; the wider average is kept unless it leaves
// the luma range of the neighbourhood, which means it crossed onto another surface
float3 Edge(float2 uv, float2 px, float3 m, float threshold) {
    float lm = Luma(m);
    float nw = Luma(Tap(uv + float2(-1, -1) * px)), ne = Luma(Tap(uv + float2(1, -1) * px));
    float sw = Luma(Tap(uv + float2(-1, 1) * px)), se = Luma(Tap(uv + float2(1, 1) * px));
    float lo = min(lm, min(min(nw, ne), min(sw, se)));
    float hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(threshold, hi * 0.125)) return m;
    float2 d = float2((sw + se) - (nw + ne), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
    d = clamp(d / (min(abs(d.x), abs(d.y)) + reduce), -8.0, 8.0) * px;
    float3 near = 0.5 * (Tap(uv - d / 6.0) + Tap(uv + d / 6.0));
    float3 wide = 0.5 * near + 0.25 * (Tap(uv - d * 0.5) + Tap(uv + d * 0.5));
    float lw = Luma(wide);
    return (lw < lo || lw > hi) ? near : wide;
}
float3 Grade(float3 x) {
    x *= b.x;
    if (c.x > 0) {
        // above the knee the range rolls off towards 1 instead of clipping
        float knee = 1.0 - 0.5 * c.x;
        float3 over = max(x - knee, 0);
        x = min(x, knee) + over / (1.0 + over / (1.0 - knee));
    }
    x = (x - 0.5) * b.z + 0.5;
    x = lerp(Luma(x).xxx, x, b.w);
    return pow(saturate(x), 1.0 / b.y);
}
float4 psmain(O i) : SV_Target {
    float4 src = img.SampleLevel(ps, i.uv, 0);
    uint flags = (uint)a.z;
    if ((flags & 2) && i.uv.x < 0.5) return src;
    float3 col = (flags & 1) ? Edge(i.uv, a.xy, src.rgb, a.w) : src.rgb;
    return float4(Grade(col), src.a);
}
)";

}  // namespace gpu
