#include "native_quad.h"
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
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
bool CheckGpuErrors(ID3D12Device* device) {
    ID3D12InfoQueue* messages = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&messages)))) return false;
    bool errors = false;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); i++) {
        SIZE_T size = 0;
        if (FAILED(messages->GetMessage(i, nullptr, &size))) continue;
        std::vector<uint8_t> storage(size);
        auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(messages->GetMessage(i, message, &size))) continue;
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            std::cerr << "D3D12: " << message->pDescription << '\n';
        errors |= message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
    }
    messages->ClearStoredMessages();
    messages->Release();
    return errors;
}
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
    std::unique_ptr<D3D12Interface> renderInterface;
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> commands;
    std::unique_ptr<RenderCommandFence> fence;
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
    uint32_t presentReadWidth = 0, presentReadHeight = 0;
    RenderInputSlot inputSlot{0, 0};
    uint32_t readPitch = 0;
    uint64_t completed = 0;
    bool debugEnabled = false;
    static constexpr uint16_t indices[] = {0, 1, 2, 0, 2, 3};

    Impl(const QuadDraw& draw, bool enable_debug) : configuration(draw) {
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
        ID3D12Debug* debug = nullptr;
        debugEnabled = enable_debug && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (debugEnabled) {
            debug->EnableDebugLayer();
            debug->Release();
        }
        renderInterface = std::make_unique<D3D12Interface>();
        device = renderInterface->createDevice("");
        Require(bool(device), "D3D12 device creation failed");
        queue = device->createCommandQueue(RenderCommandListType::DIRECT);
        commands = queue->createCommandList();
        fence = device->createCommandFence();
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

    QuadResult Present(const QuadDraw& draw, bool vsync, bool read_pixels) {
        ValidateConfiguration(draw);
        Require(window && IsWindow(window), "Native presentation window is not available");
        RECT client{};
        Require(GetClientRect(window, &client), "Cannot query native window size");
        if (IsIconic(window) || client.right <= 0 || client.bottom <= 0) {
            QuadResult skipped;
            skipped.completed_submission = completed;
            skipped.debug_layer = debugEnabled;
            return skipped;
        }
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
        const auto width = swapChain->getWidth(), height = swapChain->getHeight();
        if (read_pixels && (!presentReadback || presentReadWidth != width || presentReadHeight != height)) {
            presentReadWidth = width; presentReadHeight = height;
            presentReadback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(
                size_t((width * 4 + 255) & ~255u) * height));
            Require(bool(presentReadback), "Presentation readback allocation failed");
        }
        uint32_t index = 0;
        Require(swapChain->acquireTexture(nullptr, &index) && index < swapFramebuffers.size(),
                "Native backbuffer acquisition failed");
        swapChain->setVsyncEnabled(vsync);
        return Draw(draw, swapChain->getTexture(index), swapFramebuffers[index].get(),
                    read_pixels ? presentReadback.get() : nullptr, true, index);
    }

    QuadResult Render(const QuadDraw& draw) {
        ValidateConfiguration(draw);
        return Draw(draw, target.get(), framebuffer.get(), readback.get(), false, 0);
    }

    QuadResult Draw(const QuadDraw& draw, RenderTexture* outputTarget, RenderFramebuffer* outputFramebuffer,
                    RenderBuffer* outputReadback, bool present, uint32_t backbufferIndex) {
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
        const auto outputPitch = (width * 4 + 255) & ~255u;
        const auto& vertices = draw.vertices;
        const auto textureFormat = draw.texture_format, targetFormat = draw.target_format;
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
        commands->clearColor(0, present ? RenderColor(0, 0, 0, 1) : RenderColor(1, 0, 1, 1));
        const double scale = std::min(double(width) / draw.width, double(height) / draw.height);
        const auto viewWidth = std::min(width, uint32_t(std::lround(draw.width * scale)));
        const auto viewHeight = std::min(height, uint32_t(std::lround(draw.height * scale)));
        commands->setViewports(RenderViewport(float((width - viewWidth) / 2), float((height - viewHeight) / 2),
                                             float(viewWidth), float(viewHeight)));
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
        if (outputReadback) {
            commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(outputTarget, RenderTextureLayout::COPY_SOURCE));
            commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(outputReadback, targetFormat, width, height, 1, outputPitch / 4),
                RenderTextureCopyLocation::Subresource(outputTarget));
        }
        if (present)
            commands->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(outputTarget, RenderTextureLayout::PRESENT));
        commands->end();
        queue->executeCommandLists(commands.get(), fence.get());
        // Wait even if DXGI presentation fails, so callers can safely unwind.
        const bool presented = present && swapChain->present(backbufferIndex, nullptr, 0);
        queue->waitForCommandFence(fence.get());
        Require(!CheckGpuErrors(static_cast<D3D12Device*>(device.get())->d3d), "D3D12 validation reported an error");
        Require(!present || presented, "Native presentation failed");
        QuadResult output;
        output.width = width; output.height = height;
        output.present_submitted = presented;
        output.debug_layer = debugEnabled;
        output.completed_submission = ++completed;
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
    : impl_(std::make_unique<Impl>(configuration, enable_debug)) {}
QuadRenderer::~QuadRenderer() = default;
QuadResult QuadRenderer::Render(const QuadDraw& draw) { return impl_->Render(draw); }
void QuadRenderer::SetWindow(RenderWindow window) { impl_->SetWindow(window); }
QuadResult QuadRenderer::Present(const QuadDraw& draw, bool vsync, bool read_pixels) {
    return impl_->Present(draw, vsync, read_pixels);
}

QuadResult RenderQuad(const QuadDraw& draw, bool enable_debug) {
    return QuadRenderer(draw, enable_debug).Render(draw);
}
}  // namespace mc2::native
