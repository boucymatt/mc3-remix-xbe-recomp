#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace gpu::linuxvk {
std::vector<uint32_t> CompileSpirv(const std::string &source, bool vertex);
}
