#include "native_color_target.h"
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
struct ResolveParameters {
    uint32_t x, y, width, height;
    float scale;
};
}
struct ColorTarget::Impl {
    uint32_t width,height,samples;
    std::shared_ptr<NativeDevice> gpu;
    std::unique_ptr<RenderTexture> texture,resolved;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> readback;
    std::unique_ptr<RenderTexture> resolve_output;
    std::unique_ptr<RenderDescriptorSet> resolve_set;
    std::unique_ptr<RenderShader> resolve_shader;
    std::unique_ptr<RenderPipelineLayout> resolve_layout;
    std::unique_ptr<RenderPipeline> resolve_pipeline;
    bool fully_initialized=false;
    RenderRect cleared_rectangle, resolved_rectangle;

    Impl(uint32_t w,uint32_t h,uint32_t count,std::shared_ptr<NativeDevice> context, std::span<const uint8_t> shader)
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
        if (!shader.empty()) {
            Require(samples == 2, "Native scene color resolve requires two samples");
            auto& device = gpu->device;
            resolve_output = device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, kFormat,
                RenderTextureFlag::UNORDERED_ACCESS));
            resolve_shader = device->createShader(shader.data(), shader.size(), "main", RenderShaderFormat::DXIL);
            const RenderDescriptorRange ranges[] = {
                {RenderDescriptorRangeType::TEXTURE, 0, 1}, {RenderDescriptorRangeType::READ_WRITE_TEXTURE, 0, 1}
            };
            const RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
            resolve_set = device->createDescriptorSet(set_desc);
            Require(resolve_output && resolve_shader && resolve_set, "Native color resolve resource creation failed");
            resolve_set->setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
            resolve_set->setTexture(1, resolve_output.get(), RenderTextureLayout::GENERAL);
            const RenderPushConstantRange parameters(0, 0, 0, sizeof(ResolveParameters), RenderShaderStageFlag::COMPUTE);
            RenderPipelineLayoutDesc layout;
            layout.descriptorSetDescs = &set_desc; layout.descriptorSetDescsCount = 1;
            layout.pushConstantRanges = &parameters; layout.pushConstantRangesCount = 1;
            resolve_layout = device->createPipelineLayout(layout);
            Require(bool(resolve_layout), "Native color resolve layout creation failed");
            resolve_pipeline = device->createComputePipeline(RenderComputePipelineDesc(resolve_layout.get(), resolve_shader.get(), 8, 8, 1));
            Require(bool(resolve_pipeline), "Native color resolve pipeline creation failed");
        }
    }
    void RequireCleared(const RenderRect& rectangle) const {
        Require(Covers(fully_initialized ? RenderRect(0,0,width,height) : cleared_rectangle, rectangle),
                "Native color operation touches an uninitialized region");
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
    uint64_t ResolveAndClear(const RenderRect& rectangle, int32_t exponent) {
        Require(bool(resolve_pipeline), "Native color resolve shader is unavailable");
        Require(exponent >= -32 && exponent <= 31, "Native color resolve exponent is out of range");
        RequireCleared(rectangle);
        const ResolveParameters parameters = {uint32_t(rectangle.left), uint32_t(rectangle.top),
            uint32_t(rectangle.right - rectangle.left), uint32_t(rectangle.bottom - rectangle.top),
            std::ldexp(1.0f, exponent)};
        auto& commands = gpu->commands;
        commands->begin();
        const RenderTextureBarrier barriers[] = {
            {texture.get(), RenderTextureLayout::SHADER_READ}, {resolve_output.get(), RenderTextureLayout::GENERAL}
        };
        commands->barriers(RenderBarrierStage::COMPUTE, barriers, uint32_t(std::size(barriers)));
        commands->setComputePipelineLayout(resolve_layout.get());
        commands->setPipeline(resolve_pipeline.get());
        commands->setComputeDescriptorSet(resolve_set.get(), 0);
        commands->setComputePushConstants(0, &parameters);
        commands->dispatch((parameters.width + 7) / 8, (parameters.height + 7) / 8, 1);
        const RenderTextureBarrier after[] = {
            {texture.get(), RenderTextureLayout::COLOR_WRITE}, {resolve_output.get(), RenderTextureLayout::SHADER_READ}
        };
        commands->barriers(RenderBarrierStage::GRAPHICS, after, uint32_t(std::size(after)));
        commands->setFramebuffer(framebuffer.get());
        commands->clearColor(0, RenderColor(0,0,0,0), &rectangle, 1);
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        const auto completed = gpu->Complete();
        resolved_rectangle = rectangle;
        return completed;
    }
    std::vector<uint16_t> ReadPixels(bool scene_resolve) {
        const auto rectangle = scene_resolve ? resolved_rectangle : RenderRect(0,0,width,height);
        if (scene_resolve) Require(!rectangle.isEmpty(), "Native color resolve has not completed");
        else Require(fully_initialized,"Cannot read undefined pixels from a partially cleared native color target");
        const uint32_t read_width = rectangle.right - rectangle.left, read_height = rectangle.bottom - rectangle.top;
        const uint32_t pitch=(read_width*8+255)&~255u;
        const bool resolve_for_read = !scene_resolve && samples > 1;
        if (!readback) readback=gpu->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t((width*8+255)&~255u)*height));
        if (resolve_for_read && !resolved) resolved=gpu->device->createTexture(RenderTextureDesc::Texture2D(width,height,1,kFormat));
        Require(readback && (!resolve_for_read || resolved),"Native HDR readback allocation failed");
        auto& commands=gpu->commands;
        commands->begin();
        auto* source = scene_resolve ? resolve_output.get() : texture.get();
        if (resolve_for_read) {
            commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture.get(),RenderTextureLayout::RESOLVE_SOURCE));
            commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(resolved.get(),RenderTextureLayout::RESOLVE_DEST));
            commands->resolveTextureRegion(resolved.get(),0,0,texture.get());
            source=resolved.get();
        }
        commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(source,RenderTextureLayout::COPY_SOURCE));
        const RenderBox box(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),kFormat,read_width,read_height,1,pitch/8),
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
                         std::span<const uint8_t> resolve_shader)
    : impl_(std::make_unique<Impl>(width,height,samples,std::move(gpu),resolve_shader)) {}
ColorTarget::~ColorTarget()=default;
uint64_t ColorTarget::Clear(RenderColor value,const RenderRect* rectangle) { return impl_->Clear(value,rectangle); }
uint64_t ColorTarget::ResolveAndClear(const RenderRect& rectangle, int32_t exponent) { return impl_->ResolveAndClear(rectangle, exponent); }
std::vector<uint16_t> ColorTarget::ReadPixels() { return impl_->ReadPixels(false); }
std::vector<uint16_t> ColorTarget::ReadResolvedPixels() { return impl_->ReadPixels(true); }
RenderTexture* ColorTarget::Attachment(const NativeDevice& gpu, uint32_t width, uint32_t height, uint32_t samples) {
    Require(impl_->gpu.get() == &gpu && impl_->width == width && impl_->height == height && impl_->samples == samples,
            "Native color and depth attachments must share a device, extent and sample count");
    return impl_->texture.get();
}
void ColorTarget::RequireCleared(const RenderRect& rectangle) const {
    impl_->RequireCleared(rectangle);
}
}
