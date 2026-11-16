#pragma once
#include "native_depth.h"
#include <filesystem>
struct PPCContext;
namespace mc2::native {
struct DepthShaders {
    std::vector<uint8_t> reference, dxil;
    std::vector<VertexInput> inputs;
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
DepthShaders LoadDepthShaders(const std::filesystem::path& directory);
DepthConfiguration ConfigureDepth(const SceneSurface& surface, const DepthShaders& shaders);
DepthClear ReadDepthClear(const PPCContext& context, uint8_t* base);
SceneColorClear ReadSceneColorClear(const PPCContext& context, uint8_t* base);
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& context, uint8_t* base, const DepthShaders& shaders);
}
