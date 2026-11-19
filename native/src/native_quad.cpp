#include "native_quad.h"
#include "native_color_target.h"
#include "native_device.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace plume;
namespace mc2::native {
namespace {
void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

RenderViewport AspectViewport(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height) {
    const double scale = std::min(double(width) / source_width, double(height) / source_height);
    const auto viewport_width = std::min(width, uint32_t(std::lround(source_width * scale)));
    const auto viewport_height = std::min(height, uint32_t(std::lround(source_height * scale)));
    return RenderViewport(float((width - viewport_width) / 2), float((height - viewport_height) / 2),
                          float(viewport_width), float(viewport_height));
}

struct PresentPipeline {
    std::unique_ptr<RenderShader> vs, ps;
    std::unique_ptr<RenderSampler> sampler;
    std::unique_ptr<RenderDescriptorSet> texture_set, sampler_set;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderPipeline> pipeline;

    PresentPipeline(RenderDevice& device, RenderFormat format, std::span<const uint8_t> vertex,
                    std::span<const uint8_t> pixel) {
        Require(!vertex.empty() && !pixel.empty(), "Missing native presentation shaders");
        vs = device.createShader(vertex.data(), vertex.size(), "main", RenderShaderFormat::DXIL);
        ps = device.createShader(pixel.data(), pixel.size(), "main", RenderShaderFormat::DXIL);
        RenderSamplerDesc sampler_desc;
        sampler_desc.addressU = sampler_desc.addressV = RenderTextureAddressMode::CLAMP;
        sampler_desc.minFilter = sampler_desc.magFilter = RenderFilter::LINEAR;
        sampler_desc.maxLOD = 0;
        sampler = device.createSampler(sampler_desc);
        const RenderDescriptorRange texture_range(RenderDescriptorRangeType::TEXTURE, 0, 1);
        const RenderDescriptorRange sampler_range(RenderDescriptorRangeType::SAMPLER, 0, 1);
        const RenderDescriptorSetDesc sets[] = {{&texture_range, 1}, {&sampler_range, 1}};
        texture_set = device.createDescriptorSet(sets[0]);
        sampler_set = device.createDescriptorSet(sets[1]);
        RenderPipelineLayoutDesc layout_desc;
        layout_desc.descriptorSetDescs = sets;
        layout_desc.descriptorSetDescsCount = std::size(sets);
        layout = device.createPipelineLayout(layout_desc);
        Require(vs && ps && sampler && texture_set && sampler_set && layout, "Native presentation pipeline allocation failed");
        sampler_set->setSampler(0, sampler.get());
        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get();
        desc.vertexShader = vs.get();
        desc.pixelShader = ps.get();
        desc.renderTargetFormat[0] = format;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        pipeline = device.createGraphicsPipeline(desc);
        Require(bool(pipeline), "Native presentation pipeline creation failed");
    }
};
}  // namespace

static void ValidateDraw(const QuadDraw& draw) {
    const auto width = draw.width, height = draw.height, stride = draw.stride;
    Require(width && height && width <= 8192 && height <= 8192, "Invalid target dimensions");
    Require(stride == 16 || stride == 20, "Unsupported vertex stride");
    Require(draw.vertices.size() == stride * 4, "Expected four packed vertices");
    Require(draw.shared_constants.size() == 512 && draw.pixel_constants.size() == 4096,
            "Invalid native constant upload sizes");
    const uint32_t textureCount = static_cast<uint32_t>(draw.textures.size());
    Require(textureCount == 1 || textureCount == 3, "Unsupported texture count");
    for (uint32_t i = 0; i < textureCount; ++i) {
        uint32_t textureIndex, samplerIndex;
        std::memcpy(&textureIndex, draw.shared_constants.data() + i * 4, 4);
        std::memcpy(&samplerIndex, draw.shared_constants.data() + 192 + i * 4, 4);
        Require(textureIndex < textureCount && samplerIndex == 0, "Out-of-range native descriptor index");
    }
    auto validFormat = [](RenderFormat f) { return f == RenderFormat::R8G8B8A8_UNORM ||
                                                  f == RenderFormat::B8G8R8A8_UNORM; };
    Require(validFormat(draw.texture_format) && validFormat(draw.target_format), "Unsupported pixel format");
    Require(!draw.vertex_shader.empty() && !draw.pixel_shader.empty(), "Missing native shaders");
    for (const auto& t : draw.textures)
        Require(t.width && t.height && t.width <= 8192 && t.height <= 8192 &&
                t.bytes.size() == size_t(t.width) * t.height * 4, "Invalid texture extent");
}

struct QuadRenderer::Impl {
    QuadDraw configuration;
    std::shared_ptr<NativeDevice> gpu;
    const std::unique_ptr<RenderDevice>& device;
    const std::unique_ptr<RenderCommandQueue>& queue;
    const std::unique_ptr<RenderCommandList>& commands;
    const std::unique_ptr<RenderCommandFence>& fence;
    std::unique_ptr<RenderShader> vs, ps;
    std::unique_ptr<RenderBuffer> vertexBuffer, indexBuffer, sharedBuffer, psBuffer, readback;
    struct TextureUpload {
        uint32_t width, height, pitch;
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderBuffer> upload;
    };
    std::vector<TextureUpload> textures;
    std::unique_ptr<RenderTexture> target;
    std::unique_ptr<RenderSampler> sampler;
    std::unique_ptr<RenderDescriptorSet> textureSet, samplerSet;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderPipeline> pipeline;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    RenderWindow window = {};
    std::unique_ptr<RenderSwapChain> swapChain;
    std::vector<std::unique_ptr<RenderFramebuffer>> swapFramebuffers;
    std::unique_ptr<RenderBuffer> presentReadback;
    std::unique_ptr<PresentPipeline> presentPipeline;
    uint32_t presentReadWidth = 0, presentReadHeight = 0;
    RenderInputSlot inputSlot{0, 0};
    uint32_t readPitch = 0;
    const uint64_t& completed;
    const bool& debugEnabled;
    static constexpr uint16_t indices[] = {0, 1, 2, 0, 2, 3};

