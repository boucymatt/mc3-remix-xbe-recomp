// Native Vulkan shader modules use the same console HLSL as the other backends.
#include "vulkan_shader.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross.hpp>
#include <stdexcept>
#include <unordered_map>
namespace gpu::linuxvk {
std::vector<uint32_t> CompileSpirv(const std::string &source, bool vertex) {
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
    options.SetBindingBase(shaderc_uniform_kind_texture, 2);
    options.SetBindingBase(shaderc_uniform_kind_sampler, 6);
    auto result = compiler.CompileGlslToSpv(hlsl, vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                                            "console.hlsl", "main", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        throw std::runtime_error("Vulkan HLSL compilation: " + result.GetErrorMessage());
    std::vector<uint32_t> words(result.cbegin(), result.cend());
    spirv_cross::Compiler reflection(words);
    std::unordered_map<uint32_t, uint32_t> locations;
    auto varying = [&](const auto &r) {
        const auto dot = r.name.find_last_of('.');
        const auto field = r.name.substr(dot == std::string::npos ? 0 : dot + 1);
        const char *names[] = {"d0", "d1", "b0", "b1", "tc0", "tc1", "tc2", "tc3", "fogamt", "e"};
        for (unsigned i = 0; i < 10; ++i)
            if (field == names[i])
                locations[r.id] = i;
    };
    const auto resources = reflection.get_shader_resources();
    for (const auto &r : resources.stage_inputs) {
        if (vertex) {
            const auto pos = r.name.find_last_of('v');
            if (pos != std::string::npos && pos + 1 < r.name.size() && std::isdigit(r.name[pos + 1]))
                locations[r.id] = std::stoul(r.name.substr(pos + 1));
        } else
            varying(r);
    }
    if (vertex)
        for (const auto &r : resources.stage_outputs)
            varying(r);
    // Edit the binary decorations, not only SPIRV-Cross's reflection metadata.
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, op = words[i] & 65535;
        if (!count || i + count > words.size())
            throw std::runtime_error("Invalid SPIR-V instruction");
        if (op == spv::OpDecorate && count >= 4 && words[i + 2] == spv::DecorationLocation) {
            auto it = locations.find(words[i + 1]);
            if (it != locations.end())
                words[i + 3] = it->second;
        }
        i += count;
    }
    return words;
}
} // namespace gpu::linuxvk
