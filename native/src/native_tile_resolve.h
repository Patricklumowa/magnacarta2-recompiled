#pragma once
#include "native_device.h"
#include <span>

namespace mc2::native {
// Records one multisample-to-texture dispatch. The attachment owner validates
// the rectangle and records the matching clear and fence on the same list.
class TileResolve {
 public:
    TileResolve(NativeDevice& gpu, plume::RenderTexture& source, uint32_t width, uint32_t height,
                plume::RenderFormat format, std::span<const uint8_t> bytecode,
                const plume::RenderTextureView* source_view = nullptr);
    void Record(const plume::RenderRect& rectangle, float scale);
    uint64_t Complete(const plume::RenderRect& rectangle);
    const plume::RenderRect& CompletedRegion() const { return completed_region_; }
    plume::RenderTexture* Output() const { return output_.get(); }
 private:
    NativeDevice& gpu_;
    plume::RenderTexture& source_;
    plume::RenderRect completed_region_;
    std::unique_ptr<plume::RenderTexture> output_;
    std::unique_ptr<plume::RenderShader> shader_;
    std::unique_ptr<plume::RenderDescriptorSet> descriptors_;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderPipeline> pipeline_;
};
}
