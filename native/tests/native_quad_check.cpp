// Requires the locally translated, verified movie VS and flags-zero PS.
// No game data or shader binaries are distributed with this fixture.
#include "native_quad.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(bool(file), "Cannot open local movie DXIL");
    const auto size = file.tellg();
    Check(size > 0 && uint64_t(size) < 16 * 1024 * 1024, "Invalid DXIL size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Check(bool(file), "Short DXIL read");
    return bytes;
}
void Fill(mc2::native::QuadTexture& texture, uint8_t value) {
    for (size_t i = 0; i < texture.bytes.size(); i += 4) {
        texture.bytes[i] = texture.bytes[i+1] = texture.bytes[i+2] = value;
        texture.bytes[i+3] = 255;
    }
}
void Pixels(const mc2::native::QuadResult& result, uint64_t sequence, std::array<uint8_t, 4> expected) {
    Check(result.completed_submission == sequence, "Submission counter advanced without the expected completed draw");
    Check(result.pixels.size() == 4 * 4 * 4, "Readback extent changed");
    for (size_t i = 0; i < result.pixels.size(); ++i)
        Check(result.pixels[i] == expected[i % 4], "Persistent upload/constant/vertex/readback mismatch");
}
struct TestWindow {
    HWND handle = nullptr;
    TestWindow() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MC2NativeQuadCheck";
        Check(RegisterClassW(&windowClass) != 0, "Cannot register test window");
        // Hidden HWND: exercise DXGI without interrupting the user's desktop.
        handle = CreateWindowW(windowClass.lpszClassName, L"Native presentation check",
            WS_POPUP, 0, 0, 16, 8, nullptr, nullptr, windowClass.hInstance, nullptr);
        Check(handle != nullptr, "Cannot create test window");
    }
    ~TestWindow() { if (IsWindow(handle)) DestroyWindow(handle); }
    void Resize(int width, int height) {
        Check(SetWindowPos(handle, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER),
              "Cannot resize test window");
    }
};
void Presented(const mc2::native::QuadResult& result, uint64_t sequence, uint32_t width, uint32_t height) {
    Check(result.present_submitted && result.completed_submission == sequence, "Native present was not completed");
    Check(result.width == width && result.height == height && result.pixels.size() == size_t(width) * height * 4,
          "Presentation did not follow the window size");
    const uint32_t extent = std::min(width, height), x0 = (width - extent) / 2, y0 = (height - extent) / 2;
    const std::array<uint8_t, 4> color{155,129,203,64}, black{0,0,0,255};
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto& expected = x >= x0 && x < x0 + extent && y >= y0 && y < y0 + extent ? color : black;
            Check(std::memcmp(result.pixels.data() + (size_t(y) * width + x) * 4, expected.data(), 4) == 0,
                  "Presentation resized or letterboxed pixels incorrectly");
        }
}
}

int main(int argc, char** argv) try {
    Check(argc == 3, "Usage: mc2_native_quad_check <verified movie VS.dxil> <verified movie PS.flags-0.dxil>");
    using namespace mc2::native;
    QuadDraw draw;
    draw.width = draw.height = 4; draw.stride = 20;
    draw.vertex_shader = Read(argv[1]); draw.pixel_shader = Read(argv[2]);
    const float vertices[] = {-1,1,0,0,0, 1,1,0,1,0, -1,-1,0,0,1, 1,-1,0,1,1};
    draw.vertices.resize(sizeof(vertices));
    std::memcpy(draw.vertices.data(), vertices, sizeof(vertices));
    draw.shared_constants.resize(512);
    const uint32_t indices[] = {0, 1, 2};
    std::memcpy(draw.shared_constants.data(), indices, sizeof(indices));
    float constants[4][4] = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {1,0,0,1}};
    draw.pixel_constants.resize(4096);
    std::memcpy(draw.pixel_constants.data(), constants, sizeof(constants));
    for (uint8_t value : {16, 32, 48}) {
        QuadTexture texture{2, 2, std::vector<uint8_t>(16)};
        Fill(texture, value); draw.textures.push_back(std::move(texture));
    }
    QuadRenderer renderer(draw, true);
    Pixels(renderer.Render(draw), 1, {16,32,48,255});

    Fill(draw.textures[0], 91); Fill(draw.textures[1], 129); Fill(draw.textures[2], 203);
    constants[0][3] = .25f; constants[3][3] = .25f;
    std::memcpy(draw.pixel_constants.data(), constants, sizeof(constants));
    Pixels(renderer.Render(draw), 2, {155,129,203,64});

    // Move every vertex offscreen: the next frame must contain only the clear.
    for (size_t i = 0; i < draw.vertices.size(); i += 20) {
        float x; std::memcpy(&x, draw.vertices.data() + i, 4); x += 3;
        std::memcpy(draw.vertices.data() + i, &x, 4);
    }
    Pixels(renderer.Render(draw), 3, {255,0,255,255});

    draw.shared_constants[0] = 99;
    bool rejected = false;
    try { renderer.Render(draw); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "Out-of-range descriptor was accepted");
    draw.shared_constants[0] = 0;
    draw.textures[0].bytes.pop_back();
    rejected = false;
    try { renderer.Render(draw); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "Truncated upload was accepted");
    draw.textures[0].bytes.push_back(255);
    std::memcpy(draw.vertices.data(), vertices, sizeof(vertices));
    Pixels(renderer.Render(draw), 4, {155,129,203,64});
    TestWindow window;
    renderer.SetWindow(window.handle);
    Presented(renderer.Present(draw, false, true), 5, 16, 8);
    window.Resize(8, 16);
    Presented(renderer.Present(draw, true, true), 6, 8, 16);
    window.Resize(0, 0);
    auto skipped = renderer.Present(draw, false);
    Check(!skipped.present_submitted && skipped.pixels.empty() && skipped.completed_submission == 6,
          "Zero-size window consumed a submission");
    window.Resize(13, 9);
    Presented(renderer.Present(draw, false, true), 7, 13, 9);
    const auto noReadback = renderer.Present(draw, false);
    Check(noReadback.present_submitted && noReadback.pixels.empty() && noReadback.completed_submission == 8,
          "Normal presentation copied pixels to the CPU");
    renderer.SetWindow(nullptr);
    Pixels(renderer.Render(draw), 9, {155,129,203,64});
    renderer.SetWindow(window.handle);
    Presented(renderer.Present(draw, false, true), 10, 13, 9);
    renderer.SetWindow(nullptr);
    std::cout << "PASS: persistent uploads, rejection, ten fenced draws, swapchain resize, letterboxing, zero-size suspension, presentation without readback, and detach/reattach\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Native quad check: " << error.what() << '\n';
    return 1;
}
