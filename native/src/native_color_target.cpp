#include "native_color_target.h"
#include "native_tile_resolve.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace plume;
namespace mc2::native {
namespace {
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool Covers(const RenderRect& outer, const RenderRect& inner) {
    return !inner.isEmpty() && inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}
}
struct ColorTarget::Impl {
    uint32_t width,height,samples;
    RenderFormat format;
    std::shared_ptr<NativeDevice> gpu;
    std::unique_ptr<RenderTexture> texture,resolved;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> readback;
    std::unique_ptr<TileResolve> tile_resolve;
    bool fully_initialized=false;
    RenderRect cleared_rectangle;

    Impl(uint32_t w,uint32_t h,uint32_t count,std::shared_ptr<NativeDevice> context, std::span<const uint8_t> shader,
         RenderFormat storage_format)
        : width(w),height(h),samples(count),format(storage_format),gpu(std::move(context)) {
        Require(gpu && width && height && width<=8192 && height<=8192,"Invalid native color surface extent");
        Require(samples==1 || samples==2 || samples==4,"Unsupported native color sample count");
        Require(format == RenderFormat::R16G16B16A16_FLOAT ||
                (format == RenderFormat::R16G16B16A16_SNORM && samples == 1 && shader.empty()), "Unsupported native color storage");
        texture=gpu->device->createTexture(RenderTextureDesc::ColorTarget(width,height,format,RenderMultisampling(samples)));
        Require(bool(texture),"Native HDR surface allocation failed");
        const RenderTexture* attachment=texture.get();
        RenderFramebufferDesc desc;
        desc.colorAttachments=&attachment; desc.colorAttachmentsCount=1;
        framebuffer=gpu->device->createFramebuffer(desc);
        Require(bool(framebuffer),"Native HDR framebuffer creation failed");
        if (!shader.empty()) {
            Require(samples == 2, "Native scene color resolve requires two samples");
            tile_resolve = std::make_unique<TileResolve>(*gpu, *texture, width, height, format, shader);
        }
    }
    void RequireCleared(const RenderRect& rectangle) const {
        Require(Covers(fully_initialized ? RenderRect(0,0,width,height) : cleared_rectangle, rectangle),
                "Native color operation touches an uninitialized region");
    }
    uint64_t Clear(RenderColor value,const RenderRect* rectangle) {
        const float limit = format == RenderFormat::R16G16B16A16_SNORM ? 1.f : 65504.f;
        for (float component:value.rgba)
            Require(std::isfinite(component) && std::abs(component)<=limit,"Invalid native color clear value");
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
    uint64_t ResolveAndClear(const RenderRect& rectangle, int32_t exponent) {
        Require(bool(tile_resolve), "Native color resolve shader is unavailable");
        Require(exponent >= -32 && exponent <= 31, "Native color resolve exponent is out of range");
        RequireCleared(rectangle);
        auto& commands = gpu->commands;
        commands->begin();
        tile_resolve->Record(rectangle, std::ldexp(1.0f, exponent));
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture.get(), RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(framebuffer.get());
        commands->clearColor(0, RenderColor(0,0,0,0), &rectangle, 1);
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        return tile_resolve->Complete(rectangle);
    }
    std::vector<uint16_t> ReadPixels(bool scene_resolve) {
        if (scene_resolve) Require(tile_resolve && !tile_resolve->CompletedRegion().isEmpty(), "Native color resolve has not completed");
        else Require(fully_initialized,"Cannot read undefined pixels from a partially cleared native color target");
        const auto rectangle = scene_resolve ? tile_resolve->CompletedRegion() : RenderRect(0,0,width,height);
        const uint32_t read_width = rectangle.right - rectangle.left, read_height = rectangle.bottom - rectangle.top;
        const uint32_t pitch=(read_width*8+255)&~255u;
        const bool resolve_for_read = !scene_resolve && samples > 1;
        if (!readback) readback=gpu->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t((width*8+255)&~255u)*height));
        if (resolve_for_read && !resolved) resolved=gpu->device->createTexture(RenderTextureDesc::Texture2D(width,height,1,format));
        Require(readback && (!resolve_for_read || resolved),"Native HDR readback allocation failed");
        auto& commands=gpu->commands;
        commands->begin();
        auto* source = scene_resolve ? tile_resolve->Output() : texture.get();
        if (resolve_for_read) {
            commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture.get(),RenderTextureLayout::RESOLVE_SOURCE));
            commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(resolved.get(),RenderTextureLayout::RESOLVE_DEST));
            commands->resolveTextureRegion(resolved.get(),0,0,texture.get());
            source=resolved.get();
        }
        commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(source,RenderTextureLayout::COPY_SOURCE));
        const RenderBox box(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),format,read_width,read_height,1,pitch/8),
                                    RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
        if (scene_resolve)
            commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(source, RenderTextureLayout::SHADER_READ));
        commands->end();
        gpu->queue->executeCommandLists(commands.get(),gpu->fence.get()); gpu->Complete();
        std::vector<uint16_t> pixels(size_t(read_width)*read_height*4);
        const auto* bytes=static_cast<const uint8_t*>(readback->map());
        Require(bytes!=nullptr,"Native HDR readback mapping failed");
        for (uint32_t y=0;y<read_height;++y)
            std::memcpy(pixels.data()+size_t(y)*read_width*4,bytes+size_t(y)*pitch,read_width*8);
        readback->unmap();
        return pixels;
    }
};
ColorTarget::ColorTarget(uint32_t width,uint32_t height,uint32_t samples,std::shared_ptr<NativeDevice> gpu,
                         std::span<const uint8_t> resolve_shader, RenderFormat format)
    : impl_(std::make_unique<Impl>(width,height,samples,std::move(gpu),resolve_shader,format)) {}
