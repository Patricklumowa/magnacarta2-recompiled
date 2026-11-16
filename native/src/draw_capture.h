#pragma once

#include "shader_capture.h"
#include <cstdint>

namespace mc2::draw_capture {
inline constexpr uint32_t kDeviceBytes = 24064;
inline constexpr uint64_t kArtifactLimit = 16 * 1024 * 1024;
inline constexpr uint64_t kSessionLimit = 256 * 1024 * 1024;

inline bool Selected(uint64_t ordinal, uint32_t skip, uint32_t requested_count) {
  const uint32_t count = requested_count == 0 ? 1 : (requested_count > 32 ? 32 : requested_count);
  return ordinal >= skip && ordinal - skip < count;
}

inline bool UpVertexBytes(uint32_t address, uint32_t count, uint32_t stride, uint32_t& bytes) {
  const uint64_t length = uint64_t(count) * stride;
  if (length > kArtifactLimit || !mc2::capture::GuestRange(address, length)) return false;
  bytes = static_cast<uint32_t>(length);
  return true;
}

inline bool DeclarationBytes(const uint8_t* header, size_t available, uint32_t& bytes) {
  if (!header || available < 28 || (mc2::capture::LoadBE32(header) & 15) != 5) return false;
  const uint32_t count = mc2::capture::LoadBE32(header + 24);
  // The initializer counts terminated source elements, then allocates
  // 56+12*count and copies them at +52. A conservative cap bounds damaged data.
  if (count > 256) return false;
  bytes = 56 + count * 12;
  return true;
}

inline bool TexturePhysicalAlias(uint32_t physical, uint64_t length, uint32_t& guest) {
  constexpr uint64_t limit = 0x20000000;
  if (physical >= limit || !length || length > kArtifactLimit || length > limit - physical) return false;
  guest = 0xA0000000u + physical;
  return true;
}

inline bool BufferBytes(const uint8_t* descriptor, size_t available, bool vertex,
                        uint32_t& address, uint32_t& bytes) {
  if (!descriptor || available < 32 ||
      (mc2::capture::LoadBE32(descriptor) & 15) != (vertex ? 1u : 2u)) return false;
  address = mc2::capture::LoadBE32(descriptor + 24);
  bytes = mc2::capture::LoadBE32(descriptor + 28);
  if (vertex) {
    // The vertex resource embeds a fetch: byte address/type, then endian and
    // a 24-bit size in dwords. Its address is still a guest virtual alias.
    if ((address & 3) != 3) return false;
    address &= ~3u;
    bytes = ((bytes >> 2) & 0xFFFFFFu) * 4;
  }
  return bytes <= kArtifactLimit && mc2::capture::GuestRange(address, bytes);
}
}  // namespace mc2::draw_capture
