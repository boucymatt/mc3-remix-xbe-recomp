// Native OpenGL backend for the Xbox state captured by the existing D3D8 HLE.
#include <epoxy/gl.h>
#include "platform.h"
#include "shader.h"
#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include "draw_constants.h"

namespace gpu {
namespace {
using namespace linuxgl;
struct PerfScope {
    uint64_t& total;
    std::chrono::steady_clock::time_point start;
    static bool Enabled() { static const bool on = std::getenv("MC3_PERF") != nullptr; return on; }
    explicit PerfScope(uint64_t& t) : total(t) { if (Enabled()) start = std::chrono::steady_clock::now(); }
    ~PerfScope() { if (Enabled()) total += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(); }
};
struct Target {
    GLuint texture = 0, fbo = 0;
    TextureDesc desc{};
    uint32_t w = 0, h = 0;
    bool depth = false, written = false;
    uint64_t epoch = 0;
};
struct Texture {
    GLuint texture = 0;
    GLenum type = GL_TEXTURE_2D;
    uint64_t signature = 0, epoch = 0, checked_frame = ~0ull;
};
uint64_t TargetKey(const TextureDesc& d) {
    return uint64_t(d.data) | (uint64_t(d.width) << 32) | (uint64_t(d.height) << 46) | (uint64_t(Format(d.format).depth) << 63);
}
GLenum Address(uint32_t v) {
    switch (v) { case 1: return GL_REPEAT; case 2: return GL_MIRRORED_REPEAT; case 4: return GL_CLAMP_TO_BORDER; default: return GL_CLAMP_TO_EDGE; }
}
GLenum Compare(uint32_t v) { return GL_NEVER + CompareIndex(v); }
GLenum BlendOp(uint32_t v) {
    switch (v) { case 0x800A: case 0x800B: case 0x8007: case 0x8008: return v;
        case 0xF005: return GL_FUNC_REVERSE_SUBTRACT; default: return GL_FUNC_ADD; }
}
void Enable(GLenum cap, bool on) { if (on) glEnable(cap); else glDisable(cap); }
uint32_t PixelOffset(const TextureDesc& d, uint32_t x, uint32_t y) {
    const uint32_t bpp = Format(d.format).bits / 8;
    if (d.linear) return y * d.pitch + x * bpp;
    uint32_t address = 0, out = 0;
    for (uint32_t bit = 0; (1u << bit) < std::max(d.width, d.height); bit++) {
        if ((1u << bit) < d.width) address |= ((x >> bit) & 1u) << out++;
        if ((1u << bit) < d.height) address |= ((y >> bit) & 1u) << out++;
    }
    return address * bpp;
}

class OpenGL final : public Backend {
public:
    bool Init(uint32_t, uint32_t) override { return true; }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        if (!Ready()) return;
        BindTargets(s);
        glColorMask(flags & 0x40, flags & 0x20, flags & 0x10, flags & 0x80);
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);
        float c[4]; Color(color, c); glClearColor(c[0], c[1], c[2], c[3]);
        glClearDepth(z); glClearStencil(stencil);
        GLbitfield mask = (rt_ && (flags & kClearTarget) ? GL_COLOR_BUFFER_BIT : 0) |
                          (zs_ && (flags & kClearZ) ? GL_DEPTH_BUFFER_BIT : 0) |
                          (zs_ && (flags & kClearStencil) ? GL_STENCIL_BUFFER_BIT : 0);
        glEnable(GL_SCISSOR_TEST);
        for (uint32_t i = 0; i < std::max(1u, count); i++) {
            int32_t r[4] = {0, 0, int32_t(width_), int32_t(height_)};
            if (count && rects) std::memcpy(r, rects + i * 16, 16);
            glScissor(Scale(r[0]), Scale(r[1]), std::max(0, Scale(r[2]) - Scale(r[0])), std::max(0, Scale(r[3]) - Scale(r[1])));
            glClear(mask);
        }
        if (rt_ && (flags & kClearTarget)) rt_->written = true;
        if (zs_ && (flags & (kClearZ | kClearStencil))) zs_->written = true;
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        if (!Ready()) return;
        PerfScope draw_time(perf_draw_);
        try {
            if (!Assemble(s, prim, start, count, indices, assembled_)) return;
            auto& a = assembled_;
            BindTargets(s);
            if (!width_ || !height_) return;
            DrawConstants dc{};
            TexInfo info[4]{};
            // The actual streetlamp shafts are procedural 32-segment cone strips,
            // separate from the archive-backed glow sprites below. Match their
            // generated texture as well as their draw state to retain road lights.
            if (!app::Get().light_streaks && !a.pretransformed && !s.ps &&
                prim == 6 && count == 64 && s.vs_handle == 0x142 &&
                s.rs[kRsZEnable] && !s.simple[kMZWriteEnable/4] &&
                s.simple[kMBlendEnable/4] && s.simple[kMSrcBlend/4] == 0x302 &&
                s.simple[kMDestBlend/4] == 1 && !s.textures[1] && !s.textures[2] && !s.textures[3]) {
                TextureDesc cone;
                if (Describe(s.textures[0], cone) && cone.data && cone.format == 0x06 &&
                    cone.width == 64 && cone.height == 64 &&
                    Hash(kernel::HostPtr(cone.data), 64 * 64 * 4) == 0x2905F9F1A0D6C474ull) {
                    lamp_beams_skipped_++; return;
                }
            }
            if (!app::Get().motion_blur && a.pretransformed && prim == 6 && count == 4 &&
                s.vs_handle == 0x1C4 && !s.ps && rt_ && zs_ && width_ >= 640 && height_ == 480 &&
                s.viewport.x == 0 && s.viewport.y == 0 && s.viewport.w == width_ && s.viewport.h == height_ &&
                s.textures[0] && !s.textures[1] && !s.textures[2] && !s.textures[3] &&
                s.simple[kMBlendEnable / 4] && s.simple[kMSrcBlend / 4] == 0x302 && s.simple[kMDestBlend / 4] == 0x303) {
                TextureDesc source;
                if (Describe(s.textures[0], source) && source.width * 2 == width_ && source.height * 2 == height_) {
                    blur_skipped_++; return;
                }
            }
            // Glare cards/streaks and translucent lamp cones are separate effects.
            // Identify their complete texture data from the owned archive. Cone
            // meshes also use programmable vertex shaders and alpha blending;
            // restricting them to XYZRHW/additive draws leaves the beams visible.
            // Streetlamp streaks also use Xbox QUADLIST (8), not only triangles.
            const bool light_geometry = (a.pretransformed && s.vs_handle == 0x144) ||
                                        (!a.pretransformed && s.vs_handle == 0x142);
            const uint32_t destination_blend = s.simple[kMDestBlend/4];
            if (!app::Get().light_streaks && (prim == 5 || prim == 6 || prim == 7 || prim == 8) &&
                s.rs[kRsZEnable] && !s.simple[kMZWriteEnable/4] && s.simple[kMBlendEnable/4] &&
                s.simple[kMSrcBlend/4] == 0x302 && (destination_blend == 1 || destination_blend == 0x303)) {
                TextureDesc light;
                const uint32_t effect = Describe(s.textures[0], light) ? LightEffect(light) : 0;
                if (effect == 2) { lamp_beams_skipped_++; return; }
                if (effect == 1 && light_geometry && destination_blend == 1) {
                    if (!a.pretransformed) glare_world_skipped_++;
                    glare_skipped_++; return;
                }
            }
            for (int i = 0; i < 4; i++) BindTexture(s, i, info[i], dc);
            auto pk = MakePixelKey(s, info);
            const uint32_t equation = s.simple[kMBlendOp / 4];
            const bool signed_blend = s.simple[kMBlendEnable / 4] &&
                (equation == 0xF005 || equation == 0xF006) && s.simple[kMSrcBlend / 4] == 1 && s.simple[kMDestBlend / 4] == 1;
            if (signed_blend) pk.signed_half = 1;
            const bool program = s.vs && !s.vs->program.empty();
            auto fk = MakeFixedKey(s);
            uint64_t vk = Hash(&a.regs, sizeof(a.regs), GL_VERTEX_SHADER);
            vk = Hash(&a.pretransformed, sizeof(a.pretransformed), vk);
            vk = Hash(&program, sizeof(program), vk);
            vk = program ? Hash(s.vs->program.data(), s.vs->program.size() * 4, vk) : Hash(&fk, sizeof(fk), vk);
            uint64_t sk = Hash(&pk, sizeof(pk), GL_FRAGMENT_SHADER);
            auto [vi, new_v] = shaders_.try_emplace(vk, 0);
            if (new_v) vi->second = CompileShader(program ? VertexProgramHlsl(s.vs->program, a.regs) : FixedVertexHlsl(a.regs, a.pretransformed, 0, fk), GL_VERTEX_SHADER);
            GLuint v = vi->second;
            auto [pi, new_p] = shaders_.try_emplace(sk, 0);
            if (new_p) pi->second = CompileShader(PixelHlsl(pk), GL_FRAGMENT_SHADER);
            GLuint p = pi->second;
            uint64_t key = uint64_t(v) << 32 | p;
            auto [it, added] = programs_.try_emplace(key, 0);
            if (added) it->second = LinkProgram(v, p);
            glUseProgram(it->second);
            FillSurfaceConstants(s, {width_, height_}, dc);
            FillFogConstants(s, program, dc);
            FillCombinerConstants(s, dc);
            if (a.topology == Topology::Points) dc.surf[3] = 1;
            if (!program && !a.pretransformed) {
                FillTransformConstants(s, {width_, height_}, dc);
                FillLightConstants(s, fk, dc);
            }
            if (!constants_valid_ || std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc)) != 0) {
                glNamedBufferData(ubo_[0], sizeof(s.vsc), s.vsc.data(), GL_STREAM_DRAW);
                std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
            }
            if (!constants_valid_ || std::memcmp(&last_dc_, &dc, sizeof(dc)) != 0) {
                glNamedBufferData(ubo_[1], sizeof(dc), &dc, GL_STREAM_DRAW);
                std::memcpy(&last_dc_, &dc, sizeof(dc));
            }
            constants_valid_ = true;
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_[0]);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo_[1]);
            glBindVertexArray(vao_);
            glNamedBufferData(vbo_, a.vertices.size() * sizeof(float), a.vertices.data(), GL_STREAM_DRAW);
            glNamedBufferData(ibo_, a.indices.size() * sizeof(uint32_t), a.indices.data(), GL_STREAM_DRAW);
            glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, a.buffers[0].stride);
            for (unsigned r = 0; r < 16; r++) {
                if (a.regs & (1u << r)) {
                    glEnableVertexArrayAttrib(vao_, r);
                    glVertexArrayAttribFormat(vao_, r, 4, GL_FLOAT, GL_FALSE, a.elements[r].offset);
                    glVertexArrayAttribBinding(vao_, r, 0);
                } else glDisableVertexArrayAttrib(vao_, r);
            }
            glViewport(0, 0, Scale(width_), Scale(height_));
            glDepthRange(0, 1);
            glEnable(GL_SCISSOR_TEST);
            int left = int(s.viewport.x), top = int(s.viewport.y);
            int right = left + int(s.viewport.w), bottom = top + int(s.viewport.h);
            if (s.scissor_count) {
                left = std::max(left, int(s.scissors[0])); top = std::max(top, int(s.scissors[1]));
                right = std::min(right, int(s.scissors[2])); bottom = std::min(bottom, int(s.scissors[3]));
            }
            glScissor(Scale(left), Scale(top), std::max(Scale(right) - Scale(left), 0), std::max(Scale(bottom) - Scale(top), 0));
            auto m = [&](uint32_t k) { return s.simple[k / 4]; };
            Enable(GL_DEPTH_TEST, zs_ && s.rs[kRsZEnable]);
            glDepthFunc(Compare(m(kMZFunc))); glDepthMask(m(kMZWriteEnable) != 0);
            Enable(GL_STENCIL_TEST, zs_ && s.rs[kRsStencilEnable]);
            glStencilFunc(Compare(m(kMStencilFunc)), m(kMStencilRef), m(kMStencilMask));
            glStencilMask(m(kMStencilWriteMask));
            glStencilOp(s.rs[kRsStencilFail], m(kMStencilZFail), m(kMStencilPass));
            const auto cw = m(kMColorWriteEnable);
            glColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);
            Enable(GL_BLEND, m(kMBlendEnable));
            glBlendFunc(m(kMSrcBlend), m(kMDestBlend));
            glBlendEquation(BlendOp(m(kMBlendOp)));
            float bc[4]; Color(m(kMBlendColor), bc); glBlendColor(bc[0], bc[1], bc[2], bc[3]);
            const auto cull = s.rs[kRsCullMode];
            Enable(GL_CULL_FACE, a.topology == Topology::Triangles && (cull == GL_CW || cull == GL_CCW));
            glFrontFace(s.rs[kRsFrontFace] == GL_CCW ? GL_CCW : GL_CW);
            glCullFace(cull == s.rs[kRsFrontFace] ? GL_FRONT : GL_BACK);
            Enable(GL_POLYGON_OFFSET_FILL, m(kMSolidOffsetEnable));
            glPolygonOffset(Bits(m(kMSlopeScale)), Bits(m(kMDepthOffset)));
            glPolygonMode(GL_FRONT_AND_BACK, s.rs[kRsFillMode] == 2 ? GL_LINE : GL_FILL);
            const GLenum topology = a.topology == Topology::Points ? GL_POINTS : a.topology == Topology::Lines ? GL_LINES : GL_TRIANGLES;
            if (m(kMCountEnable)) glBeginQuery(GL_SAMPLES_PASSED, query_);
            glDrawElements(topology, GLsizei(a.indices.size()), GL_UNSIGNED_INT, nullptr);
            if (signed_blend) {
                // NV2A signed-byte blending: positive contribution, then the
                // negative magnitude with the opposite equation, as on D3D12.
                pk.signed_half = 2;
                auto [ni, new_negative] = shaders_.try_emplace(Hash(&pk, sizeof(pk), GL_FRAGMENT_SHADER), 0);
                if (new_negative) ni->second = CompileShader(PixelHlsl(pk), GL_FRAGMENT_SHADER);
                const uint64_t negative_key = uint64_t(v) << 32 | ni->second;
                auto [np, new_program] = programs_.try_emplace(negative_key, 0);
                if (new_program) np->second = LinkProgram(v, ni->second);
                glUseProgram(np->second);
                glBlendEquation(equation == 0xF006 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
                glDrawElements(topology, GLsizei(a.indices.size()), GL_UNSIGNED_INT, nullptr);
            }
            if (m(kMCountEnable)) { PerfScope query_time(perf_query_); perf_queries_++; glEndQuery(GL_SAMPLES_PASSED); GLuint n; glGetQueryObjectuiv(query_, GL_QUERY_RESULT, &n); const Target* target = rt_ ? rt_ : zs_;
                const double area = target ? double(target->w) * target->h / (double(target->desc.width) * target->desc.height) : 1.0;
                samples_ += n / area; }
            if (rt_ && cw) rt_->written = true;
            if (zs_ && (m(kMZWriteEnable) || s.rs[kRsStencilEnable])) zs_->written = true;
            draws_++;
        } catch (const std::exception& e) { Fail(e.what()); }
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) override {
        if (!Ready()) return;
        Target* from = GetTarget(src); Target* to = GetTarget(dst);
        if (!from || !to || from->depth != to->depth) {
            Sync(src.data, 0); CopySurfaceMemory(src, dst, count, rects, points); return;
        }
        glDisable(GL_SCISSOR_TEST);
        for (uint32_t i = 0; i < std::max(count, 1u); i++) {
            int32_t r[4] = {0, 0, int32_t(from->desc.width), int32_t(from->desc.height)}, p[2] = {};
            if (count && rects) std::memcpy(r, kernel::HostPtr(rects + i * 16), 16);
            if (count && points) std::memcpy(p, kernel::HostPtr(points + i * 8), 8);
            glBlitNamedFramebuffer(from->fbo, to->fbo, Scale(r[0]), Scale(r[1]), Scale(r[2]), Scale(r[3]), Scale(p[0]), Scale(p[1]),
                Scale(p[0] + r[2] - r[0]), Scale(p[1] + r[3] - r[1]), from->depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        to->written = true;
    }
    void Present(const Surface& front) override {
        if (!Ready()) return;
        Target* target = GetTarget(front);
        if (!target || target->depth) return;
        int w, h; SDL_GetWindowSizeInPixels(linuxapp::Window(), &w, &h);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        const double aspect = app::Widescreen() ? 16.0 / 9.0 : 4.0 / 3.0;
        int dw = std::min(w, int(h * aspect)), dh = std::min(h, int(w / aspect));
        glViewport((w - dw) / 2, (h - dh) / 2, dw, dh);
        glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(present_program_); glBindVertexArray(vao_);
        glBindTextureUnit(0, target->texture); glBindSampler(0, present_sampler_);
        glBindTextureUnit(1, gamma_texture_); glBindSampler(1, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const char* every = std::getenv("MC3_CAPTURE_EVERY");
        if (g_capture.exchange(false) || (every && std::atoi(every) > 0 && (frame_ + 1) % std::atoi(every) == 0)) Capture(w, h);
        SDL_GL_SwapWindow(linuxapp::Window());
        frame_++; linuxapp::frames = frame_;
        if (frame_ % 120 == 0) {
            for (auto it = textures_.begin(); it != textures_.end();) {
                if (frame_ > it->second.checked_frame + 300) {
                    glDeleteTextures(1, &it->second.texture);
                    it = textures_.erase(it);
                } else ++it;
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - last_report_).count();
            LOGR("OpenGL: frame %llu, %.1f FPS, %llu draws, %zu programs, %zu textures", (unsigned long long)frame_,
                 120.0 / elapsed, (unsigned long long)draws_, programs_.size(), textures_.size());
            if (glare_skipped_ || lamp_beams_skipped_ || blur_skipped_) LOGR("Linux effects disabled: glare draws=%llu (world streaks=%llu), lamp beams=%llu, blur composites=%llu",
                (unsigned long long)glare_skipped_, (unsigned long long)glare_world_skipped_, (unsigned long long)lamp_beams_skipped_, (unsigned long long)blur_skipped_);
            if (PerfScope::Enabled()) {
                LOGR("Linux perf: frame=%llu draw_ms=%.3f query_wait_ms=%.3f readback_ms=%.3f queries=%llu readbacks=%llu guest_wait_ms=%.3f", (unsigned long long)frame_, perf_draw_/120e6, perf_query_/120e6, perf_readback_/120e6, (unsigned long long)perf_queries_, (unsigned long long)perf_reads_, g_perf_wait_ns.exchange(0)/120e6);
                perf_draw_ = perf_query_ = perf_readback_ = perf_queries_ = perf_reads_ = 0;
            }
            last_report_ = now; draws_ = 0;
        }
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) LOGE("OpenGL frame %llu error 0x%X", (unsigned long long)frame_, error);
    }
    void Sync(uint32_t data, uint32_t size) override {
        if (!Ready()) return;
        for (auto& [key, t] : targets_)
            if (t.written && (size ? uint64_t(data) < uint64_t(t.desc.data) + TotalSize(t.desc) && uint64_t(t.desc.data) < uint64_t(data) + size : data == t.desc.data)) Readback(t);
    }
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override { if (Ready()) glFinish(); }
    void SetGamma(const uint8_t* ramp) override {
        if (!Ready()) return;
        uint8_t pixels[256 * 3];
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++) pixels[i * 3 + c] = ramp[c * 256 + i];
        glTextureSubImage1D(gamma_texture_, 0, 0, 256, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    }
    void ClearReport() override { samples_ = 0; }
    void GetReport(uint32_t report) override {
        kernel::W32(report, uint32_t(frame_));
        kernel::W32(report + 4, 0);
        kernel::W32(report + 8, uint32_t(std::min<double>(std::round(samples_), 0xffffffff)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(report + 12, 0);
    }
private:
    uint32_t LightEffect(const TextureDesc& d) {
        struct Signature { uint32_t w, h, format, bytes; uint64_t hash, effect; };
        static_assert(sizeof(Signature) == 32);
        static const auto signatures = [] {
            std::vector<Signature> result;
            std::ifstream file(app::ExeDir() / "mc3_light_glare.bin", std::ios::binary);
            char magic[5]{}; uint32_t count = 0;
            if (file.read(magic, 5) && std::memcmp(magic, "MC3L2", 5) == 0 &&
                file.read(reinterpret_cast<char*>(&count), 4) && count > 0 && count <= 128) {
                result.resize(count);
                if (!file.read(reinterpret_cast<char*>(result.data()), count * sizeof(Signature))) result.clear();
            }
            if (result.empty()) LOGW("Linux: light-glare signatures missing; run build.sh to enable the glare toggle");
            return result;
        }();
        if (!d.data || (d.format != 0x0C && d.format != 0x0F)) return false;
        const uint32_t bytes = ((d.width + 3) / 4) * ((d.height + 3) / 4) * (d.format == 0x0C ? 8 : 16);
        uint64_t hash = 0; bool hashed = false;
        for (const auto& signature : signatures) {
            if (signature.w != d.width || signature.h != d.height || signature.format != d.format || signature.bytes != bytes) continue;
            if (!hashed) { hash = Hash(kernel::HostPtr(d.data), bytes); hashed = true; }
            if (hash == signature.hash) return uint32_t(signature.effect);
        }
        return false;
    }
    int Scale(int v) const { return int(std::lround(v * (app::Get().render_height / 480.0))); }
    void Transfer(const Target& t, GLuint& texture, GLuint& fbo) {
        glCreateTextures(GL_TEXTURE_2D, 1, &texture);
        glTextureStorage2D(texture, 1, t.depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8, t.desc.width, t.desc.height);
        glCreateFramebuffers(1, &fbo);
        glNamedFramebufferTexture(fbo, t.depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0, texture, 0);
        if (t.depth) { glNamedFramebufferDrawBuffer(fbo, GL_NONE); glNamedFramebufferReadBuffer(fbo, GL_NONE); }
    }
    bool Ready() {
        if (failed_) return false;
        if (ready_) return true;
        if (!SDL_GL_MakeCurrent(linuxapp::Window(), linuxapp::Context())) { Fail(SDL_GetError()); return false; }
        LOGR("OpenGL %s — %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
        LOGR("Linux graphics: render_height=%u fps_cap=%u vsync=%u motion_blur=%u light_streaks=%u",
            app::Get().render_height, app::Get().fps_cap, app::Get().vsync, app::Get().motion_blur, app::Get().light_streaks);
        if (epoxy_gl_version() < 45) { Fail("OpenGL 4.5 is required"); return false; }
        SDL_GL_SetSwapInterval(app::Get().vsync ? 1 : 0);
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        glDisable(GL_DITHER);
        glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glCreateFramebuffers(1, &draw_fbo_);
        glCreateVertexArrays(1, &vao_);
        glCreateBuffers(1, &vbo_); glCreateBuffers(1, &ibo_); glCreateBuffers(2, ubo_);
        glVertexArrayElementBuffer(vao_, ibo_);
        glCreateSamplers(4, samplers_);
        glCreateQueries(GL_SAMPLES_PASSED, 1, &query_);
        glCreateTextures(GL_TEXTURE_2D, 1, &white_);
        glTextureStorage2D(white_, 1, GL_RGBA8, 1, 1);
        uint32_t white = 0xffffffff; glTextureSubImage2D(white_, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        // A presentation LUT preserves the game's brightness setting without
        // changing the desktop gamma or the textures read back by the guest.
        glCreateTextures(GL_TEXTURE_1D, 1, &gamma_texture_);
        glTextureStorage1D(gamma_texture_, 1, GL_RGB8, 256);
        uint8_t ramp[256 * 3];
        for (int i = 0; i < 256; i++) ramp[i * 3] = ramp[i * 3 + 1] = ramp[i * 3 + 2] = uint8_t(i);
        glTextureSubImage1D(gamma_texture_, 0, 0, 256, GL_RGB, GL_UNSIGNED_BYTE, ramp);
        glCreateSamplers(1, &present_sampler_);
        glSamplerParameteri(present_sampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(present_sampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(present_sampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(present_sampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        auto compile = [](GLenum stage, const char* source) {
            GLuint shader = glCreateShader(stage);
            glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
            GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok) { char error[2048]; glGetShaderInfoLog(shader, sizeof(error), nullptr, error); glDeleteShader(shader); throw std::runtime_error(error); }
            return shader;
        };
        try {
            GLuint v = compile(GL_VERTEX_SHADER, "#version 450\nlayout(location=0) out vec2 uv; void main(){uv=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(uv*2.0-1.0,0,1);}");
            GLuint p = compile(GL_FRAGMENT_SHADER, "#version 450\nlayout(location=0) in vec2 uv; layout(location=0) out vec4 color; layout(binding=0) uniform sampler2D scene; layout(binding=1) uniform sampler1D ramp; void main(){vec3 c=texture(scene,uv).rgb;ivec3 i=ivec3(clamp(c,0.0,1.0)*255.0+0.5);color=vec4(texelFetch(ramp,i.r,0).r,texelFetch(ramp,i.g,0).g,texelFetch(ramp,i.b,0).b,1);}");
            present_program_ = LinkProgram(v, p); glDeleteShader(v); glDeleteShader(p);
        } catch (const std::exception& e) { Fail(e.what()); return false; }
        ready_ = true; last_report_ = std::chrono::steady_clock::now();
        return true;
    }
    void Fail(const char* message) {
        LOGE("Linux renderer: %s", message);
        failed_ = true; linuxapp::exit_code = 1; linuxapp::stop = true;
    }
    Target* GetTarget(const Surface& surface) {
        TextureDesc d;
        if (!DescribeSurface(surface, d) || !d.width || !d.height || d.width > 4096 || d.height > 4096) return nullptr;
        auto [it, added] = targets_.try_emplace(TargetKey(d));
        auto& t = it->second;
        if (added) {
            t.desc = d; t.depth = Format(d.format).depth; t.w = std::max(1, Scale(d.width)); t.h = std::max(1, Scale(d.height));
            if (!t.depth && d.width >= 640 && d.height == 480)
                LOGR("Linux render target: guest %ux%u -> %ux%u", d.width, d.height, t.w, t.h);
            glCreateTextures(GL_TEXTURE_2D, 1, &t.texture);
            glTextureStorage2D(t.texture, 1, t.depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8, t.w, t.h);
            glCreateFramebuffers(1, &t.fbo);
            glNamedFramebufferTexture(t.fbo, t.depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0, t.texture, 0);
            if (t.depth) { glNamedFramebufferDrawBuffer(t.fbo, GL_NONE); glNamedFramebufferReadBuffer(t.fbo, GL_NONE); }
            UploadTarget(t);
        } else if (DirtyEpoch(d.data) != t.epoch) { t.desc = d; UploadTarget(t); }
        return &t;
    }
    void UploadTarget(Target& t) {
        GLuint transfer, fbo; Transfer(t, transfer, fbo);
        std::vector<uint8_t> pixels; uint32_t pitch;
        DecodeLevel(t.desc, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(t.desc.data)), nullptr, pixels, pitch);
        if (t.depth) {
            const float* f = reinterpret_cast<const float*>(pixels.data());
            std::vector<uint32_t> packed(size_t(t.desc.width) * t.desc.height);
            for (size_t i = 0; i < packed.size(); i++) packed[i] = uint32_t(std::clamp(f[i], 0.f, 1.f) * 16777215.f) << 8;
            glTextureSubImage2D(transfer, 0, 0, 0, t.desc.width, t.desc.height, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, packed.data());
        } else glTextureSubImage2D(transfer, 0, 0, 0, t.desc.width, t.desc.height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
        glDisable(GL_SCISSOR_TEST);
        glBlitNamedFramebuffer(fbo, t.fbo, 0, 0, t.desc.width, t.desc.height, 0, 0, t.w, t.h,
            t.depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &transfer);
        t.epoch = DirtyEpoch(t.desc.data); t.written = false;
    }
    void BindTargets(const State& s) {
        rt_ = GetTarget(s.rt); zs_ = GetTarget(s.zs);
        Target* extent = rt_ ? rt_ : zs_;
        width_ = extent ? extent->desc.width : 0; height_ = extent ? extent->desc.height : 0;
        if (zs_ && (zs_->desc.width != width_ || zs_->desc.height != height_)) zs_ = nullptr;
        const GLuint color = rt_ ? rt_->texture : 0, depth = zs_ ? zs_->texture : 0;
        // Targets persist for this context; DSA blits and presentation do not alter these attachments.
        if (color != attached_color_) {
            glNamedFramebufferTexture(draw_fbo_, GL_COLOR_ATTACHMENT0, color, 0);
            glNamedFramebufferDrawBuffer(draw_fbo_, color ? GL_COLOR_ATTACHMENT0 : GL_NONE);
            glNamedFramebufferReadBuffer(draw_fbo_, color ? GL_COLOR_ATTACHMENT0 : GL_NONE);
            attached_color_ = color;
        }
        if (depth != attached_depth_) {
            glNamedFramebufferTexture(draw_fbo_, GL_DEPTH_STENCIL_ATTACHMENT, depth, 0);
            attached_depth_ = depth;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo_);
    }
    void BindTexture(const State& s, int stage, TexInfo& info, DrawConstants& dc) {
        TextureDesc d;
        GLuint id = white_;
        if (s.textures[stage] && Describe(s.textures[stage], d)) {
            const auto& format = Format(d.format);
            info.kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
            info.rect = d.linear;
            info.shadow = format.depth ? (format.bits == 16 ? 2 : 1) : 0;
            dc.texsize[stage][0] = d.width; dc.texsize[stage][1] = d.height;
            auto rt = targets_.find(TargetKey(d));
            if (rt != targets_.end() && !d.cube && d.depth == 1) {
                auto& target = rt->second;
                if (DirtyEpoch(d.data) != target.epoch) UploadTarget(target);
                id = target.texture;
                dc.texsize[stage][2] = -.5f / d.width; dc.texsize[stage][3] = -.5f / d.height;
                if (&target == rt_ || &target == zs_) {
                    auto& copy = feedback_[TargetKey(d)];
                    if (!copy) {
                        glCreateTextures(GL_TEXTURE_2D, 1, &copy);
                        glTextureStorage2D(copy, 1, target.depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8, target.w, target.h);
                    }
                    glCopyImageSubData(id, GL_TEXTURE_2D, 0, 0, 0, 0, copy, GL_TEXTURE_2D, 0, 0, 0, 0, target.w, target.h, 1);
                    id = copy;
                }
            } else id = GetTexture(s, stage, d);
        }
        glBindTextureUnit(stage, id);
        const auto& state = s.tss[stage]; const GLuint sampler = samplers_[stage];
        const bool linear = state[kTssMinFilter] != 1;
        GLenum min = linear ? GL_LINEAR : GL_NEAREST;
        if (d.levels > 1 && state[kTssMipFilter]) min = state[kTssMipFilter] == 1 ? (linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST) : (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
        const std::array<uint32_t, 10> key = {Address(state[kTssAddressU]), Address(state[kTssAddressV]),
            Address(state[kTssAddressW]), min, uint32_t(state[kTssMagFilter] == 1 ? GL_NEAREST : GL_LINEAR),
            state[kTssMipLodBias], uint32_t(info.shadow ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE),
            Compare(s.rs[kRsShadowFunc]), state[kTssBorderColor], 1};
        if (sampler_keys_[stage] != key) {
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, key[0]);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, key[1]);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, key[2]);
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, key[3]);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, key[4]);
            glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, Bits(key[5]));
            glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, key[6]);
            glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, key[7]);
            float border[4]; Color(key[8], border); glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
            sampler_keys_[stage] = key;
        }
        glBindSampler(stage, sampler);
        dc.bump[stage][0] = Bits(state[kTssBumpEnvMat00]); dc.bump[stage][1] = Bits(state[kTssBumpEnvMat01]);
        dc.bump[stage][2] = Bits(state[kTssBumpEnvMat10]); dc.bump[stage][3] = Bits(state[kTssBumpEnvMat11]);
        dc.bumpl[stage][0] = Bits(state[kTssBumpEnvLScale]); dc.bumpl[stage][1] = Bits(state[kTssBumpEnvLOffset]);
    }
    GLuint GetTexture(const State& s, int stage, const TextureDesc& d) {
        // Hash fields, never C++ struct padding. Animated palettes update one
        // texture instead of allocating a new GPU resource every frame.
        const uint32_t layout[] = {d.data, d.format, d.width, d.height, d.depth, d.levels, d.pitch, uint32_t(d.cube), uint32_t(d.linear)};
        uint64_t key = Hash(layout, sizeof(layout));
        uint32_t palette[256]{};
        const uint32_t* pal = nullptr;
        if (d.format == 0x0b && s.palettes[stage]) {
            uint32_t va = mem::VirtualFromPhysical(kernel::R32(s.palettes[stage] + 4));
            const uint32_t size_code = kernel::R32(s.palettes[stage]) >> 30;
            std::memcpy(palette, kernel::HostPtr(va), sizeof(palette) >> size_code); pal = palette;
            key = Hash(&va, sizeof(va), key);
        }
        auto& t = textures_[key];
        uint64_t epoch = DirtyEpoch(d.data);
        if (t.texture && t.checked_frame == frame_ && t.epoch == epoch) return t.texture;
        t.checked_frame = frame_; t.epoch = epoch;
        uint32_t bytes = TotalSize(d);
        if (bytes > (64u << 20)) throw std::runtime_error("oversized guest texture");
        const auto* src = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig = Hash(src, bytes);
        if (pal) sig = Hash(palette, sizeof(palette), sig);
        if (t.texture && t.signature == sig) return t.texture;
        const auto& f = Format(d.format);
        GLenum storage = GL_RGBA8;
        if (f.host == Host::BC1) storage = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        if (f.host == Host::BC2) storage = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        if (f.host == Host::BC3) storage = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        if (f.depth) storage = GL_DEPTH_COMPONENT32F;
        if (!t.texture) {
            t.type = d.cube ? GL_TEXTURE_CUBE_MAP : d.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
            glCreateTextures(t.type, 1, &t.texture);
            if (t.type == GL_TEXTURE_3D) glTextureStorage3D(t.texture, d.levels, storage, d.width, d.height, d.depth);
            else glTextureStorage2D(t.texture, d.levels, storage, d.width, d.height);
        }
        std::vector<uint8_t> pixels;
        for (uint32_t face = 0; face < (d.cube ? 6u : 1u); face++) for (uint32_t level = 0; level < d.levels; level++) {
            uint32_t pitch; DecodeLevel(d, level, face, src, pal, pixels, pitch);
            const auto w = std::max(1u, d.width >> level), h = std::max(1u, d.height >> level), z = std::max(1u, d.depth >> level);
            if (t.type == GL_TEXTURE_2D) {
                if (f.compressed) glCompressedTextureSubImage2D(t.texture, level, 0, 0, w, h, storage, pixels.size(), pixels.data());
                else glTextureSubImage2D(t.texture, level, 0, 0, w, h, f.depth ? GL_DEPTH_COMPONENT : GL_BGRA, f.depth ? GL_FLOAT : GL_UNSIGNED_BYTE, pixels.data());
            } else {
                if (f.compressed) glCompressedTextureSubImage3D(t.texture, level, 0, 0, d.cube ? face : 0, w, h, d.cube ? 1 : z, storage, pixels.size(), pixels.data());
                else glTextureSubImage3D(t.texture, level, 0, 0, d.cube ? face : 0, w, h, d.cube ? 1 : z, f.depth ? GL_DEPTH_COMPONENT : GL_BGRA, f.depth ? GL_FLOAT : GL_UNSIGNED_BYTE, pixels.data());
            }
        }
        t.signature = sig;
        return t.texture;
    }
    void Readback(Target& t) {
        PerfScope read_time(perf_readback_); perf_reads_++;
        const auto& d = t.desc;
        std::vector<uint32_t> pixels(size_t(d.width) * d.height);
        GLuint transfer, fbo; Transfer(t, transfer, fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitNamedFramebuffer(t.fbo, fbo, 0, 0, t.w, t.h, 0, 0, d.width, d.height,
            t.depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glGetTextureImage(transfer, 0, t.depth ? GL_DEPTH_STENCIL : GL_BGRA, t.depth ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_BYTE,
                          pixels.size() * 4, pixels.data());
        glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &transfer);
        auto* dst = static_cast<uint8_t*>(kernel::HostPtr(d.data));
        const uint32_t bits = Format(d.format).bits;
        for (uint32_t y = 0; y < d.height; y++) for (uint32_t x = 0; x < d.width; x++) {
            uint32_t v = pixels[y * d.width + x];
            if (bits == 16) {
                uint16_t c = t.depth ? uint16_t(v >> 16) : uint16_t(((v >> 19) & 31) << 11 | ((v >> 10) & 63) << 5 | ((v >> 3) & 31));
                std::memcpy(dst + PixelOffset(d, x, y), &c, 2);
            } else std::memcpy(dst + PixelOffset(d, x, y), &v, 4);
        }
        t.written = false;
    }
    void Capture(int width, int height) {
        if (width <= 0 || height <= 0) return;
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadBuffer(GL_BACK);
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
        const auto dir = app::ExeDir() / "captures"; std::filesystem::create_directories(dir);
        const auto path = dir / ("linux_" + std::to_string(frame_ + 1) + ".tga");
        std::ofstream out(path, std::ios::binary);
        uint8_t header[18] = {0, 0, 2};
        header[12] = uint8_t(width); header[13] = uint8_t(width >> 8);
        header[14] = uint8_t(height); header[15] = uint8_t(height >> 8); header[16] = 32; header[17] = 0x08;
        out.write(reinterpret_cast<char*>(header), sizeof(header)); out.write(reinterpret_cast<char*>(pixels.data()), pixels.size());
        LOGR("Linux screenshot: %s", path.string().c_str());
    }
    bool ready_ = false, failed_ = false;
    bool constants_valid_ = false;
    decltype(State::vsc) last_vsc_{};
    DrawConstants last_dc_{};
    std::array<std::array<uint32_t, 10>, 4> sampler_keys_{};
    GLuint attached_color_ = ~0u, attached_depth_ = ~0u;
    GLuint vao_ = 0, vbo_ = 0, ibo_ = 0, ubo_[2]{}, draw_fbo_ = 0, samplers_[4]{}, white_ = 0, query_ = 0;
    GLuint present_program_ = 0, present_sampler_ = 0, gamma_texture_ = 0;
    uint32_t width_ = 0, height_ = 0;
    uint64_t frame_ = 0, draws_ = 0, glare_skipped_ = 0, glare_world_skipped_ = 0, lamp_beams_skipped_ = 0, blur_skipped_ = 0;
    uint64_t perf_draw_ = 0, perf_query_ = 0, perf_readback_ = 0, perf_queries_ = 0, perf_reads_ = 0;
    double samples_ = 0;
    Target *rt_ = nullptr, *zs_ = nullptr;
    Assembled assembled_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Texture> textures_;
    std::unordered_map<uint64_t, GLuint> feedback_, shaders_, programs_;
    std::chrono::steady_clock::time_point last_report_;
};
}
std::unique_ptr<Backend> CreateVulkanBackend();
std::unique_ptr<Backend> CreateBackend() {
    if (app::Get().linux_renderer == "vulkan") return CreateVulkanBackend();
    return std::make_unique<OpenGL>();
}
}
