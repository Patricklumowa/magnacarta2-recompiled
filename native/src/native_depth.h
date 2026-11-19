#pragma once
#include "native_device.h"
#include "native_vertex_layout.h"
#include <vector>

namespace mc2::native {
inline constexpr uint32_t kNativeVertexStreamLimit = 16;
inline constexpr uint32_t kNativeTextureSlotLimit = 16;
class ColorTarget;
class SampledTexture;
class ColorRenderer;
struct DepthConfiguration {
    uint32_t width = 0, height = 0;
    uint32_t sample_count = 1;
    std::vector<uint8_t> vertex_shader;
    std::vector<VertexInput> inputs;
    plume::RenderComparisonFunction comparison = plume::RenderComparisonFunction::GREATER_EQUAL;
    plume::RenderCullMode cull = plume::RenderCullMode::NONE;
    bool write = true, clip = true;
    plume::RenderComparisonFunction stencil_comparison = plume::RenderComparisonFunction::ALWAYS;
    bool operator==(const DepthConfiguration&) const = default;
};
struct DepthDraw {
    std::vector<uint8_t> vertices, vertex_constants, shared_constants;
    std::vector<std::vector<uint8_t>> additional_vertices;
    std::vector<uint32_t> indices;
    uint32_t first_index = 0, index_count = 0;
    plume::RenderViewport viewport;
    plume::RenderRect scissor;
    size_t VertexStreamCount() const { return 1 + additional_vertices.size(); }
    const std::vector<uint8_t>& VertexBytes(size_t stream) const {
        return stream ? additional_vertices[stream - 1] : vertices;
    }
};
struct DepthResult {
    std::vector<float> pixels;
    uint64_t completed_submission = 0;
    bool debug_layer = false;
};
// Native indexed depth pass. Owns its target and pipeline,
// shares the actual device/queue with other pipelines. No implicit per-draw
// clears. Stencil and 24-bit console depth quantization are not emulated.
class DepthRenderer {
 public:
    DepthRenderer(const DepthConfiguration& configuration, std::shared_ptr<NativeDevice> device,
                  std::span<const uint8_t> resolve_shader = {});
    ~DepthRenderer();
    void Clear(float depth, const plume::RenderRect* rectangle = nullptr);
    // MSAA diagnostic readback resolves the greatest depth value per pixel.
    // This does not implement the game's surface resolve/copy contract.
    DepthResult Render(const DepthDraw& draw, bool read_pixels = false);
    uint64_t CountPassingSamples(const DepthConfiguration& configuration, const DepthDraw& draw);
    // Export sample zero as decoded 20e4 values and clear that depth region.
    uint64_t ResolveAndClear(const plume::RenderRect& rectangle);
    std::vector<float> ReadResolvedPixels();
    uint64_t RestoreResolvedDepth(const DepthConfiguration& configuration, const DepthDraw& draw,
                    std::span<const uint8_t> pixel_shader, std::span<const uint8_t> pixel_constants,
                    plume::RenderTexture& target, plume::RenderFramebuffer& framebuffer);
    uint64_t OverwriteResolvedDepth(plume::RenderTexture& source, std::span<const uint8_t> shader);
 private:
    friend class ColorRenderer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Indexed color pass with an optional depth attachment. The targets must
// outlive the renderer. Each draw waits for its
// GPU fence, so the caller may release its sampled textures when Render returns.
class ColorRenderer {
 public:
    ColorRenderer(const DepthConfiguration& configuration, std::span<const uint8_t> pixel_shader,
                  std::span<const plume::RenderSamplerDesc> samplers, DepthRenderer& depth, ColorTarget& color,
                  uint32_t cube_slots = 0);
    ColorRenderer(const DepthConfiguration& configuration, std::span<const uint8_t> pixel_shader,
                  std::span<const plume::RenderSamplerDesc> samplers, ColorTarget& color,
                  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy());
    ColorRenderer(const DepthConfiguration& configuration, std::span<const uint8_t> pixel_shader,
                  std::span<const plume::RenderSamplerDesc> samplers, ColorTarget& color,
                  plume::RenderTexture& depth_stencil, plume::RenderBlendDesc blend, uint32_t cube_slots = 0);
    ~ColorRenderer();
    uint64_t Render(const DepthDraw& draw, std::span<const uint8_t> pixel_constants,
                    std::span<const SampledTexture* const> textures);
    uint64_t RenderResolved(const DepthDraw& draw, std::span<const uint8_t> pixel_constants, const ColorTarget& source);
    uint64_t RenderResolved(const DepthDraw& draw, std::span<const uint8_t> pixel_constants,
                            const DepthRenderer& depth_source, const ColorTarget& color_source,
                            const ColorTarget* bloom_source = nullptr);
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