ColorTarget::~ColorTarget()=default;
uint64_t ColorTarget::Clear(RenderColor value,const RenderRect* rectangle) { return impl_->Clear(value,rectangle); }
uint64_t ColorTarget::ResolveAndClear(const RenderRect& rectangle, int32_t exponent) { return impl_->ResolveAndClear(rectangle, exponent); }
uint64_t ColorTarget::OverwriteResolved(const ColorTarget& source, int32_t exponent, std::span<const uint8_t> shader) {
    const auto& input = *source.impl_;
    auto& gpu = Device();
    const RenderRect full(0,0,impl_->width,impl_->height);
    Require(input.gpu.get() == &gpu && input.samples == 1 && input.format == impl_->format &&
            input.width == impl_->width && input.height == impl_->height &&
            impl_->tile_resolve && impl_->tile_resolve->CompletedRegion() == full,
            "Fullscreen HDR copy requires a matching single-sample source and complete scene resolve");
    Require(exponent >= -32 && exponent <= 31 && !shader.empty(), "Invalid fullscreen HDR resolve shader or exponent");
    source.RequireCleared(full);
    TileResolve copy(gpu, *input.texture, *impl_->tile_resolve->Output(), shader);
    gpu.commands->begin();
    copy.Record(full, std::ldexp(1.0f, exponent));
    gpu.commands->end();
    gpu.queue->executeCommandLists(gpu.commands.get(), gpu.fence.get());
    return gpu.Complete();
}
std::vector<uint16_t> ColorTarget::ReadPixels() { return impl_->ReadPixels(false); }
std::vector<uint16_t> ColorTarget::ReadResolvedPixels() { return impl_->ReadPixels(true); }
NativeDevice& ColorTarget::Device() const { return *impl_->gpu; }
RenderFormat ColorTarget::Format() const { return impl_->format; }
RenderTexture* ColorTarget::Attachment(const NativeDevice& gpu, uint32_t width, uint32_t height, uint32_t samples) {
    Require(impl_->gpu.get() == &gpu && impl_->width == width && impl_->height == height && impl_->samples == samples,
            "Native color and depth attachments must share a device, extent and sample count");
    return impl_->texture.get();
}
const RenderTexture* ColorTarget::ResolvedTexture(const NativeDevice& gpu) const {
    Require(impl_->gpu.get() == &gpu && impl_->tile_resolve &&
            impl_->tile_resolve->CompletedRegion() == RenderRect(0,0,impl_->width,impl_->height),
            "Native color sampling requires a fully resolved texture on the draw device");
    return impl_->tile_resolve->Output();
}
void ColorTarget::RequireCleared(const RenderRect& rectangle) const {
    impl_->RequireCleared(rectangle);
}
}
