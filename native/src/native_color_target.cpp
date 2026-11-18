#include "native_color_target.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace plume;
namespace mc2::native {
namespace {
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
struct ColorTarget::Impl {
    uint32_t width,height,samples;
    std::shared_ptr<NativeDevice> gpu;
    std::unique_ptr<RenderTexture> texture,resolved;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> readback;
    bool fully_initialized=false;
    RenderRect cleared_rectangle;

    Impl(uint32_t w,uint32_t h,uint32_t count,std::shared_ptr<NativeDevice> context)
        : width(w),height(h),samples(count),gpu(std::move(context)) {
        Require(gpu && width && height && width<=8192 && height<=8192,"Invalid native color surface extent");
        Require(samples==1 || samples==2 || samples==4,"Unsupported native color sample count");
        texture=gpu->device->createTexture(RenderTextureDesc::ColorTarget(width,height,kFormat,RenderMultisampling(samples)));
        Require(bool(texture),"Native HDR surface allocation failed");
        const RenderTexture* attachment=texture.get();
        RenderFramebufferDesc desc;
        desc.colorAttachments=&attachment; desc.colorAttachmentsCount=1;
        framebuffer=gpu->device->createFramebuffer(desc);
        Require(bool(framebuffer),"Native HDR framebuffer creation failed");
    }
    uint64_t Clear(RenderColor value,const RenderRect* rectangle) {
        for (float component:value.rgba)
            Require(std::isfinite(component) && std::abs(component)<=65504,"Invalid native HDR clear value");
        const RenderRect full(0,0,width,height);
        if (rectangle)
            Require(rectangle->left>=0 && rectangle->top>=0 && rectangle->right>rectangle->left &&
                    rectangle->bottom>rectangle->top && uint32_t(rectangle->right)<=width &&
                    uint32_t(rectangle->bottom)<=height,"Native color clear rectangle is out of bounds");
        auto& commands=gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(texture.get(),RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(framebuffer.get());
        commands->clearColor(0,value,rectangle,rectangle ? 1 : 0);
        commands->end();
        gpu->queue->executeCommandLists(commands.get(),gpu->fence.get());
        const auto completed=gpu->Complete();
        fully_initialized|=!rectangle || *rectangle==full;
        cleared_rectangle=rectangle ? *rectangle : full;
        return completed;
    }
    std::vector<uint16_t> ReadPixels() {
        Require(fully_initialized,"Cannot read undefined pixels from a partially cleared native color target");
        const uint32_t pitch=(width*8+255)&~255u;
        if (!readback) readback=gpu->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t(pitch)*height));
        if (samples>1 && !resolved) resolved=gpu->device->createTexture(RenderTextureDesc::Texture2D(width,height,1,kFormat));
        Require(readback && (samples==1 || resolved),"Native HDR readback allocation failed");
        auto& commands=gpu->commands;
        commands->begin();
        auto* source=texture.get();
        if (samples>1) {
            commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture.get(),RenderTextureLayout::RESOLVE_SOURCE));
            commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(resolved.get(),RenderTextureLayout::RESOLVE_DEST));
            commands->resolveTextureRegion(resolved.get(),0,0,texture.get());
            source=resolved.get();
        }
        commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(source,RenderTextureLayout::COPY_SOURCE));
        commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),kFormat,width,height,1,pitch/8),
                                    RenderTextureCopyLocation::Subresource(source));
        commands->end();
        gpu->queue->executeCommandLists(commands.get(),gpu->fence.get()); gpu->Complete();
        std::vector<uint16_t> pixels(size_t(width)*height*4);
        const auto* bytes=static_cast<const uint8_t*>(readback->map());
        Require(bytes!=nullptr,"Native HDR readback mapping failed");
        for (uint32_t y=0;y<height;++y)
            std::memcpy(pixels.data()+size_t(y)*width*4,bytes+size_t(y)*pitch,width*8);
        readback->unmap();
        return pixels;
    }
};
ColorTarget::ColorTarget(uint32_t width,uint32_t height,uint32_t samples,std::shared_ptr<NativeDevice> gpu)
    : impl_(std::make_unique<Impl>(width,height,samples,std::move(gpu))) {}
ColorTarget::~ColorTarget()=default;
uint64_t ColorTarget::Clear(RenderColor value,const RenderRect* rectangle) { return impl_->Clear(value,rectangle); }
std::vector<uint16_t> ColorTarget::ReadPixels() { return impl_->ReadPixels(); }
RenderTexture* ColorTarget::Attachment(const NativeDevice& gpu, uint32_t width, uint32_t height, uint32_t samples) {
    Require(impl_->gpu.get() == &gpu && impl_->width == width && impl_->height == height && impl_->samples == samples,
            "Native color and depth attachments must share a device, extent and sample count");
    return impl_->texture.get();
}
void ColorTarget::RequireCleared(const RenderRect& rectangle) const {
    const auto& cleared = impl_->cleared_rectangle;
    Require(impl_->fully_initialized || (rectangle.left >= cleared.left && rectangle.top >= cleared.top &&
            rectangle.right <= cleared.right && rectangle.bottom <= cleared.bottom),
            "Native indexed draw touches an uninitialized color region");
}
}
