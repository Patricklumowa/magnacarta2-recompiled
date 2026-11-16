#include "native_quad.h"
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <array>
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
    messages->Release();
    return errors;
}
}  // namespace

QuadResult RenderQuad(const QuadDraw& draw, bool enable_debug) {
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
    const auto& vsBytes = draw.vertex_shader;
    const auto& psBytes = draw.pixel_shader;
    const auto& vertices = draw.vertices;
    const auto& sharedBytes = draw.shared_constants;
    const auto& psConstants = draw.pixel_constants;
    const auto textureFormat = draw.texture_format, targetFormat = draw.target_format;
    const auto addressU = draw.address_u, addressV = draw.address_v;
    struct TextureUpload {
        uint32_t width, height, pitch;
        const std::vector<uint8_t>& bytes;
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderBuffer> upload;
    };
    std::vector<TextureUpload> textures;
    textures.reserve(textureCount);
    for (const auto& t : draw.textures) {
        Require(t.width && t.height && t.width <= 8192 && t.height <= 8192 &&
                t.bytes.size() == size_t(t.width) * t.height * 4, "Invalid texture extent");
        textures.push_back({t.width, t.height, (t.width * 4 + 255) & ~255u, t.bytes, {}, {}});
    }
    ID3D12Debug* debug = nullptr;
    bool debugEnabled = enable_debug && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugEnabled) {
        debug->EnableDebugLayer();
        debug->Release();
    }
    auto renderInterface = std::make_unique<D3D12Interface>();
    auto device = renderInterface->createDevice("");
    Require(bool(device), "D3D12 device creation failed");
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    auto vs = device->createShader(vsBytes.data(), vsBytes.size(), "main", RenderShaderFormat::DXIL);
    auto ps = device->createShader(psBytes.data(), psBytes.size(), "main", RenderShaderFormat::DXIL);
    Require(bool(vs) && bool(ps), "DXIL shader creation failed");
    auto vertexBuffer = device->createBuffer(RenderBufferDesc::VertexBuffer(vertices.size(), RenderHeapType::UPLOAD));
    std::memcpy(vertexBuffer->map(), vertices.data(), vertices.size());
    vertexBuffer->unmap();
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    auto indexBuffer = device->createBuffer(RenderBufferDesc::IndexBuffer(sizeof(indices), RenderHeapType::UPLOAD));
    std::memcpy(indexBuffer->map(), indices, sizeof(indices));
    indexBuffer->unmap();
    auto sharedBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(512));
    std::memcpy(sharedBuffer->map(), sharedBytes.data(), 512);
    sharedBuffer->unmap();
    auto psBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(psConstants.size()));
    std::memcpy(psBuffer->map(), psConstants.data(), psConstants.size());
    psBuffer->unmap();
    auto target = device->createTexture(RenderTextureDesc::ColorTarget(width, height, targetFormat));
    Require(bool(target), "Render target creation failed");
    for (auto& t : textures) {
        t.texture = device->createTexture(RenderTextureDesc::Texture2D(t.width, t.height, 1, textureFormat));
        t.upload = device->createBuffer(RenderBufferDesc::UploadBuffer(size_t(t.pitch) * t.height));
        Require(bool(t.texture) && bool(t.upload), "Texture creation failed");
        auto pixels = static_cast<uint8_t*>(t.upload->map());
        for (uint32_t y = 0; y < t.height; ++y)
            std::memcpy(pixels + size_t(y) * t.pitch, t.bytes.data() + size_t(y) * t.width * 4, t.width * 4);
        t.upload->unmap();
    }
    const uint32_t readPitch = (width * 4 + 255) & ~255u;
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(size_t(readPitch) * height));
    RenderSamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = draw.filter;
    samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
    samplerDesc.addressU = addressU;
    samplerDesc.addressV = addressV;
    samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    samplerDesc.maxLOD = 0;
    auto sampler = device->createSampler(samplerDesc);
    RenderDescriptorRange textureRange(RenderDescriptorRangeType::TEXTURE, 0, textureCount);
    RenderDescriptorRange samplerRange(RenderDescriptorRangeType::SAMPLER, 0, 1);
    std::array<RenderDescriptorSetDesc, 4> sets{};
    sets[0] = RenderDescriptorSetDesc(&textureRange, 1, true, textureCount);
    sets[3] = RenderDescriptorSetDesc(&samplerRange, 1, true, 1);
    auto textureSet = device->createDescriptorSet(sets[0]);
    auto samplerSet = device->createDescriptorSet(sets[3]);
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
    auto layout = device->createPipelineLayout(layoutDesc);
    Require(bool(layout), "Pipeline layout creation failed");
    RenderInputSlot inputSlot(0, stride);
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
    auto pipeline = device->createGraphicsPipeline(pipelineDesc);
    Require(bool(pipeline), "Graphics pipeline creation failed");
    const RenderTexture* attachment = target.get();
    RenderFramebufferDesc framebufferDesc;
    framebufferDesc.colorAttachments = &attachment;
    framebufferDesc.colorAttachmentsCount = 1;
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    commands->begin();
    for (auto& t : textures) {
        commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::COPY_DEST));
        commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get()),
            RenderTextureCopyLocation::PlacedFootprint(t.upload.get(), textureFormat, t.width, t.height, 1, t.pitch / 4));
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::SHADER_READ));
    }
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer.get());
    commands->setViewports(RenderViewport(0, 0, float(width), float(height)));
    commands->setScissors(RenderRect(0, 0, width, height));
    commands->clearColor(0, RenderColor(1, 0, 1, 1));
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
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), targetFormat, width, height, 1, readPitch / 4),
        RenderTextureCopyLocation::Subresource(target.get()));
    commands->end();
    queue->executeCommandLists(commands.get(), fence.get());
    queue->waitForCommandFence(fence.get());
    Require(!CheckGpuErrors(static_cast<D3D12Device*>(device.get())->d3d), "D3D12 validation reported an error");
    const auto result = static_cast<const uint8_t*>(readback->map());
    QuadResult output;
    output.debug_layer = debugEnabled;
    output.pixels.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(output.pixels.data() + size_t(y) * width * 4, result + size_t(y) * readPitch, width * 4);
    readback->unmap();
    return output;
}
}  // namespace mc2::native
