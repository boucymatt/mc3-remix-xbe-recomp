// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/contact_shadow.h"
#include "gpu/post_process.h"
#include "gpu/motion_blur.h"
#include "gpu/render_profile.h"
#include "gpu/vehicle_shadow.h"
#include "gpu/sun_shadow.h"
#include "gpu/lamp_shadow.h"
#include "gpu/beam_light.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;
constexpr float kPassDarken = -2;  // a PassN blend: the target times one minus the pass's alpha
constexpr float kPassLight = -3;   // a PassN blend: the target plus the target times the pass's colour

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
    uint64_t skipped = 0;  // list state calls the draw state cache left out
};
Perf g_perf;
struct PerfScope {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    ~PerfScope() { acc += PerfMs(t); }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// a clear of some of a target's channels: the clear colour wherever the scissor lets, the others masked off
const char* kMaskedClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain(float4 p : SV_Position) : SV_Target { return colour; }
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default:
            LOG_FIRST(16, "d3d12: unknown Xbox blend factor %X (%s channel), using ONE", v, alpha ? "alpha" : "rgb");
            return D3D12_BLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_BLEND_OP_ADD;
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        case 0x8006: return D3D12_BLEND_OP_ADD;
        // NV2A-only signed equations. With ONE/ONE factors Draw renders them exactly in two passes
        // (SignedBlendOp); any other factor pair falls back to xemu's and Cxbx-Reloaded's
        // approximation without the 0.5 bias.
        case 0xF005: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0xF006: return D3D12_BLEND_OP_ADD;
        default:
            LOG_FIRST(8, "d3d12: unknown Xbox blend equation %X, using ADD", v);
            return D3D12_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    bool Capturing() const override { return cap_ != nullptr; }
    void Note(const char* line) override {
        if (cap_) fprintf(cap_, "     note %s\n", line);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    // done once the list being recorded, the next fence value, has run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

    ID3D12Device* Device() override { return dev_.Get(); }
    // an extension sets what it likes on the list: the draw state cache stops until its turn is over (ExtensionDone)
    ID3D12GraphicsCommandList* List() override {
        ls_ = {};
        ls_.extension = true;
        return cl_.Get();
    }

    // The viewport, scissor, blend factor, stencil reference, topology and pipeline the list has now, so that a draw
    // sets only what changed: about 97% of draws repeat the first five and 70% the pipeline (25/10, San Diego Cruise).
    // Every call that sets them on cl_ goes through these; a list reset (Begin) and an extension that took the list
    // forget them, and while an extension holds the list nothing is remembered.
    struct ListState {
        bool viewport = false, scissor = false, blend = false, stencil = false, topology = false, pipeline = false;
        bool extension = false;  // List() handed the list out during the extension's current call
        D3D12_VIEWPORT vp{};
        D3D12_RECT rect{};
        float factor[4]{};
        UINT ref = 0;
        D3D_PRIMITIVE_TOPOLOGY topo{};
        ID3D12PipelineState* pso = nullptr;
    } ls_;
    // the previous draw's four texture views and their slots: 40 to 70% of draws use the same four again, and then
    // need no new views. Valid until the slot ring is reset (Begin) or wraps (AllocSrvSlots); a released resource stays
    // alive in its frame's garbage until then, so the same address cannot be another resource.
    struct LastViews {
        bool valid = false;
        uint32_t slot = 0;
        ID3D12Resource* res[4]{};
        D3D12_SHADER_RESOURCE_VIEW_DESC sv[4]{};
    } lv_;
    void ForgetListState() {
        ls_ = {};
        lv_.valid = false;
    }
    void ExtensionDone() {
        if (ls_.extension) ls_ = {};
    }
    void SetViewport(const D3D12_VIEWPORT& v) {
        if (ls_.viewport && !std::memcmp(&ls_.vp, &v, sizeof(v))) return void(g_perf.skipped++);
        cl_->RSSetViewports(1, &v);
        if (!ls_.extension) ls_.viewport = true, ls_.vp = v;
    }
    void SetScissor(const D3D12_RECT& r) {
        if (ls_.scissor && !std::memcmp(&ls_.rect, &r, sizeof(r))) return void(g_perf.skipped++);
        cl_->RSSetScissorRects(1, &r);
        if (!ls_.extension) ls_.scissor = true, ls_.rect = r;
    }
    void SetBlendFactor(const float f[4]) {
        if (ls_.blend && !std::memcmp(ls_.factor, f, sizeof(ls_.factor))) return void(g_perf.skipped++);
        cl_->OMSetBlendFactor(f);
        if (!ls_.extension) ls_.blend = true, std::memcpy(ls_.factor, f, sizeof(ls_.factor));
    }
    void SetStencilRef(UINT ref) {
        if (ls_.stencil && ls_.ref == ref) return void(g_perf.skipped++);
        cl_->OMSetStencilRef(ref);
        if (!ls_.extension) ls_.stencil = true, ls_.ref = ref;
    }
    void SetTopology(D3D_PRIMITIVE_TOPOLOGY t) {
        if (ls_.topology && ls_.topo == t) return void(g_perf.skipped++);
        cl_->IASetPrimitiveTopology(t);
        if (!ls_.extension) ls_.topology = true, ls_.topo = t;
    }
    void SetPipeline(ID3D12PipelineState* p) {
        if (ls_.pipeline && ls_.pso == p) return void(g_perf.skipped++);
        cl_->SetPipelineState(p);
        if (!ls_.extension) ls_.pipeline = true, ls_.pso = p;
    }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    D3D12_CPU_DESCRIPTOR_HANDLE DepthView(const Target& t) override { return Cpu(dsv_heap_.Get(), t.view, dsv_inc_); }
    void Restore() override { Begin(); }
    void Host(const State& s, uint32_t id) override {
        if (!ext_ || !BindTargets(s)) return;
        Target* rt = bound_rt_;
        Target* ds = bound_ds_;
        ext_->HostDraw(*this, s, rt, ds, id);
        Begin();
    }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t), ExtensionDone();
    }
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override;
    void PassN(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
               uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float* p, uint32_t n,
               float blend);
    // kPost: the scene target, once its 3D is drawn and before the overlay, through the post pass
    void PostBeforeDraw(const State& s, bool pretransformed);
    // kContactShadow: the decal under the player's car, after the opaque world and before the first blended draw
    void ShadowBeforeDraw(const State& s, bool pretransformed);
    void ShadowApply(const State& s, const char* why);
    uint32_t shadow_scene_ = 0;
    uint64_t shadow_done_ = ~0ull;
    uint64_t shadow_frames_ = 0, shadow_no_car_ = 0, shadow_far_ = 0, shadow_no_depth_ = 0;
    bool shadow_logged_ = false;
    // the player's car is the one the main camera follows: picked once within kChase of the camera, then kept by
    // its position from frame to frame, so an opponent passing nearer does not take the shadow
    float shadow_last_[3] = {};
    uint32_t d3d_cars_seen_ = 0;  // frames' shadow attempts with cars in the host values, reset each present
    bool shadow_have_ = false;
    // kVehicleShadow: the scene's opaque draws stream their clip positions out, one buffer a frame, two in turn;
    // the next frame casts from them (vehicle_shadow.h)
    // each draw streams into its own part of the buffer with its own filled size: draws appending through one counter
    // race on the GPU without a barrier between them and lose most of their output (18/10: 18-41k of 133k written)
    static constexpr uint32_t kSoVerts = 1u << 20;
    static constexpr uint32_t kSoDraws = 8192;
    struct SoBuffer {
        ComPtr<ID3D12Resource> buf, filled;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON, filled_state = D3D12_RESOURCE_STATE_COMMON;
        uint64_t frame = ~0ull;      // presents_ when it was written
        uint64_t car_frame = ~1ull;  // presents_ when car was stored
        uint64_t inv_frame = ~2ull;  // and inv
        uint32_t verts = 0;
        uint32_t draws = 0;
        bool valid = false;  // inv was made
        float inv[16] = {};  // that frame's inverse view x projection
        float minz = 0, maxz = 1;
        float car[12] = {};  // that frame's player car
        // runs of streamed vertices (start, count) from draws without alpha test: the city casts from these, a fence or
        // a tree drawn through alpha test would cast as a solid card
        std::vector<std::pair<uint32_t, uint32_t>> solid;
    };
    SoBuffer so_[2];
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> so_psos_;
    ComPtr<ID3D12Resource> vs_map_;
    D3D12_RESOURCE_STATES vs_map_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t vs_dsv_ = ~0u;
    ComPtr<ID3D12PipelineState> caster_pso_;
    bool caster_failed_ = false;
    uint64_t vs_frames_ = 0, vs_no_prev_ = 0, vs_no_target_ = 0, vs_far_ = 0, vs_full_ = 0, vs_dark_ = 0;
    bool vs_logged_ = false;
    uint64_t vs_diag_frame_ = ~0ull;  // one light dump at most per selected frame, not once per draw
    // the title's key light, the brightest enabled directional light it set this frame other than its white default
    float game_dir_[3] = {};
    float game_lum_ = 0;
    float game_lum_smooth_ = -1;   // game_lum_ eased over about half a second: its value changes from object to object
    uint64_t game_frame_ = ~0ull;  // presents_ when it was last seen
    float game_best_ = 0;          // the brightest seen in the frame being drawn
    int game_state_ = -1;          // the source last logged: 0 fixed, 1 the title's
    void GameLight(const State& s);
    // the GPU's own count of streamed bytes, read back a frame slot later, against the CPU's count
    ComPtr<ID3D12Resource> so_rb_;
    const uint64_t* so_rb_map_ = nullptr;
    uint32_t so_expect_[kFrames] = {};
    bool so_rb_pending_[kFrames] = {};
    // during an F3 capture: the GPU's running count after each streamed draw, beside the CPU's
    static constexpr uint32_t kSoChecks = 4096;
    ComPtr<ID3D12Resource> so_chk_;
    const uint64_t* so_chk_map_ = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> so_chk_rows_;  // capture index, CPU count
    bool SoEligible(const State& s, const Assembled& a, const PsoKey& k);
    ID3D12PipelineState* EnsureSoPipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    bool SoBegin(const State& s, const Assembled& a);
    void SoEnd(const State& s, const Assembled& a);
    bool EnsureVehicleShadow(int size);
    void VehicleShadow(const State& s, const float* car, const float eye[3], float dist, const char* why);
    bool EnsureShadowMap(ComPtr<ID3D12Resource>& map, D3D12_RESOURCE_STATES& state, uint32_t& dsv, int w, int h);
    bool EnsureCaster(const char* hlsl, ComPtr<ID3D12PipelineState>& pso, bool& failed, const char* what);
    // kSunShadow: one cascade over the view's first metres from the title's environment light (sun_shadow.h)
    void SunShadow(const State& s, const float* car, const float eye[3], const char* why);
    ComPtr<ID3D12Resource> sun_map_;
    D3D12_RESOURCE_STATES sun_map_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t sun_dsv_ = ~0u;
    ComPtr<ID3D12PipelineState> sun_caster_pso_;
    bool sun_caster_failed_ = false;
    float sun_lum_smooth_ = -1;  // as game_lum_smooth_, eased for this pass alone
    int sun_state_ = -1;         // the light source last logged
    uint64_t sun_frames_ = 0, sun_no_prev_ = 0, sun_no_target_ = 0, sun_dark_ = 0;
    bool sun_logged_ = false;
    // kLightShadow: the player's headlights as a spot light; the title's headlight passes read the map in t3
    void HeadlightShadow(const State& s, const float* car, const char* why);
    ComPtr<ID3D12Resource> hl_map_;
    D3D12_RESOURCE_STATES hl_map_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t hl_dsv_ = ~0u;
    uint64_t hl_frame_ = ~0ull;  // presents_ when the map was made: the headlight passes after it use it
    float hl_k_[48] = {};        // their c[180..191]
    float* last_vc_ = nullptr;   // the vertex constants uploaded for the draw being made
    uint64_t hl_frames_ = 0, hl_no_prev_ = 0, hl_no_target_ = 0, hl_passes_ = 0;
    bool hl_logged_ = false;
    // the street lamps, from the glows at their heads: a place seen on several frames is a lamp, a moving glow never is
    struct Lamp {
        float p[3];
        uint32_t hits;
        uint64_t last;  // presents_ when last seen
    };
    std::vector<Lamp> lamps_;
    void CollectLampGlows(const State& s, const Assembled& a);
    void LampShadows(const State& s, const float* car, const char* why);
    ComPtr<ID3D12Resource> lamp_map_;
    D3D12_RESOURCE_STATES lamp_map_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t lamp_dsv_ = ~0u;
    uint64_t lamp_frames_ = 0, lamp_used_ = 0, lamp_glows_ = 0;
    bool lamp_logged_ = false;
    // the cars the title drew with its car paint program this frame and the last, as Matrix34 (rows x, y, z, position)
    struct DrawnCar {
        float m[12];
        float span;  // the widest of its parts across x or z, metres (0: not measured)
    };
    std::vector<DrawnCar> cars_now_, cars_prev_;
    uint64_t cars_frame_ = ~0ull;
    // the glows MC3 drew (lamps, headlights, tail lights) in the world, this frame and the last
    std::vector<std::array<float, 3>> glows_now_, glows_prev_;
    uint64_t glows_frame_ = ~0ull;
    // a car followed from frame to frame: its lights are on while it moves or while a glow sits at its front
    struct Track {
        float p[3];
        uint64_t seen, moved, glowed;
        float sm[12];     // its placement filtered from frame to frame: the beam follows this, not each frame's
        bool has_sm, has_v;
        float v[3];       // metres a frame: a racer moves metres a frame, an easing alone would trail it
        float span;       // the widest it was seen
        uint64_t chosen;  // presents_ when it last had a beam: it keeps one against a nearer newcomer
        float still_origin[3];
        float still_sum[12];
        uint32_t still_samples;
        bool still_locked;
    };
    std::vector<Track> tracks_;
    uint64_t beam_unlit_ = 0, beam_cones_hidden_ = 0, beam_snaps_ = 0, beam_fits_ = 0;
    double beam_miss_ = 0;  // metres the cars were from where their tracks predicted, summed
    void CollectDrawnCar(const State& s, const Assembled& a);
    void Beams(const State& s, const char* why);
    ComPtr<ID3D12Resource> beam_map_;
    D3D12_RESOURCE_STATES beam_map_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t beam_dsv_ = ~0u;
    uint64_t beam_frames_ = 0, beam_used_ = 0, beam_cars_seen_ = 0;
    bool beam_logged_ = false;
    uint32_t shadow_strays_ = 0;  // frames the kept car has been far from the camera while another was close
    void PostAtPresent(const Surface& front);
    void PostApply(Target& t, const char* why);
    void MotionBlurApply(Target& t, const char* why);
    uint32_t post_scene_ = 0;       // the guest sized colour target this frame's 3D went to
    uint64_t post_done_ = ~0ull;    // presents_ when the pass last ran
    uint64_t post_frames_ = 0, post_at_present_ = 0, post_skipped_range_ = 0;
    Surface blur_depth_{};
    Viewport blur_viewport_{};
    float blur_view_[16] = {}, blur_proj_[16] = {}, blur_previous_vp_[16] = {};
    float blur_current_cars_[State::kHostCars][12] = {};
    float blur_previous_cars_[State::kHostCars][12] = {};
    uint32_t blur_current_count_ = 0, blur_previous_count_ = 0;
    bool blur_scene_valid_ = false, blur_previous_valid_ = false;
    uint32_t blur_previous_w_ = 0, blur_previous_h_ = 0;
    uint64_t blur_previous_frame_ = ~0ull, blur_frames_ = 0, blur_no_depth_ = 0;
    uint64_t mc3_blur_composites_hidden_ = 0;
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend);
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline();
    void DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
    float PointScale() const;  // host pixels per console pixel of the bound target
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const PixelKey& pk,
                     const DrawConstants& c, const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over = false);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi);
    void BlitFrame(Target& t);
    void RenderUi();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    void MaskedClear(Target& t, const float col[4], uint32_t mask, const std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_rb_;
    const uint64_t* query_counts_ = nullptr;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    bool tearing_ = false;
    // MC3RENDER timing, with --mc3-render-timing or a capture tag: two GPU timestamps a frame around its commands,
    // the render thread's present to present on the CPU, reported every kTimingFrames
    static constexpr uint32_t kTimingFrames = 600;
    ComPtr<ID3D12QueryHeap> time_heap_;
    ComPtr<ID3D12Resource> time_rb_;
    const uint64_t* time_ticks_ = nullptr;
    double time_ticks_ms_ = 0;
    // eight timestamp pairs: frame, post, three shadows, light, lamps, motion blur
    static constexpr uint32_t kStamps = 16;
    bool time_open_[kFrames] = {};  // the frame in this slot has its first stamp
    bool time_done_[kFrames] = {};  // and its second, resolved: readable once its fence passes
    bool time_post_[kFrames] = {};  // the post pass has its two stamps in this slot
    bool time_shadow_[kFrames] = {};
    bool time_vshadow_[kFrames] = {};
    bool time_sun_[kFrames] = {};
    bool time_hl_[kFrames] = {};
    bool time_lamp_[kFrames] = {};
    bool time_blur_[kFrames] = {};
    std::vector<float> time_cpu_, time_gpu_, time_pass_, time_shadow_ms_, time_vshadow_ms_, time_sun_ms_, time_hl_ms_,
        time_lamp_ms_, time_blur_ms_;
    PerfClock::time_point time_last_{};
    std::string time_summary_ = "no timing";
    void TimingInit();
    void TimingBegin();
    void TimingEnd();
    void TimingRead();
    void TimingReport();
    void WriteCaptureMeta();
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    TimingInit();
    TimingBegin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    render::LogOutput({.guest_w = width, .guest_h = height, .out_w = bb_w_, .out_h = bb_h_, .scale = scale_,
                       .vsync = app::Get().vsync, .tearing = tearing_, .fps_cap = app::Get().fps_cap});
    render::WriteEffectiveConfig();
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // the profile is known before any device, target or pipeline
    render::LogConfig();
    // dedicated GPU first on hybrid laptops; --mc3-gpu-index picks another in the same order
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    int index = -1;
    std::vector<ComPtr<IDXGIAdapter1>> found;
    if (SUCCEEDED(factory_.As(&f6))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) !=
                         DXGI_ERROR_NOT_FOUND;
             i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            LOGR("MC3RENDER adapter[%u] %ls luid=%08lX%08lX vram_mb=%llu%s", i, d.Description,
                 d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, (unsigned long long)(d.DedicatedVideoMemory >> 20),
                 (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
            found.push_back(a);
        }
        int want = render::Get().gpu_index;
        if (want >= int(found.size())) {
            LOGE("MC3RENDER --mc3-gpu-index=%d: only %zu adapters, using 0", want, found.size());
            want = -1;
        }
        index = want >= 0 ? want : 0;
        if (!found.empty()) adapter = found[size_t(index)];
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    render::Adapter ra;
    ra.index = adapter ? index : -1;
    char name[128] = "default";
    if (adapter) WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
    ra.name = name;
    ra.luid = uint64_t(uint32_t(ad.AdapterLuid.HighPart)) << 32 | ad.AdapterLuid.LowPart;
    ra.vram_mb = ad.DedicatedVideoMemory >> 20;
    ra.vendor = ad.VendorId;
    ra.device = ad.DeviceId;
    ra.subsys = ad.SubSysId;
    ra.revision = ad.Revision;
    LARGE_INTEGER umd{};
    if (adapter && SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        char v[48];
        snprintf(v, sizeof(v), "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                 unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
        ra.driver = v;
    } else {
        ra.driver = "?";
    }
    render::LogAdapter(ra);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (render::Streams()) rsd.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
        ExtensionDone();
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
    ForgetListState();
}

void D3D12Backend::Submit(bool wait) {
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        WaitFence(fence_value_);
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    SetViewport(vp);
    SetScissor(scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
        lv_.valid = false;
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the extension's
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    float sc = scale_;
    // kReflections: the title's reflection maps (MC3: a 256 x 256 environment map and its 64 x 64 blur, each with its
    // depth) are square powers of two no larger than 512; more pixels make what the car reflects sharper
    if (render::On(render::kReflections) && t.gw == t.gh && t.gw <= 512 && t.gw >= 16 && !(t.gw & (t.gw - 1)))
        sc = std::min(sc * render::Get().refl.scale, 2048.0f / float(t.gw));
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                   IID_PPV_ARGS(&t.res)));
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        float one = 1;
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, one, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    target_used_[slot.data] = frame_;
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    target_used_[slot.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline() {
    auto& pso = blit_psos_[kDepthCopyKey];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    p.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// src's depth drawn into dst's depth buffer
void D3D12Backend::DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), dst.view, dsv_inc_);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(dst.w, dst.h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src.res.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    DrawFullScreen(pso, cb.gpu, slot);
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    DrawDepthCopy(src, dst, pso);
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    SetPipeline(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over), cb.gpu, slot, si);
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend) {
    uint64_t mode = blend >= 0 ? 1 : blend <= kPassLight ? 3 : blend <= kPassDarken ? 2 : 0;
    uint64_t seed = uint64_t(rtv) << 2 | mode;
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (mode == 2) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ZERO;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (mode == 3) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_DEST_COLOR;
        b.DestBlend = D3D12_BLEND_ONE;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend) {
    PassN(hlsl, entry, rtv, rh, w, h, src, fmt, p, 8, blend);
}

void D3D12Backend::PassN(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                         uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                         const float* p, uint32_t n, float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    // Some passes need more than one 256-byte CBV page (motion blur uploads
    // per-vehicle matrices). Never truncate their constants to 64 floats.
    Alloc cb = Upload(std::max<uint64_t>(256, (uint64_t(n) * sizeof(float) + 255) & ~255ull), 256);
    std::memcpy(cb.cpu, p, uint64_t(n) * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        SetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(t.fb.Get(), t.res.Get());
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Cmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
}

// a point is drawn as its whole console pixel by a geometry shader, solid, front facing and never culled
void D3D12Backend::ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    uint64_t hash;
    ID3DBlob* gs = Compile(PointHlsl(), "main", "gs_5_0", hash);
    if (!gs) return;
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = FALSE;
}

float D3D12Backend::PointScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (k.topo == uint32_t(Topology::Points)) ExpandPoints(d);
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) LOGE("d3d12: pipeline creation failed");
    psos_[h] = pso;
    return pso.Get();
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    return t.res.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, palette.va ? palette.colours : nullptr, same);
        }
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return tx.res.Get();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC(swap[Cmp(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    auto perf_t = PerfClock::now();
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    g_perf.bind += PerfMs(perf_t);
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, D3D12 half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    last_vc_ = reinterpret_cast<float*>(vc.cpu);
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = PointScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vc.cpu), c.ext, bound_rt_, k.blend ? k.src : 1);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    cl_->SetGraphicsRootConstantBufferView(0, vc.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const PixelKey& pk,
                               const DrawConstants& c, const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    CaptureTextures(s, stages);
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    fprintf(cap_,
            "\n     nv2a op=%X blend_color=%08X host_rgb=%u/%u host_a=%u/%u atest=%u afunc=%X aref=%u "
            "viewport=%u,%u,%u,%u scissor=%u ps_prog=%u comb_count=%08X tex_modes=%05X alphakill=%X proj=%X "
            "bf=%.3f,%.3f,%.3f,%.3f",
            k.op, SimpleState(s, kMBlendColor), unsigned(Blend(k.src, false)), unsigned(Blend(k.dst, false)),
            unsigned(Blend(k.src, true)), unsigned(Blend(k.dst, true)), SimpleState(s, kMAlphaTestEnable) != 0,
            SimpleState(s, kMAlphaFunc), SimpleState(s, kMAlphaRef) & 0xFF, s.viewport.x, s.viewport.y,
            s.viewport.w, s.viewport.h, s.scissor_count, pk.programmable, pk.count, pk.tex_modes, pk.alphakill,
            pk.proj, bf[0], bf[1], bf[2], bf[3]);
    if (pk.programmable) {
        fprintf(cap_, "\n     rc final=%08X/%08X", pk.final_abcd, pk.final_efg);
        uint32_t n = std::min<uint32_t>(pk.count & 0xFF, 8);
        for (uint32_t i = 0; i < n; i++)
            fprintf(cap_, "\n     rc%u rgb=%08X/%08X alpha=%08X/%08X", i, pk.rgb_in[i], pk.rgb_out[i],
                    pk.a_in[i], pk.a_out[i]);
    } else {
        for (int i = 0; i < 4 && pk.ff[i][0] > 1; i++)
            fprintf(cap_, "\n     ff%d color=%X,%X,%X,%X alpha=%X,%X,%X,%X result=%X", i, pk.ff[i][0],
                    pk.ff[i][1], pk.ff[i][2], pk.ff[i][3], pk.ff[i][4], pk.ff[i][5], pk.ff[i][6], pk.ff[i][7],
                    pk.ff[i][8]);
    }
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        const auto& t = s.tss[size_t(i)];
        fprintf(cap_,
                "\n     stage%d header=%08X data=%08X format=%X size=%ux%u kind=%u rect=%u "
                "address=%X/%X/%X filter=%X/%X/%X color=%X,%X,%X,%X alpha=%X,%X,%X,%X result=%X",
                i, s.textures[size_t(i)], d.data, d.format, d.width, d.height, stages.info[i].kind,
                stages.info[i].rect, t[kTssAddressU], t[kTssAddressV], t[kTssAddressW], t[kTssMagFilter],
                t[kTssMinFilter], t[kTssMipFilter], t[kTssColorOp], t[kTssColorArg1], t[kTssColorArg2],
                t[kTssColorArg0], t[kTssAlphaOp], t[kTssAlphaArg1], t[kTssAlphaArg2], t[kTssAlphaArg0],
                t[kTssResultArg]);
    }
}

