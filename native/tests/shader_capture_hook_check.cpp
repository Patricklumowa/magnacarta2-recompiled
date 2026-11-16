#include "generated/default/magnacarta2_init.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
constexpr size_t kMemorySize = 16384;
PPCContext expected_entry;
std::array<uint8_t, kMemorySize> expected_memory;
constexpr uint32_t kHighGuest = 0xFF5F6000u;
std::array<uint8_t, 4096> expected_high;
uint8_t* high_memory = nullptr;
unsigned original_calls = 0;
unsigned failures = 0;

void Original(PPCContext& ctx, uint8_t* base) {
  ++original_calls;
  if (std::memcmp(&ctx, &expected_entry, sizeof(ctx)) ||
      std::memcmp(base, expected_memory.data(), expected_memory.size()) ||
      (high_memory && std::memcmp(high_memory, expected_high.data(), expected_high.size()))) ++failures;
  ctx.r3.u64 = 0x123456789ABCDEFull;
  ctx.r4.u64 ^= 0xFEDCBA9876543210ull;
  ctx.lr += 4;
  base[1024] = 0x5A;
}
}  // namespace

REX_HOOK_RAW(__imp__sub_823724C8) { Original(ctx, base); }
REX_HOOK_RAW(__imp__sub_82372120) { Original(ctx, base); }
REX_HOOK_RAW(__imp__sub_823725B8) { Original(ctx, base); }
REX_HOOK_RAW(__imp__sub_82372210) { Original(ctx, base); }

int main() {
#ifndef _WIN32
  std::puts("Shader capture hook check requires Windows mapping validation");
  return 77;
#else
  // Reserve address space only; commit 20 KiB. A real high guest alias checks
  // the Windows +4 KiB translation rather than duplicating its arithmetic in
  // a mocked Readable implementation.
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(
      nullptr, 0x100001000ull, MEM_RESERVE, PAGE_NOACCESS));
  if (!memory) return 1;
  high_memory = memory + uint64_t(kHighGuest) + 0x1000;
  if (!VirtualAlloc(memory, kMemorySize, MEM_COMMIT, PAGE_READWRITE) ||
      !VirtualAlloc(high_memory, expected_high.size(), MEM_COMMIT, PAGE_READWRITE)) {
    VirtualFree(memory, 0, MEM_RELEASE);
    return 1;
  }
  const auto root = std::filesystem::temp_directory_path() /
      ("mc2-shader-capture-check-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  for (bool enabled : {false, true}) {
    rex::cvar::SetFlagByName("mc2_shader_capture_dir", enabled ? root.string() : "");
    for (bool binding : {false, true}) {
      for (bool vertex : {false, true}) {
        for (int input = 0; input < 7; ++input) {
          expected_memory.fill(0xA5);
          expected_high.fill(0xA5);
          const std::array<uint8_t, 12> header{0x10, 0x2A, 0x11, 1,
                                             0, 0, 0, 36, 0, 0, 0, 12};
          const auto header_address = 64 + (binding ? (vertex ? 872 : 40) : 0);
          std::memcpy(expected_memory.data() + header_address, header.data(), header.size());
          if (input == 2) expected_memory[header_address + 11] = 0;
          if (binding) {
            // Resource fields are separate from the preserved virtual container;
            // the physical allocation is deliberately non-contiguous.
            std::memset(expected_memory.data() + 64, 0, 36);
            expected_memory[67] = input == 5 ? 2 : (vertex ? 6 : 7);
            const auto physical_field = 64 + (vertex ? 32 : 24);
            if (input == 6) {
              expected_memory[physical_field] = 0xFF;
              expected_memory[physical_field + 1] = 0x5F;
              expected_memory[physical_field + 2] = 0x60;
            } else {
              expected_memory[physical_field + 2] = 0x10;  // BE address 4096.
            }
          } else if (input == 5) {
            expected_memory[header_address + 8] = 0xFF;
          } else if (input == 6) {
            std::memcpy(expected_high.data(), header.data(), header.size());
          }
          std::memcpy(memory, expected_memory.data(), expected_memory.size());
          std::memcpy(high_memory, expected_high.data(), expected_high.size());
          std::memset(&expected_entry, 0, sizeof(expected_entry));
          const auto pointer = input == 3 ? 0xDFFFFFF0u : (input == 4 ? 0u :
                               (input == 6 && !binding ? kHighGuest : 64u));
          expected_entry.r3.u64 = binding ? 2048 : pointer;
          expected_entry.r4.u64 = binding ? pointer : 0x8123456789ABCDEFull;
          expected_entry.lr = 0x8248C81C;
          expected_entry.ctr.u64 = 0x773311;
          std::memset(expected_entry.v59.u8, 0xB5, sizeof(PPCVRegister));
          PPCContext ctx;
          std::memcpy(&ctx, &expected_entry, sizeof(ctx));
          if (binding) {
            if (vertex) sub_823725B8(ctx, memory);
            else sub_82372210(ctx, memory);
          } else {
            if (vertex) sub_823724C8(ctx, memory);
            else sub_82372120(ctx, memory);
          }
          auto result_memory = expected_memory;
          result_memory[1024] = 0x5A;
          PPCContext result;
          std::memcpy(&result, &expected_entry, sizeof(result));
          result.r3.u64 = 0x123456789ABCDEFull;
          result.r4.u64 ^= 0xFEDCBA9876543210ull;
          result.lr += 4;
          if (std::memcmp(&ctx, &result, sizeof(ctx)) ||
              std::memcmp(memory, result_memory.data(), result_memory.size()) ||
              std::memcmp(high_memory, expected_high.data(), expected_high.size())) ++failures;
        }
      }
    }
    if (!enabled && std::filesystem::exists(root)) ++failures;
  }
  unsigned containers = 0, indices = 0;
  if (!std::filesystem::exists(root)) {
    ++failures;
  } else {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (entry.path().extension() == ".container") {
        ++containers;
        if (std::filesystem::file_size(entry.path()) != 48) ++failures;
      } else if (entry.path().filename() == "index.csv") {
        ++indices;
        std::ifstream index(entry.path());
        std::string line;
        unsigned lines = 0;
        while (std::getline(index, line)) ++lines;
        if (lines != 7) ++failures;  // Header, low/high creation and binding per stage.
      }
    }
  }
  if (original_calls != 56 || containers != 2 || indices != 1) ++failures;
  VirtualFree(memory, 0, MEM_RELEASE);
  std::printf("Shader capture hook checks: %u failures, %u original calls, "
              "%u unique containers\nLocal fixtures: %s\n",
              failures, original_calls, containers, root.string().c_str());
  return failures ? 1 : 0;
#endif
}
