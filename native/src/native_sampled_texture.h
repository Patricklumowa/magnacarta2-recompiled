#pragma once
#include "native_device.h"
#include <span>
#include <vector>

namespace mc2::native {
// Shared-constant bytes 288..351 hold one word per reflected fetch slot (0..15).
// Zero bypasses interpretation for legacy callers with CPU-converted uploads.
uint32_t PackTextureFetchMode(uint32_t swizzle,uint32_t gamma_mask,int32_t exp_adjust);

// Immutable BC1/BC3 texture, uploaded once with every supplied mip. Input is
// tightly packed native block order; fetch swizzle/gamma belong to the sampler.
class SampledTexture {
 public:
    SampledTexture(plume::RenderFormat format, uint32_t width, uint32_t height,
                   std::span<const std::vector<uint8_t>> mips, std::shared_ptr<NativeDevice> gpu);
    ~SampledTexture();
    const plume::RenderTexture* Texture() const;
    const NativeDevice& Device() const;
    std::vector<std::vector<uint8_t>> ReadBlocks(); // Diagnostic GPU readback only.
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
