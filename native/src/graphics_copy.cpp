#include "generated/default/magnacarta2_init.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <cstring>

REXCVAR_DEFINE_BOOL(mc2_fast_constants, false, "Development",
                    "Bulk-copy shader constants; disable for performance comparison");

// Copies float4 shader constants into the device's constant bank, then ORs
// its dirty mask. The VMX load-left/load-right pairs implement byte copies.
// Keep the final original iteration(s) so all register results, including
// scratch vectors, CTR and CR6, are identical to the original function.
REX_HOOK_RAW(sub_82371D88) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t source = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;
  if (REXCVAR_GET(mc2_fast_constants) && count >= 8 && index < 512 &&
      count <= 512 - index && !(device & 15) &&
      ctx.r3.u64 == device && ctx.r4.u64 == index &&
      ctx.r5.u64 == source && ctx.r6.u64 == count) {
    const uint64_t destination = uint64_t(device) + (index + 120) * 16;
    const uint64_t length = uint64_t(count) * 16;
    // Overlapping ranges have the original VMX loop's block-order semantics.
    // Fall back for those, address wraparound and unusual ABI inputs.
    // High guest addresses use an adjusted physical mapping in REX_RAW_ADDR;
    // leave those to the original until that range is covered by tests.
    if (destination + length <= 0xE0000000ull &&
        uint64_t(source) + length <= 0xE0000000ull &&
        (uint64_t(source) + length <= destination ||
         destination + length <= source)) {
      const uint32_t prefix = (count / 4 - 1) * 4;
      std::memcpy(base + destination, base + source, size_t(prefix) * 16);
      ctx.r4.u64 = index + prefix;
      ctx.r5.u64 = source + prefix * 16;
      ctx.r6.u64 = count - prefix;
    }
  }
  __imp__sub_82371D88(ctx, base);
}
