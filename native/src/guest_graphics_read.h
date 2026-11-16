#pragma once

#include "shader_capture.h"
#include <rex/system/xmemory.h>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace mc2::capture {
inline bool Readable(const uint8_t* pointer, size_t length) {
#ifdef _WIN32
  auto cursor = reinterpret_cast<uintptr_t>(pointer);
  if (length > UINTPTR_MAX - cursor) return false;
  const auto end = cursor + length;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    constexpr auto read_flags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(region.Protect & read_flags)) return false;
    const auto start = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (region.RegionSize > UINTPTR_MAX - start || start + region.RegionSize <= cursor) return false;
    cursor = start + region.RegionSize;
  }
  return true;
#else
  (void)pointer;
  (void)length;
  return false;
#endif
}

inline bool ReadGuest(uint8_t* base, uint32_t address, uint64_t length, std::vector<uint8_t>& bytes) {
  if (length > kMaxContainerBytes || !mc2::capture::GuestRange(address, length)) return false;
  const auto* source = rex::memory::GuestPtr(base, address);
  if (!Readable(source, static_cast<size_t>(length))) return false;
  bytes.resize(static_cast<size_t>(length));
  std::memcpy(bytes.data(), source, bytes.size());
  return true;
}

}  // namespace mc2::capture
