#include "native_depth.h"
#include "native_color_target.h"
#include "native_sampled_texture.h"
#include "native_tile_resolve.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace plume;
namespace mc2::native {
namespace {
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
RenderFormat FloatFormat(uint32_t count) {
    switch (count) {
        case 1: return RenderFormat::R32_FLOAT;
        case 2: return RenderFormat::R32G32_FLOAT;
        case 3: return RenderFormat::R32G32B32_FLOAT;
        case 4: return RenderFormat::R32G32B32A32_FLOAT;
        default: throw std::runtime_error("Invalid native float vertex input");
    }
}
void Upload(RenderBuffer* buffer, const void* bytes, size_t size) {
    auto* mapped = buffer->map();
    Require(mapped != nullptr, "Native buffer mapping failed");
    std::memcpy(mapped, bytes, size);
    buffer->unmap();
}
struct IndexedPipeline {
    NativeDevice* gpu;
    uint32_t width, height;
    std::unique_ptr<RenderShader> vertex_shader, pixel_shader;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderPipeline> pipeline;
    std::unique_ptr<RenderDescriptorSet> texture_set, sampler_set;
    std::vector<std::unique_ptr<RenderSampler>> samplers;
    std::unique_ptr<RenderBuffer> vertex_buffer, index_buffer, vertex_constants, shared_constants, pixel_constants, dummy_input;
    RenderInputSlot slot;
    RenderInputSlot dummy_slot{1, 0};
    size_t vertex_capacity = 0, index_capacity = 0;

