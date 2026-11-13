#include "generated/default/magnacarta2_init.h"
#include <rex/cvar.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <intrin.h>

// Compare against the actual generated original, including every guest byte,
// all vector/scratch registers, flags and untouched context members.
extern "C" REX_FUNC(mc2_copy_reference);
alignas(64) static std::array<uint8_t, 65536> original_memory, fast_memory, simd_memory;
static unsigned cases;

static bool check(uint32_t device, uint32_t index, uint32_t source,
                  uint32_t count, uint64_t high = 0) {
  for (size_t i = 0; i < original_memory.size(); ++i)
    original_memory[i] = uint8_t((i * 37) ^ (i >> 5) ^ cases);
  fast_memory = original_memory;
  simd_memory = original_memory;
  PPCContext original{};
  std::memset(&original, 0, sizeof(original));
  original.r3.u64 = device | high;
  original.r4.u64 = index | high;
  original.r5.u64 = source | high;
  original.r6.u64 = count; // High count bits would request a huge original loop.
  original.r7.u64 = 0x8000000100000001ull ^ cases;
  original.r12.u64 = 0xDEADBEEF12345678ull;
  original.lr = 0x81234567;
  original.ctr.u64 = 0xABCDE;
  original.xer.so = cases & 1;
  std::memset(original.v59.u8, 0xA5, 5 * sizeof(PPCVRegister));
  PPCContext fast;
  std::memcpy(&fast, &original, sizeof(fast));
  PPCContext simd;
  std::memcpy(&simd, &original, sizeof(simd));
  mc2_copy_reference(original, original_memory.data());
  __imp__sub_82371D88(simd, simd_memory.data());
  sub_82371D88(fast, fast_memory.data());
  ++cases;
  if (original_memory != fast_memory || original_memory != simd_memory ||
      std::memcmp(&original, &fast, sizeof(fast)) ||
      std::memcmp(&original, &simd, sizeof(simd))) {
    std::fprintf(stderr, "FAIL: device=%X index=%u source=%X count=%u high=%llX\n",
                 device, index, source, count, (unsigned long long)high);
    return false;
  }
  return true;
}

static double benchmark(unsigned mode, unsigned count) {
  rex::cvar::SetFlagByName("mc2_fast_constants", mode == 2 ? "true" : "false");
  PPCContext ctx{};
  constexpr unsigned iterations = 20000;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i) {
    ctx.r3.u64 = 0x1000;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 0x8003;
    ctx.r6.u64 = count;
    ctx.r7.u64 = 1;
    if (mode == 0) mc2_copy_reference(ctx, fast_memory.data());
    else sub_82371D88(ctx, fast_memory.data());
  }
  return std::chrono::duration<double, std::nano>(
      std::chrono::steady_clock::now() - start).count() / iterations;
}

int main() {
  int cpu[4];
  __cpuid(cpu, 1);
  if (!(cpu[2] & (1 << 9))) {
    std::fputs("SSSE3 is required by this build.\n", stderr);
    return 6;
  }
  for (bool enabled : {false, true}) {
    rex::cvar::SetFlagByName("mc2_fast_constants", enabled ? "true" : "false");
    for (uint32_t alignment = 0; alignment < 16; ++alignment) {
      for (uint32_t count = 0; count <= 512; ++count)
        if (!check(0x1000, 0, 0x8000 + alignment, count)) return 1;
      for (uint32_t index : {1u, 31u, 256u, 500u, 511u, 512u})
        for (uint32_t count : {0u, 1u, 7u, 8u, 9u, 15u, 16u, 64u})
          if (!check(0x1000, index, 0x8000 + alignment, count)) return 2;
      for (uint32_t count : {0u, 1u, 8u, 9u, 64u}) {
        if (!check(0x1000 + alignment, 3, 0x8000, count)) return 3;
        if (!check(0x1000, 3, 0x8000 + alignment, count, 0x100000000ull)) return 4;
        for (int offset : {-64, -16, -1, 0, 1, 16, 64})
          if (!check(0x1000, 3, 0x17B0 + offset + alignment, count)) return 5;
      }
    }
  }
  std::printf("Graphics copy check passed: %u cases; complete memory/context match.\n", cases);
  for (unsigned count : {4u, 8u, 16u, 64u, 128u, 512u}) {
    // Alternate to reduce warm-up and clock drift; this is not a game FPS test.
    benchmark(0, count); benchmark(1, count); benchmark(2, count);
    double original = 0, simd = 0, fast = 0;
    for (unsigned round = 0; round < 4; ++round) {
      for (unsigned i = 0; i < 3; ++i) {
        const unsigned mode = (round + i) % 3;
        const double ns = benchmark(mode, count);
        if (mode == 0) original += ns;
        else if (mode == 1) simd += ns;
        else fast += ns;
      }
    }
    std::printf("%u constants: SSE2 %.1f ns, SSSE3 %.1f ns, bulk %.1f ns (microbenchmark only)\n",
                count, original / 4, simd / 4, fast / 4);
  }
}
