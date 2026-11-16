// Requires the locally captured first depth VS and its translated DXIL. The
// synthetic geometry checks indexed addressing, reversed depth, retention,
// scissoring and rejection. Optional capture replay writes actual depth pixels.
#include "native_depth.h"
#include "shader_capture.h"
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(bool(file), "Cannot open local depth fixture");
    const auto size = file.tellg();
    Check(size > 0 && uint64_t(size) <= 16 * 1024 * 1024, "Invalid depth fixture size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Check(bool(file), "Short depth fixture read"); return bytes;
}
template<class F> void Reject(F&& function) {
    bool rejected = false;
    try { function(); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "Malformed native input was accepted");
}
void Pixels(const mc2::native::DepthResult& result, float expected, uint64_t sequence) {
    Check(result.completed_submission == sequence && result.pixels.size() == 16, "Depth submission or extent differs");
    for (float depth : result.pixels) Check(std::isfinite(depth) && std::abs(depth - expected) < 1e-6f, "Unexpected depth value");
}
}
int main(int argc, char** argv) try {
    Check(argc == 3 || argc == 5, "Usage: mc2_native_depth_check <VS.container> <VS.dxil> [capture-directory output.f32]");
    using namespace mc2::native;
    auto container = Read(argv[1]);
    const auto inputs = ReadVertexInputs(container);
    Check(inputs.size() == 3 && inputs[0].constant_only && inputs[0].usage == 0 &&
          inputs[1].usage == 10 && inputs[1].offset == 0 && inputs[1].components == 3 &&
          inputs[1].stride == 40 && inputs[2].usage == 5 && inputs[2].offset == 24 && inputs[2].components == 2 &&
          inputs[1].fetch_index == inputs[0].fetch_index && inputs[2].fetch_index == inputs[0].fetch_index,
          "First depth mini-fetch layout differs");
    Reject([&] { ReadVertexInputs(std::span(container).first(35)); });
    auto damaged = container; damaged[4] = 255;
    Reject([&] { ReadVertexInputs(damaged); });
    damaged = container;
    const auto shader = mc2::capture::LoadBE32(damaged.data() + 24);
    damaged[shader + 24] = 255;
    Reject([&] { ReadVertexInputs(damaged); });
    const std::array<uint8_t, 4> endian_test{1,2,3,4};
    Check(NativeVertexBytes(endian_test, 0) == std::vector<uint8_t>({1,2,3,4}) &&
          NativeVertexBytes(endian_test, 1) == std::vector<uint8_t>({2,1,4,3}) &&
          NativeVertexBytes(endian_test, 2) == std::vector<uint8_t>({4,3,2,1}) &&
          NativeVertexBytes(endian_test, 3) == std::vector<uint8_t>({3,4,1,2}), "Vertex byte order differs");
    auto gpu = std::make_shared<NativeDevice>(true);
    DepthConfiguration config;
    config.width = config.height = 4; config.vertex_shader = Read(argv[2]); config.inputs = inputs;
    DepthRenderer renderer(config, gpu);
    auto missing_signature = config;
    missing_signature.inputs.erase(missing_signature.inputs.begin());
    Reject([&] { DepthRenderer invalid(missing_signature, gpu); });
    // That deliberate invalid PSO may leave expected debug-layer messages.
    // Consume only this test's validation error before checking real draws.
    ID3D12InfoQueue* messages = nullptr;
    if (SUCCEEDED(static_cast<plume::D3D12Device*>(gpu->device.get())->d3d->QueryInterface(IID_PPV_ARGS(&messages)))) {
        messages->ClearStoredMessages(); messages->Release();
    }
    DepthDraw draw;
    draw.vertex_constants.resize(4096); draw.shared_constants.resize(512);
    float constants[9][4]{};
    for (int i = 0; i < 4; ++i) constants[i][i] = constants[5+i][i] = 1;
    std::memcpy(draw.vertex_constants.data(), constants, sizeof(constants));
    draw.vertices.resize(160);
    const float positions[][3] = {{-1,1,.25f},{1,1,.25f},{1,-1,.25f},{-1,-1,.25f}};
    for (size_t i = 0; i < 4; ++i) std::memcpy(draw.vertices.data() + i * 40, positions[i], 12);
    draw.indices = {999,999,999,0,1,2,0,2,3}; draw.first_index = 3; draw.index_count = 6;
    draw.viewport = plume::RenderViewport(0,0,4,4,1,0); draw.scissor = plume::RenderRect(0,0,4,4);
    Reject([&] { renderer.Render(draw); });
    renderer.Clear(0);
    Pixels(renderer.Render(draw, true), .75f, 2);
    const auto set_depth = [&](float z) {
        for (size_t i = 0; i < 4; ++i) std::memcpy(draw.vertices.data() + i * 40 + 8, &z, 4);
    };
    set_depth(.5f); Pixels(renderer.Render(draw, true), .75f, 3);
    set_depth(.125f); Pixels(renderer.Render(draw, true), .875f, 4);
    renderer.Clear(1); Pixels(renderer.Render(draw, true), 1, 6);
    renderer.Clear(0); draw.scissor.right = 2;
    const auto partial = renderer.Render(draw, true);
    Check(partial.completed_submission == 8, "Scissor draw submission differs");
    for (size_t i = 0; i < partial.pixels.size(); ++i)
        Check(std::abs(partial.pixels[i] - (i % 4 < 2 ? .875f : 0)) < 1e-6f, "Depth scissor was ignored");
    draw.first_index = UINT32_MAX; Reject([&] { renderer.Render(draw); }); draw.first_index = 3;
    draw.indices[3] = 4; Reject([&] { renderer.Render(draw); }); draw.indices[3] = 0;
    draw.vertex_constants.pop_back(); Reject([&] { renderer.Render(draw); }); draw.vertex_constants.push_back(0);
    const auto no_readback = renderer.Render(draw);
    Check(no_readback.pixels.empty() && no_readback.completed_submission == 9, "Rejected draw advanced GPU work or normal draw read pixels");
    // A second pipeline shares this same device, queue and completion timeline.
    config.comparison = plume::RenderComparisonFunction::LESS;
    DepthRenderer ordinary(config, gpu); ordinary.Clear(1);
    draw.scissor.right = 4; draw.viewport.minDepth = 0; draw.viewport.maxDepth = 1;
    Pixels(ordinary.Render(draw, true), .125f, 11);
    config.sample_count = 2;
    DepthRenderer multisampled(config, gpu);
    const plume::RenderRect left(0,0,2,4);
    Reject([&] { multisampled.Clear(1, &left); });
    multisampled.Clear(1);
    Pixels(multisampled.Render(draw, true), .125f, 13);
    multisampled.Clear(0, &left);
    const auto retained = multisampled.Render(draw, true);
    Check(retained.completed_submission == 15, "Partial clear completion differs");
    for (size_t i = 0; i < retained.pixels.size(); ++i)
        Check(std::abs(retained.pixels[i] - (i % 4 < 2 ? 0 : .125f)) < 1e-6f, "MSAA partial clear did not preserve other pixels");
    config.comparison = plume::RenderComparisonFunction::GREATER_EQUAL;
    DepthRenderer edge(config, gpu); edge.Clear(0);
    draw.viewport.minDepth = 1; draw.viewport.maxDepth = 0;
    draw.indices[5] = 3; draw.index_count = 3; set_depth(.25f);
    const auto edge_pixels = edge.Render(draw, true);
    for (size_t i = 0; i < edge_pixels.pixels.size(); ++i)
        Check(std::abs(edge_pixels.pixels[i] - (i/4 + i%4 <= 3 ? .75f : 0)) < 1e-6f,
              "Two-sample diagonal coverage differs");
    std::cout << "PASS: mini-fetch layout, byte order, indexed range, reversed/ordinary depth, retained target, scissor, two-sample coverage, partial clears, rejection and shared native queue; debug_layer=" << gpu->debug_enabled << '\n';
    if (argc == 5) {
        const std::filesystem::path capture(argv[3]);
        auto device = Read(capture / "device.bin");
        Check(device.size() == 24064, "Unexpected captured device extent");
        const auto word = [&](size_t offset) { return mc2::capture::LoadBE32(device.data()+offset); };
        Check(word(10548) == 0x00700766 && word(10568) == 0x00018002 && word(10572) == 0x43f &&
              word(12700) == 0 && word(12448) == 0 && word(11824) == 0 &&
              word(12672) == 0x3f800000 && word(12676) == 0, "Capture differs from the verified depth pass");
        auto captured_shader = Read(capture / "vertex-shader.container");
        Check(captured_shader == container, "Captured shader differs from the tested translation");
        const auto surface = Read(capture / "targets/depth.descriptor.bin");
        Check(surface.size() == 48 && ((mc2::capture::LoadBE32(surface.data()+24) >> 16) & 3) == 1,
              "Captured depth surface is not two-sample");
        config.sample_count = 2;
        draw.vertices = NativeVertexBytes(Read(capture / "vertices/slot-00.data.bin"), 2);
        const auto indices = NativeVertexBytes(Read(capture / "index-buffer.data.bin"), 2);
        Check(indices.size() % 4 == 0, "Unexpected captured index size");
        draw.indices.resize(indices.size() / 4); std::memcpy(draw.indices.data(), indices.data(), indices.size());
        draw.vertex_constants = NativeVertexBytes(std::span(device).subspan(1920,4096), 2);
        draw.first_index = 6; draw.index_count = 18;
        config.width = 1280; config.height = 720;
        config.comparison = plume::RenderComparisonFunction::GREATER_EQUAL;
        // The capture's front is CCW and back faces are culled. Plume's D3D12
        // pipeline fixes clockwise front faces, so exchange the cull mode.
        config.cull = plume::RenderCullMode::FRONT;
        draw.viewport = plume::RenderViewport(0,0,1280,720,1,0);
        draw.scissor = plume::RenderRect(0,0,1280,720);
        const float half_pixel[] = {1.0f/1280,-1.0f/720};
        std::memcpy(draw.shared_constants.data()+264,half_pixel,sizeof(half_pixel));
        DepthRenderer actual(config,gpu); actual.Clear(0);
        const auto result = actual.Render(draw,true);
        size_t covered = 0;
        for (float depth : result.pixels) {
            Check(std::isfinite(depth) && depth >= 0 && depth <= 1, "Captured depth is invalid");
            covered += depth != 0;
        }
        Check(covered != 0, "Captured draw produced no depth");
        std::ofstream output(argv[4],std::ios::binary);
        output.write(reinterpret_cast<const char*>(result.pixels.data()),result.pixels.size()*4);
        Check(bool(output), "Cannot write local depth readback");
        std::cout << "Captured draw: covered=" << covered << " pixels=" << result.pixels.size()
                  << " samples=" << config.sample_count << " diagnostic_resolve=maximum completed="
                  << result.completed_submission << "; full_pipeline_parity=unverified\n";
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Native depth check: " << error.what() << '\n'; return 1;
}
