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
    std::cout << "PASS: four fenced draws reuse the device, pipeline and resources; texture, constant and vertex updates render correctly; rejected inputs do not consume a submission\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Native quad check: " << error.what() << '\n';
    return 1;
}
