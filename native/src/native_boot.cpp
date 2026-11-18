// Experimental live movie backend. No Xenos command processor or MMIO mapping.
// Legacy CPU state setters still write into their allocated staging memory;
// native draws consume the cached state above that packet boundary.
#include "generated/default/magnacarta2_init.h"
#include "native_boot.h"
#include "native_movie.h"
#include "native_scene.h"
#include "native_color_target.h"
#include "native_sampled_texture.h"
#include "draw_capture.h"
#include "guest_graphics_read.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <optional>
#include <bit>
#include <stdexcept>

REXCVAR_DEFINE_BOOL(mc2_native_boot, false, "Development", "Experimental native movie boot; unsupported operations fail");
REXCVAR_DEFINE_STRING(mc2_native_boot_shader_dir, "", "Development", "Verified first-draw movie shader inputs for native boot");
REXCVAR_DEFINE_STRING(mc2_native_boot_depth_shader_dir, "", "Development", "Optional verified indexed depth shader inputs for native boot");
REXCVAR_DEFINE_STRING(mc2_native_boot_color_shader_dir, "", "Development", "Optional verified indexed color shader inputs for native boot");
REXCVAR_DEFINE_BOOL(mc2_scene_resolve_trace, false, "Development", "Log the first tiled scene resolve arguments on the reference renderer");

namespace mc2::native {
namespace {
class NativeGraphics;
std::atomic<NativeGraphics*> active{nullptr};
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::unique_ptr<SampledTexture> UploadSceneTexture(uint8_t* base, const mc2::texture::BlockTexture& description,
                                                  const std::shared_ptr<NativeDevice>& gpu) {
    std::vector<std::vector<uint8_t>> levels(description.mips.size());
    for (uint32_t level = 0; level < levels.size(); ++level) {
        const auto& mip = description.mips[level];
        uint32_t alias;
        std::vector<uint8_t> source;
        Require(mc2::draw_capture::TexturePhysicalAlias(mip.physical_address, mip.source_bytes, alias) &&
                mc2::capture::ReadGuest(base, alias, mip.source_bytes, source), "Unreadable native scene texture mip");
        std::string error;
        if (!mc2::texture::LinearizeBlockMip(description, level, source, levels[level], error))
            throw std::runtime_error(error);
    }
    const auto& top = description.mips.front();
    const auto format = description.format == 18 ? plume::RenderFormat::BC1_UNORM : plume::RenderFormat::BC3_UNORM;
    return std::make_unique<SampledTexture>(format, top.width, top.height, levels, gpu);
}

void RequireFirstTileDepth(const SceneSurface& prepass, const SceneSurface& tile) {
    const auto& full_depth = prepass.description;
    const auto& tile_depth = tile.description;
    Require(full_depth.size() == 24 && tile_depth.size() == 24 &&
            std::equal(full_depth.begin(), full_depth.begin() + 12, tile_depth.begin()) &&
            std::equal(full_depth.begin() + 16, full_depth.begin() + 20, tile_depth.begin() + 16),
            "Native first tile depth does not alias the retained prepass");
}

struct TiledSceneDraw {
    DepthDraw draw;
    std::vector<uint8_t> pixel_constants;
    SceneSurface depth;
    std::array<std::unique_ptr<SampledTexture>, 2> textures;
    std::unique_ptr<ColorRenderer> renderer;

    uint64_t Render(const plume::RenderRect& rectangle) {
        draw.scissor = rectangle;
        const std::array<const SampledTexture*, 2> bindings{textures[0].get(), textures[1].get()};
        return renderer->Render(draw, pixel_constants, bindings);
    }
};

class NativeGraphics final : public rex::system::IGraphicsSystem {
 public:
    MovieShaders shaders;
    std::mutex mutex;
    std::shared_ptr<NativeDevice> gpu;
    std::unique_ptr<QuadRenderer> renderer;
    std::optional<SceneVertexShader> depth_shaders;
    std::optional<SceneColorShaders> color_shaders;
    std::optional<SceneSurface> depth_surface,color_surface;
    std::unique_ptr<DepthRenderer> depth_renderer;
    std::unique_ptr<ColorTarget> color_target;
    plume::RenderRect cleared_tile;
    std::optional<DepthDraw> prepass_draw;
    std::optional<TiledSceneDraw> scene_draw;
    std::array<SceneResolveTexture, 2> scene_textures;
    uint32_t scene_copies = 0;
    uint64_t depth_draws = 0;
    std::optional<QuadDraw> pending;
    HWND window = nullptr;
    uint64_t completed = 0;
    uint32_t guest_device = 0;
    std::string last_clear_trace;
    uint32_t clear_trace_count = 0;