// small draws: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    if (count <= kQuadVertices) CaptureInputs(s, a, c);
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

// MC3 additive-light candidates, including the mcSpotlight 64x64 signature from
// the xboxrecomp oracle. Guest addresses differ between runs.
bool Mc3AdditiveLight(const State& s, uint32_t prim, const PsoKey& k, const PixelKey& pk, bool& cone) {
    TextureDesc d;
    if (prim != 5 || !s.textures[0] || !Describe(s.textures[0], d)) return false;
    if (d.format != 0x0F || d.width != d.height || (d.width != 64 && d.width != 128)) return false;
    if (!k.blend || k.src != 0x302 || k.dst != 1 || k.op != 0x8006) return false;  // SRC_ALPHA + ONE
    if (!k.zen || k.zfunc != 0x203 || k.zwrite) return false;  // LEQUAL, no depth write
    if (pk.programmable) {
        if ((pk.count & 0xFF) != 1 || pk.tex_modes != 1 || pk.final_abcd != 0x0000000C ||
            pk.final_efg != 0x00001C80 || pk.rgb_in[0] != 0x04080000 || pk.a_in[0] != 0x14180000 ||
            pk.rgb_out[0] != 0x00000C00 || pk.a_out[0] != 0x00000C00) return false;
    } else {
        // The fast runtime receives this v0*t0 pass as fixed-function stage 0.
        constexpr uint32_t modulate[9] = {4, 1, 2, 1, 4, 1, 2, 1, 1};
        if (std::memcmp(pk.ff[0], modulate, sizeof(modulate)) != 0 || pk.ff[1][0] > 1) return false;
    }
    cone = d.width == 64;
    return true;
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[i];
        if (!stages.res[i]) {
            sv.ViewDimension = NullViewDimension(stages.info[i].kind);
            sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
    }
    uint32_t slot = lv_.slot;
    if (lv_.valid && !std::memcmp(lv_.res, stages.res, sizeof(lv_.res)) && !std::memcmp(lv_.sv, stages.sv, sizeof(lv_.sv))) {
        g_perf.skipped += 4;
    } else {
        slot = AllocSrvSlots(4);
        for (int i = 0; i < 4; i++)
            dev_->CreateShaderResourceView(stages.res[i], &stages.sv[i], Cpu(srv_heap_.Get(), slot + i, srv_inc_));
        lv_.valid = true;
        lv_.slot = slot;
        std::memcpy(lv_.res, stages.res, sizeof(lv_.res));
        std::memcpy(lv_.sv, stages.sv, sizeof(lv_.sv));
    }
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
    uint32_t samplers = EnsureStageSamplers(s, stages, UnitTexcoords(a));
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    SetViewport(vp);
    float sc = W / gw;
    LONG x0 = LONG(s.viewport.x * sc);
    LONG y0 = LONG(s.viewport.y * sc);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sc);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sc));
        y0 = std::max(y0, LONG(s.scissors[1] * sc));
        x1 = std::min(x1, LONG(s.scissors[2] * sc));
        y1 = std::min(y1, LONG(s.scissors[3] * sc));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    SetScissor(rect);
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    SetBlendFactor(bf);
    SetStencilRef(SimpleState(s, kMStencilRef) & 0xFF);
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    SetTopology(PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    auto perf_t = PerfClock::now();
    bool assembled = Assemble(s, prim, start, count, indices, a);
    g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool D3D12Backend::EnsureQueries() {
    if (query_counts_) return true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kQueries) * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&query_rb_))) &&
                SUCCEEDED(query_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOG_FIRST(1, "d3d12: no occlusion queries, visibility tests stay pending");
        return false;
    }
    query_counts_ = static_cast<const uint64_t*>(p);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D12Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    return query;
}

void D3D12Backend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    uint64_t at = uint64_t(query) * sizeof(uint64_t);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, query_rb_.Get(), at);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float D3D12Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(t->res->GetDesc().SampleDesc.Count);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D12Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = fence_->GetCompletedValue();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) pixels += double(query_counts_[q.index]) / q.samples_per_pixel;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

// NV2A's signed blend equations, which D3D12 lacks. Measured on hardware (abaire, xemu issue #996 and
// nxdk_pgraph_tests blend_tests): the source byte is read as two's complement, so 0xFF is -1/255, and
//   ADD_SIGNED          (0xF006)  dst + signed(src)
//   REVSUBTRACT_SIGNED  (0xF005)  dst - signed(src)
// clamped to 0..1 afterwards. With ONE/ONE factors signed(src) splits into p (values 0..127) and n (the
// magnitude of -128..-1), one of them 0 in every pixel, so ADD_SIGNED is ADD p then REV_SUBTRACT n and
// REVSUBTRACT_SIGNED is REV_SUBTRACT p then ADD n, clamped like the console's single result. MC3 counts
// its car light volume this way: white faces into the alpha of a 320x240 target cleared to 0, each face
// one step up or down, a stencil count in alpha. Read unsigned (xemu, Cxbx-Reloaded, the earlier mapping
// here) every face moves a whole 1.0 and the light pool saturates into the white oval around the car.
// Returns the equation when the two passes apply, else 0.
static uint32_t SignedBlendOp(const State& s) {
    if (!SimpleState(s, kMBlendEnable)) return 0;
    uint32_t op = SimpleState(s, kMBlendOp);
    if (op != 0xF005 && op != 0xF006) return 0;
    if (SimpleState(s, kMSrcBlend) != 1 || SimpleState(s, kMDestBlend) != 1) {  // D3DBLEND_ONE
        LOG_FIRST(8, "d3d12: signed blend equation %X with factors %X/%X approximated without its bias", op,
                  SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend));
        return 0;
    }
    return op;
}

// The console draws its 3D into the guest sized colour target, then the overlay over it: HUD, Flash, text, and in a
// race the minimap last. A depth tested draw (a function other than always, or depth written) of vertices the GPU
// transforms marks the target as the frame's scene; the first such draw that tests nothing is the overlay starting,
// and the scene goes through the pass then, once a frame. Screen space (XYZRHW) draws decide nothing either way:
// Bink's frame is one depth tested XYZRHW quad, and the game's own glow and blur composites are untested XYZRHW quads
// in the middle of its 3D (captures 200316/200325/200435/200445: the HUD starts at the first untested FVF 0x142
// draw, the 3D minimap's depth tested draws in a 128x128 viewport come after it). Video, legal and 2D menus have
// no scene and are never filtered.
void D3D12Backend::PostBeforeDraw(const State& s, bool pretransformed) {
    if (post_done_ == presents_ || pretransformed) return;
    constexpr uint32_t kAlways = 0x207;
    bool zen = s.rs[kRsZEnable] != 0;
    uint32_t zfunc = zen ? SimpleState(s, kMZFunc) : kAlways;
    bool zwrite = zen && SimpleState(s, kMZWriteEnable) != 0;
    bool scene = zen && (zwrite || zfunc != kAlways);
    if (scene) {
        uint32_t w = 0, h = 0;
        SurfaceSize(s.rt, w, h);
        if (w == guest_w_ && h == guest_h_) {
            post_scene_ = s.rt.data;
            if (render::On(render::kMotionBlur)) {
                blur_depth_ = s.zs;
                blur_viewport_ = s.viewport;
                std::memcpy(blur_view_, s.transforms[2].data(), sizeof(blur_view_));
                std::memcpy(blur_proj_, s.transforms[3].data(), sizeof(blur_proj_));
                if (!blur_scene_valid_ || !blur_current_count_) {
                    blur_current_count_ = std::min(uint32_t(std::max(s.host[0], 0.0f)),
                                                   uint32_t(State::kHostCars));
                    for (uint32_t c = 0; c < blur_current_count_; c++)
                        std::memcpy(blur_current_cars_[c], s.host.data() + 4 + c * State::kHostCarStride,
                                    sizeof(blur_current_cars_[c]));
                }
                blur_scene_valid_ = true;
            }
        }
        return;
    }
    if (!post_scene_ || s.rt.data != post_scene_) return;
    Target* t = FindTarget(s.rt, false);
    if (t) PostApply(*t, "overlay");
}

// MC3 draws its opaque world and the cars (each after its reflection map), then its own blob shadows, decals and
// particles blended without depth writes, then the overlay (capture 201454: blob shadows at 0600, particles from
// 0676). The decal goes in before the first blended, depth tested, not depth written draw of transformed vertices
// that follows an opaque one in the scene target, or before the overlay when there is none; the ground and the car
// are in the depth then, smoke and glass are not yet drawn over it.
void D3D12Backend::ShadowBeforeDraw(const State& s, bool pretransformed) {
    if (shadow_done_ == presents_ || pretransformed) return;
    constexpr uint32_t kAlways = 0x207;
    bool zen = s.rs[kRsZEnable] != 0;
    uint32_t zfunc = zen ? SimpleState(s, kMZFunc) : kAlways;
    bool zwrite = zen && SimpleState(s, kMZWriteEnable) != 0;
    bool tests = zen && (zwrite || zfunc != kAlways);
    if (zwrite) {
        uint32_t w = 0, h = 0;
        SurfaceSize(s.rt, w, h);
        if (w == guest_w_ && h == guest_h_) shadow_scene_ = s.rt.data;
        return;
    }
    if (!shadow_scene_ || s.rt.data != shadow_scene_) return;
    bool blended = SimpleState(s, kMBlendEnable) != 0;
    if (tests && !blended) return;
    ShadowApply(s, tests ? "blended" : "overlay");
}

// out = m^-1 for a row vector matrix; false when singular
static bool Invert4(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] -
             m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] +
             m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] +
              m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] +
             m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] +
             m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] +
             m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] +
              m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] +
             m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] +
              m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}

// MC3 sets its environment light as fixed function directional light 2: San Diego and the MC2 cities at night a grey
// light 30 degrees up (moonlight, diffuse 0.38, 0.19 in places), Paris at dusk an orange one 10 degrees up (0.41 0.23
// 0.14); the same direction reaches the car programs as c14. Slot 0 is a white 45 degree default, slot 1 the local
// lights (street lamps) that change direction and colour from object to object (logs 19/10)
void D3D12Backend::GameLight(const State& s) {
    constexpr int kEnvironment = 2;
    if (game_frame_ != presents_) game_best_ = 0;
    for (int i = kEnvironment; i <= kEnvironment; i++) {
        if (!((s.light_enable >> i) & 1) || s.lights[size_t(i)][0] != 3) continue;
        float f[26];
        std::memcpy(f, s.lights[size_t(i)].data(), sizeof(f));
        float lum = 0.2126f * f[1] + 0.7152f * f[2] + 0.0722f * f[3];
        bool white_default = std::fabs(f[16] - 0.5f) < 1e-3f && std::fabs(f[17] + 0.7071f) < 1e-3f &&
                             std::fabs(f[18] - 0.5f) < 1e-3f && lum > 0.99f;
        float n = std::sqrt(f[16] * f[16] + f[17] * f[17] + f[18] * f[18]);
        // between its uses the title leaves the slot pointing straight down and black: not a light. The environment
        // light itself may be black (the MC2 cities at night): that is no key light, and no shadow
        bool reset = std::fabs(f[16]) < 1e-3f && std::fabs(f[17] + 1) < 1e-3f && std::fabs(f[18]) < 1e-3f && lum < 0.02f;
        if (white_default || reset || !(n > 1e-3f) || f[17] / n > -0.05f) continue;  // lit from below: no shadow
        if (game_frame_ == presents_ && lum < game_best_) continue;
        game_best_ = lum;
        game_lum_ = lum;
        for (int j = 0; j < 3; j++) game_dir_[j] = f[16 + j] / n;
        game_frame_ = presents_;
    }
}

// a depth tested triangle draw of transformed vertices into the guest sized scene target that writes depth or does
// not blend: MC3 draws parts of its cars opaque without writing depth, smoke and glows blend without writing it
bool D3D12Backend::SoEligible(const State& s, const Assembled& a, const PsoKey& k) {
    if (!(render::Streams()) || a.pretransformed || a.topology != Topology::Triangles) return false;
    if (!k.zen || !bound_ds_ || (!k.zwrite && k.blend)) return false;
    uint32_t w = 0, h = 0;
    SurfaceSize(s.rt, w, h);
    // the whole target's viewport: the minimap draws its 3D into a 128x128 corner with a camera of its own
    return w == guest_w_ && h == guest_h_ && s.viewport.x == 0 && s.viewport.y == 0 && s.viewport.w == w &&
           s.viewport.h == h;
}

