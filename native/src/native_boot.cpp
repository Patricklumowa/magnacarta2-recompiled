// Experimental live movie backend. No Xenos command processor or MMIO mapping.
// Legacy CPU state setters still write into their allocated staging memory;
// native draws consume the cached state above that packet boundary.
#include "generated/default/magnacarta2_init.h"
#include "native_boot.h"
#include "native_movie.h"
#include "native_scene.h"
#include "native_color_target.h"
#include "native_sampled_texture.h"
#include "native_shader_input.h"
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
REXCVAR_DEFINE_BOOL(mc2_scene_resolve_trace, false, "Development", "Log the first scene and postprocess resolve arguments");

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

std::unique_ptr<SampledTexture> UploadSceneTexture(uint8_t* base, const mc2::texture::BaseRGBA8& description,
                                                  const std::shared_ptr<NativeDevice>& gpu) {
    uint32_t alias;
    std::vector<uint8_t> source, pixels;
    Require(mc2::draw_capture::TexturePhysicalAlias(description.physical_address, description.source_bytes, alias) &&
            mc2::capture::ReadGuest(base, alias, description.source_bytes, source), "Unreadable native overlay texture");
    std::string error;
    if (!mc2::texture::DecodeBaseRGBA8(description, source, pixels, error)) throw std::runtime_error(error);
    return std::make_unique<SampledTexture>(plume::RenderFormat::R8G8B8A8_UNORM, description.width,
        description.height, std::span(&pixels, 1), gpu);
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

struct PendingDepthRestore {
    SceneDepthRestore scene;
    uint32_t vertices, endpoint;
};
enum class SceneQuadPass { HdrCopy, Bloom, Composition, BackBuffer, Overlay };
struct PendingUpDraw {
    SceneFullscreenDraw scene;
    uint32_t vertices, indices, endpoint;
    SceneQuadPass pass;
    std::optional<SceneOverlayBinding> overlay;
};
struct PendingMenuFan {
    SceneMenuFan scene;
    uint32_t vertices, endpoint;
};
struct CachedQuadRenderer {
    const ScenePassShaders* program;
    float max_lod;
    std::unique_ptr<ColorRenderer> renderer;
};

class NativeGraphics final : public rex::system::IGraphicsSystem {
 public:
    MovieShaders shaders;
    std::mutex mutex;
    std::shared_ptr<NativeDevice> gpu;
    std::unique_ptr<QuadRenderer> renderer;
    std::optional<SceneVertexShader> depth_shaders;
    std::optional<SceneColorShaders> color_shaders;
    std::optional<ScenePassShaders> depth_restore_shaders;
    std::optional<ScenePassShaders> fullscreen_shaders, color_grade_shaders;
    std::optional<ScenePassShaders> bloom_shaders, composition_shaders, back_buffer_shaders;
    std::vector<ScenePassShaders> filter_shaders;
    std::optional<ScenePassShaders> overlay_shaders, menu_fan_shaders, menu_fan_additive_shaders;
    std::vector<uint8_t> fullscreen_resolve_shader, bloom_resolve_shader;
    std::vector<uint8_t> present_vertex_shader, present_pixel_shader;
    std::optional<SceneSurface> depth_surface,color_surface;
    std::unique_ptr<DepthRenderer> depth_renderer;
    std::unique_ptr<ColorTarget> color_target;
    std::optional<SceneSurface> stencil_surface;
    std::unique_ptr<plume::RenderTexture> stencil_target;
    std::unique_ptr<plume::RenderFramebuffer> stencil_framebuffer;
    std::optional<PendingDepthRestore> pending_restore;
    bool depth_restored = false;
    std::optional<PendingUpDraw> pending_up;
    std::optional<PendingMenuFan> pending_menu_fan;
    std::optional<SceneSurface> fullscreen_surface;
    std::unique_ptr<ColorTarget> fullscreen_target;
    bool color_graded = false;
    bool grade_resolved = false;
    bool scene_composited = false;
    bool composition_resolved = false;
    std::optional<SceneSurface> bloom_surface;
    std::unique_ptr<ColorTarget> bloom_target;
    std::optional<SceneResolveTexture> bloom_texture;
    uint32_t bloom_draws = 0, bloom_copies = 0;
    std::unique_ptr<ColorTarget> back_buffer;
    std::optional<SceneSurface> back_buffer_surface;
    std::vector<CachedQuadRenderer> quad_renderers;
    uint32_t overlay_draws = 0;
    uint32_t scanout_texture = 0;
    plume::RenderRect cleared_tile;
    std::optional<DepthDraw> prepass_draw;
    std::optional<TiledSceneDraw> scene_draw;
    std::array<SceneResolveTexture, 2> scene_textures;
    uint32_t scene_copies = 0;
    bool retiring_tiles = false;
    bool scene_tiles_retired = false;
    uint64_t depth_draws = 0;
    uint64_t scene_frames = 0;
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
            const auto restore_directory = std::filesystem::path(color_directory) / "depth-restore";
            if (std::filesystem::is_directory(restore_directory)) depth_restore_shaders = LoadDepthRestoreShaders(restore_directory);
            const auto fullscreen_directory = std::filesystem::path(color_directory) / "fullscreen";
            if (std::filesystem::is_directory(fullscreen_directory)) fullscreen_shaders = LoadFullscreenColorShaders(fullscreen_directory, false);
            const auto fullscreen_resolve_path = fullscreen_directory / "color-resolve.dxil";
            if (std::filesystem::exists(fullscreen_resolve_path)) fullscreen_resolve_shader = ReadNativeFile(fullscreen_resolve_path);
            const auto composition_directory = fullscreen_directory / "compose";
            if (std::filesystem::is_directory(composition_directory)) composition_shaders = LoadQuadShaders(composition_directory, "pixel-7e3.dxil");
            const auto back_buffer_directory = fullscreen_directory / "back-buffer";
            if (std::filesystem::is_directory(back_buffer_directory)) back_buffer_shaders = LoadQuadShaders(back_buffer_directory, "pixel.dxil");
            const auto overlay_directory = fullscreen_directory / "overlay";
            if (std::filesystem::is_directory(overlay_directory)) overlay_shaders = LoadFullscreenColorShaders(overlay_directory, false, "pixel.dxil");
            const auto menu_fan_directory = fullscreen_directory / "menu-fan";
            if (std::filesystem::is_directory(menu_fan_directory)) menu_fan_shaders = LoadMenuFanShaders(menu_fan_directory);
            const auto menu_additive_directory = menu_fan_directory / "additive";
            if (std::filesystem::is_directory(menu_additive_directory))
                menu_fan_additive_shaders = LoadMenuFanShaders(menu_additive_directory);
            const auto present_directory = fullscreen_directory / "present";
            if (std::filesystem::is_directory(present_directory)) {
                present_vertex_shader = ReadNativeFile(present_directory / "vertex.dxil");
                present_pixel_shader = ReadNativeFile(present_directory / "pixel.dxil");
            }
            const auto grade_directory = std::filesystem::path(color_directory) / "color-grade";
            if (std::filesystem::is_directory(grade_directory)) color_grade_shaders = LoadFullscreenColorShaders(grade_directory, true);
            const auto bloom_directory = std::filesystem::path(color_directory) / "bloom";
            if (std::filesystem::is_directory(bloom_directory)) bloom_shaders = LoadQuadShaders(bloom_directory, "pixel-fixed16.dxil");
            const auto bloom_resolve_path = bloom_directory / "color-resolve.dxil";
            if (std::filesystem::exists(bloom_resolve_path)) bloom_resolve_shader = ReadNativeFile(bloom_resolve_path);
            for (const auto* name : {"filter", "filter-three-taps", "filter-five-taps"}) {
                const auto filter_directory = bloom_directory / name;
                if (std::filesystem::is_directory(filter_directory))
                    filter_shaders.push_back(LoadQuadShaders(filter_directory, "pixel-fixed16.dxil"));
            }
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
        prepass_draw.reset(); scene_draw.reset(); pending_restore.reset();
        quad_renderers.clear();
        pending_up.reset(); pending_menu_fan.reset(); fullscreen_target.reset();
        bloom_target.reset();
        back_buffer.reset();
        stencil_framebuffer.reset(); stencil_target.reset();
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

    void ClearPostprocessDepthStencil(const PPCContext& ctx, uint8_t* base) {
        using namespace plume;
        const auto surface = ReadPostprocessDepthStencilClear(ctx, base);
        const bool clear_depth = ctx.r4.u32 == 0x30;
        Flush(base, ctx.r3.u32);
        std::lock_guard lock(mutex);
        const bool expected_surface = stencil_surface
            ? fullscreen_surface && !color_graded && *stencil_surface == surface
            : !clear_depth;
        Require(scene_tiles_retired && expected_surface, "Unexpected native depth/stencil clear outside postprocessing");
        if (!stencil_target) {
            stencil_target = gpu->device->createTexture(RenderTextureDesc::DepthTarget(
                surface.width, surface.height, RenderFormat::D32_FLOAT_S8_UINT));
            Require(bool(stencil_target), "Native stencil allocation failed");
            RenderFramebufferDesc desc;
            desc.depthAttachment = stencil_target.get();
            stencil_framebuffer = gpu->device->createFramebuffer(desc);
            Require(bool(stencil_framebuffer), "Native stencil framebuffer creation failed");
        }
        auto& commands = gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(stencil_target.get(), RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(stencil_framebuffer.get());
        commands->clearDepthStencil(clear_depth, true, 0, 0, nullptr, 0);
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        completed = gpu->Complete();
        stencil_surface = surface;
        REXLOG_INFO("MC2 native scene: cleared single-sample {} at {:#x}, completed={}",
                    clear_depth ? "depth and stencil" : "stencil", surface.address, completed);
    }

    bool Clear(const PPCContext& ctx, uint8_t* base) {
        if (!depth_shaders) return false;
        bool postprocess_clear = ctx.r4.u32 == 0x20;
        if (ctx.r4.u32 == 0x30) {
            std::lock_guard lock(mutex);
            postprocess_clear = bool(fullscreen_surface);
        }
        if (postprocess_clear) {
            ClearPostprocessDepthStencil(ctx, base);
            return true;
        }
        if (!(ctx.r4.u32 & 0x30)) {
            bool bloom;
            {
                std::lock_guard lock(mutex);
                if (!depth_surface) return false;
                Require(ctx.r3.u32 && ctx.r3.u32 == guest_device, "Color clear used an unknown device");
                bloom = REX_LOAD_U32(ctx.r3.u32 + 10372) == 0x505a0;
                Require(!bloom || (fullscreen_surface && depth_restored && (!color_graded || grade_resolved)),
                        "Bloom clear requires the prepared scene color and depth");
            }
            const auto clear=bloom ? ReadBloomClear(ctx,base) : ReadSceneColorClear(ctx,base);
            Flush(base,ctx.r3.u32);
            std::lock_guard lock(mutex);
            if (bloom) {
                if (!bloom_target) {
                    bloom_target = std::make_unique<ColorTarget>(clear.surface.width, clear.surface.height, 1,
                        gpu, bloom_resolve_shader, plume::RenderFormat::R16G16B16A16_SNORM);
                }
                if (!bloom_surface) bloom_surface = clear.surface;
                Require(*bloom_surface == clear.surface, "Native bloom target allocation changed");
                completed = bloom_target->Clear(clear.value);
                REXLOG_INFO("MC2 native scene: cleared {}x{} fixed-point bloom target, completed={}",
                            clear.surface.width, clear.surface.height, completed);
                return true;
            }
            if (!color_target) {
                const auto resolve_shader = color_shaders ? std::span<const uint8_t>(color_shaders->resolve_dxil) : std::span<const uint8_t>{};
                color_target=std::make_unique<ColorTarget>(clear.surface.width,clear.surface.height,clear.surface.samples,gpu,resolve_shader);
            }
            if (!color_surface) color_surface = clear.surface;
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
        }
        if (!depth_surface) depth_surface = clear.surface;
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
        Require(!scene_draw && scene_copies == 0 && depth_renderer && color_target && depth_surface &&
                color_surface && *color_surface == snapshot.color_surface &&
                cleared_tile == plume::RenderRect(0,0,1280,512), "Native color draw requires cleared depth and color attachments");
        // The first tile starts at (0,0), with the same depth base, format and
        // pitch as the full prepass. Keep the lower region of that logical
        // attachment for the matching opaque draw when the second tile runs.
        RequireFirstTileDepth(*depth_surface, snapshot.tile_depth);
        if (depth_draws != 0)
            Require(depth_draws == 1 && prepass_draw && SameScenePositionInputs(*prepass_draw, snapshot.draw),
                    "Tiled scene draw differs from the depth prepass geometry");
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

    void RetireSceneTiles(PPCContext& ctx, uint8_t* base) {
        const uint32_t device = ctx.r3.u32;
        Require(device && device == guest_device && ctx.lr == 0x8237C1E4, "Unexpected tiled scene retirement caller");
        const auto mode = REX_LOAD_U8(device + 10941);
        const auto progress = REX_LOAD_U32(device + 10896);
        const auto marker = REX_LOAD_U32(device + 10908);
        Require((mode & ~0x10u) == 4 && progress && REX_LOAD_U32(device + 11012) == 0 && REX_LOAD_U32(device + 21532) == 0,
                "Native tiled scene retirement needs a single CPU submission queue");
        {
            std::lock_guard lock(mutex);
            Require(scene_copies == 4 && !scene_tiles_retired && !retiring_tiles && !pending && gpu && completed == gpu->completed,
                    "Tile retirement requires four completed native copies");
            retiring_tiles = true;
        }

        // The guest's no-hardware branch unlinks its staging chunks and writes
        // progress directly on the CPU. The native draw/resolve fences have
        // already completed, so no GPU callback is needed before retirement.
        REX_STORE_U8(device + 10941, mode | 2);
        try {
            __imp__sub_82388898(ctx, base);
        } catch (...) {
            REX_STORE_U8(device + 10941, mode);
            std::lock_guard lock(mutex);
            retiring_tiles = false;
            throw;
        }
        REX_STORE_U8(device + 10941, mode);
        std::lock_guard lock(mutex);
        retiring_tiles = false;
        const auto retired_cursor = REX_LOAD_U32(device + 13240) | (REX_LOAD_U32(device + 13244) & 3);
        Require(ctx.r3.u32 == 0 && retired_cursor && REX_LOAD_U32(progress) == marker && REX_LOAD_U32(progress + 4) == retired_cursor &&
                REX_LOAD_U32(device + 10908) == marker + 2 && REX_LOAD_U32(device + 11012) == 0,
                "Native tile retirement did not publish the completed guest marker");
        scene_tiles_retired = true;
        REXLOG_INFO("MC2 native scene: retired both tiles on the CPU after fence {}, guest marker={}", completed, marker);
    }

    bool RetireTileBatch(PPCContext& ctx, uint8_t* base) {
        std::lock_guard lock(mutex);
        if (!retiring_tiles) return false;
        const auto device = ctx.r3.u32;
        const bool cache_batch = ctx.lr == 0x82388A14 && ctx.r6.u32 == 11 && ctx.r8.u32 == device + 13388;
        const bool completion_batch = ctx.lr == 0x82388A94 && ctx.r6.u32 == 10 && ctx.r8.u32 == device + 13368;
        Require(device == guest_device && (cache_batch || completion_batch) && ctx.r7.u32 == 0 &&
                (REX_LOAD_U8(device + 10941) & 2) && REX_LOAD_U32(device + 11012) == 0,
                "Unsupported batch during native tile retirement");
        // Matches the no-hardware path with no outstanding GPU submits: no
        // linked entry is created and the caller's cursor is returned unchanged.
        ctx.r3.u64 = ctx.r4.u64;
        return true;
    }

    void BeginDepthRestore(PPCContext& ctx, uint8_t* base) {
        const uint32_t device = ctx.r3.u32;
        SceneDepthRestore scene;
        {
            std::lock_guard lock(mutex);
            Require(device && device == guest_device && scene_tiles_retired && stencil_surface && !pending_restore &&
                    !depth_restored && depth_restore_shaders, "Direct begin-UP draw requires the first native depth restore shaders");
            scene = ReadSceneDepthRestore(ctx, base, *depth_restore_shaders, scene_textures[1]);
            Require(scene.depth == *stencil_surface, "Native depth restore needs the cleared single-sample stencil surface");
        }
        // Preserve the guest's allocation and cursor bookkeeping. The caller
        // writes vertex bytes after BeginVertices returns.
        __imp__sub_828648A8(ctx, base);
        const auto vertices = REX_LOAD_U32(device + 13448);
        const auto endpoint = REX_LOAD_U32(device + 13444);
        Require(vertices && !(vertices & 15) && ctx.r3.u32 == vertices && endpoint && !(endpoint & 3),
                "Native depth restore staging allocation failed");
        std::lock_guard lock(mutex);
        pending_restore = PendingDepthRestore{std::move(scene), vertices, endpoint};
    }

    void CompleteDepthRestore(uint8_t* base) {
        std::lock_guard lock(mutex);
        if (!pending_restore) return;
        auto& restore = *pending_restore;
        Require(REX_LOAD_U32(guest_device + 48) == restore.endpoint && REX_LOAD_U32(guest_device + 13448) == restore.vertices,
                "Guest did not commit the native depth restore vertices");
        ReadDepthRestoreVertices(restore.scene.draw, base, restore.vertices);
        auto configuration = ConfigureDepth(restore.scene.depth, depth_restore_shaders->vertex);
        configuration.comparison = plume::RenderComparisonFunction::ALWAYS;
        configuration.cull = plume::RenderCullMode::NONE;
        completed = depth_renderer->RestoreResolvedDepth(configuration, restore.scene.draw,
            depth_restore_shaders->pixel_dxil, restore.scene.pixel_constants, *stencil_target, *stencil_framebuffer);
        pending_restore.reset();
        depth_restored = true;
        REXLOG_INFO("MC2 native scene: restored single-sample depth from texture {:#x}, completed={}",
                    scene_textures[1].address, completed);
    }

    void BeginIndexedUp(PPCContext& ctx, uint8_t* base) {
        const uint32_t device = ctx.r3.u32;
        SceneFullscreenDraw scene;
        std::optional<SceneOverlayBinding> overlay;
        SceneQuadPass pass;
        {
            std::lock_guard lock(mutex);
            Require(device && device == guest_device && depth_restored && !pending_up && !pending_menu_fan && !scanout_texture,
                    "Unexpected native indexed user-memory draw");
            if (!fullscreen_surface) {
                pass = SceneQuadPass::HdrCopy;
                Require(bool(fullscreen_shaders), "Indexed begin-UP draw requires the first fullscreen HDR shaders");
                scene = ReadSceneFullscreenDraw(ctx, base, *fullscreen_shaders, scene_textures[0], false);
            } else if (bloom_copies < 3) {
                pass = SceneQuadPass::Bloom;
                Require(bloom_surface && bloom_target && bloom_draws == bloom_copies,
                        "Bloom draw requires its cleared target and preceding resolve");
                if (bloom_draws == 0) {
                    Require(bool(bloom_shaders), "Initial bloom shaders are unavailable");
                    scene = ReadBloomDraw(ctx, base, std::span(&*bloom_shaders,1), scene_textures[1], &scene_textures[0]);
                } else {
                    Require(!filter_shaders.empty() && bloom_texture, "Bloom filter shaders or resolved texture are unavailable");
                    scene = ReadBloomDraw(ctx, base, filter_shaders, *bloom_texture);
                }
                Require(*bloom_surface == scene.surface, "Bloom draw differs from its cleared surface");
            } else if (!composition_resolved) {
                pass = SceneQuadPass::Composition;
                Require(composition_shaders && bloom_texture && !scene_composited,
                        "Fullscreen composition requires the three ready scene textures and translated shaders");
                scene = ReadCompositionDraw(ctx, base, *composition_shaders, scene_textures[1], scene_textures[0], *bloom_texture);
                Require(*fullscreen_surface == scene.surface, "Composition destination differs from the fullscreen HDR target");
            } else if (!back_buffer_surface) {
                pass = SceneQuadPass::BackBuffer;
                Require(bool(back_buffer_shaders), "Back-buffer copy requires the final translated shaders");
                scene = ReadBackBufferDraw(ctx, base, *back_buffer_shaders, scene_textures[0]);
            } else {
                pass = SceneQuadPass::Overlay;
                Require(bool(overlay_shaders), "Back-buffer overlay requires the translated shaders");
                scene = ReadBackBufferOverlay(ctx, base, *overlay_shaders, overlay.emplace());
                Require(*back_buffer_surface == scene.surface, "Overlay destination differs from the live back buffer");
            }
        }
        const auto vertex_out = REX_LOAD_U32(ctx.r1.u32 + 84), index_out = ctx.r10.u32;
        __imp__sub_82864D88(ctx, base);
        Require(ctx.r3.u32 == 0, "Native indexed user-memory allocation failed");
        const auto vertices = REX_LOAD_U32(vertex_out), indices = REX_LOAD_U32(index_out);
        const auto endpoint = REX_LOAD_U32(device + 13444);
        Require(vertices && indices && endpoint && !(vertices & 15) && !(indices & 3) && !(endpoint & 3),
                "Invalid native indexed user-memory addresses");
        std::lock_guard lock(mutex);
        pending_up = PendingUpDraw{std::move(scene), vertices, indices, endpoint, pass, std::move(overlay)};
    }

    ColorRenderer& QuadRendererFor(const SceneSurface& surface, const ScenePassShaders& program,
            std::span<const plume::RenderSamplerDesc> samplers, ColorTarget& target,
            plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy(),
            plume::RenderCullMode cull = plume::RenderCullMode::NONE) {
        // Only the first sampler's upper mip level varies within a pass.
        const float max_lod = samplers.front().maxLOD;
        const auto existing = std::find_if(quad_renderers.begin(), quad_renderers.end(), [&](const CachedQuadRenderer& cache) {
            return cache.program == &program && cache.max_lod == max_lod;
        });
        if (existing != quad_renderers.end()) return *existing->renderer;
        auto configuration = ConfigureDepth(surface, program.vertex);
        configuration.cull = cull;
        auto pipeline = std::make_unique<ColorRenderer>(configuration, program.pixel_dxil, samplers, target, blend);
        auto* result = pipeline.get();
        quad_renderers.push_back({&program, max_lod, std::move(pipeline)});
        return *result;
    }

    void BeginMenuFan(PPCContext& ctx, uint8_t* base) {
        const auto device = ctx.r3.u32;
        SceneMenuFan scene;
        {
            std::lock_guard lock(mutex);
            Require(device && device == guest_device && back_buffer_surface && menu_fan_shaders &&
                    !pending_up && !pending_menu_fan && !scanout_texture, "Menu fan requires the ready back buffer and shaders");
            scene = ReadMenuFan(ctx, base, *menu_fan_shaders, menu_fan_additive_shaders ? &*menu_fan_additive_shaders : nullptr);
            Require(*back_buffer_surface == scene.pass.surface, "Menu fan target differs from the back buffer");
        }
        __imp__sub_828648A8(ctx, base);
        const auto vertices = REX_LOAD_U32(device + 13448);
        const auto endpoint = REX_LOAD_U32(device + 13444);
        Require(vertices && !(vertices & 15) && ctx.r3.u32 == vertices && endpoint && !(endpoint & 3),
                "Native menu fan staging allocation failed");
        std::lock_guard lock(mutex);
        pending_menu_fan = PendingMenuFan{std::move(scene), vertices, endpoint};
    }

    void CompleteMenuFan(uint8_t* base) {
        std::lock_guard lock(mutex);
        if (!pending_menu_fan) return;
        auto& up = *pending_menu_fan;
        Require(REX_LOAD_U32(guest_device + 48) == up.endpoint && REX_LOAD_U32(guest_device + 13448) == up.vertices,
                "Guest did not commit its menu fan geometry");
        auto& scene = up.scene;
        ReadMenuFanVertices(scene.pass.draw, base, up.vertices);
        std::array<std::unique_ptr<SampledTexture>, 3> textures;
        std::array<const SampledTexture*, 3> bindings;
        for (size_t i = 0; i < scene.texture_count; ++i) {
            textures[i] = UploadSceneTexture(base, scene.textures[i], gpu);
            bindings[i] = textures[i].get();
        }
        auto& renderer = QuadRendererFor(scene.pass.surface, *scene.pass.shaders,
            std::span(scene.samplers).first(scene.texture_count), *back_buffer, scene.blend, plume::RenderCullMode::FRONT);
        completed = renderer.Render(scene.pass.draw, scene.pass.pixel_constants, std::span(bindings).first(scene.texture_count));
        const auto texture_count = scene.texture_count;
        const bool additive = scene.blend.blendEnabled;
        ++overlay_draws;
        pending_menu_fan.reset();
        REXLOG_INFO("MC2 native scene: drew {} menu triangle fan, bindings={}, completed={}",
                    additive ? "additive" : "base", texture_count, completed);
    }

    void CompleteIndexedUp(uint8_t* base) {
        std::lock_guard lock(mutex);
        if (!pending_up) return;
        auto& up = *pending_up;
        Require(REX_LOAD_U32(guest_device + 48) == up.endpoint, "Guest did not commit its indexed user-memory geometry");
        const auto& surface = up.scene.surface;
        const auto sampler = FullscreenColorSampler();
        if (up.pass == SceneQuadPass::Overlay)
            ReadOverlayUpVertices(up.scene.draw, base, up.vertices, up.indices, *up.overlay);
        else if (up.pass != SceneQuadPass::HdrCopy)
            ReadQuadUpVertices(up.scene.draw, base, up.vertices, up.indices);
        if (up.pass == SceneQuadPass::Bloom) {
            const auto& shaders = *up.scene.shaders;
            if (bloom_draws == 0) {
                const std::array samplers{sampler, sampler};
                auto& renderer = QuadRendererFor(surface, shaders, samplers, *bloom_target);
                completed = renderer.RenderResolved(up.scene.draw, up.scene.pixel_constants, *depth_renderer, *color_target);
                REXLOG_INFO("MC2 native scene: drew fixed-point blur and bloom from both scene textures, completed={}", completed);
            } else {
                const auto filter_sampler = FullscreenColorSampler(true);
                auto& renderer = QuadRendererFor(surface, shaders, std::span(&filter_sampler,1), *bloom_target);
                completed = renderer.RenderResolved(up.scene.draw, up.scene.pixel_constants, *bloom_target);
                const auto& vertex = shaders.vertex.reference;
                const auto hash = mc2::capture::ContentHash(vertex.data(), vertex.size());
                REXLOG_INFO("MC2 native scene: filtered bloom pass {} with vertex shader {:016x}, completed={}", bloom_draws, hash, completed);
            }
            ++bloom_draws;
        } else if (up.pass == SceneQuadPass::Composition) {
            const std::array samplers{sampler, sampler, FullscreenColorSampler(true)};
            auto& renderer = QuadRendererFor(surface, *composition_shaders, samplers, *fullscreen_target);
            completed = renderer.RenderResolved(up.scene.draw, up.scene.pixel_constants, *depth_renderer, *color_target, bloom_target.get());
            scene_composited = true;
            REXLOG_INFO("MC2 native scene: composed fullscreen depth of field and bloom, completed={}", completed);
        } else if (up.pass == SceneQuadPass::BackBuffer) {
            using namespace plume;
            if (!back_buffer)
                back_buffer = std::make_unique<ColorTarget>(surface.width, surface.height, surface.samples, gpu,
                    std::span<const uint8_t>{}, RenderFormat::R8G8B8A8_UNORM);
            back_buffer->Clear(RenderColor(0,0,0,1));
            auto& renderer = QuadRendererFor(surface, *back_buffer_shaders, std::span(&sampler,1), *back_buffer);
            completed = renderer.RenderResolved(up.scene.draw, up.scene.pixel_constants, *color_target);
            back_buffer_surface = surface;
            REXLOG_INFO("MC2 native scene: drew gamma and overlay into RGBA8 back buffer {:#x}, completed={}", surface.address, completed);
        } else if (up.pass == SceneQuadPass::Overlay) {
            const auto overlay_sampler = OverlaySampler(up.overlay->max_mip_level);
            auto& renderer = QuadRendererFor(surface, *overlay_shaders, std::span(&overlay_sampler,1), *back_buffer, OverlayBlend());
            auto texture = std::visit([&](const auto& source) { return UploadSceneTexture(base, source, gpu); }, up.overlay->texture);
            const auto* sampled = texture.get();
            completed = renderer.Render(up.scene.draw, up.scene.pixel_constants, std::span(&sampled,1));
            REXLOG_INFO("MC2 native scene: blended overlay draw {} ({} indices), completed={}",
                        ++overlay_draws, up.scene.draw.index_count, completed);
        } else {
            ReadFullscreenUpVertices(up.scene.draw, base, up.vertices, up.indices);
            if (!fullscreen_target)
                fullscreen_target = std::make_unique<ColorTarget>(surface.width, surface.height, surface.samples, gpu);
            fullscreen_target->Clear(plume::RenderColor(0,0,0,0));
            auto& renderer = QuadRendererFor(surface, *fullscreen_shaders, std::span(&sampler,1), *fullscreen_target);
            completed = renderer.RenderResolved(up.scene.draw, up.scene.pixel_constants, *color_target);
            fullscreen_surface = surface;
            REXLOG_INFO("MC2 native scene: fullscreen HDR copy, completed={}", completed);
        }
        pending_up.reset();
    }

    void DrawColorGrade(const PPCContext& ctx, uint8_t* base) {
        Require(color_grade_shaders && ctx.r3.u32 && ctx.r3.u32 == guest_device, "Nonindexed draw requires the verified color grade shaders");
        Flush(base, ctx.r3.u32);
        std::lock_guard lock(mutex);
        auto scene = ReadSceneFullscreenDraw(ctx, base, *color_grade_shaders, scene_textures[0], true);
        Require(fullscreen_target && fullscreen_surface && *fullscreen_surface == scene.surface && !color_graded && !bloom_surface,
                "Color grading requires the first fullscreen HDR target");
        const auto sampler = FullscreenColorSampler();
        auto blend = plume::RenderBlendDesc::Copy();
        blend.renderTargetWriteMask = 7;
        auto& renderer = QuadRendererFor(scene.surface, *color_grade_shaders, std::span(&sampler,1), *fullscreen_target,
            blend, plume::RenderCullMode::FRONT);
        completed = renderer.RenderResolved(scene.draw, scene.pixel_constants, *color_target);
        color_graded = true;
        REXLOG_INFO("MC2 native scene: fullscreen RGB color grading, completed={}", completed);
    }

    void ResolveFullscreen(const PPCContext& ctx, uint8_t* base) {
        std::lock_guard lock(mutex);
        const bool resolve_grade = color_graded && !grade_resolved && !bloom_surface;
        const bool resolve_composition = scene_composited && !composition_resolved;
        Require(ctx.r3.u32 && ctx.r3.u32 == guest_device && (resolve_grade || resolve_composition) &&
                !fullscreen_resolve_shader.empty(),
                "Fullscreen resolve requires the completed native pass and single-sample copy shader");
        const auto surface = ReadFullscreenResolve(ctx, base, scene_textures[0]);
        Require(*fullscreen_surface == surface, "Fullscreen resolve source differs from the drawn target");
        completed = color_target->OverwriteResolved(*fullscreen_target, -3, fullscreen_resolve_shader);
        grade_resolved |= resolve_grade;
        composition_resolved |= resolve_composition;
        REXLOG_INFO("MC2 native scene: resolved {} HDR into scene texture {:#x}, exponent=-3, completed={}",
                    resolve_composition ? "composed" : "graded", scene_textures[0].address, completed);
    }

    void ResolveBloom(const PPCContext& ctx, uint8_t* base) {
        std::lock_guard lock(mutex);
        Require(ctx.r3.u32 && ctx.r3.u32 == guest_device && bloom_draws == bloom_copies + 1,
                "Bloom resolve requires the completed native bloom draw");
        const auto [surface, destination] = ReadBloomResolve(ctx, base);
        Require(*bloom_surface == surface, "Bloom resolve source differs from the drawn target");
        Require(!bloom_texture || *bloom_texture == destination, "Bloom resolve destination changed between passes");
        completed = bloom_target->ResolveFixedPoint();
        bloom_texture = destination;
        ++bloom_copies;
        REXLOG_INFO("MC2 native scene: resolved bloom pass {} to UNORM texture {:#x}, completed={}",
                    bloom_copies, destination.address, completed);
    }

    void ResolveScanout(const PPCContext& ctx, uint8_t* base) {
        std::lock_guard lock(mutex);
        Require(ctx.r3.u32 && ctx.r3.u32 == guest_device && back_buffer && back_buffer_surface &&
                !pending_up && !pending_menu_fan && !scanout_texture && completed == gpu->completed,
                "Display resolve needs the completed native back buffer");
        // A full same-format single-sample resolve can retain the native
        // attachment until Swap consumes it. No guest texture upload is needed.
        scanout_texture = ReadScanoutResolve(ctx, base, *back_buffer_surface);
        REXLOG_INFO("MC2 native scene: ready for display resolve to guest texture {:#x}", scanout_texture);
    }

    void PresentScene(const PPCContext& ctx) {
        std::lock_guard lock(mutex);
        if (!scanout_texture) return;
        Require(ctx.lr == 0x8248B7BC && ctx.r3.u32 == guest_device && ctx.r4.u32 == scanout_texture && !ctx.r5.u32 &&
                !pending && !pending_restore && !pending_up && !pending_menu_fan && !retiring_tiles &&
                renderer && window && !present_vertex_shader.empty() && !present_pixel_shader.empty(),
                "Scene swap requires the resolved back buffer, window and presentation shaders");
        renderer->SetWindow(window);
        const auto result = renderer->PresentColor(*back_buffer, back_buffer_surface->width, back_buffer_surface->height,
            present_vertex_shader, present_pixel_shader, REXCVAR_QUERY(std::string, mc2_framerate) != "unlocked");
        completed = result.completed_submission;
        ++scene_frames;
        if (result.present_submitted)
            REXLOG_INFO("MC2 native scene: presented frame {} at {}x{} with {} overlays, completed={}",
                        scene_frames, result.width, result.height, overlay_draws, completed);
        if (REXCVAR_GET(mc2_scene_resolve_trace))
            REXLOG_INFO("MC2 native scene: {} cached graphics pipelines after frame {}", quad_renderers.size(), scene_frames);
        ResetSceneFrame();
    }

    void ResetSceneFrame() {
        // Swap has completed on the device. The attachments stay allocated
        // while the next frame supplies its own guest bindings and copies.
        depth_surface.reset();
        color_surface.reset();
        stencil_surface.reset();
        fullscreen_surface.reset();
        bloom_surface.reset();
        bloom_texture.reset();
        back_buffer_surface.reset();
        prepass_draw.reset();
        scene_draw.reset();
        cleared_tile = {};
        scene_textures = {};
        depth_draws = 0;
        depth_restored = false;
        scene_copies = 0;
        scene_tiles_retired = false;
        color_graded = false;
        grade_resolved = false;
        scene_composited = false;
        composition_resolved = false;
        bloom_draws = bloom_copies = 0;
        overlay_draws = 0;
        scanout_texture = 0;
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
        Require(!pending_restore && !pending_up && !pending_menu_fan, "Legacy staging flushed during a pending native user-memory draw");
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
    const bool postprocess = ctx.lr == 0x827B3694;
    if (!postprocess && ctx.lr != 0x826DA2D8 && ctx.lr != 0x826DA314) return;
    static std::atomic<uint32_t> tile_count = 0, postprocess_count = 0;
    auto& count = postprocess ? postprocess_count : tile_count;
    const uint32_t limit = postprocess ? 16 : 8;
    if (count.load(std::memory_order_relaxed) >= limit || count.fetch_add(1, std::memory_order_relaxed) >= limit) return;
    using mc2::capture::ReadGuest;
    using mc2::capture::LoadBE32;
    std::vector<uint8_t> device, destination, color, stencil, rect(16), origin(8);
    if (!ReadGuest(base, ctx.r3.u32, 13128, device) || (ctx.r5.u32 && !ReadGuest(base, ctx.r5.u32, 16, rect)) ||
        !ReadGuest(base, ctx.r6.u32, 52, destination) || (ctx.r7.u32 && !ReadGuest(base, ctx.r7.u32, 8, origin))) return;
    const auto state = [&](uint32_t offset) { return LoadBE32(device.data() + offset); };
    const auto dest = [&](uint32_t offset) { return LoadBE32(destination.data() + offset); };
    REXLOG_INFO("MC2 scene resolve: caller={:#x} flags={:#x} predicate={:#x} rect_ptr={:#x} rect=[{},{},{},{}] destination={:#x} origin_ptr={:#x} origin=[{},{}] depth_clear={} slice={} color_ptr={:#x}",
        ctx.lr, ctx.r4.u32, state(12716), ctx.r5.u32, LoadBE32(rect.data()), LoadBE32(rect.data()+4),
        LoadBE32(rect.data()+8), LoadBE32(rect.data()+12), ctx.r6.u32, ctx.r7.u32, LoadBE32(origin.data()),
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
void DrawNativeSceneNonIndexed(const PPCContext& ctx, uint8_t* base) {
    try { Graphics().DrawColorGrade(ctx, base); }
    catch (const std::exception& error) { UnsupportedNativeOperation(error.what(),ctx); }
}
[[noreturn]] void UnsupportedNativeOperation(const char* operation, const PPCContext& ctx) {
    const auto message = fmt::format("MC2 native boot: unsupported {} at guest caller {:#x}", operation, ctx.lr);
    REXLOG_ERROR("{}", message);
    throw std::runtime_error(message);
}
}  // namespace mc2::native

using namespace mc2::native;

REX_HOOK_RAW(sub_826D9C10) {
    __imp__sub_826D9C10(ctx, base);
    if (NativeBootActive()) {
        try { Graphics().CompleteIndexedUp(base); }
        catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
    }
}
REX_HOOK_RAW(sub_826D99A8) {
    __imp__sub_826D99A8(ctx, base);
    if (NativeBootActive()) {
        try { Graphics().CompleteMenuFan(base); }
        catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
    }
}
REX_HOOK_RAW(sub_826DA3A0) {
    __imp__sub_826DA3A0(ctx, base);
    if (NativeBootActive()) {
        try { Graphics().CompleteDepthRestore(base); }
        catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
    }
}
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
        try {
            if (ctx.lr == 0x827B3694 && ctx.r4.u32 == 0) Graphics().ResolveBloom(ctx, base);
            else if (ctx.lr == 0x827B3694) Graphics().ResolveFullscreen(ctx, base);
            else if (ctx.lr == 0x8248B7AC) Graphics().ResolveScanout(ctx, base);
            else Graphics().ResolveTile(ctx, base);
        }
        catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
        // Preserve the guest's CPU resource bookkeeping. The original GPU
        // commands go only to staging; the native copy has already completed.
    }
    __imp__sub_823893A0(ctx, base);
}
REX_HOOK_RAW(sub_82388898) {
    if (!NativeBootActive()) { __imp__sub_82388898(ctx, base); return; }
    try { Graphics().RetireSceneTiles(ctx, base); }
    catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
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
    if (Graphics().RetireTileBatch(ctx, base)) return;
    UnsupportedNativeOperation("indirect GPU command batch", ctx);
}
REX_HOOK_RAW(sub_8236C9E0) {
    if (!NativeBootActive()) { __imp__sub_8236C9E0(ctx, base); return; }
    UnsupportedNativeOperation("raw GPU ring submission", ctx);
}
REX_HOOK_RAW(sub_8237CA28) {
    if (!NativeBootActive()) { __imp__sub_8237CA28(ctx, base); return; }
    const auto device = ctx.r3.u32;
    try { Graphics().PresentScene(ctx); }
    catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
    Graphics().Flush(base, device);
    ResetStaging(base, device);
    REX_STORE_U32(device + 16560, REX_LOAD_U32(device + 16560) + 1);
    REX_STORE_U32(device + 16568, REX_LOAD_U32(device + 16560));
    REX_STORE_U32(device + 10928, REX_LOAD_U32(device + 10908) - 2);
}
REX_HOOK_RAW(sub_828648A8) {
    if (!NativeBootActive()) { __imp__sub_828648A8(ctx, base); return; }
    try {
        if (ctx.lr == 0x826D9A60) Graphics().BeginMenuFan(ctx, base);
        else Graphics().BeginDepthRestore(ctx, base);
    }
    catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
}
REX_HOOK_RAW(sub_82864D88) {
    if (!NativeBootActive()) { __imp__sub_82864D88(ctx, base); return; }
    try { Graphics().BeginIndexedUp(ctx, base); }
    catch (const std::exception& error) { UnsupportedNativeOperation(error.what(), ctx); }
}
