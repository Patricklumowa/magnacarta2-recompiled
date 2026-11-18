#pragma once
#include "native_device.h"
#include <span>
#include <vector>

namespace mc2::native {
// Persistent HDR surface on the same queue as native movie/depth work. Partial
// clears are allowed before drawing, but undefined pixels cannot be read back.
class ColorTarget {
 public:
    ColorTarget(uint32_t width, uint32_t height, uint32_t samples, std::shared_ptr<NativeDevice> gpu,
                std::span<const uint8_t> resolve_shader = {});
    ~ColorTarget();
    uint64_t Clear(plume::RenderColor value, const plume::RenderRect* rectangle = nullptr);
    // Resolve the specified 2x MSAA region into retained single-sample HDR
    // storage, apply the destination exponent and clear the source to zero.
    uint64_t ResolveAndClear(const plume::RenderRect& rectangle, int32_t exponent);
    std::vector<uint16_t> ReadPixels(); // RGBA16_FLOAT, diagnostic readback only.
    std::vector<uint16_t> ReadResolvedPixels(); // Contiguous completed resolve region.
 private:
    friend class ColorRenderer;
    static constexpr auto kFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
    NativeDevice& Device() const;
    plume::RenderTexture* Attachment(const NativeDevice& gpu, uint32_t width, uint32_t height, uint32_t samples);
    const plume::RenderTexture* ResolvedTexture(const NativeDevice& gpu) const;
    void RequireCleared(const plume::RenderRect& rectangle) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
