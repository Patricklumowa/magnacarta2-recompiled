#include "texture_decode.h"

#include <algorithm>
#include <cstring>

#include <rex/graphics/pipeline/texture/util.h>

namespace mc2::texture {
namespace {
namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
constexpr uint64_t kMaxBytes = 64 * 1024 * 1024;

bool Fail(std::string& error, const char* text) {
  error = text;
  return false;
}

uint64_t ByteOffset(const BaseRGBA8& description, uint32_t x, uint32_t y) {
  x += description.offset_x;
  y += description.offset_y;
  if (description.tiled) {
    return uint32_t(texture_util::GetTiledOffset2D(
        int32_t(x), int32_t(y), description.row_pitch_bytes / description.bytes_per_texel,
        description.bytes_per_texel == 4 ? 2 : 0));
  }
  return uint64_t(y) * description.row_pitch_bytes + uint64_t(x) * description.bytes_per_texel;
}
constexpr uint32_t kEndianXor[] = {0, 1, 3, 2};
uint64_t TexelEnd(const BaseRGBA8& description, uint32_t x, uint32_t y) {
  const auto offset = ByteOffset(description, x, y);
  if (description.bytes_per_texel == 4) return offset + 4;
  // Byte swapping may read a neighbour outside a partial final DWORD.
  return (offset ^ kEndianXor[description.endianness]) + 1;
}
}  // namespace

bool DescribeBaseRGBA8(std::span<const uint8_t> fetch_be, BaseRGBA8& output,
                       std::string& error) {
  error.clear();
  if (fetch_be.size() != 24) return Fail(error, "Texture fetch must contain exactly 24 big-endian bytes");
  BaseRGBA8 result;
  for (size_t index = 0; index < 6; ++index) {
    const auto* bytes = fetch_be.data() + index * 4;
    result.fetch_words[index] = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
                                uint32_t(bytes[2]) << 8 | bytes[3];
  }
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, result.fetch_words.data(), sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture)
    return Fail(error, "Fetch constant is not a texture");
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked)
    return Fail(error, "Only non-array 2D textures are supported by the first replay proof");
  if (fetch.format != xenos::TextureFormat::k_8_8_8_8 && fetch.format != xenos::TextureFormat::k_8)
    return Fail(error, "Unsupported texture format: replay supports format 2 (8) and 6 (8_8_8_8)");
  result.format = uint32_t(fetch.format);
  result.bytes_per_texel = fetch.format == xenos::TextureFormat::k_8 ? 1 : 4;
  if (fetch.num_format)
    return Fail(error, "Integer texture interpretation is not supported by the first replay proof");
  if (!fetch.pitch) return Fail(error, "Zero texture pitch");
  result.width = fetch.size_2d.width + 1;
  result.height = fetch.size_2d.height + 1;
  if (uint64_t(result.width) * result.height * 4 > kMaxBytes)
    return Fail(error, "Texture exceeds the 64 MiB replay capture limit");
  uint32_t base_page = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
      &base_page, nullptr, &result.mip_min_level, &result.mip_max_level);
  if (!base_page) return Fail(error, "Texture has no accessible base level");
  result.physical_address = base_page << 12;
  result.tiled = fetch.tiled;
  result.endianness = uint32_t(fetch.endianness);
  result.swizzle = fetch.swizzle;
  result.exp_adjust = fetch.exp_adjust;
  const uint32_t signs[] = {uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
                            uint32_t(fetch.sign_z), uint32_t(fetch.sign_w)};
  for (uint32_t component = 0; component < 4; ++component) {
    const uint32_t source_component = (result.swizzle >> (component * 3)) & 7;
    if (source_component >= 6) return Fail(error, "Reserved texture swizzle selector");
    if (source_component >= 4) continue;
    if (signs[source_component] == uint32_t(xenos::TextureSign::kGamma))
      result.gamma_mask |= 1u << component;
    else if (signs[source_component] != uint32_t(xenos::TextureSign::kUnsigned))
      return Fail(error, "Signed/biased texture samples require a different native view or shader");
  }
  const auto layout = texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch,
      result.width, result.height, 1, result.tiled, fetch.format, fetch.packed_mips, true, 0);
  result.row_pitch_bytes = layout.base.row_pitch_bytes;
  if (fetch.packed_mips) {
    uint32_t z = 0;
    texture_util::GetPackedMipOffset(result.width, result.height, 1, fetch.format, 0,
                                     result.offset_x, result.offset_y, z);
  }
  // Compute the exact exclusive bound of sampled base texels. The SDK layout's
  // allocation estimate may include tile padding that isn't mapped by a title.
  uint64_t extent = 0;
  if (result.tiled || (result.bytes_per_texel == 1 && result.endianness != 0)) {
    for (uint32_t y = 0; y < result.height; ++y)
      for (uint32_t x = 0; x < result.width; ++x)
        extent = std::max(extent, TexelEnd(result, x, y));
  } else {
    extent = ByteOffset(result, result.width - 1, result.height - 1) + result.bytes_per_texel;
  }
  if (!extent || extent > kMaxBytes || uint64_t(result.physical_address) + extent > 0x20000000ull)
    return Fail(error, "Texture base span exceeds capture or physical-memory bounds");
  result.source_bytes = uint32_t(extent);
  output = result;
  return true;
}

