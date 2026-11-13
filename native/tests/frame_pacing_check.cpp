#include "generated/default/magnacarta2_init.h"
#include <rex/cvar.h>
#include <rex/hook.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

REXCVAR_DEFINE_STRING(mc2_framerate, "60", "Game", "Test frame pacing hook");
static uint32_t observed_interval;
static unsigned original_calls;
static uint32_t observed_delay;
static uint64_t observed_lr;
static unsigned delay_calls;
static uint32_t gpu_result;
static unsigned gpu_calls;

REX_HOOK_RAW(__imp__sub_823691D0) {
  ++gpu_calls;
  ctx.r3.u64 = gpu_result;
  ctx.r4.u64 = 0x12345678;
}

REX_HOOK_RAW(__imp__sub_82869478) {
  observed_delay = ctx.r3.u32;
  observed_lr = ctx.lr;
  ++delay_calls;
  ctx.r3.u64 = 0x76543210;
}

REX_HOOK_RAW(__imp__sub_82369420) {
  observed_interval = REX_LOAD_U32(ctx.r3.u32 + 52);
  ++original_calls;
  ctx.r3.u64 = 0x12345678;  // Original return value must pass through.
}

int main() {
  for (const auto* mode : {"30", "60", "unlocked"}) {
    REXCVAR_SET(mc2_framerate, mode);
    for (uint32_t interval : {0u, 1u, 2u, 3u, 4u, 0x80000000u}) {
      std::array<uint8_t, 192> memory;
      memory.fill(0xA5);
      const auto encoded = std::byteswap(interval);
      std::memcpy(memory.data() + 32 + 52, &encoded, 4);
      auto expected = memory;
      const auto desired = std::string_view(mode) == "unlocked" ? 0x80000000u
          : std::string_view(mode) == "60" && interval == 2 ? 1u : interval;
      const auto desired_bytes = std::byteswap(desired);
      std::memcpy(expected.data() + 32 + 52, &desired_bytes, 4);
      PPCContext ctx{};
      ctx.r3.u64 = 32;
      sub_82369420(ctx, memory.data());
      if (observed_interval != desired || ctx.r3.u32 != 0x12345678 || memory != expected) {
        std::fprintf(stderr, "FAIL: interval %u, mode %s\n", interval, mode);
        return 1;
      }
    }
  }
  if (original_calls != 18) return 2;
  for (bool enabled : {false, true}) {
    rex::cvar::SetFlagByName("mc2_idle_wait", enabled ? "true" : "false");
    for (uint32_t caller : {0x82399BF4u, 0x82399BF0u, 0x8286A80Cu}) {
      for (uint32_t delay : {0u, 1u, 16u, 0xFFFFFFFFu}) {
        PPCContext ctx{};
        ctx.lr = caller;
        ctx.r3.u64 = delay;
        ctx.r4.u64 = 0xABCDEF;
        sub_82869478(ctx, nullptr);
        const uint32_t expected = enabled && caller == 0x82399BF4 && delay == 0 ? 1 : delay;
        if (observed_delay != expected || observed_lr != caller ||
            ctx.r3.u32 != 0x76543210 || ctx.r4.u64 != 0xABCDEF) {
          std::fprintf(stderr, "FAIL: polling delay %u, caller %X, enabled %d\n",
                       delay, caller, enabled);
          return 3;
        }
      }
    }
  }
  if (delay_calls != 24) return 4;
  for (bool enabled : {false, true}) {
    rex::cvar::SetFlagByName("mc2_idle_wait", enabled ? "true" : "false");
    for (uint32_t caller : {0x8236D288u, 0x8236D220u}) {
      for (uint32_t result : {0u, 1u}) {
        gpu_result = result;
        PPCContext ctx{};
        ctx.lr = caller;
        sub_823691D0(ctx, nullptr);
        if (ctx.r3.u32 != result || ctx.r4.u32 != 0x12345678 || ctx.lr != caller) return 5;
      }
    }
  }
  if (gpu_calls != 8) return 6;
  std::puts("Frame pacing check passed: 30/60/unlocked device intervals, "
            "scoped polling backoff, "
            "other parameters, and original return values.");
}
