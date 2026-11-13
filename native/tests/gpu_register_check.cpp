#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <bit>
#include <cstdio>
#include <vector>

#ifdef MC2_CHECK_SAMPLE_OFFSET
#include "gpu_sample_offset.inc"
static bool CheckSampleAddresses() {
  unsigned cases = 0;
  for (uint32_t count : {1u, 2u, 4u})
    for (uint32_t width : {80u, 160u, 240u, 320u, 640u})
      for (uint32_t host_coverage = 0; host_coverage < (count == 1 ? 2u : 16u); ++host_coverage)
        for (uint32_t write_mask = 0; write_mask < 16; ++write_mask) {
          // Map D3D coverage to the existing Xbox sample ordering. At 2x,
          // Nvidia uses host samples 0 and 3; at 4x, swap samples 1 and 2.
          uint32_t coverage = count == 4
              ? (host_coverage & 9) | ((host_coverage & 2) << 1) | ((host_coverage & 4) >> 1)
              : (host_coverage & 1) | ((host_coverage & 8) >> 2);
          std::vector<int32_t> original, specialized;
          int32_t old_address = 0, new_address = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            if (coverage & write_mask & (1u << i)) original.push_back(old_address);
            old_address += (i & 1) ? -int32_t(width) + 2 - int32_t(i) : int32_t(width);
          }
          for (uint32_t i = 0; i < count; ++i) {
            if (coverage & write_mask & (1u << i)) specialized.push_back(new_address);
            new_address += SampleOffset(i, count, width);
          }
          if (original != specialized || old_address != 0 || new_address != 0) {
            std::fprintf(stderr, "FAIL sample addresses: count=%u width=%u coverage=%u writes=%u\n",
                         count, width, host_coverage, write_mask);
            return false;
          }
          ++cases;
        }
  std::printf("ROV sample address check passed: %u cases; same active writes and restored depth address.\n", cases);
  return true;
}
#endif

REXCVAR_DEFINE_BOOL(gpu_batch_type0_constants, true, "GPU", "Test upload routing");

namespace rex::graphics {
// A register-write sink, without a GPU. The decoder and ring-range methods
// below are extracted verbatim from the patched SDK, not reimplemented here.
class CommandProcessor {
 public:
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  unsigned uploads = 0;
  void WriteRegister(uint32_t index, uint32_t value) { writes.emplace_back(index, value); }
  void WriteRegistersFromMem(uint32_t base, uint32_t* data, uint32_t count) {
    ++uploads;
    for (uint32_t i = 0; i < count; ++i) WriteRegister(base + i, std::byteswap(data[i]));
  }
  bool ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet);
  void WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t count);
};
#include "gpu_register_methods.inc"
}  // namespace rex::graphics

int main() {
#ifdef MC2_CHECK_SAMPLE_OFFSET
  if (!CheckSampleAddresses()) return 1;
#endif
  constexpr uint32_t capacity = 65540;
  std::vector<uint32_t> storage(capacity / 4);
  unsigned cases = 0;
  for (bool enabled : {false, true}) {
    REXCVAR_SET(gpu_batch_type0_constants, enabled);
    for (bool repeat : {false, true})
      for (uint32_t base : {0u, 0x3FFFu, 0x4000u, 0x4001u, 0x47FEu, 0x47FFu, 0x4800u, 0x7FFFu})
        for (uint32_t count : {1u, 2u, 3u, 4u, 7u, 64u, 1024u, 2048u, 16384u})
          for (uint32_t offset : {0u, 4u, capacity - 4, capacity - 16})
            for (bool truncated : {false, true}) {
              rex::memory::RingBuffer ring(reinterpret_cast<uint8_t*>(storage.data()), capacity);
              ring.set_read_offset(offset);
              ring.set_write_offset((offset + (count - unsigned(truncated)) * 4) % capacity);
              std::vector<std::pair<uint32_t, uint32_t>> expected;
              for (uint32_t i = 0; i < count; ++i) {
                uint32_t value = 0x89ABCDEFu ^ (i * 0x10203u);
                storage[((offset / 4) + i) % storage.size()] = std::byteswap(value);
                expected.emplace_back(repeat ? base : base + i, value);
              }
              rex::graphics::CommandProcessor processor;
              const bool ok = processor.ExecutePacketType0(
                  &ring, base | (unsigned(repeat) << 15) | ((count - 1) << 16));
              const bool batch = enabled && !repeat && base >= 0x4000 && base + count <= 0x4800;
              const unsigned uploads = batch ? (offset + count * 4 > capacity ? 2 : 1) : 0;
              if (ok == truncated ||
                  (truncated && (!processor.writes.empty() || ring.read_offset() != offset)) ||
                  (!truncated && (processor.writes != expected || processor.uploads != uploads ||
                                   ring.read_offset() != (offset + count * 4) % capacity))) {
                std::fprintf(stderr, "FAIL base=%X count=%u offset=%u repeat=%d enabled=%d truncated=%d\n",
                             base, count, offset, repeat, enabled, truncated);
                return 1;
              }
              ++cases;
            }
  }
  std::printf("GPU register check passed: %u packets; values/order, wrapping, repeated writes, bank boundaries and truncation.\n", cases);
}