    IndexedPipeline(const DepthConfiguration& c, NativeDevice* device_context, std::span<const uint8_t> ps = {},
                    std::span<const RenderSamplerDesc> sampler_descs = {}, RenderFormat color_format = RenderFormat::UNKNOWN)
        : gpu(device_context), width(c.width), height(c.height) {
        Require(gpu && c.width && c.height && c.width <= 8192 && c.height <= 8192,
                "Invalid native indexed target");
        Require(c.sample_count == 1 || c.sample_count == 2 || c.sample_count == 4, "Unsupported native sample count");
        Require(!c.vertex_shader.empty() && !c.inputs.empty() && c.inputs.size() <= 32,
                "Missing native vertex shader or inputs");
        Require(ps.empty() == (color_format == RenderFormat::UNKNOWN) && sampler_descs.size() <= 16 &&
                (!ps.empty() || sampler_descs.empty()), "Invalid native pixel pipeline");
        slot = RenderInputSlot(0, c.inputs.front().stride);
        Require(slot.stride > 0 && slot.stride <= 1020, "Invalid native indexed vertex stride");
        std::vector<RenderInputElement> elements;
        for (const auto& input : c.inputs) {
            const auto element_bytes=input.components*(input.storage==VertexStorage::Float32 ? 4 : 1);
            Require(input.stride == slot.stride && input.fetch_index == c.inputs.front().fetch_index &&
                    (input.constant_only || uint64_t(input.offset) + element_bytes <= slot.stride),
                    "Native indexed pass requires one bounded vertex stream");
            Require(input.storage==VertexStorage::Float32 || input.components==4,"Invalid native byte vertex input");
            const auto format=input.storage==VertexStorage::UInt8 ? RenderFormat::R8G8B8A8_UINT :
                input.storage==VertexStorage::UNorm8 ? RenderFormat::R8G8B8A8_UNORM : FloatFormat(input.components);
            elements.emplace_back(VertexSemantic(input.usage), input.usage_index, uint32_t(elements.size()),
                                  format, input.constant_only ? 1 : 0, input.offset);
        }
        auto& device = gpu->device;
        const RenderRootDescriptorDesc roots[] = {
            {0, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
            {2, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
            {1, 4, RenderRootDescriptorType::CONSTANT_BUFFER}
        };
        const uint32_t count = uint32_t(sampler_descs.size());
        const RenderDescriptorRange texture_range(RenderDescriptorRangeType::TEXTURE, 0, count);
        const RenderDescriptorRange sampler_range(RenderDescriptorRangeType::SAMPLER, 0, count);
        std::array<RenderDescriptorSetDesc, 4> sets{};
        RenderPipelineLayoutDesc layout_desc;
        layout_desc.rootDescriptorDescs = roots;
        layout_desc.rootDescriptorDescsCount = ps.empty() ? 2 : 3;
        layout_desc.allowInputLayout = true;
        if (count) {
            sets[0] = RenderDescriptorSetDesc(&texture_range, 1, true, count);
            sets[3] = RenderDescriptorSetDesc(&sampler_range, 1, true, count);
            layout_desc.descriptorSetDescs = sets.data();
            layout_desc.descriptorSetDescsCount = uint32_t(sets.size());
            texture_set = device->createDescriptorSet(sets[0]);
            sampler_set = device->createDescriptorSet(sets[3]);
            Require(texture_set && sampler_set, "Native indexed descriptor creation failed");
            for (const auto& sampler_desc : sampler_descs) {
                auto sampler = device->createSampler(sampler_desc);
                Require(bool(sampler), "Native indexed sampler creation failed");
                sampler_set->setSampler(uint32_t(samplers.size()), sampler.get());
                samplers.push_back(std::move(sampler));
            }
        }
        layout = device->createPipelineLayout(layout_desc);
        vertex_shader = device->createShader(c.vertex_shader.data(), c.vertex_shader.size(), "main", RenderShaderFormat::DXIL);
        if (!ps.empty()) {
            pixel_shader = device->createShader(ps.data(), ps.size(), "main", RenderShaderFormat::DXIL);
            pixel_constants = device->createBuffer(RenderBufferDesc::UploadBuffer(4096));
            Require(pixel_shader && pixel_constants, "Native pixel shader or constant allocation failed");
        }
        Require(layout && vertex_shader, "Native indexed shader/layout creation failed");
        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get(); desc.vertexShader = vertex_shader.get();
        if (pixel_shader) {
            desc.pixelShader = pixel_shader.get();
            desc.renderTargetCount = 1;
            desc.renderTargetFormat[0] = color_format;
            desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        }
        const RenderInputSlot slots[] = {slot, dummy_slot};
        desc.inputSlots = slots; desc.inputSlotsCount = 2;
        desc.inputElements = elements.data(); desc.inputElementsCount = uint32_t(elements.size());
        desc.depthEnabled = true; desc.depthWriteEnabled = c.write; desc.depthClipEnabled = c.clip;
        desc.depthFunction = c.comparison; desc.cullMode = c.cull;
        desc.multisampling = RenderMultisampling(c.sample_count);
        desc.depthTargetFormat = RenderFormat::D32_FLOAT;
        pipeline = device->createGraphicsPipeline(desc);
        Require(bool(pipeline), "Native indexed pipeline creation failed");
        vertex_constants = device->createBuffer(RenderBufferDesc::UploadBuffer(4096));
        shared_constants = device->createBuffer(RenderBufferDesc::UploadBuffer(512));
        dummy_input = device->createBuffer(RenderBufferDesc::VertexBuffer(16, RenderHeapType::UPLOAD));
        Require(vertex_constants && shared_constants && dummy_input, "Native indexed resource creation failed");
        const float dummy[] = {0,0,0,1}; Upload(dummy_input.get(), dummy, sizeof(dummy));
    }
    void Prepare(const DepthDraw& draw, std::span<const uint8_t> ps = {}, std::span<const SampledTexture* const> textures = {}) {
        Require(draw.vertex_constants.size() == 4096 && draw.shared_constants.size() == 512,
                "Invalid native vertex constants");
        Require(ps.size() == (pixel_shader ? 4096u : 0u) && textures.size() == samplers.size(),
                "Invalid native pixel constants or texture count");
        for (const auto* texture : textures)
            Require(texture && &texture->Device() == gpu, "Native sampled texture belongs to a different device or is missing");
        if (!textures.empty()) {
            for (uint32_t slot = 0; slot < 16; ++slot) {
                uint32_t texture_index, sampler_index;
                std::memcpy(&texture_index, draw.shared_constants.data() + slot * 4, 4);
                std::memcpy(&sampler_index, draw.shared_constants.data() + 192 + slot * 4, 4);
                Require(texture_index < textures.size() && sampler_index < samplers.size(),
                        "Native texture or sampler index is out of range");
            }
        }
        Require(!draw.vertices.empty() && draw.vertices.size() <= 16 * 1024 * 1024 &&
                draw.vertices.size() % slot.stride == 0 && draw.indices.size() <= 4 * 1024 * 1024,
                "Invalid native indexed geometry extent");
        Require(draw.index_count && draw.index_count % 3 == 0 &&
                uint64_t(draw.first_index) + draw.index_count <= draw.indices.size(), "Invalid native triangle range");
        for (size_t i = draw.first_index; i < uint64_t(draw.first_index) + draw.index_count; ++i)
            Require(draw.indices[i] < draw.vertices.size() / slot.stride, "Native index exceeds its vertex buffer");
        const auto& v = draw.viewport;
        for (float f : {v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth})
            Require(std::isfinite(f), "Non-finite native viewport");
        // Keep the verified subpixel viewport jitter. The scissor bounds all writes.
        Require(v.x >= -1 && v.y >= -1 && v.width > 0 && v.height > 0 && v.width <= width && v.height <= height &&
                v.x + v.width <= width + 1 && v.y + v.height <= height + 1 &&
                v.minDepth >= 0 && v.minDepth <= 1 && v.maxDepth >= 0 && v.maxDepth <= 1,
                "Native indexed viewport is out of bounds");
        Require(draw.scissor.left >= 0 && draw.scissor.top >= 0 && draw.scissor.right > draw.scissor.left &&
                draw.scissor.bottom > draw.scissor.top && uint32_t(draw.scissor.right) <= width &&
                uint32_t(draw.scissor.bottom) <= height, "Native scissor is out of bounds");
        if (draw.vertices.size() > vertex_capacity) {
            auto buffer = gpu->device->createBuffer(RenderBufferDesc::VertexBuffer(draw.vertices.size(), RenderHeapType::UPLOAD));
            Require(bool(buffer), "Native vertex allocation failed");
            vertex_buffer = std::move(buffer); vertex_capacity = draw.vertices.size();
        }
        const size_t index_bytes = draw.indices.size() * 4;
        if (index_bytes > index_capacity) {
            auto buffer = gpu->device->createBuffer(RenderBufferDesc::IndexBuffer(index_bytes, RenderHeapType::UPLOAD));
            Require(bool(buffer), "Native index allocation failed");
            index_buffer = std::move(buffer); index_capacity = index_bytes;
        }
        Upload(vertex_buffer.get(), draw.vertices.data(), draw.vertices.size());
        Upload(index_buffer.get(), draw.indices.data(), index_bytes);
        Upload(vertex_constants.get(), draw.vertex_constants.data(), 4096);
        Upload(shared_constants.get(), draw.shared_constants.data(), 512);
        if (pixel_shader) Upload(pixel_constants.get(), ps.data(), ps.size());
        for (uint32_t i = 0; i < textures.size(); ++i)
            texture_set->setTexture(i, textures[i]->Texture(), RenderTextureLayout::SHADER_READ);
    }
    void Record(const DepthDraw& draw) const {
        auto& commands = gpu->commands;
        commands->setGraphicsPipelineLayout(layout.get()); commands->setPipeline(pipeline.get());
        commands->setViewports(draw.viewport); commands->setScissors(draw.scissor);
        commands->setGraphicsRootDescriptor(vertex_constants.get(), 0);
        commands->setGraphicsRootDescriptor(shared_constants.get(), 1);
        if (pixel_shader) commands->setGraphicsRootDescriptor(pixel_constants.get(), 2);
        if (texture_set) {
            commands->setGraphicsDescriptorSet(texture_set.get(), 0);
            commands->setGraphicsDescriptorSet(sampler_set.get(), 3);
        }
        const RenderVertexBufferView vertex_view(vertex_buffer.get(), draw.vertices.size());
        const RenderIndexBufferView index_view(index_buffer.get(), draw.indices.size() * 4, RenderFormat::R32_UINT);
        commands->setVertexBuffers(0, &vertex_view, 1, &slot); commands->setIndexBuffer(&index_view);
        const RenderVertexBufferView dummy_view(dummy_input.get(), 16);
        commands->setVertexBuffers(1, &dummy_view, 1, &dummy_slot);
        commands->drawIndexedInstanced(draw.index_count, 1, draw.first_index, 0, 0);
    }
};
}

struct DepthRenderer::Impl {
    uint32_t width, height, samples;
    std::shared_ptr<NativeDevice> gpu;
    IndexedPipeline draw_pipeline;
    std::unique_ptr<RenderTexture> target, resolved;
    std::unique_ptr<RenderTextureView> depth_view, sample_view;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> readback;
    std::unique_ptr<TileResolve> tile_resolve;
    bool initialized = false;

