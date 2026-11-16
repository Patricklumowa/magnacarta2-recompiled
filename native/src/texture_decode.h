#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mc2::texture {

// Base level only. Fetch words remain available for sampler/shader semantics.
// Bytes returned by DecodeBaseRGBA8 have the fetch swizzle already applied.
// Gamma is intentionally NOT applied: Xenos uses a piecewise-linear transfer,
// not the standard sRGB transfer. gamma_mask refers to output RGBA channels.
struct BaseRGBA8 {
  std::array<uint32_t, 6> fetch_words{};
  uint32_t width = 0, height = 0;
  uint32_t physical_address = 0, source_bytes = 0, row_pitch_bytes = 0;
  uint32_t offset_x = 0, offset_y = 0;
  uint32_t format = 0, bytes_per_texel = 0;
  uint32_t endianness = 0, swizzle = 0, gamma_mask = 0;
  uint32_t mip_min_level = 0, mip_max_level = 0;
  int32_t exp_adjust = 0;
  bool tiled = false;
};

// Takes six raw big-endian fetch DWORDs, not a 52-byte guest resource object.
// Rejects formats/dimensions/sign modes outside this first replay proof.
bool DescribeBaseRGBA8(std::span<const uint8_t> fetch_be, BaseRGBA8& output,
                       std::string& error);

// guest_bytes starts at physical_address. No live guest-memory reads occur here.
// On failure, output is unchanged and error explains the unsupported input.
bool DecodeBaseRGBA8(const BaseRGBA8& description, std::span<const uint8_t> guest_bytes,
                    std::vector<uint8_t>& rgba, std::string& error);

}  // namespace mc2::texture
