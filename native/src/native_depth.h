#pragma once
#include "native_device.h"
#include "native_vertex_layout.h"
#include <vector>

namespace mc2::native {
struct DepthConfiguration {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> vertex_shader;
    std::vector<VertexInput> inputs;
    plume::RenderComparisonFunction comparison = plume::RenderComparisonFunction::GREATER_EQUAL;
    plume::RenderCullMode cull = plume::RenderCullMode::NONE;
    bool write = true, clip = true;
};
struct DepthDraw {
    std::vector<uint8_t> vertices, vertex_constants, shared_constants;
    std::vector<uint32_t> indices;
    uint32_t first_index = 0, index_count = 0;
    plume::RenderViewport viewport;
    plume::RenderRect scissor;
};
struct DepthResult {
    std::vector<float> pixels;
    uint64_t completed_submission = 0;
    bool debug_layer = false;
};
// Native, single-stream float-input depth pass. Owns its target and pipeline,
// shares the actual device/queue with other pipelines. No implicit per-draw
// clears. Stencil, MSAA and 24-bit console depth quantization are not emulated.
class DepthRenderer {
 public:
    DepthRenderer(const DepthConfiguration& configuration, std::shared_ptr<NativeDevice> device);
    ~DepthRenderer();
    void Clear(float depth);
    DepthResult Render(const DepthDraw& draw, bool read_pixels = false);
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