// the draw's own pipeline with its clip positions streamed out
ID3D12PipelineState* D3D12Backend::EnsureSoPipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = so_psos_.find(h);
    if (it != so_psos_.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    static const D3D12_SO_DECLARATION_ENTRY decl{0, "SV_Position", 0, 0, 4, 0};
    static const UINT stride = 16;
    d.StreamOutput = {&decl, 1, &stride, 1, 0};
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
        LOG_FIRST(4, "MC3RENDER vehicle-shadow: no stream output pipeline for a draw, it does not cast");
        pso.Reset();
    }
    so_psos_[h] = pso;
    return pso.Get();
}

// this frame's buffer bound for one draw; the first draw of a frame starts it over
bool D3D12Backend::SoBegin(const State& s, const Assembled& a) {
    SoBuffer& b = so_[presents_ & 1];
    if (!b.buf) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kSoVerts) * 16);
        D3D12_RESOURCE_DESC fd = BufferDesc(kSoDraws * 8);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&b.buf))) ||
            FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &fd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&b.filled)))) {
            LOG_FIRST(1, "MC3RENDER vehicle-shadow: no stream output buffer");
            b.buf.Reset();
            return false;
        }
        b.state = b.filled_state = D3D12_RESOURCE_STATE_COMMON;
    }
    if (b.frame != presents_) {
        Transition(b.filled.Get(), b.filled_state, D3D12_RESOURCE_STATE_COPY_DEST);
        Alloc z = Upload(kSoDraws * 8, 16);
        std::memset(z.cpu, 0, kSoDraws * 8);
        cl_->CopyBufferRegion(b.filled.Get(), 0, z.res, z.off, kSoDraws * 8);
        Transition(b.filled.Get(), b.filled_state, D3D12_RESOURCE_STATE_STREAM_OUT);
        Transition(b.buf.Get(), b.state, D3D12_RESOURCE_STATE_STREAM_OUT);
        b.frame = presents_;
        b.verts = 0;
        b.draws = 0;
        b.solid.clear();
    }
    uint32_t n = a.index_count ? a.index_count : a.draw_count;
    uint32_t out = a.strip ? (n >= 3 ? 3 * (n - 2) : 0) : n;
    if (b.verts + out > kSoVerts || b.draws >= kSoDraws) {
        vs_full_++;
        D3D12_STREAM_OUTPUT_BUFFER_VIEW none{};
        cl_->SOSetTargets(0, 1, &none);
        return false;
    }
    D3D12_STREAM_OUTPUT_BUFFER_VIEW v{b.buf->GetGPUVirtualAddress() + uint64_t(b.verts) * 16,
                                      uint64_t(kSoVerts - b.verts) * 16,
                                      b.filled->GetGPUVirtualAddress() + uint64_t(b.draws) * 8};
    cl_->SOSetTargets(0, 1, &v);
    return true;
}

void D3D12Backend::SoEnd(const State& s, const Assembled& a) {
    SoBuffer& b = so_[presents_ & 1];
    uint32_t n = a.index_count ? a.index_count : a.draw_count;
    uint32_t out = a.strip ? (n >= 3 ? 3 * (n - 2) : 0) : n - n % 3;
    // MC3 leaves the alpha test on for nearly everything, at ALWAYS or GEQUAL 0: only a test that can discard cuts
    uint32_t afunc = SimpleState(s, kMAlphaFunc), aref = SimpleState(s, kMAlphaRef) & 0xFF;
    bool cuts = SimpleState(s, kMAlphaTestEnable) && afunc != 0x207 && !(afunc == 0x206 && aref == 0) &&
                !(afunc == 0x203 && aref == 0xFF);
    if (out && !cuts) {
        if (!b.solid.empty() && b.solid.back().first + b.solid.back().second == b.verts) b.solid.back().second += out;
        else b.solid.push_back({b.verts, out});
    }
    b.verts += out;
    b.draws++;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW none{};
    cl_->SOSetTargets(0, 1, &none);
}

bool D3D12Backend::EnsureShadowMap(ComPtr<ID3D12Resource>& map, D3D12_RESOURCE_STATES& state, uint32_t& dsv, int w,
                                   int h) {
    if (map) return true;
    if (free_dsv_.empty()) return false;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_R32_TYPELESS, uint64_t(w), uint32_t(h));
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                             IID_PPV_ARGS(&map))))
        return false;
    state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    dsv = free_dsv_.back();
    free_dsv_.pop_back();
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev_->CreateDepthStencilView(map.Get(), &dv, Cpu(dsv_heap_.Get(), dsv, dsv_inc_));
    return true;
}

// a depth only pipeline drawing streamed clip positions through hlsl's vs
bool D3D12Backend::EnsureCaster(const char* hlsl, ComPtr<ID3D12PipelineState>& pso, bool& failed, const char* what) {
    if (!pso && !failed) {
        uint64_t h;
        ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
        if (!vs) {
            failed = true;
            return false;
        }
        static const D3D12_INPUT_ELEMENT_DESC el{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_.Get();
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.InputLayout = {&el, 1};
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.SampleDesc.Count = 1;
        if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
            LOGE("MC3RENDER %s: the caster pipeline could not be made", what);
            failed = true;
            return false;
        }
    }
    return pso != nullptr;
}

bool D3D12Backend::EnsureVehicleShadow(int size) {
    return EnsureShadowMap(vs_map_, vs_map_state_, vs_dsv_, size, size) &&
           EnsureCaster(kCasterHlsl, caster_pso_, caster_failed_, "vehicle-shadow");
}

// the car into the light's depth map from the previous frame's streamed positions, then the ground near it
void D3D12Backend::VehicleShadow(const State& s, const float* car, const float eye[3], float dist, const char* why) {
    const render::VShadow& q = render::Get().vshadow;
    // diagnostic: the lights the title has set, every ten seconds or so
    bool first_diag = !vs_logged_ && vs_diag_frame_ == ~0ull;
    if ((presents_ % 600 == 7 || first_diag) && vs_diag_frame_ != presents_) {
        vs_diag_frame_ = presents_;
        float fc[4];
        Color(s.rs[kRsFogColor], fc);
        LOGR("MC3RENDER vehicle-shadow: fog %u colour %.3f,%.3f,%.3f start %.1f end %.1f ambient %08X",
             s.rs[kRsFogEnable], fc[0], fc[1], fc[2], Bits(s.rs[kRsFogStart]), Bits(s.rs[kRsFogEnd]), s.rs[kRsAmbient]);
        for (int i = 0; i < 8; i++) {
            const auto& l = s.lights[size_t(i)];
            float f[26];
            std::memcpy(f, l.data(), sizeof(f));
            if (!l[0]) continue;
            LOGR("MC3RENDER vehicle-shadow: light %d %s type %u dir %.3f,%.3f,%.3f diffuse %.2f,%.2f,%.2f ambient "
                 "%.2f,%.2f,%.2f",
                 i, (s.light_enable >> i) & 1 ? "on " : "off", l[0], f[16], f[17], f[18], f[1], f[2], f[3], f[9], f[10],
                 f[11]);
        }
    }
    SoBuffer& pb = so_[(presents_ & 1) ^ 1];
    if (!pb.buf || pb.frame + 1 != presents_ || pb.car_frame != pb.frame || pb.inv_frame != pb.frame || !pb.valid ||
        pb.verts == 0) {
        vs_no_prev_++;
        return;
    }
    float strength = q.strength * std::clamp(1.0f - (dist - q.near_m) / (q.far_m - q.near_m), 0.0f, 1.0f);
    // the light: the title's environment light as it last set it (the MC2 cities set it at the start of a race and not
    // again), else, until it sets one, the configured one
    constexpr float kPi = 3.14159265f;
    bool game = q.game && game_frame_ != ~0ull;
    float dir[3];
    if (game) {
        std::memcpy(dir, game_dir_, sizeof(dir));
        game_lum_smooth_ = game_lum_smooth_ < 0 ? game_lum_ : game_lum_smooth_ + (game_lum_ - game_lum_smooth_) * 0.05f;
        strength *= std::clamp(std::max(game_lum_smooth_ / q.lumref, q.floor), 0.0f, 1.0f);
    } else {
        float el = q.elev * kPi / 180, az = q.azim * kPi / 180;
        dir[0] = -std::cos(el) * std::sin(az), dir[1] = -std::sin(el), dir[2] = -std::cos(el) * std::cos(az);
    }
    float elev = std::asin(std::clamp(-dir[1], 0.0f, 1.0f));
    // a low light casts long: the box grows to hold a 1.6 m tall car's shadow, up to 12 m
    float R = std::clamp(1.5f + 1.6f / std::max(std::tan(elev), 0.05f), q.radius, 12.0f);
    if (game_state_ != int(game)) {
        game_state_ = int(game);
        LOGR("MC3RENDER vehicle-shadow: light from %s, direction %.3f,%.3f,%.3f (%.1f degrees up), brightness %.2f, box %.1f m",
             game ? "the title's key light" : "the configured angles", dir[0], dir[1], dir[2], elev * 180 / kPi,
             game ? game_lum_ : 1.0f, R);
    }
    if (strength <= 0.001f && !q.debug) {
        (game && game_lum_smooth_ / q.lumref <= q.floor + 1e-3f ? vs_dark_ : vs_far_)++;
        return;
    }
    Target* rt = FindTarget(s.rt, false);
    Target* ds = s.zs.data ? FindTarget(s.zs, true) : nullptr;
    if (!rt || !ds || ds->w != rt->w || ds->h != rt->h || rt->range != 1 || !EnsureVehicleShadow(q.size)) {
        vs_no_target_++;
        return;
    }
    // the car's axes normalised, its position
    auto axes = [](const float* m, float* out) {
        for (int r = 0; r < 3; r++) {
            float n = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
            if (!(n > 1e-4f)) n = 1;
            for (int c = 0; c < 3; c++) out[r * 4 + c] = m[r * 3 + c] / n;
            out[r * 4 + 3] = 0;
        }
        out[12] = m[9], out[13] = m[10], out[14] = m[11], out[15] = 0;
    };
    // the light: travelling along dir, an orthographic box of 2 R square round the car, 2 kReach deep
    constexpr float kReach = 30;
    float up0[3] = {0, 1, 0};
    if (std::fabs(dir[1]) > 0.99f) up0[0] = 0, up0[1] = 0, up0[2] = 1;
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
    };
    auto norm = [](float* v) {
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n > 0) v[0] /= n, v[1] /= n, v[2] /= n;
    };
    float right[3], up[3];
    cross(up0, dir, right);
    norm(right);
    cross(dir, right, up);
    float leye[3] = {car[9] - dir[0] * kReach, car[10] - dir[1] * kReach, car[11] - dir[2] * kReach};
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float F = 2 * kReach;
    float lvp[16] = {right[0] / R, up[0] / R, dir[0] / F, 0, right[1] / R, up[1] / R, dir[1] / F, 0,
                     right[2] / R, up[2] / R, dir[2] / F, 0,
                     -dot3(leye, right) / R, -dot3(leye, up) / R, -dot3(leye, dir) / F, 1};
    // the box moves in whole texels: shadow edges stay put while the car moves
    float texel = 2.0f / float(q.size);
    float cx = car[9] * lvp[0] + car[10] * lvp[4] + car[11] * lvp[8] + lvp[12];
    float cy = car[9] * lvp[1] + car[10] * lvp[5] + car[11] * lvp[9] + lvp[13];
    lvp[12] += std::round(cx / texel) * texel - cx;
    lvp[13] += std::round(cy / texel) * texel - cy;

    // casters
    float kc[kCasterConstants] = {};
    std::memcpy(kc, pb.inv, sizeof(pb.inv));
    axes(pb.car, kc + 16);
    axes(car, kc + 32);
    std::memcpy(kc + 48, lvp, sizeof(lvp));
    kc[64] = pb.minz, kc[65] = pb.maxz;
    // the car's box: half width, top, half length, bottom; the bottom just clears the road, so the tyres touch it
    kc[68] = q.boxw, kc[69] = q.boxt, kc[70] = q.boxl, kc[71] = 0.02f;
    uint32_t slot = uint32_t(frame_ % kFrames);
    bool stamp = time_heap_ && time_open_[slot] && !time_vshadow_[slot];
    if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 6);
    Transition(pb.buf.Get(), pb.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(vs_map_.Get(), vs_map_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), vs_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(uint32_t(q.size), uint32_t(q.size));
    SetPipeline(caster_pso_.Get());
    Alloc cb = Upload(512, 256);
    std::memcpy(cb.cpu, kc, sizeof(kc));
    cl_->SetGraphicsRootConstantBufferView(1, cb.gpu);
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    uint32_t verts = std::min(pb.verts, kSoVerts);
    D3D12_VERTEX_BUFFER_VIEW vb{pb.buf->GetGPUVirtualAddress(), verts * 16, 16};
    cl_->IASetVertexBuffers(0, 1, &vb);
    cl_->DrawInstanced(verts, 1, 0, 0);
    bound_rt_ = bound_ds_ = nullptr;
    Transition(vs_map_.Get(), vs_map_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // receivers
    const float* view = s.transforms[2].data();
    const float* proj = s.transforms[3].data();
    float vp[16], inv[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            vp[r * 4 + c] = view[r * 4] * proj[c] + view[r * 4 + 1] * proj[4 + c] + view[r * 4 + 2] * proj[8 + c] +
                            view[r * 4 + 3] * proj[12 + c];
    if (!Invert4(vp, inv)) {
        vs_no_target_++;
        return;
    }
    float sc = float(rt->w) / float(rt->gw);
    float k[kReceiverConstants] = {};
    std::memcpy(k, inv, sizeof(inv));
    std::memcpy(k + 16, lvp, sizeof(lvp));
    axes(car, k + 32);
    k[47] = strength;
    k[48] = float(s.viewport.x) * sc, k[49] = float(s.viewport.y) * sc, k[50] = float(s.viewport.w) * sc,
    k[51] = float(s.viewport.h) * sc;
    k[52] = s.viewport.min_z, k[53] = s.viewport.max_z, k[54] = float(q.debug), k[55] = float(q.size);
    k[56] = q.bias / F, k[57] = q.slope * (2 * R / float(q.size)) / F, k[58] = float(q.pcf), k[59] = q.top;
    k[60] = R, k[61] = dir[0], k[62] = dir[1], k[63] = dir[2];
    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_vshadow_pre_%s", cap_i_, why);
        CapSave(*rt, name);
    }
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* srcs[3] = {ds->res.Get(), vs_map_.Get(), nullptr};
    DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32_FLOAT, kBgra};
    PassN(kReceiverHlsl, "psmain", rt->fmt, View(*rt), rt->w, rt->h, srcs, fmts, k, kReceiverConstants, kPassDarken);
    if (stamp) {
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 7);
        time_vshadow_[slot] = true;
    }
    rt->drawn = true;
    vs_frames_++;
    if (cap_) {
        fprintf(cap_,
                "%04u vshadow %s rt %08X casters %u car %.2f,%.2f,%.2f camera %.1f m strength %.3f light %s %.3f,%.3f,%.3f "
                "box %.1f %s axes %.4f,%.4f,%.4f/%.4f,%.4f,%.4f/%.4f,%.4f,%.4f\n",
                cap_i_, why, rt->data, verts, car[9], car[10], car[11], dist, strength, game ? "game" : "fixed", dir[0],
                dir[1], dir[2], R, render::VShadowNames(q).c_str(), car[0], car[1], car[2], car[3], car[4], car[5], car[6],
                car[7], car[8]);
        char name[48];
        snprintf(name, sizeof(name), "%04u_vshadow_%s", cap_i_++, why);
        CapSave(*rt, name);
    }
    if (!vs_logged_) {
        vs_logged_ = true;
        LOGR("MC3RENDER vehicle-shadow: first pass, %u streamed vertices from the previous frame, map %d, light %.2f,%.2f,%.2f",
             verts, q.size, dir[0], dir[1], dir[2]);
    }
}

