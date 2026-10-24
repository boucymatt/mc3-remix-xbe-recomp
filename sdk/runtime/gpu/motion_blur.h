#pragma once
// LA-inspired screen-space streak: the current scene depth is unprojected with
// this frame's camera, reprojected through the previous camera, then the current
// colour is sampled along that displacement. This is an original D3D12 pass,
// not MCLA's Xbox 360 shader. It runs before MC3's HUD/Flash overlay.

namespace gpu {

inline constexpr int kMotionBlurCars = 4;
inline constexpr int kMotionBlurConstants = 44 + kMotionBlurCars * 32;
inline const char* kMotionBlurHlsl = R"(
cbuffer B : register(b1) {
    float4 inv0, inv1, inv2, inv3;
    float4 old0, old1, old2, old3;
    float4 viewport;
    float4 depth_range;
    float4 blur;
    float4 cars[32];  // current x/y/z/position, then previous x/y/z/position, four pairs
};
Texture2D image : register(t0);
Texture2D<float> depth : register(t1);
SamplerState linear_sampler : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 center = image.Load(int3(i.p.xy, 0));
    if (blur.w > 0.5 && i.uv.x < 0.5) return center;
    float d = depth.Load(int3(i.p.xy, 0));
    if (d >= depth_range.y - 0.00001) return center;
    float2 ndc = float2((i.p.x - viewport.x) / viewport.z * 2 - 1,
                        1 - (i.p.y - viewport.y) / viewport.w * 2);
    float4 world = ndc.x * inv0 + ndc.y * inv1 +
                   ((d - depth_range.x) / (depth_range.y - depth_range.x)) * inv2 + inv3;
    if (abs(world.w) < 0.000001) return center;
    world /= world.w;
    // LA tags vehicle pixels in stencil. MC3 has no such tag in this path, so
    // test the reconstructed point against nearby live car boxes instead.
    // Transforming a car point through its previous body matrix cancels the
    // vehicle's own motion instead of smearing it with the road.
    [loop] for (int car = 0; car < (int)depth_range.z; car++) {
        int b = car * 8;
        float3 delta = world.xyz - cars[b + 3].xyz;
        float3 local = float3(dot(delta, cars[b + 0].xyz),
                              dot(delta, cars[b + 1].xyz),
                              dot(delta, cars[b + 2].xyz));
        if (abs(local.x) < 1.7 && local.y > 0.2 && local.y < 3.0 && abs(local.z) < 3.8) {
            world.xyz = cars[b + 7].xyz + local.x * cars[b + 4].xyz +
                        local.y * cars[b + 5].xyz + local.z * cars[b + 6].xyz;
            break;
        }
    }
    float4 old = world.x * old0 + world.y * old1 + world.z * old2 + old3;
    if (old.w <= 0.000001) return center;
    float2 old_uv = float2(old.x / old.w * 0.5 + 0.5, 0.5 - old.y / old.w * 0.5);
    float2 span = (i.uv - old_uv) * blur.x;
    uint width, height;
    image.GetDimensions(width, height);
    float2 size = float2(width, height);
    float pixels = length(span * size);
    if (pixels < 0.65) return center;
    span *= min(1.0, blur.y / pixels);
    int count = (int)blur.z;
    float3 sum = center.rgb;
    float weight = 1;
    [loop] for (int n = 1; n < count; n++) {
        float t = ((float)n / (count - 1)) - 0.5;
        float2 uv = i.uv + span * t;
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        float tap_depth = depth.Load(int3(uv * size, 0));
        if (tap_depth >= depth_range.y - 0.00001 ||
            abs(tap_depth - d) > max(0.0005, abs(d) * 0.002)) continue;
        sum += image.SampleLevel(linear_sampler, uv, 0).rgb;
        weight += 1;
    }
    return float4(sum / weight, center.a);
}
)";

}  // namespace gpu
