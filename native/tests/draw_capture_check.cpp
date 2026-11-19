#include "generated/default/magnacarta2_init.h"
#include "../src/draw_capture.h"
#include "../src/guest_graphics_read.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
constexpr uint32_t kDevice = 256;
constexpr size_t kMemorySize = 65536;
constexpr uint32_t kTextureAlias = 0xA0100000;
constexpr size_t kTextureBytes = 4096;
std::array<uint8_t, kMemorySize> expected_memory;
std::array<uint8_t, kTextureBytes> expected_texture;
PPCContext expected_context;
unsigned failures = 0;
unsigned original_calls = 0;

void Check(bool result) { if (!result) ++failures; }
void StoreBE32(uint32_t offset, uint32_t value) {
  expected_memory[offset] = value >> 24;
  expected_memory[offset + 1] = value >> 16;
  expected_memory[offset + 2] = value >> 8;
  expected_memory[offset + 3] = value;
}
void Original(PPCContext& ctx, uint8_t* base) {
  ++original_calls;
  Check(!std::memcmp(&ctx, &expected_context, sizeof(ctx)));
  Check(!std::memcmp(base, expected_memory.data(), expected_memory.size()));
  Check(!std::memcmp(base + kTextureAlias, expected_texture.data(), expected_texture.size()));
  ctx.r3.u64 = 0x123456789ABCDEF0ull;
  ctx.r9.u64 ^= 0xAABBCCDD12345678ull;
  base[40] = 0x7F;
}
void Invoke(PPCFunc* function, uint8_t* memory, bool bad_up = false) {
  std::memcpy(memory, expected_memory.data(), expected_memory.size());
  std::memcpy(memory + kTextureAlias, expected_texture.data(), expected_texture.size());
  std::memset(&expected_context, 0, sizeof(expected_context));
  expected_context.r3.u64 = kDevice;
  expected_context.r4.u64 = 13;
  expected_context.r5.u64 = bad_up ? UINT32_MAX : 4;
  expected_context.r6.u64 = 0x9000;
  expected_context.r7.u64 = 16;
  if (function == sub_82865690) {
    expected_context.r5.u64 = bad_up ? UINT32_MAX : 2;
    expected_context.r6.u64 = 6;
  }
  expected_context.r9.u64 = 0x9876543210ABCDEF;
  expected_context.lr = 0x8248CB34;
  std::memset(expected_context.v59.u8, 0xA5, sizeof(PPCVRegister));
  PPCContext ctx;
  std::memcpy(&ctx, &expected_context, sizeof(ctx));
  function(ctx, memory);
  PPCContext result;
  std::memcpy(&result, &expected_context, sizeof(result));
  result.r3.u64 = 0x123456789ABCDEF0ull;
  result.r9.u64 ^= 0xAABBCCDD12345678ull;
  Check(!std::memcmp(&ctx, &result, sizeof(ctx)));
  auto result_memory = expected_memory;
  result_memory[40] = 0x7F;
  Check(!std::memcmp(memory, result_memory.data(), result_memory.size()));
  Check(!std::memcmp(memory + kTextureAlias, expected_texture.data(), expected_texture.size()));
}
}  // namespace

REX_HOOK_RAW(__imp__sub_828652A0) { Original(ctx, base); }
REX_HOOK_RAW(__imp__sub_82865690) { Original(ctx, base); }
REX_HOOK_RAW(__imp__sub_82864D40) { Original(ctx, base); }