bool DecodeBaseRGBA8(const BaseRGBA8& description, std::span<const uint8_t> guest_bytes,
                    std::vector<uint8_t>& rgba, std::string& error) {
  // Reconstruct and validate from the canonical raw words rather than trusting
  // mutable size/offset fields supplied by a caller or capture manifest.
  std::array<uint8_t, 24> fetch_be;
  for (size_t i = 0; i < description.fetch_words.size(); ++i)
    for (uint32_t byte = 0; byte < 4; ++byte)
      fetch_be[i * 4 + byte] = uint8_t(description.fetch_words[i] >> (24 - byte * 8));
  BaseRGBA8 validated;
  if (!DescribeBaseRGBA8(fetch_be, validated, error)) return false;
  if (guest_bytes.size() < validated.source_bytes)
    return Fail(error, "Truncated texture payload");
  std::vector<uint8_t> pixels(size_t(validated.width) * validated.height * 4);
  // Movie planes are linear 8-bit textures with identical RGB channels and
  // constant alpha. Copy each row without per-channel swizzle or address math.
  if (!validated.tiled && validated.bytes_per_texel == 1 && !validated.endianness && validated.swizzle == 0xa00) {
    for (uint32_t y = 0; y < validated.height; ++y) {
      const auto* row = guest_bytes.data() + ByteOffset(validated, 0, y);
      auto* pixel = pixels.data() + size_t(y) * validated.width * 4;
      for (uint32_t x = 0; x < validated.width; ++x, pixel += 4) {
        pixel[0] = pixel[1] = pixel[2] = row[x];
        pixel[3] = 255;
      }
    }
    rgba = std::move(pixels);
    return true;
  }
  // Endian modes swap bytes within 16 bits, within 32 bits, or 16-bit halves.
  for (uint32_t y = 0; y < validated.height; ++y) {
    for (uint32_t x = 0; x < validated.width; ++x) {
      const auto offset = ByteOffset(validated, x, y);
      auto* pixel = pixels.data() + (size_t(y) * validated.width + x) * 4;
      for (uint32_t component = 0; component < 4; ++component) {
        const uint32_t selector = (validated.swizzle >> (component * 3)) & 7;
        // Xenos format 8 replicates its single component before fetch swizzle.
        const auto source = (offset + (validated.bytes_per_texel == 1 ? 0 : selector)) ^
                            kEndianXor[validated.endianness];
        pixel[component] = selector < 4 ? guest_bytes[source]
                                      : selector == 4 ? 0 : 255;
      }
    }
  }
  rgba = std::move(pixels);
  return true;
}
}  // namespace mc2::texture
