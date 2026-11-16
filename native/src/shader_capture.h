#pragma once

#include <cstddef>
#include <cstdint>

namespace mc2::capture {
inline constexpr uint64_t kMaxContainerBytes = 16 * 1024 * 1024;
inline constexpr uint64_t kGuestMappingLimit = 0x100000000ull;

inline bool GuestRange(uint32_t address, uint64_t length) {
  if (!address || !length || length > kGuestMappingLimit - address) return false;
  const uint64_t end = uint64_t(address) + length;
  // E0 physical aliases have a host offset on Windows. One contiguous read
  // cannot straddle the point where that translation changes.
  return !(address < 0xE0000000u && end > 0xE0000000ull);
}

struct ContainerSize {
  uint32_t virtual_bytes;
  uint32_t physical_bytes;
  uint32_t total_bytes;
};

inline uint32_t LoadBE32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
         uint32_t(bytes[2]) << 8 | uint32_t(bytes[3]);
}

// Only bounds are established here. Instruction/metadata validity is checked
// by the offline probe; capture must preserve unfamiliar shader features.
inline bool ReadContainerSize(const uint8_t* bytes, size_t available,
                              uint32_t guest_address, ContainerSize& result,
                              bool contiguous = true) {
  if (!bytes || available < 12 || !guest_address) return false;
  const auto virtual_bytes = LoadBE32(bytes + 4);
  const auto physical_bytes = LoadBE32(bytes + 8);
  const uint64_t total = uint64_t(virtual_bytes) + physical_bytes;
  if (virtual_bytes < 36 || !physical_bytes || total > kMaxContainerBytes ||
      !GuestRange(guest_address, contiguous ? total : virtual_bytes)) return false;
  result = {virtual_bytes, physical_bytes, static_cast<uint32_t>(total)};
  return true;
}

inline uint64_t ContentHash(const uint8_t* bytes, size_t length) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
}  // namespace mc2::capture
