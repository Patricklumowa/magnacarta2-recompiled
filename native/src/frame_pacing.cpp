#include "generated/default/magnacarta2_init.h"
#include <rex/hook.h>
#include <rex/cvar.h>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(mc2_idle_wait, true, "Development",
                    "Back off the game's frame synchronization poll (disable for profiling)");

namespace {
void BackOffGpuPoll() {
#ifdef _WIN32
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~Timer() { if (handle) CloseHandle(handle); }
  };
  thread_local const Timer timer;
  LARGE_INTEGER due;
  due.QuadPart = -1000;  // Relative 100 ns units: request 100 microseconds.
  if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer.handle, INFINITE);
  } else {
    Sleep(1);
  }
#else
  std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
}
}  // namespace

// This edition passes D3DPRESENT_PARAMETERS in r3. PresentationInterval is
// at +52: the caller at 0x8248B440 loads 2 and stores it at stack +164,
// relative to the parameter block at stack +112, then calls this function.
REX_HOOK_RAW(sub_82369420) {
  const uint32_t interval_address = ctx.r3.u32 + 52;
  const uint32_t interval = REX_LOAD_U32(interval_address);
  const auto mode = REXCVAR_QUERY(std::string, mc2_framerate);
  if (mode == "unlocked") {
    REX_STORE_U32(interval_address, 0x80000000u);
    REXLOG_INFO("MC2: immediate presentation selected for unlocked frame rate");
  } else if (mode == "60" && interval == 2) {
    REX_STORE_U32(interval_address, 1);
    REXLOG_INFO("MC2: presentation interval changed from 2 to 1 for 60 FPS");
  }
  __imp__sub_82369420(ctx, base);
}

// sub_82399BC0 waits while an engine counter exceeds its requested threshold.
// Sleep(0) in that loop made roughly 19 million HLE calls in a 20-second trace
// and occupied most of a CPU core. Keep the counter and all original wait logic;
// only give this polling call a 1 ms sleep, including the runtime's termination
// checks. Other callers, real delays, and alertable sleeps are unchanged.
REX_HOOK_RAW(sub_82869478) {
  if (ctx.lr == 0x82399BF4 && ctx.r3.u32 == 0 && REXCVAR_GET(mc2_idle_wait)) {
    ctx.r3.u64 = 1;
  }
  __imp__sub_82869478(ctx, base);
}

// The D3D fence-wait loop calls this progress/timeout check continuously. Its
// Xbox "db16cyc" delays compile to no-ops, leaving the render thread at 100%.
// Back off only while the original check says to continue; completion and
// timeout decisions still come from the unmodified D3D code.
REX_HOOK_RAW(sub_823691D0) {
  const auto caller = ctx.lr;
  __imp__sub_823691D0(ctx, base);
  if (caller == 0x8236D288 && ctx.r3.u32 != 0 && REXCVAR_GET(mc2_idle_wait)) {
    BackOffGpuPoll();
  }
}