// the city into the light's depth map from the previous frame's streamed positions, one cascade for the view's first
// range metres and one coarser for the rest out to far, then the shadow over both. Each cascade is the sphere round its
// slice of the view, so it neither swims when the camera turns nor shrinks with the angle to the light; it moves in
// whole texels. The two overlap over the last fifth of the near one, where the receiver hands over.
void D3D12Backend::SunShadow(const State& s, const float* car, const float eye[3], const char* why) {
    const render::Sun& q = render::Get().sun;
    SoBuffer& pb = so_[(presents_ & 1) ^ 1];
    if (!pb.buf || pb.frame + 1 != presents_ || pb.inv_frame != pb.frame || !pb.valid || pb.solid.empty()) {
        sun_no_prev_++;
        return;
    }
    constexpr float kPi = 3.14159265f;
    bool game = q.game && game_frame_ != ~0ull;
    float dir[3];
    float strength = q.strength;
    if (game) {
        std::memcpy(dir, game_dir_, sizeof(dir));
        sun_lum_smooth_ = sun_lum_smooth_ < 0 ? game_lum_ : sun_lum_smooth_ + (game_lum_ - sun_lum_smooth_) * 0.05f;
        strength *= std::clamp(std::max(sun_lum_smooth_ / q.lumref, q.floor), 0.0f, 1.0f);
    } else {
        float el = q.elev * kPi / 180, az = q.azim * kPi / 180;
        dir[0] = -std::cos(el) * std::sin(az), dir[1] = -std::sin(el), dir[2] = -std::cos(el) * std::cos(az);
    }
    if (sun_state_ != int(game)) {
        sun_state_ = int(game);
        LOGR("MC3RENDER sun-shadow: light from %s, direction %.3f,%.3f,%.3f (%.1f degrees up), brightness %.2f",
             game ? "the title's key light" : "the configured angles", dir[0], dir[1], dir[2],
             std::asin(std::clamp(-dir[1], 0.0f, 1.0f)) * 180 / kPi, game ? game_lum_ : 1.0f);
    }
    if (strength <= 0.001f && !q.debug) {
        sun_dark_++;
        return;
    }
    Target* rt = FindTarget(s.rt, false);
    Target* ds = s.zs.data ? FindTarget(s.zs, true) : nullptr;
    if (!rt || !ds || ds->w != rt->w || ds->h != rt->h || rt->range != 1 ||
        !EnsureShadowMap(sun_map_, sun_map_state_, sun_dsv_, q.size * q.cascades, q.size) ||
        !EnsureCaster(kSunCasterHlsl, sun_caster_pso_, sun_caster_failed_, "sun-shadow")) {
        sun_no_target_++;
        return;
    }
    // this frame's camera
    const float* view = s.transforms[2].data();
    const float* proj = s.transforms[3].data();
    float vp[16], inv[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            vp[r * 4 + c] = view[r * 4] * proj[c] + view[r * 4 + 1] * proj[4 + c] + view[r * 4 + 2] * proj[8 + c] +
                            view[r * 4 + 3] * proj[12 + c];
    if (!Invert4(vp, inv)) {
        sun_no_target_++;
        return;
    }
    auto unproject = [&](float z, float* o) {
        float w = z * inv[11] + inv[15];
        if (std::fabs(w) < 1e-12f) w = 1e-12f;
        for (int i = 0; i < 3; i++) o[i] = (z * inv[8 + i] + inv[12 + i]) / w;
    };
    auto norm = [](float* v) {
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n > 0) v[0] /= n, v[1] /= n, v[2] /= n;
    };
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
    };
    float p0[3], p1[3], fwd[3];
    unproject(0.0f, p0);
    unproject(0.5f, p1);
    for (int i = 0; i < 3; i++) fwd[i] = p1[i] - p0[i];
    norm(fwd);
    // the sphere round the view's slice n..f: its centre t along the view, as far from the near corners as from the
    // far ones (or at f), its radius in whole metres
    float tx = 1 / std::max(std::fabs(proj[0]), 1e-3f), ty = 1 / std::max(std::fabs(proj[5]), 1e-3f);
    float T2 = tx * tx + ty * ty;
    struct Cascade {
        float n, f, t, R, F;
        float center[3];
        float lvp[16];
    };
    // the light: an orthographic box of 2 R square round the sphere, reaching kBack metres further toward the light
    // for what stands between the sun and the slice (a 30 m building 10 degrees under the sun casts 170 m)
    constexpr float kBack = 200;
    float up0[3] = {0, 1, 0};
    if (std::fabs(dir[1]) > 0.99f) up0[0] = 0, up0[1] = 0, up0[2] = 1;
    float right[3], up[3];
    cross(up0, dir, right);
    norm(right);
    cross(dir, right, up);
    auto fit = [&](float n, float f, Cascade& c) {
        c.n = n, c.f = f;
        c.t = std::min((f + n) * (1 + T2) / 2, f);
        c.R = std::ceil(std::max(std::sqrt((c.t - n) * (c.t - n) + n * n * T2), std::sqrt((f - c.t) * (f - c.t) + f * f * T2)));
        for (int i = 0; i < 3; i++) c.center[i] = eye[i] + fwd[i] * c.t;
        float R = c.R, F = c.F = 2 * c.R + kBack;
        float leye[3] = {c.center[0] - dir[0] * (R + kBack), c.center[1] - dir[1] * (R + kBack),
                         c.center[2] - dir[2] * (R + kBack)};
        float m[16] = {right[0] / R, up[0] / R, dir[0] / F, 0, right[1] / R, up[1] / R, dir[1] / F, 0,
                       right[2] / R, up[2] / R, dir[2] / F, 0,
                       -dot3(leye, right) / R, -dot3(leye, up) / R, -dot3(leye, dir) / F, 1};
        float texel = 2.0f / float(q.size);
        float cx = c.center[0] * m[0] + c.center[1] * m[4] + c.center[2] * m[8] + m[12];
        float cy = c.center[0] * m[1] + c.center[1] * m[5] + c.center[2] * m[9] + m[13];
        m[12] += std::round(cx / texel) * texel - cx;
        m[13] += std::round(cy / texel) * texel - cy;
        std::memcpy(c.lvp, m, sizeof(m));
    };
    float L = q.range;
    int cascades = q.cascades >= 2 && q.far_m > L + 1 ? 2 : 1;
    Cascade cs[2];
    fit(0, L, cs[0]);
    if (cascades == 2) fit(0.8f * L, q.far_m, cs[1]);

    // casters: the player's car moves to where it is now, or is left out when the vehicle shadow draws it
    auto axes = [](const float* m, float* out) {
        for (int r = 0; r < 3; r++) {
            float n = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
            if (!(n > 1e-4f)) n = 1;
            for (int c = 0; c < 3; c++) out[r * 4 + c] = m[r * 3 + c] / n;
            out[r * 4 + 3] = 0;
        }
        out[12] = m[9], out[13] = m[10], out[14] = m[11], out[15] = 0;
    };
    const render::VShadow& vq = render::Get().vshadow;
    int mode = car && pb.car_frame == pb.frame ? (render::On(render::kVehicleShadow) ? 2 : 1) : 0;
    float kc[kSunCasterConstants] = {};
    std::memcpy(kc, pb.inv, sizeof(pb.inv));
    if (mode) {
        axes(pb.car, kc + 16);
        axes(car, kc + 32);
    }
    kc[64] = pb.minz, kc[65] = pb.maxz, kc[66] = float(mode);
    kc[68] = vq.boxw, kc[69] = vq.boxt, kc[70] = vq.boxl, kc[71] = 0.02f;
    uint32_t slot = uint32_t(frame_ % kFrames);
    bool stamp = time_heap_ && time_open_[slot] && !time_sun_[slot];
    if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 8);
    Transition(pb.buf.Get(), pb.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(sun_map_.Get(), sun_map_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), sun_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetPipeline(sun_caster_pso_.Get());
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{pb.buf->GetGPUVirtualAddress(), kSoVerts * 16, 16};
    cl_->IASetVertexBuffers(0, 1, &vb);
    uint32_t casters = 0;
    for (int ci = 0; ci < cascades; ci++) {
        D3D12_VIEWPORT v{float(ci * q.size), 0, float(q.size), float(q.size), 0, 1};
        D3D12_RECT r{LONG(ci * q.size), 0, LONG((ci + 1) * q.size), LONG(q.size)};
        SetViewport(v);
        SetScissor(r);
        std::memcpy(kc + 48, cs[ci].lvp, sizeof(cs[ci].lvp));
        Alloc cb = Upload(512, 256);
        std::memcpy(cb.cpu, kc, sizeof(kc));
        cl_->SetGraphicsRootConstantBufferView(1, cb.gpu);
        casters = 0;
        for (const auto& run : pb.solid) {
            if (run.first >= kSoVerts) break;
            uint32_t n = std::min(run.second, kSoVerts - run.first);
            cl_->DrawInstanced(n, 1, run.first, 0);
            casters += n;
        }
    }
    bound_rt_ = bound_ds_ = nullptr;
    Transition(sun_map_.Get(), sun_map_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // receivers; a cascade's light x, y and depth as columns of its matrix
    auto columns = [](const float* m, float* out) {
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 4; r++) out[c * 4 + r] = m[r * 4 + c];
    };
    float sc = float(rt->w) / float(rt->gw);
    float k[kSunReceiverConstants] = {};
    std::memcpy(k, inv, sizeof(inv));
    columns(cs[0].lvp, k + 16);
    if (cascades == 2) columns(cs[1].lvp, k + 28);
    k[40] = eye[0], k[41] = eye[1], k[42] = eye[2], k[43] = strength;
    k[44] = float(s.viewport.x) * sc, k[45] = float(s.viewport.y) * sc, k[46] = float(s.viewport.w) * sc,
    k[47] = float(s.viewport.h) * sc;
    k[48] = s.viewport.min_z, k[49] = s.viewport.max_z, k[50] = float(q.debug), k[51] = float(q.size);
    auto texel = [&](const Cascade& c) { return 2 * c.R / float(q.size); };
    k[52] = q.bias / cs[0].F, k[53] = q.slope * texel(cs[0]) / cs[0].F, k[54] = float(q.pcf), k[55] = L;
    k[56] = dir[0], k[57] = dir[1], k[58] = dir[2], k[59] = 0.8f * L;
    if (cascades == 2) {
        // the constant bias grows with the texel: the far cascade's texels are several times the near one's
        k[60] = q.bias * std::max(1.0f, cs[1].R / cs[0].R) / cs[1].F, k[61] = q.slope * texel(cs[1]) / cs[1].F;
        k[62] = q.far_m, k[63] = 0.8f * q.far_m;
    } else {
        k[62] = L;  // no further than range: one cascade
    }
    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_sun_pre_%s", cap_i_, why);
        CapSave(*rt, name);
    }
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* srcs[3] = {ds->res.Get(), sun_map_.Get(), nullptr};
    DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32_FLOAT, kBgra};
    PassN(kSunReceiverHlsl, "psmain", rt->fmt, View(*rt), rt->w, rt->h, srcs, fmts, k, kSunReceiverConstants, kPassDarken);
    if (stamp) {
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 9);
        time_sun_[slot] = true;
    }
    rt->drawn = true;
    sun_frames_++;
    if (cap_) {
        fprintf(cap_,
                "%04u sunshadow %s rt %08X casters %u of %u in %zu runs cascades %d near %.0f m at %.1f,%.1f,%.1f texel %.3f m "
                "far %.0f m at %.1f,%.1f,%.1f texel %.3f m strength %.3f light %s %.3f,%.3f,%.3f car %d %s\n",
                cap_i_, why, rt->data, casters, pb.verts, pb.solid.size(), cascades, cs[0].R, cs[0].center[0],
                cs[0].center[1], cs[0].center[2], texel(cs[0]), cascades == 2 ? cs[1].R : 0.0f,
                cascades == 2 ? cs[1].center[0] : 0.0f, cascades == 2 ? cs[1].center[1] : 0.0f,
                cascades == 2 ? cs[1].center[2] : 0.0f, cascades == 2 ? texel(cs[1]) : 0.0f, strength,
                game ? "game" : "fixed", dir[0], dir[1], dir[2], mode, render::SunNames(q).c_str());
        char name[48];
        snprintf(name, sizeof(name), "%04u_sun_%s", cap_i_++, why);
        CapSave(*rt, name);
    }
    if (!sun_logged_) {
        sun_logged_ = true;
        LOGR("MC3RENDER sun-shadow: first pass, %u of %u streamed vertices cast (%zu runs), %d cascade(s) of %d, near "
             "sphere %.0f m, far sphere %.0f m, light %.2f,%.2f,%.2f",
             casters, pb.verts, pb.solid.size(), cascades, q.size, cs[0].R, cascades == 2 ? cs[1].R : 0.0f, dir[0],
             dir[1], dir[2]);
    }
}

// the player's headlights as one spot light: the scene the title drew last frame into its depth map, the player's car
// left out; the title's headlight passes after this point take their light out where the map hides the light
void D3D12Backend::HeadlightShadow(const State& s, const float* car, const char* why) {
    const render::Lights& q = render::Get().lights;
    SoBuffer& pb = so_[(presents_ & 1) ^ 1];
    SoBuffer& cur = so_[presents_ & 1];
    if (!pb.buf || pb.frame + 1 != presents_ || pb.inv_frame != pb.frame || !pb.valid || pb.solid.empty() || !cur.valid ||
        cur.inv_frame != presents_) {
        hl_no_prev_++;
        return;
    }
    Target* rt = FindTarget(s.rt, false);
    if (!rt || !EnsureShadowMap(hl_map_, hl_map_state_, hl_dsv_, q.size, q.size) ||
        !EnsureCaster(kSunCasterHlsl, sun_caster_pso_, sun_caster_failed_, "light-shadow")) {
        hl_no_target_++;
        return;
    }
    auto norm = [](float* v) {
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n > 0) v[0] /= n, v[1] /= n, v[2] /= n;
    };
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
    };
    // the car looks down -z, y is its up
    float up_c[3] = {car[3], car[4], car[5]}, fwd[3] = {-car[6], -car[7], -car[8]};
    norm(up_c);
    norm(fwd);
    constexpr float kPi = 3.14159265f;
    float pr = q.pitch * kPi / 180;
    float dir[3], pos[3];
    for (int i = 0; i < 3; i++) {
        dir[i] = fwd[i] * std::cos(pr) + up_c[i] * std::sin(pr);
        pos[i] = car[9 + i] + fwd[i] * q.fwd + up_c[i] * q.height;
    }
    norm(dir);
    float up0[3] = {0, 1, 0};
    if (std::fabs(dir[1]) > 0.99f) up0[0] = 0, up0[1] = 0, up0[2] = 1;
    float right[3], up[3];
    cross(up0, dir, right);
    norm(right);
    cross(dir, right, up);
    float sx = 1 / std::tan(q.fov * kPi / 360);
    float N = q.near_m, F = q.far_m, A = F / (F - N), B = -N * F / (F - N);
    float lvp[16] = {sx * right[0], sx * up[0], A * dir[0], dir[0], sx * right[1], sx * up[1], A * dir[1], dir[1],
                     sx * right[2], sx * up[2], A * dir[2], dir[2],
                     -sx * dot3(pos, right), -sx * dot3(pos, up), -A * dot3(pos, dir) + B, -dot3(pos, dir)};
    // casters: the previous frame's scene, the player's car dropped (its own lights do not shade it)
    auto axes = [](const float* m, float* out) {
        for (int r = 0; r < 3; r++) {
            float n = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
            if (!(n > 1e-4f)) n = 1;
            for (int c = 0; c < 3; c++) out[r * 4 + c] = m[r * 3 + c] / n;
            out[r * 4 + 3] = 0;
        }
        out[12] = m[9], out[13] = m[10], out[14] = m[11], out[15] = 0;
    };
    const render::VShadow& vq = render::Get().vshadow;
    int mode = pb.car_frame == pb.frame ? 2 : 0;
    float kc[kSunCasterConstants] = {};
    std::memcpy(kc, pb.inv, sizeof(pb.inv));
    if (mode) {
        axes(pb.car, kc + 16);
        axes(car, kc + 32);
    }
    std::memcpy(kc + 48, lvp, sizeof(lvp));
    kc[64] = pb.minz, kc[65] = pb.maxz, kc[66] = float(mode);
    kc[68] = vq.boxw, kc[69] = vq.boxt, kc[70] = vq.boxl, kc[71] = -0.5f;
    uint32_t slot = uint32_t(frame_ % kFrames);
    bool stamp = time_heap_ && time_open_[slot] && !time_hl_[slot];
    if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 10);
    Transition(pb.buf.Get(), pb.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(hl_map_.Get(), hl_map_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), hl_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(uint32_t(q.size), uint32_t(q.size));
    SetPipeline(sun_caster_pso_.Get());
    Alloc cb = Upload(512, 256);
    std::memcpy(cb.cpu, kc, sizeof(kc));
    cl_->SetGraphicsRootConstantBufferView(1, cb.gpu);
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{pb.buf->GetGPUVirtualAddress(), kSoVerts * 16, 16};
    cl_->IASetVertexBuffers(0, 1, &vb);
    uint32_t casters = 0;
    for (const auto& run : pb.solid) {
        if (run.first >= kSoVerts) break;
        uint32_t n = std::min(run.second, kSoVerts - run.first);
        cl_->DrawInstanced(n, 1, run.first, 0);
        casters += n;
    }
    bound_rt_ = bound_ds_ = nullptr;
    Transition(hl_map_.Get(), hl_map_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (stamp) {
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 11);
        time_hl_[slot] = true;
    }
    // what the headlight passes read
    float sc = float(rt->w) / float(rt->gw);
    std::memcpy(hl_k_, cur.inv, sizeof(cur.inv));
    std::memcpy(hl_k_ + 16, lvp, sizeof(lvp));
    hl_k_[32] = float(s.viewport.x) * sc, hl_k_[33] = float(s.viewport.y) * sc, hl_k_[34] = float(s.viewport.w) * sc,
    hl_k_[35] = float(s.viewport.h) * sc;
    hl_k_[36] = cur.minz, hl_k_[37] = cur.maxz, hl_k_[38] = float(q.size), hl_k_[39] = q.strength;
    hl_k_[40] = N, hl_k_[41] = F, hl_k_[42] = q.bias, hl_k_[43] = float(q.pcf);
    hl_k_[44] = float(q.debug), hl_k_[45] = q.slope, hl_k_[46] = 0, hl_k_[47] = 0;
    hl_frame_ = presents_;
    hl_frames_++;
    if (cap_)
        fprintf(cap_, "%04u lightshadow %s casters %u of %u light %.2f,%.2f,%.2f dir %.3f,%.3f,%.3f %s\n", cap_i_, why, casters,
                pb.verts, pos[0], pos[1], pos[2], dir[0], dir[1], dir[2], render::LightsNames(q).c_str());
    if (!hl_logged_) {
        hl_logged_ = true;
        LOGR("MC3RENDER light-shadow: first map, %u streamed vertices cast, headlight at %.2f,%.2f,%.2f looking %.3f,%.3f,%.3f",
             casters, pos[0], pos[1], pos[2], dir[0], dir[1], dir[2]);
    }
}

// the glows MC3 draws at its lamps' heads are screen space cards (XYZRHW: guest pixels, depth, 1/w), four corners each,
// strung into one strip by repeated vertices; a card's centre and depth go back to the world with this frame's camera,
// and those at a lamp head's height over the player's car go into the cache
void D3D12Backend::CollectLampGlows(const State& s, const Assembled& a) {
    const render::Lights& q = render::Get().lights;
    if (!shadow_have_ || !a.pretransformed) return;
    SoBuffer& cur = so_[presents_ & 1];
    if (!cur.valid || cur.inv_frame != presents_) return;
    uint32_t n = a.vertex_count;
    if (!n || n > 8192) return;
    const VertexElement& e = a.elements[0];
    size_t regs = a.vertices.empty() ? 0 : a.vertices.size() / (size_t(n) * 4);
    if (a.vertices.empty() && e.format != DXGI_FORMAT_R32G32B32A32_FLOAT) return;
    auto pos = [&](uint32_t v, float* f) {
        if (regs) {
            std::memcpy(f, &a.vertices[(size_t(v) * regs + e.offset / 16) * 4], 12);
        } else {
            const VertexBuffer& vb = a.buffers[e.buffer];
            uint64_t at = uint64_t(v) * vb.stride + e.offset;
            if (!vb.src || at + 12 > vb.bytes) return false;
            std::memcpy(f, vb.src + at, 12);
        }
        return std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
    };
    // the cards: four distinct corners in a row, on two x and two y values
    std::vector<std::array<float, 3>> cards;
    float quad[4][3], prev[3] = {1e30f, 1e30f, 1e30f};
    int k = 0;
    for (uint32_t v = 0; v < n; v++) {
        float f[3];
        if (!pos(v, f)) continue;
        if (std::fabs(f[0] - prev[0]) < 1e-3f && std::fabs(f[1] - prev[1]) < 1e-3f) continue;  // a strip's join
        std::memcpy(prev, f, sizeof(prev));
        std::memcpy(quad[k++], f, sizeof(f));
        if (k < 4) continue;
        auto near1 = [](float x, float y) { return std::fabs(x - y) < 0.5f; };
        bool rect = (near1(quad[0][0], quad[2][0]) && near1(quad[1][0], quad[3][0]) && near1(quad[0][1], quad[1][1]) &&
                     near1(quad[2][1], quad[3][1])) ||
                    (near1(quad[0][1], quad[2][1]) && near1(quad[1][1], quad[3][1]) && near1(quad[0][0], quad[1][0]) &&
                     near1(quad[2][0], quad[3][0]));
        if (rect) {
            cards.push_back({(quad[0][0] + quad[1][0] + quad[2][0] + quad[3][0]) / 4,
                             (quad[0][1] + quad[1][1] + quad[2][1] + quad[3][1]) / 4,
                             (quad[0][2] + quad[1][2] + quad[2][2] + quad[3][2]) / 4});
            k = 0;
        } else {
            std::memmove(quad[0], quad[1], sizeof(float) * 9);  // out of step: drop the first corner
            k = 3;
        }
    }
    struct Acc {
        float sum[3];
        uint32_t n;
    };
    std::vector<Acc> glows;
    const float vx = float(s.viewport.x), vy = float(s.viewport.y), vw = float(s.viewport.w), vh = float(s.viewport.h);
    for (const auto& c : cards) {
        if (c[0] < vx || c[0] > vx + vw || c[1] < vy || c[1] > vy + vh || !(c[2] > 0) || !(c[2] < 1)) continue;
        float nx = (c[0] - vx) / vw * 2 - 1, ny = 1 - (c[1] - vy) / vh * 2;
        float nz = (c[2] - cur.minz) / std::max(cur.maxz - cur.minz, 1e-6f);
        float w[4];
        for (int i = 0; i < 4; i++) w[i] = nx * cur.inv[i] + ny * cur.inv[4 + i] + nz * cur.inv[8 + i] + cur.inv[12 + i];
        if (!(std::fabs(w[3]) > 1e-9f)) continue;
        glows.push_back({{w[0] / w[3], w[1] / w[3], w[2] / w[3]}, 1});
    }
    if (glows_frame_ != presents_) {
        glows_prev_.swap(glows_now_);
        glows_now_.clear();
        glows_frame_ = presents_;
    }
    for (const auto& g : glows)
        if (glows_now_.size() < 1024) glows_now_.push_back({g.sum[0], g.sum[1], g.sum[2]});
    if (q.lamps <= 0) return;
    constexpr float kSame = 1.5f;
    for (const auto& g : glows) {
        float c[3] = {g.sum[0] / g.n, g.sum[1] / g.n, g.sum[2] / g.n};
        float h = c[1] - shadow_last_[1];
        if (h < q.lamp_min_h || h > q.lamp_max_h) continue;
        lamp_glows_++;
        Lamp* found = nullptr;
        for (auto& l : lamps_) {
            float dx = c[0] - l.p[0], dy = c[1] - l.p[1], dz = c[2] - l.p[2];
            if (dx * dx + dy * dy + dz * dz < kSame * kSame) {
                found = &l;
                break;
            }
        }
        if (found) {
            if (found->last == presents_) continue;
            float wgt = float(std::min<uint32_t>(found->hits, 32));
            for (int i = 0; i < 3; i++) found->p[i] = (found->p[i] * wgt + c[i]) / (wgt + 1);
            found->hits++;
            found->last = presents_;
        } else {
            if (lamps_.size() >= 4096) {
                auto oldest = std::min_element(lamps_.begin(), lamps_.end(),
                                               [](const Lamp& x, const Lamp& y) { return x.last < y.last; });
                *oldest = Lamp{{c[0], c[1], c[2]}, 1, presents_};
            } else {
                lamps_.push_back(Lamp{{c[0], c[1], c[2]}, 1, presents_});
            }
        }
    }
}