    NativeGraphics() : shaders(LoadMovieShaders(REXCVAR_GET(mc2_native_boot_shader_dir))) {
        const auto depth_directory = REXCVAR_GET(mc2_native_boot_depth_shader_dir);
        if (!depth_directory.empty()) depth_shaders=LoadDepthShaders(depth_directory);
        const auto color_directory = REXCVAR_GET(mc2_native_boot_color_shader_dir);
        if (!color_directory.empty()) {
            Require(bool(depth_shaders), "Native color shaders require the verified depth shader directory");
            color_shaders = LoadColorShaders(color_directory);
        }
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
        prepass_draw.reset(); scene_draw.reset();
        pending.reset(); renderer.reset(); depth_renderer.reset(); color_target.reset(); gpu.reset();
    }
    void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
        Require(callback == 0x8236D4E0 && user_data != 0, "Unexpected legacy graphics interrupt callback");
        guest_device = user_data;
    }
    void InitializeRingBuffer(uint32_t, uint32_t) override {
        REXLOG_INFO("MC2 native boot: legacy staging memory retained for CPU setters; no packet execution");
    }
    void EnableReadPointerWriteBack(uint32_t, uint32_t) override {}

    bool Clear(const PPCContext& ctx, uint8_t* base) {
        if (!depth_shaders) return false;
        if (!(ctx.r4.u32 & 0x30)) {
            {
                std::lock_guard lock(mutex);
                if (!depth_renderer) return false;
            }
            const auto clear=ReadSceneColorClear(ctx,base);
            Flush(base,ctx.r3.u32);
            std::lock_guard lock(mutex);
            if (!color_target) {
                const auto resolve_shader = color_shaders ? std::span<const uint8_t>(color_shaders->resolve_dxil) : std::span<const uint8_t>{};
                color_target=std::make_unique<ColorTarget>(clear.surface.width,clear.surface.height,clear.surface.samples,gpu,resolve_shader);
                color_surface=clear.surface;
            }
            Require(*color_surface==clear.surface,"Native color surface allocation changed");
            completed=color_target->Clear(clear.value,&clear.rectangle);
            cleared_tile = clear.rectangle;
            REXLOG_INFO("MC2 native scene: cleared HDR rectangle [{},{},{},{}] on {}x{}, samples={}, completed={}",
                clear.rectangle.left,clear.rectangle.top,clear.rectangle.right,clear.rectangle.bottom,
                clear.surface.width,clear.surface.height,clear.surface.samples,completed);
            return true;
        }
        const auto clear=ReadDepthClear(ctx,base);
        Flush(base,ctx.r3.u32);
        std::lock_guard lock(mutex);
        if (!gpu) gpu=std::make_shared<NativeDevice>();
        if (!depth_renderer) {
            const auto resolve_shader = color_shaders ? std::span<const uint8_t>(color_shaders->depth_resolve_dxil) : std::span<const uint8_t>{};
            depth_renderer=std::make_unique<DepthRenderer>(ConfigureDepth(clear.surface,*depth_shaders),gpu,resolve_shader);
            depth_surface=clear.surface;
        }
        Require(*depth_surface == clear.surface, "Native depth surface allocation changed");
        // The verified pass clears stencil to zero but never reads it. Until
        // native stencil storage exists, ReadSceneDepthDraw rejects every
        // stencil-tested draw, so an unsupported effect cannot silently vanish.
        depth_renderer->Clear(clear.value);
        completed=gpu->completed;
        REXLOG_INFO("MC2 native scene: cleared {}x{} depth, samples={}, completed={}",
                    clear.surface.width,clear.surface.height,clear.surface.samples,completed);
        return true;
    }
    void DrawDepth(const PPCContext& ctx, uint8_t* base) {
        Require(bool(depth_shaders), "Indexed depth shader directory is not configured");
        auto snapshot=ReadSceneDepthDraw(ctx,base,*depth_shaders);
        Flush(base,ctx.r3.u32);
        std::lock_guard lock(mutex);
        Require(depth_renderer && depth_surface && *depth_surface == snapshot.surface,
                "Native scene draw has no matching cleared depth surface");
        const auto result=depth_renderer->Render(snapshot.draw);
        completed=result.completed_submission;
        ++depth_draws;
        if (depth_draws <= 16 || depth_draws%120 == 0)
            REXLOG_INFO("MC2 native scene: indexed depth draw {}, indices={}, samples={}, completed={}",
                        depth_draws,snapshot.draw.index_count,snapshot.surface.samples,completed);
        if (color_shaders && depth_draws == 1) prepass_draw = std::move(snapshot.draw);
    }