    Impl(const DepthConfiguration& c, std::shared_ptr<NativeDevice> context, std::span<const uint8_t> resolve_shader)
        : width(c.width), height(c.height), samples(c.sample_count), gpu(std::move(context)), draw_pipeline(c, gpu.get()) {
        const bool sample_depth = !resolve_shader.empty();
        Require(!sample_depth || samples == 2, "Native scene depth resolve requires two samples");
        const auto storage = sample_depth ? RenderFormat::R32_TYPELESS : RenderFormat::D32_FLOAT;
        target = gpu->device->createTexture(RenderTextureDesc::DepthTarget(width, height, storage,
                                                                          RenderMultisampling(samples)));
        Require(bool(target), "Native depth allocation failed");
        if (sample_depth) {
            depth_view = target->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::D32_FLOAT));
            sample_view = target->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R32_FLOAT));
            Require(depth_view && sample_view, "Native depth view creation failed");
            tile_resolve = std::make_unique<TileResolve>(*gpu, *target, width, height, RenderFormat::R32_FLOAT,
                                                        resolve_shader, sample_view.get());
        }
        RenderFramebufferDesc framebuffer_desc;
        framebuffer_desc.depthAttachment = target.get();
        framebuffer_desc.depthAttachmentView = depth_view.get();
        framebuffer = gpu->device->createFramebuffer(framebuffer_desc);
        Require(bool(framebuffer), "Native depth framebuffer creation failed");
    }
    void Clear(float depth, const RenderRect* rectangle) {
        Require(std::isfinite(depth) && depth >= 0 && depth <= 1, "Invalid native depth clear");
        const RenderRect full(0,0,width,height);
        if (rectangle) {
            Require(rectangle->left >= 0 && rectangle->top >= 0 && rectangle->right > rectangle->left &&
                    rectangle->bottom > rectangle->top && uint32_t(rectangle->right) <= width &&
                    uint32_t(rectangle->bottom) <= height, "Native depth clear rectangle is out of bounds");
            Require(initialized || *rectangle == full, "Partial clear cannot initialize a new native depth target");
        }
        auto& commands = gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(framebuffer.get());
        commands->clearDepth(true, depth, rectangle, rectangle ? 1 : 0);
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        gpu->Complete();
        initialized = true;
    }
    void EnsureReadback() {
        if (!readback)
            readback = gpu->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t((width * 4 + 255) & ~255u) * height));
        Require(bool(readback), "Native depth readback allocation failed");
    }
    void RecordReadback(RenderTexture* source, const RenderRect& rectangle) {
        const uint32_t w = rectangle.right - rectangle.left, h = rectangle.bottom - rectangle.top;
        const auto pitch = (w * 4 + 255) & ~255u;
        const RenderBox box(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        auto& commands = gpu->commands;
        commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(source, RenderTextureLayout::COPY_SOURCE));
        commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R32_FLOAT,
            w, h, 1, pitch / 4), RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
    }
    std::vector<float> MappedDepth(uint32_t w, uint32_t h) {
        const auto pitch = (w * 4 + 255) & ~255u;
        std::vector<float> pixels(size_t(w) * h);
        const auto* bytes = static_cast<const uint8_t*>(readback->map());
        Require(bytes != nullptr, "Native depth readback mapping failed");
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(pixels.data() + size_t(y) * w, bytes + size_t(y) * pitch, w * 4);
        readback->unmap();
        return pixels;
    }
    uint64_t ResolveAndClear(const RenderRect& rectangle) {
        Require(tile_resolve && initialized, "Native depth resolve requires a configured and initialized target");
        Require(!rectangle.isEmpty() && rectangle.left >= 0 && rectangle.top >= 0 &&
                uint32_t(rectangle.right) <= width && uint32_t(rectangle.bottom) <= height,
                "Native depth resolve rectangle is out of bounds");
        auto& commands = gpu->commands;
        commands->begin();
        tile_resolve->Record(rectangle, 1);
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(framebuffer.get());
        commands->clearDepth(true, 0, &rectangle, 1);
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        return tile_resolve->Complete(rectangle);
    }
    std::vector<float> ReadResolvedPixels() {
        Require(tile_resolve && !tile_resolve->CompletedRegion().isEmpty(), "Native depth resolve has not completed");
        const auto& resolved_rectangle = tile_resolve->CompletedRegion();
        EnsureReadback();
        auto& commands = gpu->commands;
        auto* source = tile_resolve->Output();
        commands->begin();
        RecordReadback(source, resolved_rectangle);
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(source, RenderTextureLayout::SHADER_READ));
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        gpu->Complete();
        return MappedDepth(resolved_rectangle.right - resolved_rectangle.left, resolved_rectangle.bottom - resolved_rectangle.top);
    }
    DepthResult Render(const DepthDraw& draw, bool read_pixels) {
        Require(initialized, "Native depth target has not been cleared");
        draw_pipeline.Prepare(draw);
        if (read_pixels) EnsureReadback();
        if (read_pixels && samples > 1 && !resolved)
            resolved = gpu->device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R32_FLOAT));
        Require(!read_pixels || samples == 1 || bool(resolved), "Native depth resolve allocation failed");
        auto& commands = gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(framebuffer.get());
        draw_pipeline.Record(draw);
        if (read_pixels) {
            auto* read_target = target.get();
            if (samples > 1) {
                commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::RESOLVE_SOURCE));
                commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(resolved.get(), RenderTextureLayout::RESOLVE_DEST));
                commands->resolveTextureRegion(resolved.get(), 0, 0, target.get(), nullptr, RenderResolveMode::MAX);
                read_target = resolved.get();
            }
            RecordReadback(read_target, RenderRect(0,0,width,height));
        }
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        DepthResult result;
        result.completed_submission = gpu->Complete(); result.debug_layer = gpu->debug_enabled;
        if (read_pixels) result.pixels = MappedDepth(width, height);
        return result;
    }
};
DepthRenderer::DepthRenderer(const DepthConfiguration& configuration, std::shared_ptr<NativeDevice> device,
                             std::span<const uint8_t> resolve_shader)
    : impl_(std::make_unique<Impl>(configuration, std::move(device), resolve_shader)) {}
