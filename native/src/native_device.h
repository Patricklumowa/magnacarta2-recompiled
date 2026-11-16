#pragma once
#include <plume_render_interface.h>
#include <memory>
#include <cstdint>

namespace mc2::native {
// One native queue shared by movie and scene pipelines. Calls are serialized by
// the owner; uploads/allocators may be reused only after Complete() returns.
class NativeDevice {
 public:
    explicit NativeDevice(bool enable_debug = false);
    ~NativeDevice();
    std::unique_ptr<plume::RenderInterface> render_interface;
    std::unique_ptr<plume::RenderDevice> device;
    std::unique_ptr<plume::RenderCommandQueue> queue;
    std::unique_ptr<plume::RenderCommandList> commands;
    std::unique_ptr<plume::RenderCommandFence> fence;
    bool debug_enabled = false;
    uint64_t completed = 0;
    // End/execute the command list before calling this. Also checks device loss.
    uint64_t Complete();
};
}
