#pragma once
#include "native_device.h"
#include <vector>

namespace mc2::native {
// Persistent HDR surface on the same queue as native movie/depth work. Partial
// clears are allowed before drawing, but undefined pixels cannot be read back.
class ColorTarget {
 public:
    ColorTarget(uint32_t width, uint32_t height, uint32_t samples, std::shared_ptr<NativeDevice> gpu);
    ~ColorTarget();
    uint64_t Clear(plume::RenderColor value, const plume::RenderRect* rectangle = nullptr);
    std::vector<uint16_t> ReadPixels(); // RGBA16_FLOAT, diagnostic readback only.
 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
