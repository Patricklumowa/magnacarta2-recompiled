#pragma once
#include "native_depth.h"
#include "texture_blocks.h"
#include <array>
#include <filesystem>
struct PPCContext;
namespace mc2::native {
struct SceneVertexShader {
    std::vector<uint8_t> reference, dxil;
    std::vector<VertexInput> inputs;
};
struct SceneColorShaders {
    SceneVertexShader vertex;
    std::vector<uint8_t> pixel_reference, pixel_dxil;
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
    DepthDraw draw;
    std::vector<uint8_t> pixel_constants;
    std::array<mc2::texture::BlockTexture, 2> textures;
    std::array<plume::RenderSamplerDesc, 2> samplers;
};
SceneVertexShader LoadDepthShaders(const std::filesystem::path& directory);
SceneColorShaders LoadColorShaders(const std::filesystem::path& directory);
DepthConfiguration ConfigureDepth(const SceneSurface& surface, const SceneVertexShader& shader);
DepthClear ReadDepthClear(const PPCContext& context, uint8_t* base);
SceneColorClear ReadSceneColorClear(const PPCContext& context, uint8_t* base);
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& context, uint8_t* base, const SceneVertexShader& shader);
SceneColorDraw ReadSceneColorDraw(const PPCContext& context, uint8_t* base, const SceneColorShaders& shaders);
}
