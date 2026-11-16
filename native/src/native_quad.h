#pragma once

#include <plume_render_interface.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace mc2::native {
struct QuadTexture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bytes;
};
struct QuadDraw {
    uint32_t width = 0, height = 0, stride = 0;
    bool quad_list = false;
    plume::RenderFormat texture_format = plume::RenderFormat::R8G8B8A8_UNORM;
    plume::RenderFormat target_format = plume::RenderFormat::R8G8B8A8_UNORM;
    plume::RenderFilter filter = plume::RenderFilter::LINEAR;
    plume::RenderTextureAddressMode address_u = plume::RenderTextureAddressMode::CLAMP;
    plume::RenderTextureAddressMode address_v = plume::RenderTextureAddressMode::CLAMP;
    std::vector<uint8_t> vertex_shader, pixel_shader, vertices, shared_constants, pixel_constants;
    std::vector<QuadTexture> textures;
};
struct QuadResult {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    bool debug_layer = false;
    bool present_submitted = false;
    uint64_t completed_submission = 0;
};

// Reuses one pipeline and its GPU resources. Calls must be serialized.
// Each Render waits for actual GPU completion before uploads can be reused.
// Pipeline/texture extents are immutable; per-draw bytes and constants may change.
class QuadRenderer {
 public:
    explicit QuadRenderer(const QuadDraw& configuration, bool enable_debug = false);
    ~QuadRenderer();
    QuadRenderer(const QuadRenderer&) = delete;
    QuadRenderer& operator=(const QuadRenderer&) = delete;
    QuadResult Render(const QuadDraw& draw);
    // The caller owns the HWND and its message loop. Null detaches it.
    void SetWindow(plume::RenderWindow window);
    // Draw directly at the window's current resolution, preserving aspect.
    // Minimized windows submit no work. Readback is optional diagnostics only.
    QuadResult Present(const QuadDraw& draw, bool vsync = true, bool read_pixels = false);
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience for one offscreen validation draw, not a presentation backend.
// All input state is a value-owned snapshot; a real GPU fence precedes readback.
// Never enable the debug layer after another D3D12 device has been created.
QuadResult RenderQuad(const QuadDraw& draw, bool enable_debug = false);
}  // namespace mc2::native
