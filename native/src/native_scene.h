#pragma once
#include "native_depth.h"
#include <filesystem>
struct PPCContext;
namespace mc2::native {
struct DepthShaders {
    std::vector<uint8_t> reference, dxil;
    std::vector<VertexInput> inputs;
};
struct DepthSurface {
    uint32_t address = 0, width = 0, height = 0, samples = 0;
    // Preserve the allocation description to reject reuse with another layout.
    std::vector<uint8_t> description;
    bool operator==(const DepthSurface&) const = default;
};
struct DepthClear {
    DepthSurface surface;
    float value = 0;
};
struct SceneDepthDraw {
    DepthSurface surface;
    DepthDraw draw;
};
DepthShaders LoadDepthShaders(const std::filesystem::path& directory);
DepthConfiguration ConfigureDepth(const DepthSurface& surface, const DepthShaders& shaders);
DepthClear ReadDepthClear(const PPCContext& context, uint8_t* base);
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& context, uint8_t* base, const DepthShaders& shaders);
}
