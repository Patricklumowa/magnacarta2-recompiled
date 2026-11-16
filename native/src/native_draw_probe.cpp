// Offline, single-draw D3D12 experiment. Inputs are local capture artifacts.
// This executable is not a game backend and makes no visual-equivalence claim.
#include "native_quad.h"
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
struct ProbeWindow {
    HWND handle = nullptr;
    ~ProbeWindow() { if (IsWindow(handle)) DestroyWindow(handle); }
    void Open(uint32_t width, uint32_t height) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MC2NativeDrawProbe";
        Require(RegisterClassW(&windowClass) != 0, "Cannot register presentation window");
        RECT bounds{0, 0, LONG(width), LONG(height)};
        Require(AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE), "Cannot calculate window bounds");
        handle = CreateWindowW(windowClass.lpszClassName, L"Magna Carta 2 - native draw validation",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
            bounds.bottom - bounds.top, nullptr, nullptr, windowClass.hInstance, nullptr);
        Require(handle != nullptr, "Cannot create presentation window");
        ShowWindow(handle, SW_SHOWNOACTIVATE);
    }
    bool Pump() const {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return IsWindow(handle);
    }
};
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
            "Optional: --present-frames N --window-width N --window-height N\n"
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
    const auto presentFrames = args.count("--present-frames") ? number("--present-frames") : 0;
    const auto windowWidth = args.count("--window-width") ? number("--window-width") : width;
    const auto windowHeight = args.count("--window-height") ? number("--window-height") : height;
    Require(args.empty(), "Unknown argument: " + (args.empty() ? std::string() : args.begin()->first));

    mc2::native::QuadDraw draw;
    draw.width = width; draw.height = height; draw.stride = stride;
    draw.quad_list = topology == "quad-list";
    draw.texture_format = textureFormat; draw.target_format = targetFormat;
    draw.filter = filter == "point" ? RenderFilter::NEAREST : RenderFilter::LINEAR;
    draw.address_u = addressU; draw.address_v = addressV;
    draw.vertex_shader = vsBytes; draw.pixel_shader = psBytes;
    draw.vertices = std::move(vertices); draw.shared_constants = sharedBytes;
    draw.pixel_constants = std::move(psConstants);
    for (auto& t : textures) draw.textures.push_back({t.width, t.height, std::move(t.bytes)});
    ProbeWindow window;
    mc2::native::QuadRenderer renderer(draw, true);
    mc2::native::QuadResult rendered;
    uint32_t presents = 0;
    if (presentFrames) {
        window.Open(windowWidth, windowHeight);
        renderer.SetWindow(window.handle);
        for (uint32_t frame = 0; frame < presentFrames && window.Pump(); ++frame) {
            auto result = renderer.Present(draw, true, presents == 0);
            if (result.present_submitted) {
                if (presents++ == 0) rendered = std::move(result);
            } else {
                // A minimized probe must still pump messages and terminate.
                Sleep(16);
            }
        }
        renderer.SetWindow(nullptr);
        Require(presents != 0, "No native frame was presented");
    } else {
        rendered = renderer.Render(draw);
    }
    const auto* result = rendered.pixels.data();
    const uint32_t outputWidth = rendered.width, outputHeight = rendered.height;
    const uint32_t readPitch = outputWidth * 4;
    const bool debugEnabled = rendered.debug_layer;
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::binary);
    Require(bool(file), "Cannot create output file");
    size_t changed = 0;
    for (uint32_t y = 0; y < outputHeight; y++) {
        const auto row = result + size_t(y) * readPitch;
        file.write(reinterpret_cast<const char*>(row), outputWidth * 4);
        for (uint32_t x = 0; x < outputWidth; x++)
            changed += row[x * 4] != 255 || row[x * 4 + 1] != 0 || row[x * 4 + 2] != 255 || row[x * 4 + 3] != 255;
    }
    Require(bool(file), "Failed writing readback");
    Require(changed > 0, "No pixels differ from the magenta clear; draw proof failed");
    std::cout << "label=" << label << " width=" << outputWidth << " height=" << outputHeight << " format=" << targetFormatText
              << " changed_pixels=" << changed << " debug_layer=" << (debugEnabled ? "enabled" : "unavailable")
              << " presents_submitted=" << presents
              << " visual_equivalence=unverified\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Native draw probe failed: " << error.what() << '\n';
    return 1;
}