int main() {
  using namespace mc2::draw_capture;
  Check(!Selected(0, 1, 2));
  Check(Selected(1, 1, 2) && Selected(2, 1, 2) && !Selected(3, 1, 2));
  Check(Selected(0, 0, 0) && !Selected(1, 0, 0));
  Check(!Selected(32, 0, UINT32_MAX));
  Check(!Selected(UINT64_MAX, 0, 32));
  uint32_t extent = 0;
  Check(UpVertexBytes(0x9000, 4, 16, extent) && extent == 64);
  Check(!UpVertexBytes(0x9000, UINT32_MAX, UINT32_MAX, extent));
  Check(!UpVertexBytes(0xDFFFFFF0, 4, 16, extent));
  Check(!UpVertexBytes(0xFFFFFFF0, 4, 16, extent));
  Check(!UpVertexBytes(0x9000, 0, 16, extent));
  std::array<uint8_t, 28> decl{};
  decl[3] = 5; decl[27] = 2;
  Check(DeclarationBytes(decl.data(), decl.size(), extent) && extent == 80);
  decl[26] = 2;
  Check(!DeclarationBytes(decl.data(), decl.size(), extent));
  Check(!DeclarationBytes(decl.data(), 27, extent));
  Check(TexturePhysicalAlias(0x100000, 264, extent) && extent == kTextureAlias);
  Check(TexturePhysicalAlias(0x1FFFFFFF, 1, extent) && extent == 0xBFFFFFFF);
  Check(!TexturePhysicalAlias(0x1FFFFFFF, 2, extent));
  Check(!TexturePhysicalAlias(0x20000000, 1, extent));
  Check(!TexturePhysicalAlias(0x100000, UINT64_MAX, extent));
  Check(!TexturePhysicalAlias(0x100000, 0, extent));
  std::array<uint8_t, 32> buffer{};
  buffer[3] = 1; buffer[26] = 0x90; buffer[27] = 3; buffer[31] = 0x42;
  uint32_t address = 0;
  Check(BufferBytes(buffer.data(), buffer.size(), true, address, extent) && address == 0x9000 && extent == 64);
  Check(!BufferBytes(buffer.data(), 31, true, address, extent));
  buffer[28] = 1; buffer[31] = 4;  // 16 MiB + four bytes.
  Check(!BufferBytes(buffer.data(), buffer.size(), true, address, extent));
  buffer[28] = 0; buffer[31] = 0x42; buffer[27] = 2;
  Check(!BufferBytes(buffer.data(), buffer.size(), true, address, extent));
  buffer[0] = 0x80; buffer[3] = 2; buffer[27] = 0; buffer[31] = 64;
  Check(BufferBytes(buffer.data(), buffer.size(), false, address, extent) && address == 0x9000 && extent == 64);
  buffer[31] = 0;
  Check(!BufferBytes(buffer.data(), buffer.size(), false, address, extent));
  buffer[31] = 64; buffer[24] = 0xDF; buffer[25] = buffer[26] = 0xFF; buffer[27] = 0xFC;
  Check(!BufferBytes(buffer.data(), buffer.size(), false, address, extent));
#ifndef _WIN32
  std::printf("Draw capture pure checks: %u failures; live mapping tests need Windows\n", failures);
  return failures ? 1 : 77;
#else
  // Reserve the actual A-range guest alias, committing only the fixture pages.
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(kTextureAlias) + 65536,
      MEM_RESERVE, PAGE_NOACCESS));
  if (!memory) return 1;
  if (!VirtualAlloc(memory, kMemorySize, MEM_COMMIT, PAGE_READWRITE) ||
      !VirtualAlloc(memory + kTextureAlias, kTextureBytes, MEM_COMMIT, PAGE_READWRITE)) {
    VirtualFree(memory, 0, MEM_RELEASE);
    return 1;
  }
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const auto boundary = 2 * info.dwPageSize;
  memory[boundary - 1] = 0xA6;
  memory[boundary] = 0x4D;
  std::vector<uint8_t> snapshot;
  Check(mc2::capture::ReadGuest(memory, boundary - 1, 2, snapshot) &&
        snapshot == std::vector<uint8_t>{0xA6, 0x4D});
  DWORD previous;
  if (!VirtualProtect(memory + boundary, info.dwPageSize, PAGE_READWRITE | PAGE_GUARD, &previous)) {
    VirtualFree(memory, 0, MEM_RELEASE);
    return 1;
  }
  Check(!mc2::capture::ReadGuest(memory, boundary - 1, 2, snapshot));
  MEMORY_BASIC_INFORMATION guarded{};
  Check(VirtualQuery(memory + boundary, &guarded, sizeof(guarded)) && (guarded.Protect & PAGE_GUARD));
  Check(VirtualProtect(memory + boundary, info.dwPageSize, previous, &previous));
  Check(!mc2::capture::ReadGuest(memory, uint32_t(kMemorySize) - 1, 2, snapshot));
  expected_memory.fill(0);
  StoreBE32(kDevice + 12704, 0x7000);
  StoreBE32(kDevice + 12700, 0x7800);
  StoreBE32(kDevice + 11824, 0x8000);
  StoreBE32(kDevice + 12448, 0xA100);
  StoreBE32(kDevice + 12464, 0xA200);
  StoreBE32(kDevice + 12468, 0xA300);
  StoreBE32(kDevice + 12444, 0xA400);
  StoreBE32(0xA300, 1); StoreBE32(0xA300 + 24, 0xB003); StoreBE32(0xA300 + 28, 0x42);
  StoreBE32(0xA400, 0x80000002); StoreBE32(0xA400 + 24, 0xB100); StoreBE32(0xA400 + 28, 64);
  for (uint32_t i = 0; i < 64; ++i) expected_memory[0xB000 + i] = uint8_t(i * 7 + 3);
  for (uint32_t i = 0; i < 16; ++i) StoreBE32(0xB100 + i * 4, i);
  StoreBE32(kDevice + 12552, 0xA500);
  StoreBE32(kDevice + 10896, 0xA600);
  StoreBE32(kDevice + 10900, 0xA700);
  StoreBE32(kDevice + 10908, 1);
  StoreBE32(0xA600, 0xFFFFFFFE);
  StoreBE32(0xA600 + 60, 0x1234);
  const uint32_t fetch[] = {2 | (2u << 22), 0x00100006, 1 | (1u << 13), 0x688 << 1, 0, 1u << 9};
  for (size_t i = 0; i < 6; ++i) StoreBE32(kDevice + 1152 + uint32_t(i) * 4, fetch[i]);
  const uint32_t video_fetch[] = {2 | (8u << 22), 0x00100002, 2 | (1u << 13), 0xa00 << 1, 0, 1u << 9};
  for (uint32_t slot : {1u, 2u})
    for (size_t i = 0; i < 6; ++i)
      StoreBE32(kDevice + 1152 + slot * 24 + uint32_t(i) * 4, video_fetch[i]);
  for (size_t i = 0; i < expected_texture.size(); ++i) expected_texture[i] = uint8_t(i * 3 + i / 256);
  StoreBE32(0x7000, 6); StoreBE32(0x7000 + 32, 0xC000);
  StoreBE32(0x7800, 7); StoreBE32(0x7800 + 24, 0xC100);
  for (auto shader : {0x7000u + 872, 0x7800u + 40}) {
    StoreBE32(shader, 0x102A1100); StoreBE32(shader + 4, 36); StoreBE32(shader + 8, 12);
  }
  StoreBE32(0x8000, 5); StoreBE32(0x8000 + 24, 2);
  for (size_t i = 0; i < 64; ++i) expected_memory[0x9000 + i] = uint8_t(i);
  const auto directory = std::filesystem::temp_directory_path() /
      ("mc2-draw-capture-check-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  rex::cvar::SetFlagByName("mc2_draw_capture_dir", "");
  Invoke(sub_828652A0, memory); Invoke(sub_82865690, memory); Invoke(sub_82864D40, memory);
  Check(!std::filesystem::exists(directory));
  rex::cvar::SetFlagByName("mc2_draw_capture_dir", directory.string());
  rex::cvar::SetFlagByName("mc2_draw_capture_hook", "up");
  rex::cvar::SetFlagByName("mc2_draw_capture_skip", "1");
  rex::cvar::SetFlagByName("mc2_draw_capture_count", "2");
  Invoke(sub_828652A0, memory);  // Filtered: does not consume skip.
  Invoke(sub_82864D40, memory);  // Matching ordinal 0: skipped.
  Check(!std::filesystem::exists(directory));
  Invoke(sub_82864D40, memory);  // Matching ordinal 1: captured.
  Invoke(sub_82864D40, memory, true);  // Invalid UP extent is recorded, original still called.
  Invoke(sub_82864D40, memory);  // Outside selected interval.
  rex::cvar::SetFlagByName("mc2_draw_capture_hook", "indexed");
  rex::cvar::SetFlagByName("mc2_draw_capture_skip", "0");
  rex::cvar::SetFlagByName("mc2_draw_capture_count", "32");
  Invoke(sub_82865690, memory);
  rex::cvar::SetFlagByName("mc2_draw_capture_hook", "nonindexed");
  Invoke(sub_828652A0, memory);
  rex::cvar::SetFlagByName("mc2_draw_capture_hook", "indexed");
  Invoke(sub_82865690, memory, true);  // Reject the overflowing indexed range.
  unsigned manifests = 0, up_data = 0, devices = 0, containers = 0, raw_textures = 0, rgba_textures = 0, video_textures = 0, progress_blocks = 0;
  unsigned vertex_buffers = 0, index_buffers = 0;
  for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
    if (item.path().filename() == "metadata.json") {
      ++manifests;
      std::ifstream stream(item.path());
      const std::string json((std::istreambuf_iterator<char>(stream)), {});
      Check(json.find("\"replayable\": false") != std::string::npos);
      Check(json.find("attachment_contents_and_prior_draw_dependencies") != std::string::npos);
      Check(json.find("\"rgba8_decoded\":true") != std::string::npos);
      Check(json.find("texture_payload_slot_0") == std::string::npos);
      Check(json.find("texture_payload_slot_1") == std::string::npos);
      Check(json.find("texture_payload_slot_2") == std::string::npos);
      Check(json.find("vertex_buffer_payload_slot_0") == std::string::npos);
      const bool bad_index = json.find("\"hook\": \"indexed\"") != std::string::npos &&
          json.find("\"r5\":4294967295") != std::string::npos;
      Check((json.find("index_buffer_payload") != std::string::npos) == bad_index);
    } else if (item.path().filename() == "up-vertices.bin") {
      ++up_data; Check(std::filesystem::file_size(item.path()) == 64);
      std::array<uint8_t, 64> actual{};
      std::ifstream stream(item.path(), std::ios::binary);
      stream.read(reinterpret_cast<char*>(actual.data()), actual.size());
      Check(!std::memcmp(actual.data(), expected_memory.data() + 0x9000, actual.size()));
    } else if (item.path().filename() == "device.bin") {
      ++devices; Check(std::filesystem::file_size(item.path()) == kDeviceBytes);
    } else if (item.path().filename() == "gpu-progress.bin") {
      ++progress_blocks;
      std::ifstream stream(item.path(), std::ios::binary);
      const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(stream)), {});
      Check(actual.size() == 96 && !std::memcmp(actual.data(), expected_memory.data() + 0xA600, 96));
    } else if (item.path().filename() == "slot-00.data.bin" || item.path().filename() == "index-buffer.data.bin") {
      const bool vertex = item.path().filename() == "slot-00.data.bin";
      if (vertex) ++vertex_buffers; else ++index_buffers;
      std::ifstream stream(item.path(), std::ios::binary);
      const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(stream)), {});
      Check(actual.size() == 64 && !std::memcmp(actual.data(), expected_memory.data() + (vertex ? 0xB000 : 0xB100), 64));
    } else if (item.path().extension() == ".container") {
      ++containers; Check(std::filesystem::file_size(item.path()) == 48);
    } else if (item.path().filename() == "slot-00.base.bin") {
      ++raw_textures;
      std::ifstream stream(item.path(), std::ios::binary);
      const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(stream)), {});
      Check(actual.size() == 264 && !std::memcmp(actual.data(), expected_texture.data(), actual.size()));
    } else if (item.path().filename() == "slot-01.rgba8.bin" || item.path().filename() == "slot-02.rgba8.bin") {
      ++video_textures;
      std::ifstream stream(item.path(), std::ios::binary);
      const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(stream)), {});
      Check(actual.size() == 24);
      if (actual.size() == 24) for (uint32_t pixel = 0; pixel < 6; ++pixel) {
        const auto value = expected_texture[(pixel / 3) * 256 + pixel % 3];
        Check(actual[pixel*4] == value && actual[pixel*4+1] == value &&
              actual[pixel*4+2] == value && actual[pixel*4+3] == 255);
      }
    } else if (item.path().filename() == "slot-00.rgba8.bin") {
      ++rgba_textures;
      std::ifstream stream(item.path(), std::ios::binary);
      const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(stream)), {});
      Check(actual.size() == 16 && !std::memcmp(actual.data(), expected_texture.data(), 8) &&
            !std::memcmp(actual.data() + 8, expected_texture.data() + 256, 8));
    }
  }
  Check(original_calls == 11 && manifests == 5 && devices == 5 && containers == 10 && up_data == 1 &&
        raw_textures == 5 && rgba_textures == 5 && video_textures == 10 && progress_blocks == 5 &&
        vertex_buffers == 3 && index_buffers == 1);
  VirtualFree(memory, 0, MEM_RELEASE);
  std::printf("Draw capture checks: %u failures, %u original calls, %u manifests\nFixtures: %s\n",
              failures, original_calls, manifests, directory.string().c_str());
  return failures ? 1 : 0;
#endif
}
