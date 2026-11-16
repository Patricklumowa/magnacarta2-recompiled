#include "native_depth.h"
#include <algorithm>
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
}
struct DepthRenderer::Impl {
    DepthConfiguration config;
    std::shared_ptr<NativeDevice> gpu;
    std::unique_ptr<RenderShader> shader;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderPipeline> pipeline;
    std::unique_ptr<RenderTexture> target, resolved;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> vertex_buffer, index_buffer, vertex_constants, shared_constants, readback, dummy_input;
    RenderInputSlot slot;
    RenderInputSlot dummy_slot{1, 0};
    size_t vertex_capacity = 0, index_capacity = 0;
    bool initialized = false;

    Impl(const DepthConfiguration& c, std::shared_ptr<NativeDevice> context) : config(c), gpu(std::move(context)) {
        Require(gpu && c.width && c.height && c.width <= 8192 && c.height <= 8192,
                "Invalid native depth target");
        Require(c.sample_count == 1 || c.sample_count == 2 || c.sample_count == 4, "Unsupported native sample count");
        Require(!c.vertex_shader.empty() && !c.inputs.empty() && c.inputs.size() <= 32,
                "Missing native depth shader or inputs");
        slot = RenderInputSlot(0, c.inputs.front().stride);
        Require(slot.stride > 0 && slot.stride <= 1020, "Invalid native depth vertex stride");
        std::vector<RenderInputElement> elements;
        for (const auto& input : c.inputs) {
            const auto element_bytes=input.components*(input.storage==VertexStorage::Float32 ? 4 : 1);
            Require(input.stride == slot.stride && input.fetch_index == c.inputs.front().fetch_index &&
                    (input.constant_only || uint64_t(input.offset) + element_bytes <= slot.stride),
                    "Native depth pass requires one bounded vertex stream");
            Require(input.storage==VertexStorage::Float32 || input.components==4,"Invalid native byte vertex input");
            const auto format=input.storage==VertexStorage::UInt8 ? RenderFormat::R8G8B8A8_UINT :
                input.storage==VertexStorage::UNorm8 ? RenderFormat::R8G8B8A8_UNORM : FloatFormat(input.components);
            elements.emplace_back(VertexSemantic(input.usage), input.usage_index, uint32_t(elements.size()),
                                  format, input.constant_only ? 1 : 0, input.offset);
        }
        auto& device = gpu->device;
        const RenderRootDescriptorDesc roots[] = {
            {0, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
            {2, 4, RenderRootDescriptorType::CONSTANT_BUFFER}
        };
        RenderPipelineLayoutDesc layout_desc;
        layout_desc.rootDescriptorDescs = roots;
        layout_desc.rootDescriptorDescsCount = std::size(roots);
        layout_desc.allowInputLayout = true;
        layout = device->createPipelineLayout(layout_desc);
        shader = device->createShader(c.vertex_shader.data(), c.vertex_shader.size(), "main", RenderShaderFormat::DXIL);
        Require(layout && shader, "Native depth shader/layout creation failed");
        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get(); desc.vertexShader = shader.get();
        const RenderInputSlot slots[] = {slot, dummy_slot};
        desc.inputSlots = slots; desc.inputSlotsCount = 2;
        desc.inputElements = elements.data(); desc.inputElementsCount = uint32_t(elements.size());
        desc.depthEnabled = true; desc.depthWriteEnabled = c.write; desc.depthClipEnabled = c.clip;
        desc.depthFunction = c.comparison; desc.cullMode = c.cull;
        desc.multisampling = RenderMultisampling(c.sample_count);
        desc.depthTargetFormat = RenderFormat::D32_FLOAT;
        pipeline = device->createGraphicsPipeline(desc);
        Require(bool(pipeline), "Native depth pipeline creation failed");
        target = device->createTexture(RenderTextureDesc::DepthTarget(c.width, c.height, RenderFormat::D32_FLOAT,
                                                                     RenderMultisampling(c.sample_count)));
        Require(bool(target), "Native depth allocation failed");
        RenderFramebufferDesc framebuffer_desc;
        framebuffer_desc.depthAttachment = target.get();
        framebuffer = device->createFramebuffer(framebuffer_desc);
        vertex_constants = device->createBuffer(RenderBufferDesc::UploadBuffer(4096));
        shared_constants = device->createBuffer(RenderBufferDesc::UploadBuffer(512));
        dummy_input = device->createBuffer(RenderBufferDesc::VertexBuffer(16, RenderHeapType::UPLOAD));
        Require(framebuffer && vertex_constants && shared_constants && dummy_input, "Native depth resource creation failed");
        const float dummy[] = {0,0,0,1}; Upload(dummy_input.get(), dummy, sizeof(dummy));
    }
    void Clear(float depth, const RenderRect* rectangle) {
        Require(std::isfinite(depth) && depth >= 0 && depth <= 1, "Invalid native depth clear");
        const RenderRect full(0,0,config.width,config.height);
        if (rectangle) {
            Require(rectangle->left >= 0 && rectangle->top >= 0 && rectangle->right > rectangle->left &&
                    rectangle->bottom > rectangle->top && uint32_t(rectangle->right) <= config.width &&
                    uint32_t(rectangle->bottom) <= config.height, "Native depth clear rectangle is out of bounds");
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
    DepthResult Render(const DepthDraw& draw, bool read_pixels) {
        Require(initialized, "Native depth target has not been cleared");
        Require(draw.vertex_constants.size() == 4096 && draw.shared_constants.size() == 512,
                "Invalid native vertex constants");
        Require(!draw.vertices.empty() && draw.vertices.size() <= 16 * 1024 * 1024 &&
                draw.vertices.size() % slot.stride == 0 && draw.indices.size() <= 4 * 1024 * 1024,
                "Invalid native depth geometry extent");
        Require(draw.index_count && draw.index_count % 3 == 0 &&
                uint64_t(draw.first_index) + draw.index_count <= draw.indices.size(), "Invalid native triangle range");
        for (size_t i = draw.first_index; i < uint64_t(draw.first_index) + draw.index_count; ++i)
            Require(draw.indices[i] < draw.vertices.size() / slot.stride, "Native index exceeds its vertex buffer");
        const auto& v = draw.viewport;
        for (float f : {v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth})
            Require(std::isfinite(f), "Non-finite native viewport");
        Require(v.x >= 0 && v.y >= 0 && v.width > 0 && v.height > 0 &&
                v.x + v.width <= config.width && v.y + v.height <= config.height &&
                v.minDepth >= 0 && v.minDepth <= 1 && v.maxDepth >= 0 && v.maxDepth <= 1,
                "Native depth viewport is out of bounds");
        Require(draw.scissor.left >= 0 && draw.scissor.top >= 0 && draw.scissor.right > draw.scissor.left &&
                draw.scissor.bottom > draw.scissor.top && uint32_t(draw.scissor.right) <= config.width &&
                uint32_t(draw.scissor.bottom) <= config.height, "Native scissor is out of bounds");
        if (draw.vertices.size() > vertex_capacity) {
            vertex_capacity = draw.vertices.size();
            vertex_buffer = gpu->device->createBuffer(RenderBufferDesc::VertexBuffer(vertex_capacity, RenderHeapType::UPLOAD));
        }
        const size_t index_bytes = draw.indices.size() * 4;
        if (index_bytes > index_capacity) {
            index_capacity = index_bytes;
            index_buffer = gpu->device->createBuffer(RenderBufferDesc::IndexBuffer(index_capacity, RenderHeapType::UPLOAD));
        }
        Require(vertex_buffer && index_buffer, "Native geometry allocation failed");
        Upload(vertex_buffer.get(), draw.vertices.data(), draw.vertices.size());
        Upload(index_buffer.get(), draw.indices.data(), index_bytes);
        Upload(vertex_constants.get(), draw.vertex_constants.data(), 4096);
        Upload(shared_constants.get(), draw.shared_constants.data(), 512);
        const auto pitch = (config.width * 4 + 255) & ~255u;
        if (read_pixels && !readback)
            readback = gpu->device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t(pitch) * config.height));
        Require(!read_pixels || bool(readback), "Native depth readback allocation failed");
        if (read_pixels && config.sample_count > 1 && !resolved)
            resolved = gpu->device->createTexture(RenderTextureDesc::Texture2D(config.width, config.height, 1, RenderFormat::R32_FLOAT));
        Require(!read_pixels || config.sample_count == 1 || bool(resolved), "Native depth resolve allocation failed");
        auto& commands = gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::DEPTH_WRITE));
        commands->setFramebuffer(framebuffer.get());
        commands->setGraphicsPipelineLayout(layout.get()); commands->setPipeline(pipeline.get());
        commands->setViewports(draw.viewport); commands->setScissors(draw.scissor);
        commands->setGraphicsRootDescriptor(vertex_constants.get(), 0);
        commands->setGraphicsRootDescriptor(shared_constants.get(), 1);
        const RenderVertexBufferView vertex_view(vertex_buffer.get(), draw.vertices.size());
        const RenderIndexBufferView index_view(index_buffer.get(), index_bytes, RenderFormat::R32_UINT);
        commands->setVertexBuffers(0, &vertex_view, 1, &slot); commands->setIndexBuffer(&index_view);
        const RenderVertexBufferView dummy_view(dummy_input.get(), 16);
        commands->setVertexBuffers(1, &dummy_view, 1, &dummy_slot);
        commands->drawIndexedInstanced(draw.index_count, 1, draw.first_index, 0, 0);
        if (read_pixels) {
            auto* read_target = target.get();
            if (config.sample_count > 1) {
                commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::RESOLVE_SOURCE));
                commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(resolved.get(), RenderTextureLayout::RESOLVE_DEST));
                commands->resolveTextureRegion(resolved.get(), 0, 0, target.get(), nullptr, RenderResolveMode::MAX);
                read_target = resolved.get();
            }
            commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(read_target, RenderTextureLayout::COPY_SOURCE));
            commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R32_FLOAT,
                config.width, config.height, 1, pitch / 4), RenderTextureCopyLocation::Subresource(read_target));
        }
        commands->end();
        gpu->queue->executeCommandLists(commands.get(), gpu->fence.get());
        DepthResult result;
        result.completed_submission = gpu->Complete(); result.debug_layer = gpu->debug_enabled;
        if (read_pixels) {
            result.pixels.resize(size_t(config.width) * config.height);
            const auto* bytes = static_cast<const uint8_t*>(readback->map());
            Require(bytes != nullptr, "Native depth readback mapping failed");
            for (uint32_t y = 0; y < config.height; ++y)
                std::memcpy(result.pixels.data() + size_t(y) * config.width, bytes + size_t(y) * pitch, config.width * 4);
            readback->unmap();
        }
        return result;
    }
};
DepthRenderer::DepthRenderer(const DepthConfiguration& configuration, std::shared_ptr<NativeDevice> device)
    : impl_(std::make_unique<Impl>(configuration, std::move(device))) {}
DepthRenderer::~DepthRenderer() = default;
void DepthRenderer::Clear(float depth, const RenderRect* rectangle) { impl_->Clear(depth, rectangle); }
DepthResult DepthRenderer::Render(const DepthDraw& draw, bool read_pixels) { return impl_->Render(draw, read_pixels); }
}
