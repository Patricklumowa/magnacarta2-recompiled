// Offline, single-draw D3D12 experiment. Inputs are local capture artifacts.
// This executable is not a game backend and makes no visual-equivalence claim.
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

using namespace plume;
namespace {
void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
std::vector<uint8_t> Read(const std::string& name, size_t limit) {
    std::ifstream file(std::filesystem::u8path(name), std::ios::binary | std::ios::ate);
    Require(bool(file), "Cannot read " + name);
    auto size = file.tellg();
    Require(size > 0 && uint64_t(size) <= limit, "Invalid file size: " + name);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Require(bool(file), "Short read: " + name);
    return bytes;
}
RenderFormat Format(const std::string& value) {
    if (value == "rgba8-unorm") return RenderFormat::R8G8B8A8_UNORM;
    if (value == "bgra8-unorm") return RenderFormat::B8G8R8A8_UNORM;
    throw std::runtime_error("Unsupported format: " + value);
}
RenderTextureAddressMode Address(const std::string& value) {
    if (value == "clamp") return RenderTextureAddressMode::CLAMP;
    if (value == "wrap") return RenderTextureAddressMode::WRAP;
    throw std::runtime_error("Unsupported texture address mode: " + value);
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
}

int main(int argc, char** argv) try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout <<
            "Offline native quad draw probe; input validation does not establish game-state equivalence.\n"
            "Required --key value pairs:\n"
            "  --label synthetic|captured --vs file.dxil --ps file.dxil\n"
            "  --vertices file.bin --vertex-endian big|little --shared file.bin\n"
            "  --vertex-layout xy-uv|xyz-uv --position-space clip|pixel\n"
            "  --texture-count 1|3 [--ps-constants file.bin]\n"
            "  --texture file.bin --texture-width N --texture-height N\n"
            "  --texture-format rgba8-unorm|bgra8-unorm\n"
            "  With three textures: --texture1 file --texture1-width N --texture1-height N (and texture2)\n"
            "  --width N --height N --target-format rgba8-unorm|bgra8-unorm\n"
            "  --filter point|linear --address-u clamp|wrap --address-v clamp|wrap\n"
            "  --topology quad-list|triangle-strip --output file.bin\n"
            "  --depth off --blend copy --cull none --alpha-test off\n"
            "  --samples 1 --viewport full --scissor full\n"
            "Vertices: four packed float position/uv records. Pixel space requires a verified passthrough VS.\n"
            "Shared constants: 512 host-endian bytes. Optional PS constants: 4096 host-endian bytes.\n"
            "Texture must already be decoded; gamma/half-pixel state must be established by capture analysis.\n"
            "Output is tight raw four-byte pixels in the requested target format.\n";
        return 0;
    }
    std::map<std::string, std::string> args;
    Require(argc > 1 && argc % 2 == 1, "Expected explicit --key value pairs; use --help");
    for (int i = 1; i < argc; i += 2)
        Require(args.emplace(argv[i], argv[i + 1]).second, "Duplicate argument");
    auto get = [&](const char* key) {
        auto found = args.find(key);
        Require(found != args.end(), std::string("Missing explicit parameter: ") + key);
        auto value = found->second;
        args.erase(found);
        return value;
    };
    auto number = [&](const char* key) {
        auto text = get(key);
        size_t consumed = 0;
        auto value = std::stoul(text, &consumed);
        Require(consumed == text.size() && value > 0 && value <= 8192, std::string("Invalid dimension: ") + key);
        return static_cast<uint32_t>(value);
    };
    const auto label = get("--label");
    Require(label == "synthetic" || label == "captured", "--label must distinguish synthetic and captured inputs");
    const auto vsBytes = Read(get("--vs"), 16 * 1024 * 1024);
    const auto psBytes = Read(get("--ps"), 16 * 1024 * 1024);
    const auto vertexLayout = get("--vertex-layout");
    Require(vertexLayout == "xy-uv" || vertexLayout == "xyz-uv", "Unsupported vertex layout");
    const uint32_t stride = vertexLayout == "xy-uv" ? 16 : 20;
    auto vertices = Read(get("--vertices"), 80);
    Require(vertices.size() == stride * 4, "Expected four tightly packed vertices in the specified layout");
    const auto endian = get("--vertex-endian");
    Require(endian == "big" || endian == "little", "Unknown vertex byte order");
    if (endian == "big")
        for (size_t i = 0; i < vertices.size(); i += 4) std::reverse(vertices.begin() + i, vertices.begin() + i + 4);
    for (size_t i = 0; i < vertices.size(); i += 4) {
        float value;
        std::memcpy(&value, vertices.data() + i, 4);
        Require(std::isfinite(value), "Vertex contains a non-finite float");
    }
    const uint32_t width = number("--width"), height = number("--height");
    const auto positionSpace = get("--position-space");
    Require(positionSpace == "clip" || positionSpace == "pixel", "Unsupported position space");
    if (positionSpace == "pixel") {
        // The captured movie VS passes position through. Xenos disables its
        // viewport scale/offset; convert screen XY to the host's clip space.
        // This conversion is not valid for a VS that transforms positions.
        for (size_t i = 0; i < vertices.size(); i += stride) {
            float xy[2];
            std::memcpy(xy, vertices.data() + i, sizeof(xy));
            xy[0] = 2 * xy[0] / width - 1;
            xy[1] = 1 - 2 * xy[1] / height;
            std::memcpy(vertices.data() + i, xy, sizeof(xy));
        }
    }
    const uint32_t textureCount = number("--texture-count");
    Require(textureCount == 1 || textureCount == 3, "Only one or three textures are supported");
    const auto textureFormat = Format(get("--texture-format"));
    const auto targetFormatText = get("--target-format");
    const auto targetFormat = Format(targetFormatText);
    struct TextureInput {
        uint32_t width, height, pitch;
        std::vector<uint8_t> bytes;
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderBuffer> upload;
    };
    std::vector<TextureInput> textures(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i) {
        const auto prefix = std::string("--texture") + (i ? std::to_string(i) : "");
        auto& t = textures[i];
        t.width = number((prefix + "-width").c_str());
        t.height = number((prefix + "-height").c_str());
        t.pitch = (t.width * 4 + 255) & ~255u;
        t.bytes = Read(get(prefix.c_str()), 256 * 1024 * 1024);
        Require(t.bytes.size() == size_t(t.width) * t.height * 4,
                "Texture must be tightly packed, decoded four-byte pixels");
    }
    const auto sharedBytes = Read(get("--shared"), 512);
    Require(sharedBytes.size() == 512, "Shared constants must be a 512-byte little-endian upload, with captured half-pixel state");
    for (uint32_t i = 0; i < textureCount; ++i) {
        uint32_t textureIndex, samplerIndex;
        std::memcpy(&textureIndex, sharedBytes.data() + i * 4, 4);
        std::memcpy(&samplerIndex, sharedBytes.data() + 192 + i * 4, 4);
        Require(textureIndex == i && samplerIndex == 0, "Probe binds sequential textures with sampler zero");
    }
    std::vector<uint8_t> psConstants(4096);
    Require(textureCount != 3 || args.count("--ps-constants"),
            "The three-plane movie draw requires captured pixel constants");
    if (args.count("--ps-constants")) {
        psConstants = Read(get("--ps-constants"), 4096);
        Require(psConstants.size() == 4096, "Expected 256 little-endian pixel float4 constants");
    }
    const auto filter = get("--filter");
    Require(filter == "point" || filter == "linear", "Only explicit point/linear filtering is supported");
    const auto addressU = Address(get("--address-u")), addressV = Address(get("--address-v"));
    const auto topology = get("--topology");
    Require(topology == "quad-list" || topology == "triangle-strip", "Unsupported primitive topology");
    Require(get("--depth") == "off" && get("--blend") == "copy" && get("--cull") == "none" &&
            get("--alpha-test") == "off" && get("--samples") == "1" && get("--viewport") == "full" &&
            get("--scissor") == "full", "Probe supports only verified opaque, depth-disabled, unculled, single-sample full-viewport draws");
    const auto output = std::filesystem::u8path(get("--output"));
    Require(args.empty(), "Unknown argument: " + (args.empty() ? std::string() : args.begin()->first));

    std::cerr << "Creating D3D12 device\n";
    ID3D12Debug* debug = nullptr;
    bool debugEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
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
    std::cerr << "Creating shaders and buffers\n";
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
    std::cerr << "Creating texture and readback resources\n";
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
    samplerDesc.minFilter = samplerDesc.magFilter = filter == "point" ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
    samplerDesc.addressU = addressU;
    samplerDesc.addressV = addressV;
    samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    samplerDesc.maxLOD = 0;
    auto sampler = device->createSampler(samplerDesc);
    std::cerr << "Creating descriptors\n";
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
    std::cerr << "Creating pipeline layout\n";
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
    pipelineDesc.primitiveTopology = topology == "quad-list" ? RenderPrimitiveTopology::TRIANGLE_LIST : RenderPrimitiveTopology::TRIANGLE_STRIP;
    std::cerr << "Creating graphics pipeline\n";
    auto pipeline = device->createGraphicsPipeline(pipelineDesc);
    Require(bool(pipeline), "Graphics pipeline creation failed");
    const RenderTexture* attachment = target.get();
    RenderFramebufferDesc framebufferDesc;
    framebufferDesc.colorAttachments = &attachment;
    framebufferDesc.colorAttachmentsCount = 1;
    auto framebuffer = device->createFramebuffer(framebufferDesc);
    std::cerr << "Recording native draw\n";
    commands->begin();
    for (auto& t : textures) {
        commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::COPY_DEST));
        commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get()),
            RenderTextureCopyLocation::PlacedFootprint(t.upload.get(), textureFormat, t.width, t.height, 1, t.pitch / 4));
        commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::SHADER_READ));
    }
    std::cerr << "Texture upload recorded\n";
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer.get());
    commands->setViewports(RenderViewport(0, 0, float(width), float(height)));
    commands->setScissors(RenderRect(0, 0, width, height));
    commands->clearColor(0, RenderColor(1, 0, 1, 1));
    std::cerr << "Target clear recorded\n";
    commands->setGraphicsPipelineLayout(layout.get());
    commands->setPipeline(pipeline.get());
    std::cerr << "Pipeline bound\n";
    commands->setGraphicsDescriptorSet(textureSet.get(), 0);
    commands->setGraphicsDescriptorSet(samplerSet.get(), 3);
    commands->setGraphicsRootDescriptor(sharedBuffer.get(), 0);
    commands->setGraphicsRootDescriptor(psBuffer.get(), 1);
    std::cerr << "Descriptors bound\n";
    RenderVertexBufferView vertexView(vertexBuffer.get(), vertices.size());
    commands->setVertexBuffers(0, &vertexView, 1, &inputSlot);
    if (topology == "quad-list") {
        RenderIndexBufferView indexView(indexBuffer.get(), sizeof(indices), RenderFormat::R16_UINT);
        commands->setIndexBuffer(&indexView);
        commands->drawIndexedInstanced(6, 1, 0, 0, 0);
    } else {
        commands->drawInstanced(4, 1, 0, 0);
    }
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    std::cerr << "Draw recorded\n";
    commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), targetFormat, width, height, 1, readPitch / 4),
        RenderTextureCopyLocation::Subresource(target.get()));
    commands->end();
    std::cerr << "Submitting native draw\n";
    queue->executeCommandLists(commands.get(), fence.get());
    queue->waitForCommandFence(fence.get());
    Require(!CheckGpuErrors(static_cast<D3D12Device*>(device.get())->d3d), "D3D12 validation reported an error");
    std::cerr << "Reading rendered pixels\n";
    const auto result = static_cast<const uint8_t*>(readback->map());
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::binary);
    Require(bool(file), "Cannot create output file");
    size_t changed = 0;
    for (uint32_t y = 0; y < height; y++) {
        const auto row = result + size_t(y) * readPitch;
        file.write(reinterpret_cast<const char*>(row), width * 4);
        for (uint32_t x = 0; x < width; x++)
            changed += row[x * 4] != 255 || row[x * 4 + 1] != 0 || row[x * 4 + 2] != 255 || row[x * 4 + 3] != 255;
    }
    readback->unmap();
    Require(bool(file), "Failed writing readback");
    Require(changed > 0, "No pixels differ from the magenta clear; draw proof failed");
    std::cout << "label=" << label << " width=" << width << " height=" << height << " format=" << targetFormatText
              << " changed_pixels=" << changed << " debug_layer=" << (debugEnabled ? "enabled" : "unavailable")
              << " visual_equivalence=unverified\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Native draw probe failed: " << error.what() << '\n';
    return 1;
}