    Impl(const QuadDraw& draw, std::shared_ptr<NativeDevice> context) : configuration(draw),
        gpu(context ? std::move(context) : throw std::runtime_error("Missing native device")),
        device(gpu->device), queue(gpu->queue), commands(gpu->commands), fence(gpu->fence),
        completed(gpu->completed), debugEnabled(gpu->debug_enabled) {
        ValidateDraw(draw);
        // Retain only the immutable pipeline/extent description between draws.
        configuration.vertices = std::vector<uint8_t>{};
        configuration.shared_constants = std::vector<uint8_t>{};
        configuration.pixel_constants = std::vector<uint8_t>{};
        for (auto& t : configuration.textures) t.bytes = std::vector<uint8_t>{};
        const auto width = draw.width, height = draw.height, stride = draw.stride;
        const uint32_t textureCount = static_cast<uint32_t>(draw.textures.size());
        const auto& vsBytes = draw.vertex_shader;
        const auto& psBytes = draw.pixel_shader;
        const auto& vertices = draw.vertices;
        const auto& psConstants = draw.pixel_constants;
        const auto textureFormat = draw.texture_format, targetFormat = draw.target_format;
        const auto addressU = draw.address_u, addressV = draw.address_v;
        textures.reserve(textureCount);
        for (const auto& t : draw.textures)
            textures.push_back({t.width, t.height, (t.width * 4 + 255) & ~255u, {}, {}});
        vs = device->createShader(vsBytes.data(), vsBytes.size(), "main", RenderShaderFormat::DXIL);
        ps = device->createShader(psBytes.data(), psBytes.size(), "main", RenderShaderFormat::DXIL);
        Require(bool(vs) && bool(ps), "DXIL shader creation failed");
        vertexBuffer = device->createBuffer(RenderBufferDesc::VertexBuffer(vertices.size(), RenderHeapType::UPLOAD));
        indexBuffer = device->createBuffer(RenderBufferDesc::IndexBuffer(sizeof(indices), RenderHeapType::UPLOAD));
        std::memcpy(indexBuffer->map(), indices, sizeof(indices));
        indexBuffer->unmap();
        sharedBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(512));
        psBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(psConstants.size()));
        target = device->createTexture(RenderTextureDesc::ColorTarget(width, height, targetFormat));
        Require(bool(target), "Render target creation failed");
        for (auto& t : textures) {
            t.texture = device->createTexture(RenderTextureDesc::Texture2D(t.width, t.height, 1, textureFormat));
            t.upload = device->createBuffer(RenderBufferDesc::UploadBuffer(size_t(t.pitch) * t.height));
            Require(bool(t.texture) && bool(t.upload), "Texture creation failed");
        }
        readPitch = (width * 4 + 255) & ~255u;
        readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t(readPitch) * height));
        RenderSamplerDesc samplerDesc;
        samplerDesc.minFilter = samplerDesc.magFilter = draw.filter;
        samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
        samplerDesc.addressU = addressU;
        samplerDesc.addressV = addressV;
        samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
        samplerDesc.maxLOD = 0;
        sampler = device->createSampler(samplerDesc);
        RenderDescriptorRange textureRange(RenderDescriptorRangeType::TEXTURE, 0, textureCount);
        RenderDescriptorRange samplerRange(RenderDescriptorRangeType::SAMPLER, 0, 1);
        std::array<RenderDescriptorSetDesc, 4> sets{};
        sets[0] = RenderDescriptorSetDesc(&textureRange, 1, true, textureCount);
        sets[3] = RenderDescriptorSetDesc(&samplerRange, 1, true, 1);
        textureSet = device->createDescriptorSet(sets[0]);
        samplerSet = device->createDescriptorSet(sets[3]);
        for (uint32_t i = 0; i < textureCount; ++i)
            textureSet->setTexture(i, textures[i].texture.get(), RenderTextureLayout::SHADER_READ);
        samplerSet->setSampler(0, sampler.get());
        const RenderRootDescriptorDesc roots[] = {
            {2, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
            {1, 4, RenderRootDescriptorType::CONSTANT_BUFFER}
        };
        RenderPipelineLayoutDesc layoutDesc;
        layoutDesc.descriptorSetDescs = sets.data();
        layoutDesc.descriptorSetDescsCount = sets.size();
        layoutDesc.rootDescriptorDescs = roots;
        layoutDesc.rootDescriptorDescsCount = std::size(roots);
        layoutDesc.allowInputLayout = true;
        layout = device->createPipelineLayout(layoutDesc);
        Require(bool(layout), "Pipeline layout creation failed");
        inputSlot = RenderInputSlot(0, stride);
        const RenderInputElement elements[] = {
            {"POSITION", 0, 0, stride == 16 ? RenderFormat::R32G32_FLOAT : RenderFormat::R32G32B32_FLOAT, 0, 0},
            {"TEXCOORD", 0, 4, RenderFormat::R32G32_FLOAT, 0, stride - 8}
        };
        RenderGraphicsPipelineDesc pipelineDesc;
        pipelineDesc.pipelineLayout = layout.get();
        pipelineDesc.vertexShader = vs.get();
        pipelineDesc.pixelShader = ps.get();
        pipelineDesc.inputSlots = &inputSlot;
        pipelineDesc.inputSlotsCount = 1;
        pipelineDesc.inputElements = elements;
        pipelineDesc.inputElementsCount = 2;
        pipelineDesc.renderTargetFormat[0] = targetFormat;
        pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pipelineDesc.renderTargetCount = 1;
        pipelineDesc.primitiveTopology = draw.quad_list ? RenderPrimitiveTopology::TRIANGLE_LIST : RenderPrimitiveTopology::TRIANGLE_STRIP;
        pipeline = device->createGraphicsPipeline(pipelineDesc);
        Require(bool(pipeline), "Graphics pipeline creation failed");
        const RenderTexture* attachment = target.get();
        RenderFramebufferDesc framebufferDesc;
        framebufferDesc.colorAttachments = &attachment;
        framebufferDesc.colorAttachmentsCount = 1;
        framebuffer = device->createFramebuffer(framebufferDesc);
    }

    void ValidateConfiguration(const QuadDraw& draw) const {
        ValidateDraw(draw);
        const auto& c = configuration;
        Require(draw.width == c.width && draw.height == c.height && draw.stride == c.stride &&
                draw.quad_list == c.quad_list && draw.texture_format == c.texture_format &&
                draw.target_format == c.target_format && draw.filter == c.filter &&
                draw.address_u == c.address_u && draw.address_v == c.address_v &&
                draw.vertex_shader == c.vertex_shader && draw.pixel_shader == c.pixel_shader &&
                draw.textures.size() == c.textures.size(), "Draw differs from the renderer pipeline configuration");
        for (size_t i = 0; i < textures.size(); ++i)
            Require(draw.textures[i].width == textures[i].width && draw.textures[i].height == textures[i].height,
                    "Texture extent changed; create a renderer for the new layout");
    }

    void SetWindow(RenderWindow handle) {
        Require(!handle || IsWindow(handle), "Invalid native presentation window");
        if (handle == window) return;
        // Both rendering entry points return only after their GPU fence.
        swapFramebuffers.clear();
        swapChain.reset();
        presentReadback.reset();
        presentReadWidth = presentReadHeight = 0;
        window = handle;
    }

    bool PrepareWindow() {
        Require(window && IsWindow(window), "Native presentation window is not available");
        RECT client{};
        Require(GetClientRect(window, &client), "Cannot query native window size");
        if (IsIconic(window) || client.right <= 0 || client.bottom <= 0) return false;
        Require(client.right <= 8192 && client.bottom <= 8192, "Native window exceeds supported dimensions");
        if (!swapChain) {
            swapChain = queue->createSwapChain(window, 2, configuration.target_format, 1);
            Require(swapChain && !swapChain->isEmpty(), "Native swapchain creation failed");
        } else if (swapChain->needsResize()) {
            swapFramebuffers.clear();
            Require(swapChain->resize(), "Native swapchain resize failed");
        }
        if (swapFramebuffers.empty()) {
            for (uint32_t i = 0; i < swapChain->getTextureCount(); ++i) {
                const RenderTexture* attachment = swapChain->getTexture(i);
                RenderFramebufferDesc desc;
                desc.colorAttachments = &attachment;
                desc.colorAttachmentsCount = 1;
                swapFramebuffers.push_back(device->createFramebuffer(desc));
                Require(bool(swapFramebuffers.back()), "Swapchain framebuffer creation failed");
            }
        }
        return true;
    }

    RenderBuffer* PresentReadback(bool read_pixels) {
        if (!read_pixels) return nullptr;
        const auto width = swapChain->getWidth(), height = swapChain->getHeight();
        if (!presentReadback || presentReadWidth != width || presentReadHeight != height) {
            presentReadWidth = width; presentReadHeight = height;
            presentReadback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(
                size_t((width * 4 + 255) & ~255u) * height));
            Require(bool(presentReadback), "Presentation readback allocation failed");
        }
        return presentReadback.get();
    }

    uint32_t Acquire(bool vsync) {
        uint32_t index = 0;
        Require(swapChain->acquireTexture(nullptr, &index) && index < swapFramebuffers.size(),
                "Native backbuffer acquisition failed");
        swapChain->setVsyncEnabled(vsync);
        return index;
    }

    QuadResult Present(const QuadDraw& draw, bool vsync, bool read_pixels) {
        ValidateConfiguration(draw);
        if (!PrepareWindow()) return {.debug_layer = debugEnabled, .completed_submission = completed};
        auto* buffer = PresentReadback(read_pixels);
        const auto index = Acquire(vsync);
        return Draw(draw, swapChain->getTexture(index), swapFramebuffers[index].get(), buffer, true, index);
    }

    QuadResult PresentColor(RenderTexture& source, uint32_t source_width, uint32_t source_height,
                            std::span<const uint8_t> vertex, std::span<const uint8_t> pixel,
                            bool vsync, bool read_pixels) {
        if (!PrepareWindow()) return {.debug_layer = debugEnabled, .completed_submission = completed};
        if (!presentPipeline) presentPipeline = std::make_unique<PresentPipeline>(*device, configuration.target_format, vertex, pixel);
        auto* buffer = PresentReadback(read_pixels);
        const auto index = Acquire(vsync);
        auto* target = swapChain->getTexture(index);
        auto& blit = *presentPipeline;
        const auto width = swapChain->getWidth(), height = swapChain->getHeight();
        blit.texture_set->setTexture(0, &source, RenderTextureLayout::SHADER_READ);
        commands->begin();
        const RenderTextureBarrier barriers[] = {{&source, RenderTextureLayout::SHADER_READ},
                                                {target, RenderTextureLayout::COLOR_WRITE}};
        commands->barriers(RenderBarrierStage::GRAPHICS, barriers, std::size(barriers));
        commands->setFramebuffer(swapFramebuffers[index].get());
        commands->setScissors(RenderRect(0,0,width,height));
        commands->clearColor(0, RenderColor(0,0,0,1));
        commands->setViewports(AspectViewport(source_width, source_height, width, height));
        commands->setGraphicsPipelineLayout(blit.layout.get());
        commands->setPipeline(blit.pipeline.get());
        commands->setGraphicsDescriptorSet(blit.texture_set.get(), 0);
        commands->setGraphicsDescriptorSet(blit.sampler_set.get(), 1);
        commands->drawInstanced(3,1,0,0);
        return FinishDraw(target, width, height, buffer, true, index);
    }

    QuadResult Render(const QuadDraw& draw) {
        ValidateConfiguration(draw);
        return Draw(draw, target.get(), framebuffer.get(), readback.get(), false, 0);
    }

    QuadResult Draw(const QuadDraw& draw, RenderTexture* outputTarget, RenderFramebuffer* outputFramebuffer,
                    RenderBuffer* outputReadback, bool present, uint32_t backbufferIndex, bool clear_target = true) {
        // The prior Render returned only after its real GPU fence. These
        // persistent uploads and command allocator are now safe to reuse.
        std::memcpy(vertexBuffer->map(), draw.vertices.data(), draw.vertices.size());
        vertexBuffer->unmap();
        std::memcpy(sharedBuffer->map(), draw.shared_constants.data(), 512);
        sharedBuffer->unmap();
        std::memcpy(psBuffer->map(), draw.pixel_constants.data(), 4096);
        psBuffer->unmap();
        for (size_t i = 0; i < textures.size(); ++i) {
            auto& t = textures[i];
            auto pixels = static_cast<uint8_t*>(t.upload->map());
            for (uint32_t y = 0; y < t.height; ++y)
                std::memcpy(pixels + size_t(y) * t.pitch, draw.textures[i].bytes.data() + size_t(y) * t.width * 4, t.width * 4);
            t.upload->unmap();
        }
        const auto width = outputFramebuffer->getWidth(), height = outputFramebuffer->getHeight();
        const auto& vertices = draw.vertices;
        const auto textureFormat = draw.texture_format;
        commands->begin();
        for (auto& t : textures) {
            commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::COPY_DEST));
            commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get()),
                RenderTextureCopyLocation::PlacedFootprint(t.upload.get(), textureFormat, t.width, t.height, 1, t.pitch / 4));
            commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::SHADER_READ));
        }
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(outputTarget, RenderTextureLayout::COLOR_WRITE));
        commands->setFramebuffer(outputFramebuffer);
        commands->setScissors(RenderRect(0, 0, width, height));
        if (clear_target)
            commands->clearColor(0, present ? RenderColor(0, 0, 0, 1) : RenderColor(1, 0, 1, 1));
        commands->setViewports(AspectViewport(draw.width, draw.height, width, height));
        commands->setGraphicsPipelineLayout(layout.get());
        commands->setPipeline(pipeline.get());
        commands->setGraphicsDescriptorSet(textureSet.get(), 0);
        commands->setGraphicsDescriptorSet(samplerSet.get(), 3);
        commands->setGraphicsRootDescriptor(sharedBuffer.get(), 0);
        commands->setGraphicsRootDescriptor(psBuffer.get(), 1);
        RenderVertexBufferView vertexView(vertexBuffer.get(), vertices.size());
        commands->setVertexBuffers(0, &vertexView, 1, &inputSlot);
        if (draw.quad_list) {
            RenderIndexBufferView indexView(indexBuffer.get(), sizeof(indices), RenderFormat::R16_UINT);
            commands->setIndexBuffer(&indexView);
            commands->drawIndexedInstanced(6, 1, 0, 0, 0);
        } else {
            commands->drawInstanced(4, 1, 0, 0);
        }
        return FinishDraw(outputTarget, width, height, outputReadback, present, backbufferIndex);
    }

    QuadResult FinishDraw(RenderTexture* outputTarget, uint32_t width, uint32_t height,
                          RenderBuffer* outputReadback, bool present, uint32_t backbufferIndex) {
        const auto outputPitch = (width * 4 + 255) & ~255u;
        if (outputReadback) {
            commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(outputTarget, RenderTextureLayout::COPY_SOURCE));
            commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(outputReadback, configuration.target_format, width, height, 1, outputPitch / 4),
                RenderTextureCopyLocation::Subresource(outputTarget));
        }
        if (present)
            commands->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(outputTarget, RenderTextureLayout::PRESENT));
        commands->end();
        queue->executeCommandLists(commands.get(), fence.get());
        // Wait even if DXGI presentation fails, so callers can safely unwind.
        const bool presented = present && swapChain->present(backbufferIndex, nullptr, 0);
        gpu->Complete();
        Require(!present || presented, "Native presentation failed");
        QuadResult output;
        output.width = width; output.height = height;
        output.present_submitted = presented;
        output.debug_layer = debugEnabled;
        output.completed_submission = completed;
        if (outputReadback) {
            const auto result = static_cast<const uint8_t*>(outputReadback->map());
            output.pixels.resize(size_t(width) * height * 4);
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(output.pixels.data() + size_t(y) * width * 4, result + size_t(y) * outputPitch, width * 4);
            outputReadback->unmap();
        }
        return output;
    }
};