// the nearest lamps each into a downward depth map (a 2 x 2 atlas) from the previous frame's streamed scene, the player's
// car moved to where it is now; then, per lamp, the baked light taken down where the lamp is hidden
void D3D12Backend::LampShadows(const State& s, const float* car, const char* why) {
    const render::Lights& q = render::Get().lights;
    SoBuffer& pb = so_[(presents_ & 1) ^ 1];
    SoBuffer& cur = so_[presents_ & 1];
    if (!pb.buf || pb.frame + 1 != presents_ || pb.inv_frame != pb.frame || !pb.valid || pb.solid.empty() || !cur.valid ||
        cur.inv_frame != presents_)
        return;
    // the camera
    const float* view = s.transforms[2].data();
    float eye[3];
    for (int j = 0; j < 3; j++) eye[j] = -(view[12] * view[j * 4] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    std::vector<std::pair<float, const Lamp*>> nearest;
    for (const auto& l : lamps_) {
        if (l.hits < uint32_t(q.lamp_hits)) continue;
        float dx = l.p[0] - eye[0], dy = l.p[1] - eye[1], dz = l.p[2] - eye[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < q.lamp_range * q.lamp_range) nearest.push_back({d2, &l});
    }
    if (nearest.empty()) return;
    std::sort(nearest.begin(), nearest.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    size_t count = std::min<size_t>(nearest.size(), size_t(std::min(q.lamps, 4)));
    Target* rt = FindTarget(s.rt, false);
    Target* ds = s.zs.data ? FindTarget(s.zs, true) : nullptr;
    int size = q.lamp_size;
    if (!rt || !ds || ds->w != rt->w || ds->h != rt->h || rt->range != 1 ||
        !EnsureShadowMap(lamp_map_, lamp_map_state_, lamp_dsv_, size * 2, size * 2) ||
        !EnsureCaster(kSunCasterHlsl, sun_caster_pso_, sun_caster_failed_, "light-shadow"))
        return;
    auto axes = [](const float* m, float* out) {
        for (int r = 0; r < 3; r++) {
            float n = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
            if (!(n > 1e-4f)) n = 1;
            for (int c = 0; c < 3; c++) out[r * 4 + c] = m[r * 3 + c] / n;
            out[r * 4 + 3] = 0;
        }
        out[12] = m[9], out[13] = m[10], out[14] = m[11], out[15] = 0;
    };
    constexpr float kPi = 3.14159265f;
    float sx = 1 / std::tan(q.lamp_fov * kPi / 360);
    float N = 0.3f, F = q.lamp_far, A = F / (F - N), B = -N * F / (F - N);
    // looking straight down: x east, y north on the map
    float lvps[4][16], lpos[4][3];
    for (size_t i = 0; i < count; i++) {
        const Lamp& l = *nearest[i].second;
        float pos[3] = {l.p[0], l.p[1] - q.lamp_drop, l.p[2]};
        std::memcpy(lpos[i], pos, sizeof(pos));
        float right[3] = {1, 0, 0}, up[3] = {0, 0, 1}, dir[3] = {0, -1, 0};
        auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        float m[16] = {sx * right[0], sx * up[0], A * dir[0], dir[0], sx * right[1], sx * up[1], A * dir[1], dir[1],
                       sx * right[2], sx * up[2], A * dir[2], dir[2],
                       -sx * dot3(pos, right), -sx * dot3(pos, up), -A * dot3(pos, dir) + B, -dot3(pos, dir)};
        std::memcpy(lvps[i], m, sizeof(m));
    }
    const render::VShadow& vq = render::Get().vshadow;
    int mode = pb.car_frame == pb.frame ? 1 : 0;
    float kc[kSunCasterConstants] = {};
    std::memcpy(kc, pb.inv, sizeof(pb.inv));
    if (mode) {
        axes(pb.car, kc + 16);
        axes(car, kc + 32);
    }
    kc[64] = pb.minz, kc[65] = pb.maxz, kc[66] = float(mode);
    kc[68] = vq.boxw, kc[69] = vq.boxt, kc[70] = vq.boxl, kc[71] = -0.5f;
    uint32_t slot = uint32_t(frame_ % kFrames);
    bool stamp = time_heap_ && time_open_[slot] && !time_lamp_[slot];
    if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 12);
    Transition(pb.buf.Get(), pb.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(lamp_map_.Get(), lamp_map_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), lamp_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetPipeline(sun_caster_pso_.Get());
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{pb.buf->GetGPUVirtualAddress(), kSoVerts * 16, 16};
    cl_->IASetVertexBuffers(0, 1, &vb);
    for (size_t i = 0; i < count; i++) {
        float tx = float((i & 1) * size), ty = float((i >> 1) * size);
        D3D12_VIEWPORT v{tx, ty, float(size), float(size), 0, 1};
        D3D12_RECT r{LONG(tx), LONG(ty), LONG(tx) + size, LONG(ty) + size};
        SetViewport(v);
        SetScissor(r);
        std::memcpy(kc + 48, lvps[i], sizeof(lvps[i]));
        Alloc cb = Upload(512, 256);
        std::memcpy(cb.cpu, kc, sizeof(kc));
        cl_->SetGraphicsRootConstantBufferView(1, cb.gpu);
        for (const auto& run : pb.solid) {
            if (run.first >= kSoVerts) break;
            cl_->DrawInstanced(std::min(run.second, kSoVerts - run.first), 1, run.first, 0);
        }
    }
    bound_rt_ = bound_ds_ = nullptr;
    Transition(lamp_map_.Get(), lamp_map_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // the receivers, a pass a lamp
    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_lamp_pre_%s", cap_i_, why);
        CapSave(*rt, name);
    }
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* srcs[3] = {ds->res.Get(), lamp_map_.Get(), nullptr};
    DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32_FLOAT, kBgra};
    float sc = float(rt->w) / float(rt->gw);
    for (size_t i = 0; i < count; i++) {
        float k[kLampReceiverConstants] = {};
        std::memcpy(k, cur.inv, sizeof(cur.inv));
        std::memcpy(k + 16, lvps[i], sizeof(lvps[i]));
        k[32] = lpos[i][0], k[33] = lpos[i][1], k[34] = lpos[i][2], k[35] = q.lamp_radius;
        k[36] = float(s.viewport.x) * sc, k[37] = float(s.viewport.y) * sc, k[38] = float(s.viewport.w) * sc,
        k[39] = float(s.viewport.h) * sc;
        k[40] = cur.minz, k[41] = cur.maxz, k[42] = float(size), k[43] = q.lamp_strength;
        k[44] = q.lamp_bias, k[45] = q.lamp_slope, k[46] = float(q.pcf), k[47] = N;
        k[48] = float((i & 1) * size), k[49] = float((i >> 1) * size), k[50] = F, k[51] = float(q.debug);
        PassN(kLampReceiverHlsl, "psmain", rt->fmt, View(*rt), rt->w, rt->h, srcs, fmts, k, kLampReceiverConstants,
              kPassDarken);
    }
    if (stamp) {
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 13);
        time_lamp_[slot] = true;
    }
    rt->drawn = true;
    lamp_frames_++;
    lamp_used_ += count;
    if (cap_) {
        fprintf(cap_, "%04u lampshadow %s lamps %zu of %zu cached:", cap_i_, why, count, lamps_.size());
        for (size_t i = 0; i < count; i++)
            fprintf(cap_, " %.1f,%.1f,%.1f (%.0f m, %u hits)", nearest[i].second->p[0], nearest[i].second->p[1],
                    nearest[i].second->p[2], std::sqrt(nearest[i].first), nearest[i].second->hits);
        fprintf(cap_, "\n");
        char name[48];
        snprintf(name, sizeof(name), "%04u_lamp_%s", cap_i_++, why);
        CapSave(*rt, name);
    }
    if (!lamp_logged_) {
        lamp_logged_ = true;
        LOGR("MC3RENDER light-shadow: first lamp pass, %zu lamps of %zu cached, nearest at %.1f,%.1f,%.1f", count,
             lamps_.size(), nearest[0].second->p[0], nearest[0].second->p[1], nearest[0].second->p[2]);
    }
}

// a car the title draws: its vertex program's c100-c103 are the world x view x projection columns
// (oPos = dot(position, c100..c103)); with this draw's view x projection taken back out, the world matrix is left
void D3D12Backend::CollectDrawnCar(const State& s, const Assembled& a) {
    if (cars_frame_ != presents_) {
        cars_prev_.swap(cars_now_);
        cars_now_.clear();
        cars_frame_ = presents_;
    }
    if (cars_now_.size() >= 32) return;
    const float* view = s.transforms[2].data();
    const float* proj = s.transforms[3].data();
    float vp[16], inv[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            vp[r * 4 + c] = view[r * 4] * proj[c] + view[r * 4 + 1] * proj[4 + c] + view[r * 4 + 2] * proj[8 + c] +
                            view[r * 4 + 3] * proj[12 + c];
    if (!Invert4(vp, inv)) return;
    float wvp[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) wvp[r * 4 + c] = s.vsc[size_t(100 + c)][size_t(r)];
    float w[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            w[r * 4 + c] = wvp[r * 4] * inv[c] + wvp[r * 4 + 1] * inv[4 + c] + wvp[r * 4 + 2] * inv[8 + c] +
                           wvp[r * 4 + 3] * inv[12 + c];
    for (float v : w)
        if (!std::isfinite(v)) return;
    // a rigid placement: rows of about unit length, the last column 0 0 0 1
    for (int r = 0; r < 3; r++) {
        float n = std::sqrt(w[r * 4] * w[r * 4] + w[r * 4 + 1] * w[r * 4 + 1] + w[r * 4 + 2] * w[r * 4 + 2]);
        if (n < 0.5f || n > 2.0f) return;
    }
    if (std::fabs(w[3]) > 0.01f || std::fabs(w[7]) > 0.01f || std::fabs(w[11]) > 0.01f || std::fabs(w[15] - 1) > 0.01f) return;
    DrawnCar d;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) d.m[r * 3 + c] = w[r * 4 + c];
    d.m[9] = w[12], d.m[10] = w[13], d.m[11] = w[14];
    d.span = 0;
    // how wide the part is, from up to 64 of its vertices in its own space (a pedestrian is half a metre across)
    {
        const VertexElement& e = a.elements[0];
        uint32_t n = a.vertex_count;
        size_t regs = a.vertices.empty() || !n ? 0 : a.vertices.size() / (size_t(n) * 4);
        bool raw = a.vertices.empty() && (e.format == DXGI_FORMAT_R32G32B32_FLOAT || e.format == DXGI_FORMAT_R32G32B32A32_FLOAT);
        if (n && (regs || raw)) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            uint32_t step = std::max<uint32_t>(1, n / 64);
            for (uint32_t v = 0; v < n; v += step) {
                float f[3];
                if (regs) {
                    std::memcpy(f, &a.vertices[(size_t(v) * regs + e.offset / 16) * 4], sizeof(f));
                } else {
                    const VertexBuffer& vb = a.buffers[e.buffer];
                    uint64_t at = uint64_t(v) * vb.stride + e.offset;
                    if (!vb.src || at + 12 > vb.bytes) break;
                    std::memcpy(f, vb.src + at, sizeof(f));
                }
                for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], f[i]), hi[i] = std::max(hi[i], f[i]);
            }
            if (hi[0] >= lo[0]) d.span = std::max(hi[0] - lo[0], hi[2] - lo[2]);
            if (!std::isfinite(d.span) || d.span > 50) d.span = 0;
        }
    }
    beam_cars_seen_++;
    // a car's parts come with matrices of their own (wheels spinning, panels a metre above or behind): one car within
    // 3 m, kept at its most upright matrix (the body's), then at the lowest (the car's own sits on its contact plane)
    auto upright = [](const float* m) {
        float n = std::sqrt(m[3] * m[3] + m[4] * m[4] + m[5] * m[5]);
        return n > 1e-4f ? m[4] / n : 0.0f;
    };
    if (upright(d.m) < 0.7f) return;
    for (auto& c : cars_now_) {
        float dx = c.m[9] - w[12], dz = c.m[11] - w[14];
        if (dx * dx + dz * dz < 9.0f && std::fabs(c.m[10] - w[13]) < 3.0f) {
            float a = upright(d.m), b = upright(c.m);
            float span = std::max(c.span, d.span);
            if (a > b + 0.01f || (std::fabs(a - b) <= 0.01f && w[13] < c.m[10])) c = d;
            c.span = span;
            return;
        }
    }
    cars_now_.push_back(d);
}

