#pragma once
#include "native_depth.h"
#include "texture_blocks.h"
#include "texture_decode.h"
#include <array>
#include <filesystem>
#include <utility>
#include <variant>
struct PPCContext;
namespace mc2::native {
struct SceneVertexShader {
    std::vector<uint8_t> reference, dxil;
    std::vector<VertexInput> inputs;
};
inline constexpr uint32_t kTiledTextureSlots = 6;
struct SceneColorShaders {
    SceneVertexShader vertex;
    std::vector<uint8_t> pixel_reference, pixel_dxil, resolve_dxil, depth_resolve_dxil;
    uint32_t texture_count = 2;
};
struct SceneSurface {
    uint32_t address = 0, width = 0, height = 0, samples = 0;
    // Preserve the allocation description to reject reuse with another layout.
    std::vector<uint8_t> description;
    bool operator==(const SceneSurface&) const = default;
};
struct DepthClear {
    SceneSurface surface;
    float value = 0;
};
struct SceneDepthDraw {
    SceneSurface surface;
    DepthDraw draw;
};
struct SceneColorClear {
    // The logical dimensions come from the verified tile layout. The retained
    // description still identifies the original, smaller allocation.
    SceneSurface surface;
    plume::RenderColor value;
    plume::RenderRect rectangle;
};
struct SceneColorDraw {
    SceneSurface color_surface, tile_depth;
    const SceneColorShaders* shaders = nullptr;
    plume::RenderCullMode cull = plume::RenderCullMode::FRONT;
    DepthDraw draw;
    std::vector<uint8_t> pixel_constants;
    std::array<mc2::texture::BlockTexture, kTiledTextureSlots> textures;
    std::array<plume::RenderSamplerDesc, kTiledTextureSlots> samplers;
};
struct SceneResolveTexture {
    uint32_t address = 0;
    std::array<uint32_t, 6> fetch_words{};
    bool operator==(const SceneResolveTexture&) const = default;
};
struct ScenePassShaders {
    SceneVertexShader vertex;
    std::vector<uint8_t> pixel_reference, pixel_dxil;
};
struct SceneDepthRestore {
    SceneSurface depth;
    DepthDraw draw;
    std::vector<uint8_t> pixel_constants;
};
struct SceneFullscreenDraw {
    SceneSurface surface;
    DepthDraw draw;
    std::vector<uint8_t> pixel_constants;
    const ScenePassShaders* shaders = nullptr;
};
struct SceneOverlayBinding {
    std::variant<mc2::texture::BaseRGBA8, mc2::texture::BlockTexture> texture;
    plume::RenderSamplerDesc sampler;
    uint32_t vertex_count = 0, index_count = 0;
};
struct SceneMenuFanProgram {
    ScenePassShaders shaders;
    std::array<uint32_t, 3> block_formats;
    uint32_t blend_control, clamp_slots = 0;
};
struct SceneMenuFan {
    SceneFullscreenDraw pass;
    std::array<mc2::texture::BlockTexture, 3> textures;
    std::array<plume::RenderSamplerDesc, 3> samplers;
    plume::RenderBlendDesc blend;
    uint32_t texture_count = 0;
};
struct SceneTileResolve {
    SceneSurface color, depth;
    SceneResolveTexture destination;
    plume::RenderRect rectangle;
    uint32_t tile = 0;
    int32_t exponent = 0;
    bool copy_depth = false;
};
SceneVertexShader LoadDepthShaders(const std::filesystem::path& directory);
SceneVertexShader LoadOcclusionShader(const std::filesystem::path& directory);
SceneColorShaders LoadColorShaders(const std::filesystem::path& directory);
DepthConfiguration ConfigureDepth(const SceneSurface& surface, const SceneVertexShader& shader);
DepthClear ReadDepthClear(const PPCContext& context, uint8_t* base);
SceneSurface ReadPostprocessDepthStencilClear(const PPCContext& context, uint8_t* base);
SceneColorClear ReadSceneColorClear(const PPCContext& context, uint8_t* base);
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& context, uint8_t* base, const SceneVertexShader& shader);
SceneDepthDraw ReadOcclusionDraw(const PPCContext& context, uint8_t* base, const SceneVertexShader& shader);
void ReadOcclusionVertices(DepthDraw& draw, uint8_t* base, uint32_t vertices, uint32_t indices);
SceneColorDraw ReadSceneColorDraw(const PPCContext& context, uint8_t* base, const SceneColorShaders& shaders,
                                  std::span<const SceneColorShaders> world_shaders = {});
bool SameScenePositionInputs(const DepthDraw& depth, const DepthDraw& color);
SceneTileResolve ReadSceneTileResolve(const PPCContext& context, uint8_t* base);
ScenePassShaders LoadDepthRestoreShaders(const std::filesystem::path& directory);
SceneDepthRestore ReadSceneDepthRestore(const PPCContext& context, uint8_t* base,
    const ScenePassShaders& shaders, const SceneResolveTexture& source);
void ReadDepthRestoreVertices(DepthDraw& draw, uint8_t* base, uint32_t address);
ScenePassShaders LoadFullscreenColorShaders(const std::filesystem::path& directory, bool grade,
    const char* pixel_dxil = "pixel-7e3.dxil");
SceneFullscreenDraw ReadSceneFullscreenDraw(const PPCContext& context, uint8_t* base,
    const ScenePassShaders& shaders, const SceneResolveTexture& source, bool grade);
void ReadFullscreenUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address);
plume::RenderSamplerDesc FullscreenColorSampler(bool linear = false);
SceneSurface ReadFullscreenResolve(const PPCContext& context, uint8_t* base, const SceneResolveTexture& destination);
SceneColorClear ReadBloomClear(const PPCContext& context, uint8_t* base);
ScenePassShaders LoadQuadShaders(const std::filesystem::path& directory, const char* pixel_dxil);
SceneFullscreenDraw ReadBloomDraw(const PPCContext& context, uint8_t* base,
    std::span<const ScenePassShaders> programs, const SceneResolveTexture& primary,
    const SceneResolveTexture* color = nullptr);
void ReadQuadUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address);
SceneFullscreenDraw ReadCompositionDraw(const PPCContext& context, uint8_t* base,
    const ScenePassShaders& shaders, const SceneResolveTexture& depth, const SceneResolveTexture& color,
    const SceneResolveTexture& bloom);
SceneFullscreenDraw ReadBackBufferDraw(const PPCContext& context, uint8_t* base,
    const ScenePassShaders& shaders, const SceneResolveTexture& color);
SceneFullscreenDraw ReadBackBufferOverlay(const PPCContext& context, uint8_t* base,
    const ScenePassShaders& shaders, SceneOverlayBinding& binding);
void ReadOverlayUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address,
    const SceneOverlayBinding& binding);
plume::RenderBlendDesc OverlayBlend();
plume::RenderSamplerDesc OverlaySampler(uint32_t max_mip_level = 0, bool clamp_uv = false);
ScenePassShaders LoadMenuFanShaders(const std::filesystem::path& directory);
SceneMenuFan ReadMenuFan(const PPCContext& context, uint8_t* base, std::span<const SceneMenuFanProgram> programs);
void ReadMenuFanVertices(DepthDraw& draw, uint8_t* base, uint32_t address);
uint32_t ReadScanoutResolve(const PPCContext& context, uint8_t* base, const SceneSurface& back_buffer);
std::pair<SceneSurface, SceneResolveTexture> ReadBloomResolve(const PPCContext& context, uint8_t* base);
}
