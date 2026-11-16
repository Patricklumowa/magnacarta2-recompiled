// Experimental live movie backend. No Xenos command processor or MMIO mapping.
// Legacy CPU state setters still write into their allocated staging memory;
// native draws consume the cached state above that packet boundary.
#include "generated/default/magnacarta2_init.h"
#include "native_boot.h"
#include "native_movie.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>

REXCVAR_DEFINE_BOOL(mc2_native_boot, false, "Development", "Experimental native movie boot; unsupported operations fail");
REXCVAR_DEFINE_STRING(mc2_native_boot_shader_dir, "", "Development", "Verified first-draw movie shader inputs for native boot");

namespace mc2::native {
namespace {
class NativeGraphics;
std::atomic<NativeGraphics*> active{nullptr};
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class NativeGraphics final : public rex::system::IGraphicsSystem {
 public:
    MovieShaders shaders;
    std::mutex mutex;
    std::unique_ptr<QuadRenderer> renderer;
    std::optional<QuadDraw> pending;
    HWND window = nullptr;
    uint64_t completed = 0;
    uint32_t guest_device = 0;

    NativeGraphics() : shaders(LoadMovieShaders(REXCVAR_GET(mc2_native_boot_shader_dir))) {
        NativeGraphics* expected = nullptr;
        Require(active.compare_exchange_strong(expected, this), "Only one native graphics system is supported");
    }
    ~NativeGraphics() override { active.store(nullptr); }
    rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override { return 0; }
    rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher*, rex::system::KernelState*) override {
        Require(!GetModuleHandleW(L"rexgpu-xenosrd.dll") && !GetModuleHandleW(L"rexgpu-xenos.dll"),
                "Xenos was unexpectedly loaded during native startup");
        REXLOG_INFO("MC2 native boot: Xenos plugin, command processor and GPU MMIO are disabled");
        return 0;
    }
    bool has_presentation() const override { return true; }
    void Shutdown() override {
        std::lock_guard lock(mutex);
        pending.reset(); renderer.reset();
    }
    void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
        Require(callback == 0x8236D4E0 && user_data != 0, "Unexpected legacy graphics interrupt callback");
        guest_device = user_data;
    }
    void InitializeRingBuffer(uint32_t, uint32_t) override {
        REXLOG_INFO("MC2 native boot: legacy staging memory retained for CPU setters; no packet execution");
    }
    void EnableReadPointerWriteBack(uint32_t, uint32_t) override {}

    void Flush(uint8_t* base, uint32_t device) {
        std::lock_guard lock(mutex);
        Require(device != 0 && device == guest_device, "Native operation used an unknown device");
        if (pending) {
            Require(window && IsWindow(window), "Native game window is unavailable");
            if (!renderer) {
                renderer = std::make_unique<QuadRenderer>(*pending);
                renderer->SetWindow(window);
            }
            const auto result = renderer->Present(*pending, REXCVAR_QUERY(std::string, mc2_framerate) != "unlocked");
            completed = result.completed_submission;
            pending.reset();
            if (completed == 1 || (completed && completed % 120 == 0))
                REXLOG_INFO("MC2 native boot: {} native movie submissions completed", completed);
        }
        // Every native draw is synchronous. Publish a legacy retirement marker
        // only after pending native work has reached its actual queue fence.
        const auto progress = REX_LOAD_U32(device + 10896);
        Require(progress != 0, "Native retirement progress block is missing");
        const auto marker = REX_LOAD_U32(device + 10908);
        REX_STORE_U32(progress, marker);
        REX_STORE_U32(device + 10908, marker + 2);
    }
};
NativeGraphics& Graphics() {
    auto* system = active.load();
    Require(system != nullptr, "Native graphics system is not active");
    return *system;
}
uint32_t ResetStaging(uint8_t* base, uint32_t device) {
    const auto begin = REX_LOAD_U32(device + 14904);
    const auto extent = REX_LOAD_U32(device + 14912);
    Require(begin >= 4 && extent >= 4096 && extent <= 16 * 1024 * 1024 && begin <= UINT32_MAX - extent,
            "Invalid legacy CPU staging extent");
    REX_STORE_U32(device + 48, begin - 4);
    REX_STORE_U32(device + 52, begin + extent);
    REX_STORE_U32(device + 56, begin + extent - 160);
    REX_STORE_U32(device + 14928, begin);
    return begin - 4;
}
}
bool NativeBootRequested() { return REXCVAR_GET(mc2_native_boot); }
bool NativeBootActive() { return active.load() != nullptr; }
std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeBootGraphics() {
    return std::make_unique<NativeGraphics>();
}
void SetNativeBootWindow(void* window) {
    auto& system = Graphics();
    std::lock_guard lock(system.mutex);
    system.window = static_cast<HWND>(window);
}
void QueueNativeMovie(const PPCContext& ctx, uint8_t* base) {
    auto& system = Graphics();
    std::lock_guard lock(system.mutex);
    Require(!system.pending, "Native movie backend cannot compose multiple draws yet");
    try {
        system.pending = ReadMovieDraw(ctx, base, system.shaders).draw;
    } catch (const std::exception& error) {
        UnsupportedNativeOperation(error.what(), ctx);
    }
}
[[noreturn]] void UnsupportedNativeOperation(const char* operation, const PPCContext& ctx) {
    const auto message = fmt::format("MC2 native boot: unsupported {} at guest caller {:#x}", operation, ctx.lr);
    REXLOG_ERROR("{}", message);
    throw std::runtime_error(message);
}
}  // namespace mc2::native