    void DrawColor(const PPCContext& ctx, uint8_t* base) {
        Require(bool(color_shaders), "Indexed color shader directory is not configured");
        auto snapshot = ReadSceneColorDraw(ctx, base, *color_shaders);
        Flush(base, ctx.r3.u32);
        std::lock_guard lock(mutex);
        Require(!scene_draw && scene_copies == 0 && depth_draws == 1 && prepass_draw && depth_renderer && color_target && depth_surface &&
                color_surface && *color_surface == snapshot.color_surface &&
                cleared_tile == plume::RenderRect(0,0,1280,512), "Native color draw has no matching first prepass and color clear");
        // The first tile starts at (0,0), with the same depth base, format and
        // pitch as the full prepass. Keep the lower region of that logical
        // attachment for the matching opaque draw when the second tile runs.
        RequireFirstTileDepth(*depth_surface, snapshot.tile_depth);
        Require(SameScenePositionInputs(*prepass_draw, snapshot.draw), "Tiled scene draw differs from the verified depth prepass geometry");
        TiledSceneDraw draw;
        draw.draw = std::move(snapshot.draw);
        draw.pixel_constants = std::move(snapshot.pixel_constants);
        draw.depth = std::move(snapshot.tile_depth);
        for (uint32_t slot = 0; slot < draw.textures.size(); ++slot)
            draw.textures[slot] = UploadSceneTexture(base, snapshot.textures[slot], gpu);
        draw.renderer = std::make_unique<ColorRenderer>(ConfigureDepth(snapshot.color_surface, color_shaders->vertex),
            color_shaders->pixel_dxil, snapshot.samplers, *depth_renderer, *color_target);
        completed = draw.Render(cleared_tile);
        REXLOG_INFO("MC2 native scene: indexed HDR color draw in first tile, indices={}, BC mip levels={}/{}, completed={}",
            draw.draw.index_count, snapshot.textures[0].mips.size(), snapshot.textures[1].mips.size(), completed);
        scene_draw = std::move(draw);
        prepass_draw.reset();
    }

    bool HasColorDraw() {
        std::lock_guard lock(mutex);
        return scene_draw || scene_copies != 0;
    }

    void ResolveTile(const PPCContext& ctx, uint8_t* base) {
        std::lock_guard lock(mutex);
        Require(color_shaders && !color_shaders->resolve_dxil.empty(), "Tiled scene resolve shader is not configured");
        Require(ctx.r3.u32 == guest_device && scene_draw && scene_copies < 4, "Unexpected native scene resolve");
        const auto resolve = ReadSceneTileResolve(ctx, base);
        const uint32_t kind = resolve.copy_depth ? 1 : 0;
        Require(scene_copies == resolve.tile * 2 + kind, "Native tiled scene copies are out of order");
        Require(*color_surface == resolve.color && scene_draw->depth == resolve.depth,
                "Native resolve attachments differ from the rendered tile");
        auto& destination = scene_textures[kind];
        Require(!destination.address || destination == resolve.destination, "Native resolve destination allocation changed between tiles");
        Require(scene_textures[1 - kind].address != resolve.destination.address, "Native color and depth resolves require separate textures");
        if (resolve.tile == 1 && !resolve.copy_depth) {
            color_target->Clear(plume::RenderColor(0,0,0,0), &resolve.rectangle);
            completed = scene_draw->Render(resolve.rectangle);
            cleared_tile = resolve.rectangle;
            REXLOG_INFO("MC2 native scene: replayed indexed HDR draw in lower tile, indices={}, completed={}",
                        scene_draw->draw.index_count, completed);
        }
        Require(cleared_tile == resolve.rectangle, "Native resolve has no matching rendered tile");
        if (resolve.copy_depth) {
            Require(!color_shaders->depth_resolve_dxil.empty(), "Tiled depth resolve shader is not configured");
            completed = depth_renderer->ResolveAndClear(resolve.rectangle);
            REXLOG_INFO("MC2 native scene: resolved and cleared depth tile {} into texture {:#x}, sample=0, completed={}",
                        resolve.tile, resolve.destination.address, completed);
        } else {
            completed = color_target->ResolveAndClear(resolve.rectangle, resolve.exponent);
            REXLOG_INFO("MC2 native scene: resolved and cleared HDR tile {} into texture {:#x}, exponent={}, completed={}",
                        resolve.tile, resolve.destination.address, resolve.exponent, completed);
        }
        destination = resolve.destination;
        if (++scene_copies == 4) scene_draw.reset();
    }

