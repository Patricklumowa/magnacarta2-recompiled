#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace mc2::native {
std::vector<uint8_t> ReadNativeFile(const std::filesystem::path& path);
std::vector<uint8_t> ReadBoundShader(uint8_t* base, uint32_t resource, bool vertex);
}