DepthRenderer::~DepthRenderer() = default;
void DepthRenderer::Clear(float depth, const RenderRect* rectangle) { impl_->Clear(depth, rectangle); }
DepthResult DepthRenderer::Render(const DepthDraw& draw, bool read_pixels) { return impl_->Render(draw, read_pixels); }
uint64_t DepthRenderer::ResolveAndClear(const RenderRect& rectangle) { return impl_->ResolveAndClear(rectangle); }
std::vector<float> DepthRenderer::ReadResolvedPixels() { return impl_->ReadResolvedPixels(); }

struct ColorRenderer::Impl {
    DepthRenderer::Impl& depth;
    ColorTarget& color;
    RenderTexture* attachment;
    IndexedPipeline pipeline;
    std::unique_ptr<RenderFramebuffer> framebuffer;

    Impl(const DepthConfiguration& configuration, std::span<const uint8_t> pixel_shader,
         std::span<const RenderSamplerDesc> samplers, DepthRenderer& depth_renderer, ColorTarget& color_target)
        : depth(*depth_renderer.impl_), color(color_target),
          attachment(color.Attachment(*depth.gpu, depth.width, depth.height, depth.samples)),
          pipeline(configuration, depth.gpu.get(), pixel_shader, samplers, ColorTarget::kFormat) {
        Require(configuration.width == depth.width && configuration.height == depth.height &&
                configuration.sample_count == depth.samples, "Native color pipeline differs from its attachments");
        RenderFramebufferDesc desc;
        const RenderTexture* target = attachment;
        desc.colorAttachments = &target; desc.colorAttachmentsCount = 1;
        desc.depthAttachment = depth.target.get();
        desc.depthAttachmentView = depth.depth_view.get();
        framebuffer = depth.gpu->device->createFramebuffer(desc);
        Require(bool(framebuffer), "Native color/depth framebuffer creation failed");
    }
    uint64_t Render(const DepthDraw& draw, std::span<const uint8_t> pixel_constants,
                    std::span<const SampledTexture* const> textures) {
        Require(depth.initialized, "Native depth target has not been cleared");
        color.RequireCleared(draw.scissor);
        pipeline.Prepare(draw, pixel_constants, textures);
        auto& gpu = *depth.gpu;
        auto& commands = gpu.commands;
        const RenderTextureBarrier barriers[] = {
            {attachment, RenderTextureLayout::COLOR_WRITE}, {depth.target.get(), RenderTextureLayout::DEPTH_WRITE}
        };
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS, barriers, uint32_t(std::size(barriers)));
        commands->setFramebuffer(framebuffer.get());
        pipeline.Record(draw);
        commands->end();
        gpu.queue->executeCommandLists(commands.get(), gpu.fence.get());
        return gpu.Complete();
    }
};
ColorRenderer::ColorRenderer(const DepthConfiguration& c, std::span<const uint8_t> ps,
                             std::span<const RenderSamplerDesc> samplers, DepthRenderer& depth, ColorTarget& color)
    : impl_(std::make_unique<Impl>(c, ps, samplers, depth, color)) {}
ColorRenderer::~ColorRenderer() = default;
uint64_t ColorRenderer::Render(const DepthDraw& draw, std::span<const uint8_t> ps,
                               std::span<const SampledTexture* const> textures) {
    return impl_->Render(draw, ps, textures);
}
}