using namespace mc2::native;

REX_HOOK_RAW(sub_82388AA0) {
    if (NativeBootActive()) {
        // This routine already has a single-thread path; no legacy GPU workers
        // are needed when native submission owns the queue.
        REX_STORE_U32(ctx.r3.u32 + 23980, REX_LOAD_U32(ctx.r3.u32 + 23980) | 0x100);
    }
    __imp__sub_82388AA0(ctx, base);
}
REX_HOOK_RAW(sub_82387370) {
    if (!NativeBootActive()) { __imp__sub_82387370(ctx, base); return; }
    // EDRAM retraining has no host GPU counterpart or CPU cached-state effects.
    ctx.r3.u64 = 1;
}
REX_HOOK_RAW(sub_8236D0C8) {
    if (!NativeBootActive()) { __imp__sub_8236D0C8(ctx, base); return; }
    const auto device = ctx.r3.u32;
    Graphics().Flush(base, device);
    ctx.r3.u64 = ResetStaging(base, device);
}
REX_HOOK_RAW(sub_8236CE70) {
    if (!NativeBootActive()) { __imp__sub_8236CE70(ctx, base); return; }
    const auto device = ctx.r3.u32;
    Graphics().Flush(base, device);
    ResetStaging(base, device);
}
REX_HOOK_RAW(sub_8236D1C8) {
    if (!NativeBootActive()) { __imp__sub_8236D1C8(ctx, base); return; }
    const auto device = ctx.r3.u32, target = ctx.r4.u32;
    Graphics().Flush(base, device);
    const auto current = REX_LOAD_U32(device + 10908);
    const auto completed = REX_LOAD_U32(REX_LOAD_U32(device + 10896));
    Require(uint32_t(current - target) >= uint32_t(current - completed), "Native wait requested an unsubmitted marker");
}
REX_HOOK_RAW(sub_8236C680) {
    if (!NativeBootActive()) { __imp__sub_8236C680(ctx, base); return; }
    const auto device = ctx.r3.u32;
    const auto extent = REX_LOAD_U32(device + 14912);
    Require(extent >= 160 && ctx.r4.u32 <= (extent - 160) / 4,
            "Native CPU staging request exceeds its allocation");
    Graphics().Flush(base, device);
    ctx.r3.u64 = ResetStaging(base, device) + 4;
}
REX_HOOK_RAW(sub_8236CFF8) {
    if (!NativeBootActive()) { __imp__sub_8236CFF8(ctx, base); return; }
    UnsupportedNativeOperation("indirect GPU command batch", ctx);
}
REX_HOOK_RAW(sub_8236C9E0) {
    if (!NativeBootActive()) { __imp__sub_8236C9E0(ctx, base); return; }
    UnsupportedNativeOperation("raw GPU ring submission", ctx);
}
REX_HOOK_RAW(sub_8237CA28) {
    if (!NativeBootActive()) { __imp__sub_8237CA28(ctx, base); return; }
    const auto device = ctx.r3.u32;
    Graphics().Flush(base, device);
    ResetStaging(base, device);
    REX_STORE_U32(device + 16560, REX_LOAD_U32(device + 16560) + 1);
    REX_STORE_U32(device + 16568, REX_LOAD_U32(device + 16560));
    REX_STORE_U32(device + 10928, REX_LOAD_U32(device + 10908) - 2);
}
REX_HOOK_RAW(sub_828648A8) {
    if (!NativeBootActive()) { __imp__sub_828648A8(ctx, base); return; }
    UnsupportedNativeOperation("direct begin-UP draw", ctx);
}
REX_HOOK_RAW(sub_82864D88) {
    if (!NativeBootActive()) { __imp__sub_82864D88(ctx, base); return; }
    UnsupportedNativeOperation("indexed begin-UP draw", ctx);
}
