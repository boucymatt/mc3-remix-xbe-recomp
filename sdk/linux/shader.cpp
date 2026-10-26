// Keep the console shader translator as the source of truth; lower its HLSL through SPIR-V to OpenGL.
#include "shader.h"
#include "gpu/shaders.h"
#include "app/app.h"
#include "core/log.h"
#include <shaderc/shaderc.hpp>
#include <spirv_glsl.hpp>
#include <fstream>
#include <stdexcept>

namespace gpu::linuxgl {
namespace {
int VaryingLocation(const std::string& name) {
    const auto dot = name.find_last_of('.');
    const auto field = name.substr(dot == std::string::npos ? 0 : dot + 1);
    const char* fields[] = {"d0", "d1", "b0", "b1", "tc0", "tc1", "tc2", "tc3", "fogamt", "e"};
    for (int i = 0; i < 10; i++) if (field == fields[i]) return i;
    return -1;
}
}
GLuint CompileShader(const std::string& source, GLenum stage) {
    std::string hlsl = source;
    for (size_t at = 0; (at = hlsl.find("1.#INF", at)) != std::string::npos;)
        hlsl.replace(at, 6, "asfloat(0x7f800000u)");
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetSourceLanguage(shaderc_source_language_hlsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    options.SetHlslIoMapping(true);
    options.SetAutoMapLocations(true);
    options.SetAutoBindUniforms(true);
    auto result = compiler.CompileGlslToSpv(hlsl, stage == GL_VERTEX_SHADER ? shaderc_vertex_shader : shaderc_fragment_shader,
                                           "console.hlsl", "main", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        std::filesystem::create_directories(app::ExeDir() / "logs");
        std::ofstream(app::ExeDir() / "logs/failed_shader.hlsl") << hlsl;
        throw std::runtime_error("HLSL compilation: " + result.GetErrorMessage());
    }
    spirv_cross::CompilerGLSL cross(std::vector<uint32_t>(result.cbegin(), result.cend()));
    auto resources = cross.get_shader_resources();
    for (auto& r : resources.uniform_buffers) {
        cross.unset_decoration(r.id, spv::DecorationDescriptorSet);
        cross.set_decoration(r.id, spv::DecorationBinding, r.name == "VC" ? 0 : 1);
    }
    for (auto& r : resources.stage_inputs) {
        if (stage == GL_VERTEX_SHADER) {
            const auto pos = r.name.find_last_of('v');
            if (pos != std::string::npos) cross.set_decoration(r.id, spv::DecorationLocation, std::stoul(r.name.substr(pos + 1)));
        } else if (int loc = VaryingLocation(r.name); loc >= 0) cross.set_decoration(r.id, spv::DecorationLocation, loc);
    }
    if (stage == GL_VERTEX_SHADER)
        for (auto& r : resources.stage_outputs)
            if (int loc = VaryingLocation(r.name); loc >= 0) cross.set_decoration(r.id, spv::DecorationLocation, loc);
    cross.build_dummy_sampler_for_combined_images();
    cross.build_combined_image_samplers();
    for (auto& c : cross.get_combined_image_samplers()) {
        const uint32_t binding = cross.get_decoration(c.image_id, spv::DecorationBinding);
        cross.set_decoration(c.combined_id, spv::DecorationBinding, binding);
        cross.set_name(c.combined_id, "console_texture" + std::to_string(binding));
    }
    auto opts = cross.get_common_options();
    opts.version = 450;
    opts.es = false;
    opts.enable_420pack_extension = true;
    cross.set_common_options(opts);
    const auto glsl = cross.compile();
    const char* text = glsl.c_str();
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char error[8192]; glGetShaderInfoLog(shader, sizeof(error), nullptr, error);
        glDeleteShader(shader);
        std::filesystem::create_directories(app::ExeDir() / "logs");
        std::ofstream(app::ExeDir() / "logs/failed_shader.glsl") << glsl;
        throw std::runtime_error(std::string("GLSL compilation: ") + error);
    }
    return shader;
}
GLuint LinkProgram(GLuint vertex, GLuint fragment) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char error[8192]; glGetProgramInfoLog(program, sizeof(error), nullptr, error);
        glDeleteProgram(program);
        throw std::runtime_error(std::string("OpenGL program link: ") + error);
    }
    return program;
}
}