    // Record bounded changes at the clear boundary while surface support grows.
    // Logging itself never substitutes a clear or changes the PPC call.
    void TraceClear(const PPCContext& ctx, uint8_t* base) {
        std::lock_guard lock(mutex);
        if (clear_trace_count >= 64) return;
        std::vector<uint8_t> state, rect, descriptor;
        using mc2::capture::ReadGuest;
        using mc2::capture::LoadBE32;
        if (!ReadGuest(base, ctx.r3.u32, 24064, state) || !ReadGuest(base, ctx.r5.u32, 16, rect)) return;
        const auto word = [&](uint32_t offset) { return LoadBE32(state.data() + offset); };
        const auto number = [&](uint32_t offset) { return std::bit_cast<float>(word(offset)); };
        const auto depth = word(12464);
        uint32_t samples = 0, width = 0, height = 0;
        if (depth && ReadGuest(base, depth, 48, descriptor)) {
            const auto surface = LoadBE32(descriptor.data() + 24);
            const auto dimensions = LoadBE32(descriptor.data() + 36);
            samples = 1u << ((surface >> 16) & 3);
            width = ((dimensions >> 18) & 16383) + 1;
            height = ((dimensions >> 3) & 32767) + 1;
        }
        const auto trace = fmt::format("flags={:#x} colors=[{:#x},{:#x},{:#x},{:#x}] depth={:#x} {}x{} samples={} z={} rect=[{},{},{},{}] viewport=[{},{},{},{}] scissor_enabled={} scissor=[{},{},{},{}] tiling={:#x} tiles={} logical={}x{} predicate_mode={} predicate={:#x}",
            ctx.r4.u32, word(12448), word(12452), word(12456), word(12460), depth, width, height, samples,
            ctx.f1.f64, LoadBE32(rect.data()), LoadBE32(rect.data()+4), LoadBE32(rect.data()+8), LoadBE32(rect.data()+12),
            number(12656), number(12660), number(12664), number(12668), word(11864),
            word(12684), word(12688), word(12692), word(12696),state[10940],word(12756),
            word(13188),word(13192),word(12724),word(12716));
        if (trace != last_clear_trace) {
            REXLOG_INFO("MC2 native clear boundary: {}", trace);
            for (uint32_t i=0;i<std::min(word(12756),uint32_t(8));++i) {
                const auto offset=12760+i*16;
                REXLOG_INFO("MC2 native tile {}: [{},{},{},{}]",i,word(offset),word(offset+4),word(offset+8),word(offset+12));
            }
            for (uint32_t i=0;i<4;++i) {
                if (!word(12448+i*4) || !ReadGuest(base,word(12448+i*4),48,descriptor)) continue;
                REXLOG_INFO("MC2 native color surface {}: info={:#x} color_info={:#x} format={:#x} dimensions={:#x}",
                    i,LoadBE32(descriptor.data()+24),LoadBE32(descriptor.data()+28),
                    LoadBE32(descriptor.data()+32),LoadBE32(descriptor.data()+36));
            }
            std::vector<uint8_t> color;
            if (ReadGuest(base,ctx.r6.u32 ? ctx.r6.u32 : 0x82247720,16,color)) {
                REXLOG_INFO("MC2 native clear color: [{},{},{},{}]",
                    std::bit_cast<float>(LoadBE32(color.data())),std::bit_cast<float>(LoadBE32(color.data()+4)),
                    std::bit_cast<float>(LoadBE32(color.data()+8)),std::bit_cast<float>(LoadBE32(color.data()+12)));
            }
            last_clear_trace = trace; ++clear_trace_count;
        }
    }