// the headlight beams of the cars nearest the camera (the previous frame's cars, as its casters are): each into a
// perspective depth map from its front bumper (a 2 x 2 atlas), then its light multiplied into the scene where it falls
void D3D12Backend::Beams(const State& s, const char* why) {
    const render::Lights& q = render::Get().lights;
    if (cars_frame_ != presents_) {  // no car body drawn yet this frame: the list in hand is the last frame's
        cars_prev_.swap(cars_now_);
        cars_now_.clear();
        cars_frame_ = presents_;
    }
    SoBuffer& pb = so_[(presents_ & 1) ^ 1];
    SoBuffer& cur = so_[presents_ & 1];
    if (cars_prev_.empty() || !pb.buf || pb.frame + 1 != presents_ || pb.inv_frame != pb.frame || !pb.valid ||
        pb.solid.empty() || !cur.valid || cur.inv_frame != presents_)
        return;
    const float* view = s.transforms[2].data();
    float eye[3];
    for (int j = 0; j < 3; j++) eye[j] = -(view[12] * view[j * 4] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    // which cars have their lights on: the player's, and a car that moved or had a glow at its front lately (a parked
    // car neither moves nor glows)
    constexpr uint64_t kLitFrames = 300;
    constexpr float kCarSpan = 2.0f;  // a car's body is longer than this; a pedestrian or a sign is not
    constexpr float kEase = 0.5f;      // the share of the miss (drawn less predicted place) the filtered place takes
    constexpr float kEaseV = 0.25f;    // and the speed
    constexpr float kEaseAxes = 0.5f;  // the share of a frame's axes the filtered ones take
    constexpr float kStillRadius = 2.0f;
    constexpr uint32_t kStillFrames = 60;
    std::vector<int> track_of(cars_prev_.size(), -1);
    std::vector<bool> lit(cars_prev_.size(), false);
    for (size_t i = 0; i < cars_prev_.size(); i++) {
        const float* m = cars_prev_[i].m;
        // the track whose car would be here now, at the speed it had; one car a track a frame
        int ti = -1;
        float best = 2.5f * 2.5f;
        for (size_t k = 0; k < tracks_.size(); k++) {
            const Track& tr = tracks_[k];
            if (tr.seen == presents_) continue;
            float dt = float(presents_ - tr.seen);
            float dx = tr.p[0] + tr.v[0] * dt - m[9], dy = tr.p[1] + tr.v[1] * dt - m[10],
                  dz = tr.p[2] + tr.v[2] * dt - m[11];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) best = d2, ti = int(k);
        }
        if (ti >= 0) {
            float dx = tracks_[size_t(ti)].p[0] - m[9], dz = tracks_[size_t(ti)].p[2] - m[11];
            if (dx * dx + dz * dz > 0.03f * 0.03f) tracks_[size_t(ti)].moved = presents_;
        } else if (tracks_.size() < 64) {
            Track nt{};
            std::memcpy(nt.p, &m[9], sizeof(nt.p));
            nt.seen = presents_;
            tracks_.push_back(nt);
            ti = int(tracks_.size() - 1);
        }
        if (ti < 0) continue;
        Track& t = tracks_[size_t(ti)];
        track_of[i] = ti;
        float dt = std::max(1.0f, float(presents_ - t.seen));
        std::memcpy(t.p, &m[9], sizeof(t.p));
        t.seen = presents_;
        t.span = std::max(t.span, cars_prev_[i].span);
        // MC3's cutscene cars can remain physically parked while the chosen paint matrix wanders by a few tenths of
        // a metre. Velocity prediction turns that part-selection noise into a visibly trembling light pool. Once a
        // track stays inside a car-sized stationary window for 60 frames, hold its averaged pose until it actually
        // leaves the window. A moving car crosses the window before it can lock.
        float still_dx = m[9] - t.still_origin[0], still_dy = m[10] - t.still_origin[1],
              still_dz = m[11] - t.still_origin[2];
        if (!t.still_samples || still_dx * still_dx + still_dz * still_dz > kStillRadius * kStillRadius ||
            std::fabs(still_dy) > kStillRadius) {
            std::memcpy(t.still_origin, &m[9], sizeof(t.still_origin));
            std::memcpy(t.still_sum, m, sizeof(t.still_sum));
            t.still_samples = 1;
            t.still_locked = false;
        } else if (!t.still_locked) {
            for (int j = 0; j < 12; j++) t.still_sum[j] += m[j];
            if (++t.still_samples >= kStillFrames) {
                t.still_locked = true;
                for (int j = 0; j < 12; j++) t.sm[j] = t.still_sum[j] / float(t.still_samples);
                t.v[0] = t.v[1] = t.v[2] = 0;
                t.has_sm = t.has_v = true;
            }
        }
        // the filtered placement: predicted at the car's speed and pulled towards what was drawn, so a frame's jitter
        // does not swing the beam and a racer's does not trail it
        if (t.still_locked) {
            // The held matrix and zero velocity are already in sm/v.
        } else if (!t.has_sm) {
            std::memcpy(t.sm, m, sizeof(t.sm));
            t.has_sm = true;
        } else if (!t.has_v) {
            for (int j = 0; j < 3; j++) t.v[j] = (m[9 + j] - t.sm[9 + j]) / dt;
            std::memcpy(t.sm, m, sizeof(t.sm));
            t.has_v = true;
        } else {
            float r[3], r2 = 0;
            for (int j = 0; j < 3; j++) {
                r[j] = m[9 + j] - (t.sm[9 + j] + t.v[j] * dt);
                r2 += r[j] * r[j];
            }
            beam_fits_++;
            beam_miss_ += std::sqrt(r2);
            if (r2 > 4.0f * 4.0f) {  // a jump (a respawn, a crash) is taken at once
                beam_snaps_++;
                std::memcpy(t.sm, m, sizeof(t.sm));
                t.v[0] = t.v[1] = t.v[2] = 0;
                t.has_v = false;
            } else {
                for (int j = 0; j < 3; j++) {
                    t.sm[9 + j] += t.v[j] * dt + r[j] * kEase;
                    t.v[j] += r[j] * kEaseV / dt;
                }
                for (int j = 0; j < 9; j++) t.sm[j] += (m[j] - t.sm[j]) * kEaseAxes;
            }
        }
        float n = std::sqrt(m[6] * m[6] + m[7] * m[7] + m[8] * m[8]);
        if (n < 1e-4f) n = 1;
        float front[3] = {m[9] - m[6] / n * 2.0f, m[10] + 0.6f, m[11] - m[8] / n * 2.0f};
        for (const auto* list : {&glows_prev_, &glows_now_})
            for (const auto& g : *list) {
                float gx = g[0] - front[0], gy = g[1] - front[1], gz = g[2] - front[2];
                if (gx * gx + gy * gy + gz * gz < 3.0f * 3.0f) t.glowed = presents_;
            }
        bool player = shadow_have_ && std::fabs(m[9] - shadow_last_[0]) < 2.5f && std::fabs(m[11] - shadow_last_[2]) < 2.5f;
        bool car_sized = player || t.span >= kCarSpan;
        lit[i] = car_sized && (player || (t.moved && presents_ - t.moved < kLitFrames) ||
                               (t.glowed && presents_ - t.glowed < kLitFrames));
    }
    std::vector<std::pair<float, int>> order;
    for (size_t ci = 0; ci < cars_prev_.size(); ci++) {
        if (!lit[ci] || track_of[ci] < 0) {
            beam_unlit_++;
            continue;
        }
        const Track& t = tracks_[size_t(track_of[ci])];
        float dx = t.sm[9] - eye[0], dy = t.sm[10] - eye[1], dz = t.sm[11] - eye[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        float reach = q.beam_range + 25;
        if (d2 >= reach * reach) continue;
        // a car that had a beam last frame keeps it against one up to a fifth nearer
        if (t.chosen + 1 == presents_) d2 *= 0.64f;
        order.push_back({d2, int(ci)});
    }
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    // A long car can yield more than one paint matrix (body, doors, bumper). Those parts can be over 3 m apart at
    // collection time and acquire separate tracks, yet converge on nearly the same headlight position. In the intro
    // capture this spent three of four slots in one Hummer-sized footprint and doubled the light on the road. Keep
    // the nearest track from each car-sized footprint, while preserving the neighbouring lane (3.5 m away there).
    std::vector<std::pair<float, int>> distinct;
    distinct.reserve(order.size());
    for (const auto& candidate : order) {
        const Track& a = tracks_[size_t(track_of[size_t(candidate.second)])];
        bool duplicate = false;
        for (auto& selected : distinct) {
            const Track& b = tracks_[size_t(track_of[size_t(selected.second)])];
            float dx = a.sm[9] - b.sm[9], dz = a.sm[11] - b.sm[11];
            float an = std::sqrt(a.sm[6] * a.sm[6] + a.sm[7] * a.sm[7] + a.sm[8] * a.sm[8]);
            float bn = std::sqrt(b.sm[6] * b.sm[6] + b.sm[7] * b.sm[7] + b.sm[8] * b.sm[8]);
            float facing = (a.sm[6] * b.sm[6] + a.sm[7] * b.sm[7] + a.sm[8] * b.sm[8]) /
                           std::max(an * bn, 1e-6f);
            if (dx * dx + dz * dz < 2.5f * 2.5f && std::fabs(a.sm[10] - b.sm[10]) < 1.5f && facing > 0.7f) {
                // The body matrix is lower than roof/bumper parts; a newly nearer part must not displace it.
                if ((a.still_locked && !b.still_locked) ||
                    (a.still_locked == b.still_locked && a.sm[10] < b.sm[10] - 0.15f))
                    selected = candidate;
                duplicate = true;
                break;
            }
        }
        if (!duplicate) distinct.push_back(candidate);
    }
    size_t overlapping_tracks = order.size() - distinct.size();
    order.swap(distinct);
    std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    size_t count = std::min<size_t>(order.size(), size_t(std::min(q.cars, 4)));
    Target* rt = FindTarget(s.rt, false);
    Target* ds = s.zs.data ? FindTarget(s.zs, true) : nullptr;
    int size = q.beam_size;
    if (!rt || !ds || ds->w != rt->w || ds->h != rt->h || rt->range != 1 ||
        !EnsureShadowMap(beam_map_, beam_map_state_, beam_dsv_, size * 2, size * 2) ||
        !EnsureCaster(kSunCasterHlsl, sun_caster_pso_, sun_caster_failed_, "light-shadow"))
        return;
    auto norm = [](float* v) {
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n > 0) v[0] /= n, v[1] /= n, v[2] /= n;
    };
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const float* a, const float* b, float* o) {
        o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
    };
    constexpr float kPi = 3.14159265f;
    float sx = 1 / std::tan(q.beam_fov * kPi / 360);
    float N = 0.3f, F = q.beam_range + 5, A = F / (F - N), B = -N * F / (F - N);
    float pr = q.beam_pitch * kPi / 180;
    float lvps[4][16], lpos[4][3], ldir[4][3];
    bool beam_stationary[4] = {};
    bool beam_anchored[4] = {};
    bool host_used[State::kHostCars] = {};
    size_t host_count = std::isfinite(s.host[0]) && s.host[0] > 0
                            ? std::min<size_t>(size_t(s.host[0]), State::kHostCars)
                            : 0;
    for (size_t i = 0; i < count; i++) {
        Track& tr = tracks_[size_t(track_of[size_t(order[i].second)])];
        tr.chosen = presents_;
        const float* m = tr.sm;
        beam_stationary[i] = tr.still_locked;
        // The title also exposes each live vehCar Matrix34. Match this drawn
        // car to that authoritative matrix and use its model's skeleton cone
        // anchor. It is rigidly attached to the car, including yaw/roll/pitch;
        // the tracked paint matrix remains the fallback for unrecognized cars.
        const float* anchor = nullptr;
        float best_host = 4.0f * 4.0f;
        size_t best_index = host_count;
        for (size_t h = 0; h < host_count; h++) {
            if (host_used[h]) continue;
            const float* hm = s.host.data() + 4 + h * State::kHostCarStride;
            if (hm[15] < 0.5f) continue;
            float dx = hm[9] - tr.p[0], dy = hm[10] - tr.p[1], dz = hm[11] - tr.p[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            float facing = hm[6] * m[6] + hm[7] * m[7] + hm[8] * m[8];
            if (d2 < best_host && facing > 0.5f) best_host = d2, best_index = h;
        }
        if (best_index < host_count) {
            host_used[best_index] = true;
            m = s.host.data() + 4 + best_index * State::kHostCarStride;
            anchor = m + 12;
            beam_anchored[i] = true;
        }
        float up_c[3] = {m[3], m[4], m[5]}, fwd[3] = {-m[6], -m[7], -m[8]};
        norm(up_c);
        norm(fwd);
        float dir[3], pos[3];
        for (int j = 0; j < 3; j++) {
            dir[j] = fwd[j] * std::cos(pr) + up_c[j] * std::sin(pr);
            pos[j] = anchor ? m[9 + j] + anchor[0] * m[j] + anchor[1] * m[3 + j] + anchor[2] * m[6 + j]
                            : m[9 + j] + tr.v[j] + fwd[j] * q.beam_fwd + up_c[j] * q.beam_height;
        }
        norm(dir);
        float up0[3] = {0, 1, 0};
        if (std::fabs(dir[1]) > 0.99f) up0[0] = 0, up0[1] = 0, up0[2] = 1;
        float right[3], up[3];
        cross(up0, dir, right);
        norm(right);
        cross(dir, right, up);
        float lm[16] = {sx * right[0], sx * up[0], A * dir[0], dir[0], sx * right[1], sx * up[1], A * dir[1], dir[1],
                        sx * right[2], sx * up[2], A * dir[2], dir[2],
                        -sx * dot3(pos, right), -sx * dot3(pos, up), -A * dot3(pos, dir) + B, -dot3(pos, dir)};
        std::memcpy(lvps[i], lm, sizeof(lm));
        std::memcpy(lpos[i], pos, sizeof(pos));
        std::memcpy(ldir[i], dir, sizeof(dir));
    }
    // casters: the previous frame's scene as it was drawn, the cars in it where they were, each beam's own car left out
    // by its box at the place it was drawn (moving, its body last frame would stand in front of its own lamps)
    const render::VShadow& vq = render::Get().vshadow;
    auto axes = [](const float* m, float* out) {
        for (int r = 0; r < 3; r++) {
            float n = std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
            if (!(n > 1e-4f)) n = 1;
            for (int c = 0; c < 3; c++) out[r * 4 + c] = m[r * 3 + c] / n;
            out[r * 4 + 3] = 0;
        }
        out[12] = m[9], out[13] = m[10], out[14] = m[11], out[15] = 0;
    };
    float kc[kSunCasterConstants] = {};
    std::memcpy(kc, pb.inv, sizeof(pb.inv));
    kc[64] = pb.minz, kc[65] = pb.maxz, kc[66] = 2;
    Transition(pb.buf.Get(), pb.state, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(beam_map_.Get(), beam_map_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), beam_dsv_, dsv_inc_);
    cl_->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetPipeline(sun_caster_pso_.Get());
    SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{pb.buf->GetGPUVirtualAddress(), kSoVerts * 16, 16};
    cl_->IASetVertexBuffers(0, 1, &vb);
    for (size_t i = 0; i < count; i++) {
        float tx = float((i & 1) * size), ty = float((i >> 1) * size);
        D3D12_VIEWPORT v{tx, ty, float(size), float(size), 0, 1};
        D3D12_RECT r{LONG(tx), LONG(ty), LONG(tx) + size, LONG(ty) + size};
        SetViewport(v);
        SetScissor(r);
        std::memcpy(kc + 48, lvps[i], sizeof(lvps[i]));
        size_t ci = size_t(order[i].second);
        axes(cars_prev_[ci].m, kc + 16);
        float half_l = std::max(vq.boxl, tracks_[size_t(track_of[ci])].span * 0.5f + 0.3f);  // a bus is longer
        kc[68] = vq.boxw + 0.15f, kc[69] = vq.boxt + 0.6f, kc[70] = std::min(half_l, 9.0f), kc[71] = -0.5f;
        Alloc cb = Upload(512, 256);
        std::memcpy(cb.cpu, kc, sizeof(kc));
        cl_->SetGraphicsRootConstantBufferView(1, cb.gpu);
        for (const auto& run : pb.solid) {
            if (run.first >= kSoVerts) break;
            cl_->DrawInstanced(std::min(run.second, kSoVerts - run.first), 1, run.first, 0);
        }
    }
    bound_rt_ = bound_ds_ = nullptr;
    Transition(beam_map_.Get(), beam_map_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_beam_pre_%s", cap_i_, why);
        CapSave(*rt, name);
    }
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* srcs[3] = {ds->res.Get(), beam_map_.Get(), nullptr};
    DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R32_FLOAT, kBgra};
    float sc = float(rt->w) / float(rt->gw);
    float half = q.beam_fov * kPi / 360;
    for (size_t i = 0; i < count; i++) {
        float k[kBeamConstants] = {};
        std::memcpy(k, cur.inv, sizeof(cur.inv));
        std::memcpy(k + 16, lvps[i], sizeof(lvps[i]));
        k[32] = lpos[i][0], k[33] = lpos[i][1], k[34] = lpos[i][2], k[35] = q.beam_range;
        k[36] = ldir[i][0], k[37] = ldir[i][1], k[38] = ldir[i][2], k[39] = std::cos(half);
        k[40] = q.beam_r, k[41] = q.beam_g, k[42] = q.beam_b, k[43] = q.beam_strength;
        k[44] = float(s.viewport.x) * sc, k[45] = float(s.viewport.y) * sc, k[46] = float(s.viewport.w) * sc,
        k[47] = float(s.viewport.h) * sc;
        k[48] = cur.minz, k[49] = cur.maxz, k[50] = float(size), k[51] = std::cos(half * q.beam_inner);
        k[52] = q.beam_bias, k[53] = q.beam_slope, k[54] = float(q.pcf), k[55] = N;
        k[56] = float((i & 1) * size), k[57] = float((i >> 1) * size), k[58] = F, k[59] = float(q.beam_debug);
        PassN(kBeamHlsl, "psmain", rt->fmt, View(*rt), rt->w, rt->h, srcs, fmts, k, kBeamConstants, kPassLight);
    }
    rt->drawn = true;
    beam_frames_++;
    beam_used_ += count;
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&](const Track& tr) { return presents_ - tr.seen > 120; }),
                  tracks_.end());
    if (cap_) {
        fprintf(cap_, "%04u beams %s %zu of %zu cars:", cap_i_, why, count, cars_prev_.size());
        size_t stationary = 0, anchored = 0;
        for (size_t i = 0; i < count; i++) stationary += beam_stationary[i], anchored += beam_anchored[i];
        for (size_t i = 0; i < count; i++)
            fprintf(cap_, " %.1f,%.1f,%.1f looking %.2f,%.2f,%.2f (%.0f m)", lpos[i][0], lpos[i][1], lpos[i][2], ldir[i][0],
                    ldir[i][1], ldir[i][2], std::sqrt(order[i].first));
        fprintf(cap_, " overlaps=%zu stationary=%zu anchored=%zu\n", overlapping_tracks, stationary, anchored);
        char name[48];
        snprintf(name, sizeof(name), "%04u_beam_%s", cap_i_++, why);
        CapSave(*rt, name);
    }
    if (!beam_logged_) {
        beam_logged_ = true;
        LOGR("MC3RENDER light-shadow: first beams, %zu of %zu drawn cars (%zu model anchored), nearest at %.1f,%.1f,%.1f looking %.2f,%.2f,%.2f",
             count, cars_prev_.size(), size_t(std::count(beam_anchored, beam_anchored + count, true)),
             lpos[0][0], lpos[0][1], lpos[0][2], ldir[0][0], ldir[0][1], ldir[0][2]);
    }
}

