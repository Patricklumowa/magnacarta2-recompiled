#pragma once

#include "shader_capture.h"
#include <rex/system/xmemory.h>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

namespace mc2::capture {
#ifdef _WIN32
inline bool ReadableProtection(DWORD protection) {
  constexpr DWORD read_flags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
      PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (protection & read_flags) && !(protection & (PAGE_NOACCESS | PAGE_GUARD));
}

inline bool ReadableResidentPages(uintptr_t start, uintptr_t end) {
  static const size_t page_size = [] {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
  }();
  constexpr size_t max_pages = 256;
  const auto first_page = start - start % page_size;
  const auto count = (end - 1 - first_page) / page_size + 1;
  if (count > max_pages) return false;

  PSAPI_WORKING_SET_EX_INFORMATION pages[max_pages];
  for (size_t i = 0; i < count; ++i)
    pages[i] = {reinterpret_cast<void*>(first_page + i * page_size), {}};
  if (!K32QueryWorkingSetEx(GetCurrentProcess(), pages, DWORD(count * sizeof(pages[0])))) return false;
  for (size_t i = 0; i < count; ++i) {
    const auto page = pages[i].VirtualAttributes;
    if (!page.Valid || page.Bad || !ReadableProtection(DWORD(page.Win32Protection))) return false;
  }
  return true;
}
#endif

inline bool Readable(const uint8_t* pointer, size_t length) {
#ifdef _WIN32
  auto cursor = reinterpret_cast<uintptr_t>(pointer);
  if (length > UINTPTR_MAX - cursor) return false;
  const auto end = cursor + length;
  // Use per-page protection for resident data. The region query still
  // handles nonresident pages and ranges that exceed the bounded batch.
  if (cursor < end && ReadableResidentPages(cursor, end)) return true;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || !ReadableProtection(region.Protect)) return false;
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