QuadRenderer::QuadRenderer(const QuadDraw& configuration, bool enable_debug)
    : QuadRenderer(configuration, std::make_shared<NativeDevice>(enable_debug)) {}
QuadRenderer::QuadRenderer(const QuadDraw& configuration, std::shared_ptr<NativeDevice> device)
    : impl_(std::make_unique<Impl>(configuration, std::move(device))) {}
QuadRenderer::~QuadRenderer() = default;
QuadResult QuadRenderer::Render(const QuadDraw& draw) { return impl_->Render(draw); }
uint64_t QuadRenderer::RenderInto(const QuadDraw& draw, ColorTarget& color) {
    impl_->ValidateConfiguration(draw);
    Require(color.Format() == draw.target_format, "Native movie and back-buffer formats differ");
    auto* texture = color.Attachment(*impl_->gpu, draw.width, draw.height, 1);
    const RenderTexture* attachment = texture;
    RenderFramebufferDesc desc;
    desc.colorAttachments = &attachment;
    desc.colorAttachmentsCount = 1;
    auto framebuffer = impl_->device->createFramebuffer(desc);
    Require(bool(framebuffer), "Native movie framebuffer allocation failed");
    color.Clear(RenderColor(0, 0, 0, 1));
    return impl_->Draw(draw, texture, framebuffer.get(), nullptr, false, 0, false).completed_submission;
}
void QuadRenderer::SetWindow(RenderWindow window) { impl_->SetWindow(window); }
QuadResult QuadRenderer::Present(const QuadDraw& draw, bool vsync, bool read_pixels) {
    return impl_->Present(draw, vsync, read_pixels);
}
QuadResult QuadRenderer::PresentColor(ColorTarget& color, uint32_t width, uint32_t height,
                                      std::span<const uint8_t> vertex, std::span<const uint8_t> pixel,
                                      bool vsync, bool read_pixels) {
    Require(color.Format() == impl_->configuration.target_format, "Native scene and swapchain color formats differ");
    auto* texture = color.Attachment(*impl_->gpu, width, height, 1);
    color.RequireCleared(RenderRect(0,0,width,height));
    return impl_->PresentColor(*texture, width, height, vertex, pixel, vsync, read_pixels);
}

QuadResult RenderQuad(const QuadDraw& draw, bool enable_debug) {
    return QuadRenderer(draw, enable_debug).Render(draw);
}
}  // namespace mc2::native