void D3D12Backend::ShadowApply(const State& s, const char* why) {
    shadow_done_ = presents_;
    if (s.host[0] >= 1) d3d_cars_seen_++;
    const render::Shadow& q = render::Get().shadow;
    // the main camera from the scene's view (rows of its rotation are the camera's axes)
    const float* view = s.transforms[2].data();
    float eye[3];
    for (int j = 0; j < 3; j++) eye[j] = -(view[12] * view[j * 4] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
    // this frame's camera, for the next frame's casters (vehicle_shadow.h, sun_shadow.h)
    SoBuffer& cur = so_[presents_ & 1];
    {
        const float* cp = s.transforms[3].data();
        float cvp[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                cvp[r * 4 + c] = view[r * 4] * cp[c] + view[r * 4 + 1] * cp[4 + c] + view[r * 4 + 2] * cp[8 + c] +
                                 view[r * 4 + 3] * cp[12 + c];
        cur.valid = Invert4(cvp, cur.inv);
        cur.inv_frame = presents_;
        cur.minz = s.viewport.min_z;
        cur.maxz = s.viewport.max_z;
    }
    // metres: a chase camera; the most a car moves between two frames; a kept car this far from the camera while
    // another is within kClose for kStrays frames was picked during an intro camera, and the close one takes over
    constexpr float kChase = 30, kStep = 6, kStray = 20, kClose = 12;
    constexpr uint32_t kStrays = 60;
    size_t cars = std::min<size_t>(size_t(s.host[0]), State::kHostCars);
    auto d2 = [](const float* m, const float* ref) {
        float ax = m[9] - ref[0], ay = m[10] - ref[1], az = m[11] - ref[2];
        return ax * ax + ay * ay + az * az;
    };
    const float* car = nullptr;
    const float* near_cam = nullptr;
    float best = 1e30f, cam_best = 1e30f;
    for (size_t i = 0; i < cars; i++) {
        const float* m = s.host.data() + 4 + i * State::kHostCarStride;
        float d = d2(m, shadow_have_ ? shadow_last_ : eye);
        if (d < best) best = d, car = m;
        if (d2(m, eye) < cam_best) cam_best = d2(m, eye), near_cam = m;
    }
    if (shadow_have_ && car && near_cam != car && d2(car, eye) > kStray * kStray && cam_best < kClose * kClose) {
        if (++shadow_strays_ >= kStrays) {
            LOGR("MC3RENDER contact-shadow: the kept car stayed %.1f m from the camera, switching to the one %.1f m away",
                 std::sqrt(d2(car, eye)), std::sqrt(cam_best));
            car = near_cam;
            best = 0;
            shadow_strays_ = 0;
        }
    } else {
        shadow_strays_ = 0;
    }
    float limit = shadow_have_ ? kStep : kChase;
    if (car && best > limit * limit) {
        // lost (a respawn, a reset): pick again by the camera next frame
        car = nullptr;
        shadow_have_ = false;
    }
    if (!car) {
        if (render::On(render::kSunShadow)) SunShadow(s, nullptr, eye, why);
        shadow_no_car_++;
        return;
    }
    if (!shadow_have_)
        LOGR("MC3RENDER contact-shadow: player car at %.1f,%.1f,%.1f, %.1f m from the camera", car[9], car[10], car[11],
             std::sqrt(best));
    shadow_have_ = true;
    for (int i = 0; i < 3; i++) shadow_last_[i] = car[9 + i];
    // the fade with the camera's distance to the car
    float dx = car[9] - eye[0], dy = car[10] - eye[1], dz = car[11] - eye[2];
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    std::memcpy(cur.car, car, sizeof(cur.car));
    cur.car_frame = presents_;
    if (render::On(render::kSunShadow)) SunShadow(s, car, eye, why);
    if (render::On(render::kLightShadow)) {
        if (!render::Get().lights.cars || render::Get().lights.beam_cone) HeadlightShadow(s, car, why);
        if (render::Get().lights.lamps > 0) LampShadows(s, car, why);
        if (render::Get().lights.cars > 0) Beams(s, why);
    }
    if (render::On(render::kVehicleShadow)) VehicleShadow(s, car, eye, dist, why);
    if (!render::On(render::kContactShadow)) return;
    float strength = q.strength * std::clamp(1.0f - (dist - q.near_m) / (q.far_m - q.near_m), 0.0f, 1.0f);
    if (strength <= 0.001f && !q.debug) {
        shadow_far_++;
        return;
    }
    Target* rt = FindTarget(s.rt, false);
    Target* ds = s.zs.data ? FindTarget(s.zs, true) : nullptr;
    if (!rt || !ds || ds->w != rt->w || ds->h != rt->h || rt->range != 1) {
        shadow_no_depth_++;
        return;
    }
    float vp[16], inv[16];
    const float* proj = s.transforms[3].data();
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            vp[r * 4 + c] = view[r * 4] * proj[c] + view[r * 4 + 1] * proj[4 + c] + view[r * 4 + 2] * proj[8 + c] +
                            view[r * 4 + 3] * proj[12 + c];
    if (!Invert4(vp, inv)) {
        shadow_no_depth_++;
        return;
    }
    float sc = float(rt->w) / float(rt->gw);
    float k[kShadowConstants] = {};
    std::memcpy(k, inv, sizeof(inv));
    for (int r = 0; r < 3; r++) {
        // the car's axes, normalised: its matrix may carry the model's scale
        float n = std::sqrt(car[r * 3] * car[r * 3] + car[r * 3 + 1] * car[r * 3 + 1] + car[r * 3 + 2] * car[r * 3 + 2]);
        if (!(n > 1e-4f)) n = 1;
        for (int c = 0; c < 3; c++) k[16 + r * 4 + c] = car[r * 3 + c] / n;
    }
    k[28] = car[9], k[29] = car[10], k[30] = car[11], k[31] = strength;
    k[32] = float(s.viewport.x) * sc, k[33] = float(s.viewport.y) * sc, k[34] = float(s.viewport.w) * sc,
    k[35] = float(s.viewport.h) * sc;
    k[36] = s.viewport.min_z, k[37] = s.viewport.max_z, k[38] = float(q.debug);
    k[40] = q.width, k[41] = q.length, k[42] = q.ground, k[43] = q.top;
    k[44] = q.fade, k[45] = q.soft;
    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_shadow_pre_%s", cap_i_, why);
        CapSave(*rt, name);
    }
    // the depth is read, not tested: the pass binds the colour target alone
    Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* srcs[3] = {ds->res.Get(), nullptr, nullptr};
    DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kBgra, kBgra};
    uint32_t slot = uint32_t(frame_ % kFrames);
    bool stamp = time_heap_ && time_open_[slot] && !time_shadow_[slot];
    if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 4);
    PassN(kShadowHlsl, "psmain", rt->fmt, View(*rt), rt->w, rt->h, srcs, fmts, k, kShadowConstants, kPassDarken);
    if (stamp) {
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 5);
        time_shadow_[slot] = true;
    }
    rt->drawn = true;
    shadow_frames_++;
    if (cap_) {
        // where the car's origin and the tip of each axis (1 m) land on screen, in guest pixels
        auto screen = [&](float x, float y, float z, float* out) {
            float c[4];
            for (int i = 0; i < 4; i++) c[i] = x * vp[i] + y * vp[4 + i] + z * vp[8 + i] + vp[12 + i];
            if (c[3] <= 0) c[3] = 1e-6f;
            out[0] = float(s.viewport.x) + (c[0] / c[3] * 0.5f + 0.5f) * float(s.viewport.w);
            out[1] = float(s.viewport.y) + (0.5f - c[1] / c[3] * 0.5f) * float(s.viewport.h);
        };
        float o[2], ax[2], ay[2], az[2];
        screen(car[9], car[10], car[11], o);
        screen(car[9] + car[0], car[10] + car[1], car[11] + car[2], ax);
        screen(car[9] + car[3], car[10] + car[4], car[11] + car[5], ay);
        screen(car[9] + car[6], car[10] + car[7], car[11] + car[8], az);
        fprintf(cap_, "%04u shadow %s rt %08X car %.2f,%.2f,%.2f camera %.1f m strength %.3f %s screen o=%.0f,%.0f "
                      "x=%.0f,%.0f y=%.0f,%.0f z=%.0f,%.0f\n",
                cap_i_, why, rt->data, car[9], car[10], car[11], dist, strength, render::ShadowNames(q).c_str(), o[0], o[1],
                ax[0], ax[1], ay[0], ay[1], az[0], az[1]);
        char name[48];
        snprintf(name, sizeof(name), "%04u_shadow_%s", cap_i_++, why);
        CapSave(*rt, name);
    }
    if (!shadow_logged_) {
        shadow_logged_ = true;
        LOGR("MC3RENDER contact-shadow: first car x=%.2f,%.2f,%.2f y=%.2f,%.2f,%.2f z=%.2f,%.2f,%.2f at %.1f,%.1f,%.1f, "
             "camera %.1f m, depth %u x %u, viewport z %.3f..%.3f",
             car[0], car[1], car[2], car[3], car[4], car[5], car[6], car[7], car[8], car[9], car[10], car[11], dist, ds->w,
             ds->h, s.viewport.min_z, s.viewport.max_z);
    }
}

void D3D12Backend::PostAtPresent(const Surface& front) {
    if (post_done_ != presents_ && post_scene_ && post_scene_ == front.data) {
        if (Target* t = FindTarget(front, false)) {
            post_at_present_++;
            PostApply(*t, "present");
        }
    }
    if (!post_scene_) blur_previous_valid_ = false;
    post_scene_ = 0;
    blur_scene_valid_ = false;
    if (render::On(render::kPost) && presents_ % 600 == 599) {
        LOGR("MC3RENDER post: %llu of the last 600 frames had the pass (%llu at present, %llu skipped for range)",
             (unsigned long long)post_frames_, (unsigned long long)post_at_present_,
             (unsigned long long)post_skipped_range_);
        post_frames_ = post_at_present_ = post_skipped_range_ = 0;
    }
    if (render::On(render::kMotionBlur) && presents_ % 600 == 599) {
        LOGR("MC3RENDER motion-blur: %llu of the last 600 frames (no depth %llu, MC3 composites hidden %llu)",
             (unsigned long long)blur_frames_, (unsigned long long)blur_no_depth_,
             (unsigned long long)mc3_blur_composites_hidden_);
        blur_frames_ = blur_no_depth_ = mc3_blur_composites_hidden_ = 0;
    }
}

void D3D12Backend::MotionBlurApply(Target& t, const char* why) {
    if (!render::MotionBlurTuneReady()) { blur_previous_valid_ = false; return; }
    if (!blur_scene_valid_ || !blur_depth_.data) { blur_no_depth_++; return; }
    Target* ds = FindTarget(blur_depth_, true);
    if (!ds || !ds->res || ds->w != t.w || ds->h != t.h) { blur_no_depth_++; return; }
    float vp[16], inv[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            vp[r * 4 + c] = blur_view_[r * 4] * blur_proj_[c] + blur_view_[r * 4 + 1] * blur_proj_[4 + c] +
                            blur_view_[r * 4 + 2] * blur_proj_[8 + c] + blur_view_[r * 4 + 3] * blur_proj_[12 + c];
    if (!Invert4(vp, inv)) { blur_previous_valid_ = false; return; }
    bool consecutive = blur_previous_valid_ && blur_previous_frame_ + 1 == presents_ &&
                       blur_previous_w_ == t.w && blur_previous_h_ == t.h;
    if (consecutive) {
        if (cap_) {
            char name[48];
            snprintf(name, sizeof(name), "%04u_blur_pre_%s", cap_i_, why);
            CapSave(t, name);
        }
        ID3D12Resource* src = Feedback(t);
        if (src) {
            uint32_t slot = uint32_t(frame_ % kFrames);
            bool stamp = time_heap_ && time_open_[slot] && !time_blur_[slot];
            if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 14);
            Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const render::Blur& q = render::Get().blur;
            float k[kMotionBlurConstants] = {};
            std::memcpy(k, inv, sizeof(inv));
            std::memcpy(k + 16, blur_previous_vp_, sizeof(blur_previous_vp_));
            float scale = float(t.w) / float(t.gw);
            k[32] = blur_viewport_.x * scale;
            k[33] = blur_viewport_.y * scale;
            k[34] = blur_viewport_.w * scale;
            k[35] = blur_viewport_.h * scale;
            k[36] = blur_viewport_.min_z;
            k[37] = blur_viewport_.max_z;
            k[40] = q.strength;
            k[41] = q.max_pixels;
            k[42] = float(q.samples);
            k[43] = q.split ? 1.0f : 0.0f;
            float view_inv[16] = {};
            float eye[3] = {};
            if (Invert4(blur_view_, view_inv)) {
                eye[0] = view_inv[12], eye[1] = view_inv[13], eye[2] = view_inv[14];
            }
            std::vector<std::pair<float, uint32_t>> near_cars;
            for (uint32_t i = 0; i < blur_current_count_; i++) {
                const float* m = blur_current_cars_[i];
                float dx = m[9] - eye[0], dy = m[10] - eye[1], dz = m[11] - eye[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < 2500.0f) near_cars.emplace_back(d2, i);
            }
            std::sort(near_cars.begin(), near_cars.end());
            bool old_used[State::kHostCars] = {};
            uint32_t pairs = 0;
            for (const auto& [distance, ci] : near_cars) {
                if (pairs == kMotionBlurCars) break;
                const float* cur = blur_current_cars_[ci];
                int best = -1;
                float best_d2 = 36.0f;
                for (uint32_t oi = 0; oi < blur_previous_count_; oi++) {
                    if (old_used[oi]) continue;
                    const float* old = blur_previous_cars_[oi];
                    float dx = cur[9] - old[9], dy = cur[10] - old[10], dz = cur[11] - old[11];
                    float d2 = dx * dx + dy * dy + dz * dz;
                    float facing = cur[6] * old[6] + cur[7] * old[7] + cur[8] * old[8];
                    if (d2 < best_d2 && facing > 0.5f) best = int(oi), best_d2 = d2;
                }
                if (best < 0) continue;
                old_used[best] = true;
                const float* old = blur_previous_cars_[best];
                float* pair = k + 44 + pairs * 32;
                for (int side = 0; side < 2; side++) {
                    const float* m = side ? old : cur;
                    float* row = pair + side * 16;
                    for (int axis = 0; axis < 3; axis++)
                        for (int c = 0; c < 3; c++) row[axis * 4 + c] = m[axis * 3 + c];
                    row[12] = m[9], row[13] = m[10], row[14] = m[11];
                }
                pairs++;
            }
            k[38] = float(pairs);
            ID3D12Resource* srcs[3] = {src, ds->res.Get(), nullptr};
            DXGI_FORMAT fmts[3] = {t.fmt, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kBgra};
            PassN(kMotionBlurHlsl, "psmain", t.fmt, View(t), t.w, t.h, srcs, fmts, k, kMotionBlurConstants, -1);
            if (stamp) {
                cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 15);
                time_blur_[slot] = true;
            }
            t.drawn = true;
            blur_frames_++;
            if (cap_) {
                fprintf(cap_, "%04u motion-blur %s rt %08X %ux%u cars %u %s\n", cap_i_, why, t.data, t.w, t.h,
                        pairs, render::BlurNames(q).c_str());
                char name[48];
                snprintf(name, sizeof(name), "%04u_blur_%s", cap_i_++, why);
                CapSave(t, name);
            }
        }
    }
    std::memcpy(blur_previous_vp_, vp, sizeof(vp));
    blur_previous_valid_ = true;
    blur_previous_frame_ = presents_;
    blur_previous_w_ = t.w;
    blur_previous_h_ = t.h;
    blur_previous_count_ = blur_current_count_;
    for (uint32_t i = 0; i < blur_current_count_; i++)
        std::memcpy(blur_previous_cars_[i], blur_current_cars_[i], sizeof(blur_previous_cars_[i]));
}

void D3D12Backend::PostApply(Target& t, const char* why) {
    post_done_ = presents_;
    // an extension's own range: its Resolve owns the colour, the pass would read it divided
    if (t.range != 1) {
        post_skipped_range_++;
        LOG_FIRST(4, "MC3RENDER post: target %08X has range %.2f, skipped", t.data, t.range);
        return;
    }
    if (render::On(render::kMotionBlur)) MotionBlurApply(t, why);
    if (!render::On(render::kPost)) return;
    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_post_pre_%s", cap_i_, why);
        CapSave(t, name);
    }
    // the pass's cost on the GPU is the copy it reads and the pass itself
    uint32_t slot = uint32_t(frame_ % kFrames);
    bool stamp = time_heap_ && time_open_[slot] && !time_post_[slot];
    if (stamp) cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 2);
    ID3D12Resource* src = Feedback(t);
    if (!src) return;
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const render::Post& q = render::Get().post;
    float p[kPostConstants] = {1.0f / float(t.w), 1.0f / float(t.h), float((q.fxaa ? 1 : 0) | (q.split ? 2 : 0)),
                               q.threshold, q.exposure, q.gamma, q.contrast, q.saturation, q.shoulder, 0, 0, 0};
    ID3D12Resource* srcs[3] = {src, nullptr, nullptr};
    DXGI_FORMAT fmts[3] = {t.fmt, kBgra, kBgra};
    PassN(kPostHlsl, "psmain", t.fmt, View(t), t.w, t.h, srcs, fmts, p, kPostConstants, -1);
    if (stamp) {
        cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 3);
        time_post_[slot] = true;
    }
    t.drawn = true;
    post_frames_++;
    if (cap_) {
        fprintf(cap_, "%04u post %s rt %08X %ux%u %s\n", cap_i_, why, t.data, t.w, t.h,
                render::PostNames(q).c_str());
        char name[48];
        snprintf(name, sizeof(name), "%04u_post_%s", cap_i_++, why);
        CapSave(t, name);
    }
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (render::On(render::kContactShadow) || render::Streams()) ShadowBeforeDraw(s, a.pretransformed);
    if (render::On(render::kPost) || render::On(render::kMotionBlur)) PostBeforeDraw(s, a.pretransformed);
    if (!BindTargets(s)) return;
    if (ext_) {
        const bool rebind = ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed);
        ExtensionDone();
        if (rebind && !BindTargets(s)) return;
    }

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;

    // mcSenseOfSpeed finishes its radial speed blur by alpha-compositing a
    // half-resolution colour target over the main scene. The tune sidecar
    // removes its configured scale/alpha, but Remix still emits this composite
    // at speed. When the replacement blur is active, suppress this one draw so
    // the two effects cannot stack. The complete signature is intentionally
    // narrow: it excludes the half-resolution glow/additive passes and every
    // full-resolution Flash/video overlay. If the sidecar validation failed,
    // MotionBlurTuneReady is false and the original effect remains untouched.
    if (render::On(render::kMotionBlur) && render::MotionBlurTuneReady() && a.pretransformed && prim == 6 && count == 4 &&
        s.vs_handle == 0x1C4 && !s.ps && bound_rt_ && bound_ds_ && bound_rt_->gw == guest_w_ &&
        bound_rt_->gh == guest_h_ && s.viewport.x == 0 && s.viewport.y == 0 && s.viewport.w == guest_w_ &&
        s.viewport.h == guest_h_ && stages.rt[0] && !stages.res[1] && !stages.res[2] && !stages.res[3] &&
        SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == 0x302 &&
        SimpleState(s, kMDestBlend) == 0x303) {
        Target* half = nullptr;
        for (auto& [key, target] : targets_) {
            if (!target.depth && target.res.Get() == stages.res[0]) {
                half = &target;
                break;
            }
        }
        if (half && half->gw * 2 == guest_w_ && half->gh * 2 == guest_h_) {
            mc3_blur_composites_hidden_++;
            LOG_FIRST(2, "MC3RENDER motion-blur: suppressed MC3 half-resolution speed-blur composite");
            if (cap_)
                fprintf(cap_, "     mc3_speed_blur_composite_suppressed=1 source=%ux%u target=%ux%u\n",
                        half->gw, half->gh, bound_rt_->gw, bound_rt_->gh);
            return;
        }
    }
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    if (render::On(render::kLightShadow) && bound_rt_) {
        PsoKey k0{};
        KeyBlend(s, k0);
        KeyDepthStencil(s, a, k0);
        bool cone = false;
        if (Mc3AdditiveLight(s, prim, k0, pk, cone)) {
            if (cone && render::Get().lights.cars > 0 && !render::Get().lights.beam_cone) {
                beam_cones_hidden_++;
                return;  // the beams light the road instead
            }
            if (cone && hl_frame_ == presents_ && !stages.res[3]) pk.light_shadow = 1;
            if (!cone && (render::Get().lights.lamps > 0 || render::Get().lights.cars > 0)) CollectLampGlows(s, a);
        }
        // a car: MC3 makes its four car vertex programs one after another (the player's paint, glass and parts, and the
        // traffic's, which has no environment map), and their handles come out the same in every run and city seen
        // (B0101FC1, FD1, FE1, FF1: garage, San Diego, MC2 Los Angeles, 18-20/10); the matrix checks reject the rest
        constexpr uint32_t kCarProgramFirst = 0xB0101FC1, kCarProgramLast = 0xB0101FF1;
        uint32_t tw = 0, th = 0;
        SurfaceSize(s.rt, tw, th);
        bool main_view = tw == guest_w_ && th == guest_h_ && s.viewport.x == 0 && s.viewport.y == 0 &&
                         s.viewport.w == tw && s.viewport.h == th;
        if (render::Get().lights.cars > 0 && sh.prog && !a.pretransformed && main_view &&
            s.vs_handle >= kCarProgramFirst && s.vs_handle <= kCarProgramLast)
            CollectDrawnCar(s, a);
    }
    const uint32_t signed_op = SignedBlendOp(s);
    if (signed_op) pk.signed_half = 1;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    // pipeline; k keeps the console's equation for the capture, the signed passes draw with their own
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    PsoKey kp = k;
    if (signed_op) kp.op = signed_op == 0xF006 ? 0x8006u : 0x800Bu;
    ID3D12PipelineState* pso = EnsurePipeline(kp, sh.vs_code, sh.ps_code);
    if (!pso) return;
    if (render::Streams()) GameLight(s);
    bool so = !signed_op && SoEligible(s, a, k);
    if (so) {
        if (ID3D12PipelineState* sp = EnsureSoPipeline(kp, sh.vs_code, sh.ps_code)) pso = sp;
        else so = false;
    }
    SetPipeline(pso);

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    UploadConstants(s, sh, k, stages, size, c);
    if (pk.light_shadow && last_vc_) {
        std::memcpy(last_vc_ + 180 * 4, hl_k_, sizeof(hl_k_));
        stages.res[3] = hl_map_.Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[3];
        sv = {};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.Texture2D.MipLevels = 1;
        hl_passes_++;
    }

    bool headlight_cone = false;
    bool capture_light = cap_ && bound_rt_ && Mc3AdditiveLight(s, prim, k, pk, headlight_cone);

    if (cap_) {
        CaptureDraw(s, prim, count, k, pk, c, stages);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
        if (capture_light) {
            fprintf(cap_, "     mc3_%s_candidate=1 header=%08X specular=%u\n",
                    headlight_cone ? "headlight" : "additive_light", s.textures[0],
                    s.rs[kRsSpecularEnable] != 0);
        }
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    if (so) so = SoBegin(s, a);
    DrawGeometry(a);
    if (so) {
        SoEnd(s, a);
        if (cap_) {
            fprintf(cap_, "     so %u\n", so_[presents_ & 1].verts);
            if (!so_chk_) {
                D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
                D3D12_RESOURCE_DESC rd = BufferDesc(kSoChecks * 8);
                void* m = nullptr;
                if (SUCCEEDED(dev_->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&so_chk_))) &&
                    SUCCEEDED(so_chk_->Map(0, nullptr, &m)))
                    so_chk_map_ = static_cast<const uint64_t*>(m);
            }
            SoBuffer& b = so_[presents_ & 1];
            if (so_chk_map_ && so_chk_rows_.size() < kSoChecks) {
                Transition(b.filled.Get(), b.filled_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cl_->CopyBufferRegion(so_chk_.Get(), uint64_t(so_chk_rows_.size()) * 8, b.filled.Get(),
                                      uint64_t(b.draws - 1) * 8, 8);
                Transition(b.filled.Get(), b.filled_state, D3D12_RESOURCE_STATE_STREAM_OUT);
                so_chk_rows_.push_back({cap_i_ - 1, b.verts});
            }
        }
    }
    if (signed_op) {
        // the negative values, with the opposite equation; root arguments stay bound across the pipeline change
        pk.signed_half = 2;
        DrawShaders sn = sh;
        sn.pkey = Hash(&pk, sizeof(pk));
        EnsurePixelShader(pk, sn);
        PsoKey kn;
        BuildPipelineKey(s, a, sn.vh, sn.ph, kn);
        kn.op = signed_op == 0xF006 ? 0x800Bu : 0x8006u;
        ID3D12PipelineState* pso_n = sn.ps_code ? EnsurePipeline(kn, sn.vs_code, sn.ps_code) : nullptr;
        if (pso_n) {
            SetPipeline(pso_n);
            DrawGeometry(a);
        }
    }
    EndCountedDraw(query);
    if (capture_light) Capture(*bound_rt_, headlight_cone ? "mc3_headlight_post" : "mc3_additive_post", 1);
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, std::clamp(c.z, 0.0f, 1.0f),
                                   UINT8(c.stencil), UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        // the console clears channels one by one (D3DCLEAR_TARGET_R 0x10, G 0x20, B 0x40, A 0x80): MC3's minimap
        // clears the alpha alone before drawing its round mask there, a full clear left a black square
        uint32_t mask = (c.flags & kClearTarget) >> 4;
        if (mask == 0xF) {
            cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
        } else {
            MaskedClear(t, col, mask, rs);
        }
    }
    t.drawn = true;
}