    void Flush(uint8_t* base, uint32_t device) {
        std::lock_guard lock(mutex);
        Require(device != 0 && device == guest_device, "Native operation used an unknown device");
        if (pending) {
            Require(window && IsWindow(window), "Native game window is unavailable");
            if (!renderer) {
                if (!gpu) gpu=std::make_shared<NativeDevice>();
                renderer = std::make_unique<QuadRenderer>(*pending,gpu);
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

void TraceSceneResolve(const PPCContext& ctx, uint8_t* base) {
    if (ctx.lr != 0x826DA2D8 && ctx.lr != 0x826DA314) return;
    static std::atomic<uint32_t> count = 0;
    if (count.load(std::memory_order_relaxed) >= 8 || count.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    using mc2::capture::ReadGuest;
    using mc2::capture::LoadBE32;
    std::vector<uint8_t> device, rect, destination, origin, color, stencil;
    if (!ReadGuest(base, ctx.r3.u32, 13128, device) || !ReadGuest(base, ctx.r5.u32, 16, rect) ||
        !ReadGuest(base, ctx.r6.u32, 52, destination) || !ReadGuest(base, ctx.r7.u32, 8, origin)) return;
    const auto state = [&](uint32_t offset) { return LoadBE32(device.data() + offset); };
    const auto dest = [&](uint32_t offset) { return LoadBE32(destination.data() + offset); };
    REXLOG_INFO("MC2 scene resolve: caller={:#x} flags={:#x} predicate={:#x} rect=[{},{},{},{}] destination={:#x} origin=[{},{}] depth_clear={} slice={} color_ptr={:#x}",
        ctx.lr, ctx.r4.u32, state(12716), LoadBE32(rect.data()), LoadBE32(rect.data()+4),
        LoadBE32(rect.data()+8), LoadBE32(rect.data()+12), ctx.r6.u32, LoadBE32(origin.data()),
        LoadBE32(origin.data()+4), ctx.f1.f64, ctx.r8.u32, ctx.r9.u32);
    REXLOG_INFO("MC2 scene resolve surface: type={:#x} words=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}] color={:#x} depth={:#x} hiz=[{:#x},{:#x}]",
        dest(0), dest(24), dest(28), dest(32), dest(36), dest(40), dest(44), dest(48),
        state(10372), state(10376), state(13120), state(13124));
    if (ReadGuest(base, ctx.r9.u32 ? ctx.r9.u32 : 0x82247720, 16, color) &&
        mc2::capture::GuestRange(ctx.r1.u32, 88) && ReadGuest(base, ctx.r1.u32 + 84, 4, stencil))
        REXLOG_INFO("MC2 scene resolve clear words: [{:#x},{:#x},{:#x},{:#x}] stencil={:#x}",
            LoadBE32(color.data()), LoadBE32(color.data()+4), LoadBE32(color.data()+8), LoadBE32(color.data()+12),
            LoadBE32(stencil.data()));
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
void DrawNativeSceneIndexed(const PPCContext& ctx, uint8_t* base) {
    try {
        auto& graphics = Graphics();
        Require(ctx.r3.u32 != 0 && ctx.r3.u32 == graphics.guest_device, "Native indexed draw used an unknown device");
        Require(!graphics.HasColorDraw(), "Additional draws before the native scene resolve are unsupported");
        if (REX_LOAD_U32(ctx.r3.u32 + 12448)) graphics.DrawColor(ctx, base);
        else graphics.DrawDepth(ctx, base);
    }
    catch (const std::exception& error) { UnsupportedNativeOperation(error.what(),ctx); }
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
REX_HOOK_RAW(sub_8238AD80) {
    if (NativeBootActive()) {
        Graphics().TraceClear(ctx, base);
        try { if (Graphics().Clear(ctx,base)) return; }
        catch (const std::exception& error) { UnsupportedNativeOperation(error.what(),ctx); }
    }
    __imp__sub_8238AD80(ctx, base);
}
REX_HOOK_RAW(sub_823893A0) {
    if (REXCVAR_GET(mc2_scene_resolve_trace)) TraceSceneResolve(ctx, base);
    if (NativeBootActive() && Graphics().HasColorDraw()) {
        try { Graphics().ResolveTile(ctx, base); }
        catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
        // Preserve the guest's CPU resource bookkeeping. The original GPU
        // commands go only to staging; the native copy has already completed.
    }
    __imp__sub_823893A0(ctx, base);
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