// D3D12 clears whole pixels: the channels in mask (red 1, green 2, blue 4, alpha 8) are drawn instead
void D3D12Backend::MaskedClear(Target& t, const float col[4], uint32_t mask, const std::vector<D3D12_RECT>& rs) {
    auto& pso = blit_psos_[Hash(kMaskedClearHlsl, std::strlen(kMaskedClearHlsl), uint64_t(t.fmt) << 4 | mask)];
    if (!pso) {
        uint64_t h;
        ID3DBlob* vs = Compile(kMaskedClearHlsl, "vs", "vs_5_0", h);
        ID3DBlob* ps = Compile(kMaskedClearHlsl, "psmain", "ps_5_0", h);
        if (!vs || !ps) return;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, t.fmt);
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = UINT8(mask);
        if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return;
    }
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, col, 4 * sizeof(float));
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    for (const D3D12_RECT& r : rs) {
        SetFullViewport(t.w, t.h);
        SetScissor(r);
        DrawFullScreen(pso.Get(), cb.gpu, slot);
    }
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        SetViewport(vp);
        SetScissor(scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt) {
        // formats differ: drawn across
        CopyByDraw(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    swap_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (!so_chk_rows_.empty() && so_chk_map_) {
        Flush();
        uint64_t prev_gpu = 0, prev_cpu = 0;
        uint32_t shown = 0, bad = 0;
        for (size_t i = 0; i < so_chk_rows_.size(); i++) {
            // each slot holds that draw's own bytes
            uint64_t cpu = so_chk_rows_[i].second, gpu = prev_gpu + so_chk_map_[i] / 16;
            bool off = gpu - prev_gpu != cpu - prev_cpu;
            bad += off;
            if (off && shown++ < 64)
                fprintf(cap_, "so-check draw %04u cpu +%llu gpu +%llu (totals %llu / %llu)\n", so_chk_rows_[i].first,
                        (unsigned long long)(cpu - prev_cpu), (unsigned long long)(gpu - prev_gpu),
                        (unsigned long long)cpu, (unsigned long long)gpu);
            prev_gpu = gpu, prev_cpu = cpu;
        }
        fprintf(cap_, "so-check %zu streamed draws, %u where the GPU wrote another count\n", so_chk_rows_.size(), bad);
        so_chk_rows_.clear();
    }
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    std::string dir = stamp;
    const render::Config& rc = render::Get();
    if (!rc.capture_ab.empty()) dir += "_" + rc.capture_ab + "_" + render::ProfileName(rc.profile);
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / dir, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
    WriteCaptureMeta();
}

// what an A/B pair compares besides the pixels: the effective config and the frame it was taken at
void D3D12Backend::WriteCaptureMeta() {
    std::string name = "render_effective_config";
    if (app::Instance() > 1) name += "-" + std::to_string(app::Instance());
    std::error_code ec;
    std::filesystem::copy_file(app::ExeDir() / "logs" / (name + ".txt"), cap_dir_ / "render_effective_config.txt",
                               std::filesystem::copy_options::overwrite_existing, ec);
    FILE* m = _wfopen((cap_dir_ / "capture_meta.txt").c_str(), L"w");
    if (!m) return;
    const render::Config& rc = render::Get();
    fprintf(m, "tag=%s\nprofile=%s\nfeatures=%s\nconfig_hash=%016llX\npresent=%llu\nscale=%.3f\nguest=%ux%u\n"
               "output=%ux%u\nlast_timing=%s\n",
            rc.capture_ab.c_str(), render::ProfileName(rc.profile), render::FeatureNames(rc.effective).c_str(),
            (unsigned long long)rc.hash, (unsigned long long)presents_, scale_, guest_w_, guest_h_, bb_w_, bb_h_,
            time_summary_.c_str());
    fclose(m);
    LOGR("MC3RENDER capture tag=%s profile=%s present=%llu dir=%s", rc.capture_ab.empty() ? "-" : rc.capture_ab.c_str(),
         render::ProfileName(rc.profile), (unsigned long long)presents_, cap_dir_.string().c_str());
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::BlitFrame(Target& t) {
    // 16:9 when the console reports a widescreen display, else 4:3
    bool wide = app::Widescreen();
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    SetViewport(vp);
    SetScissor(scissor);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Blit(t.res.Get(), t.fmt, uv, {.gamma = gamma_});
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t);
    RenderUi();
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
    TimingRead();
    TimingBegin();
}

void D3D12Backend::TimingInit() {
    if (!render::Get().timing) return;
    UINT64 freq = 0;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps * kFrames, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(kStamps * kFrames * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(queue_->GetTimestampFrequency(&freq)) && freq &&
                SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&time_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&time_rb_))) &&
                SUCCEEDED(time_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOGE("MC3RENDER timing: no timestamp queries, GPU time not measured");
        time_heap_.Reset();
        return;
    }
    time_ticks_ = static_cast<const uint64_t*>(p);
    time_ticks_ms_ = double(freq) / 1000.0;
    time_cpu_.reserve(kTimingFrames);
    time_gpu_.reserve(kTimingFrames);
    time_pass_.reserve(kTimingFrames);
}

void D3D12Backend::TimingBegin() {
    if (!time_heap_) return;
    uint32_t slot = uint32_t(frame_ % kFrames);
    cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps);
    time_open_[slot] = true;
    time_done_[slot] = time_post_[slot] = time_shadow_[slot] = time_vshadow_[slot] = time_sun_[slot] = time_hl_[slot] =
        time_lamp_[slot] = time_blur_[slot] = false;
}

void D3D12Backend::TimingEnd() {
    if (!time_heap_) return;
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (!time_open_[slot]) return;
    cl_->EndQuery(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + 1);
    // only stamps this frame wrote: the pass's two when it ran
    cl_->ResolveQueryData(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps, 2, time_rb_.Get(),
                          uint64_t(slot) * kStamps * sizeof(uint64_t));
    for (uint32_t pair = 1; pair < kStamps / 2; pair++) {
        if (!(pair == 1   ? time_post_[slot]
              : pair == 2 ? time_shadow_[slot]
              : pair == 3 ? time_vshadow_[slot]
              : pair == 4 ? time_sun_[slot]
              : pair == 5 ? time_hl_[slot]
              : pair == 6 ? time_lamp_[slot]
                          : time_blur_[slot]))
            continue;
        cl_->ResolveQueryData(time_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + pair * 2, 2, time_rb_.Get(),
                              (uint64_t(slot) * kStamps + pair * 2) * sizeof(uint64_t));
    }
    time_done_[slot] = true;
}

// the slot's frame is past its fence: its stamps are in the read-back buffer
void D3D12Backend::TimingRead() {
    if (!time_heap_) return;
    uint32_t slot = uint32_t(frame_ % kFrames);
    if (time_done_[slot]) {
        const uint64_t* t = time_ticks_ + slot * kStamps;
        if (t[1] >= t[0]) time_gpu_.push_back(float(double(t[1] - t[0]) / time_ticks_ms_));
        if (time_post_[slot] && t[3] >= t[2]) time_pass_.push_back(float(double(t[3] - t[2]) / time_ticks_ms_));
        if (time_shadow_[slot] && t[5] >= t[4]) time_shadow_ms_.push_back(float(double(t[5] - t[4]) / time_ticks_ms_));
        if (time_vshadow_[slot] && t[7] >= t[6]) time_vshadow_ms_.push_back(float(double(t[7] - t[6]) / time_ticks_ms_));
        if (time_sun_[slot] && t[9] >= t[8]) time_sun_ms_.push_back(float(double(t[9] - t[8]) / time_ticks_ms_));
        if (time_hl_[slot] && t[11] >= t[10]) time_hl_ms_.push_back(float(double(t[11] - t[10]) / time_ticks_ms_));
        if (time_lamp_[slot] && t[13] >= t[12]) time_lamp_ms_.push_back(float(double(t[13] - t[12]) / time_ticks_ms_));
        if (time_blur_[slot] && t[15] >= t[14]) time_blur_ms_.push_back(float(double(t[15] - t[14]) / time_ticks_ms_));
    }
    time_open_[slot] = time_done_[slot] = time_post_[slot] = time_shadow_[slot] = time_vshadow_[slot] = time_sun_[slot] =
        time_hl_[slot] = time_lamp_[slot] = time_blur_[slot] = false;
}

void D3D12Backend::TimingReport() {
    if (!time_heap_) return;
    auto now = PerfClock::now();
    if (time_last_ != PerfClock::time_point{})
        time_cpu_.push_back(float(std::chrono::duration<double, std::milli>(now - time_last_).count()));
    time_last_ = now;
    if (time_cpu_.size() < kTimingFrames) return;
    auto stats = [](std::vector<float>& v, double& avg, double& p95, double& p99) {
        avg = p95 = p99 = 0;
        if (v.empty()) return;
        double sum = 0;
        for (float x : v) sum += x;
        avg = sum / double(v.size());
        std::sort(v.begin(), v.end());
        p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
        p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
    };
    double ca, c95, c99, ga, g95, g99, pa, p95, p99;
    size_t gn = time_gpu_.size(), pn = time_pass_.size();
    stats(time_cpu_, ca, c95, c99);
    stats(time_gpu_, ga, g95, g99);
    stats(time_pass_, pa, p95, p99);
    char b[320];
    snprintf(b, sizeof(b),
             "frames=%zu cpu_ms avg=%.3f p95=%.3f p99=%.3f gpu_ms avg=%.3f p95=%.3f p99=%.3f (%zu stamped) | "
             "psos=%zu shaders=%zu textures=%zu targets=%zu presents=%llu",
             time_cpu_.size(), ca, c95, c99, ga, g95, g99, gn, psos_.size(), blobs_.size(), textures_.size(),
             targets_.size(), (unsigned long long)presents_);
    time_summary_ = b;
    LOGR("MC3RENDER timing %s", b);
    if (pn) LOGR("MC3RENDER timing post_pass_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", pa, p95, p99, pn);
    if (!time_shadow_ms_.empty()) {
        size_t sn = time_shadow_ms_.size();
        double sa, s95, s99;
        stats(time_shadow_ms_, sa, s95, s99);
        LOGR("MC3RENDER timing contact_shadow_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", sa, s95, s99, sn);
        time_shadow_ms_.clear();
    }
    if (!time_vshadow_ms_.empty()) {
        size_t vn = time_vshadow_ms_.size();
        double va, v95, v99;
        stats(time_vshadow_ms_, va, v95, v99);
        LOGR("MC3RENDER timing vehicle_shadow_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", va, v95, v99, vn);
        time_vshadow_ms_.clear();
    }
    if (!time_sun_ms_.empty()) {
        size_t un = time_sun_ms_.size();
        double ua, u95, u99;
        stats(time_sun_ms_, ua, u95, u99);
        LOGR("MC3RENDER timing sun_shadow_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", ua, u95, u99, un);
        time_sun_ms_.clear();
    }
    if (!time_hl_ms_.empty()) {
        size_t hn = time_hl_ms_.size();
        double ha, h95, h99;
        stats(time_hl_ms_, ha, h95, h99);
        LOGR("MC3RENDER timing light_shadow_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", ha, h95, h99, hn);
        time_hl_ms_.clear();
    }
    if (!time_lamp_ms_.empty()) {
        size_t ln = time_lamp_ms_.size();
        double la, l95, l99;
        stats(time_lamp_ms_, la, l95, l99);
        LOGR("MC3RENDER timing lamp_shadow_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", la, l95, l99, ln);
        time_lamp_ms_.clear();
    }
    if (!time_blur_ms_.empty()) {
        size_t bn = time_blur_ms_.size();
        double ba, b95, b99;
        stats(time_blur_ms_, ba, b95, b99);
        LOGR("MC3RENDER timing motion_blur_ms avg=%.3f p95=%.3f p99=%.3f (%zu passes)", ba, b95, b99, bn);
        time_blur_ms_.clear();
    }
    time_cpu_.clear();
    time_gpu_.clear();
    time_pass_.clear();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        // XBE_PERF is useful on the shipped Release build too. Info lines are
        // intentionally discarded there, so keep this opt-in diagnostic at
        // Report level instead of silently producing no output.
        LOGR("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGR("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGR("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        LOGR("perf: per frame %.0f list state calls left out as already set", p.skipped / n);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

void D3D12Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (render::On(render::kVehicleShadow) && presents_ % 600 == 599) {
        LOGR("MC3RENDER vehicle-shadow: %llu of the last 600 frames (no previous frame %llu, no target %llu, too far %llu, "
             "no key light %llu, stream output full %llu)",
             (unsigned long long)vs_frames_, (unsigned long long)vs_no_prev_, (unsigned long long)vs_no_target_,
             (unsigned long long)vs_far_, (unsigned long long)vs_dark_, (unsigned long long)vs_full_);
        vs_frames_ = vs_no_prev_ = vs_no_target_ = vs_far_ = vs_full_ = vs_dark_ = 0;
    }
    if (render::On(render::kLightShadow) && presents_ % 600 == 599) {
        LOGR("MC3RENDER light-shadow: %llu of the last 600 frames had the map (no previous frame %llu, no target %llu), "
             "%llu headlight passes read it",
             (unsigned long long)hl_frames_, (unsigned long long)hl_no_prev_, (unsigned long long)hl_no_target_,
             (unsigned long long)hl_passes_);
        hl_frames_ = hl_no_prev_ = hl_no_target_ = hl_passes_ = 0;
        size_t valid = 0;
        for (const auto& l : lamps_) valid += l.hits >= uint32_t(render::Get().lights.lamp_hits);
        LOGR("MC3RENDER light-shadow: lamps %zu cached (%zu seen often enough), %llu glows taken, %llu frames cast "
             "%.2f lamps on average",
             lamps_.size(), valid, (unsigned long long)lamp_glows_, (unsigned long long)lamp_frames_,
             lamp_frames_ ? double(lamp_used_) / double(lamp_frames_) : 0.0);
        lamp_frames_ = lamp_used_ = lamp_glows_ = 0;
        LOGR("MC3RENDER light-shadow: %llu car bodies taken, %llu frames with beams, %.2f beams on average, %llu car "
             "frames left dark (parked), %llu title headlight cones left out, tracks %.3f m off their "
             "prediction on average, %llu snapped",
             (unsigned long long)beam_cars_seen_, (unsigned long long)beam_frames_,
             beam_frames_ ? double(beam_used_) / double(beam_frames_) : 0.0, (unsigned long long)beam_unlit_,
             (unsigned long long)beam_cones_hidden_, beam_fits_ ? beam_miss_ / double(beam_fits_) : 0.0,
             (unsigned long long)beam_snaps_);
        beam_cars_seen_ = beam_frames_ = beam_used_ = beam_unlit_ = beam_cones_hidden_ = beam_snaps_ = beam_fits_ = 0;
        beam_miss_ = 0;
    }
    if (render::On(render::kSunShadow) && presents_ % 600 == 599) {
        LOGR("MC3RENDER sun-shadow: %llu of the last 600 frames (no previous frame %llu, no target %llu, no key light %llu)",
             (unsigned long long)sun_frames_, (unsigned long long)sun_no_prev_, (unsigned long long)sun_no_target_,
             (unsigned long long)sun_dark_);
        sun_frames_ = sun_no_prev_ = sun_no_target_ = sun_dark_ = 0;
    }
    if (render::On(render::kContactShadow) || render::Streams()) {
        shadow_scene_ = 0;
        if (d3d_cars_seen_ == 0) shadow_have_ = false;
        d3d_cars_seen_ = 0;
        if (presents_ % 600 == 599) {
            LOGR("MC3RENDER contact-shadow: %llu of the last 600 frames (no car %llu, too far %llu, no depth %llu)",
                 (unsigned long long)shadow_frames_, (unsigned long long)shadow_no_car_,
                 (unsigned long long)shadow_far_, (unsigned long long)shadow_no_depth_);
            shadow_frames_ = shadow_no_car_ = shadow_far_ = shadow_no_depth_ = 0;
        }
    }
    if (render::On(render::kPost) || render::On(render::kMotionBlur)) PostAtPresent(front);
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false) && CaptureOn()) {
        StartCapture();
    }
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    if (ramp_dirty_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    DrawFrame(t, bi);
    TimingEnd();
    Submit(false);
    DeliverReports();
    bool vsync = app::Get().vsync;
    auto perf_swap = PerfClock::now();
    swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    g_perf.swap += PerfMs(perf_swap);

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    TimingReport();
    if (diag::Perf()) LogPerf();
    ApplySettings();
}

void D3D12Backend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    bool remake = ext_ && ext_->Settings();
    if (std::fabs(want - scale_) > 1e-4f || remake) {
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        scale_ = want;
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = nullptr;
        ExtensionDone();
    }
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Resolve(t);
    CopySource out{t.res.Get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(t.res.Get(), t.state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    DXGI_FORMAT fmt = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    float mul = t.depth ? 1 : t.range;
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = t.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

}  // namespace gpu
