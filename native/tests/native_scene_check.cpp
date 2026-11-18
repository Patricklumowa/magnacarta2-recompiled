#include "generated/default/magnacarta2_init.h"
#include "native_color_target.h"
#include "native_sampled_texture.h"
#include "native_scene.h"
#include "native_shader_input.h"
#include "shader_capture.h"
#include <plume_d3d12.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
template<class F> void Reject(F&& action) {
    bool rejected=false; try { action(); } catch (const std::runtime_error&) { rejected=true; }
    Check(rejected,"Unsupported native scene state was accepted");
}

void CheckDepthRestore(mc2::native::DepthRenderer& depth, mc2::native::NativeDevice& gpu,
                       const mc2::native::SceneSurface& surface, const std::vector<float>& source,
                       const std::filesystem::path& directory) {
    using namespace mc2::native;
    using namespace plume;
    auto shaders = LoadDepthRestoreShaders(directory);
    auto single_sample = surface;
    single_sample.samples = 1;
    auto config = ConfigureDepth(single_sample, shaders.vertex);
    config.comparison = RenderComparisonFunction::ALWAYS;
    config.cull = RenderCullMode::NONE;
    DepthDraw draw;
    std::array<uint8_t,64> guest{};
    for (size_t offset : {16,20,32,40,48}) {
        // Three big-endian float4 corners at a nonzero guest address.
        guest[offset] = 0x3f;
        guest[offset + 1] = 0x80;
    }
    ReadDepthRestoreVertices(draw, guest.data(), 4);
    draw.vertex_constants.resize(4096);
    const std::vector<uint8_t> pixel_constants(4096);
    draw.shared_constants.resize(512);
    const float half_pixel[] = {1.0f / surface.width, -1.0f / surface.height};
    const auto fetch_mode = PackTextureFetchMode(0x688, 0, 0);
    std::memcpy(draw.shared_constants.data() + 264, half_pixel, sizeof(half_pixel));
    std::memcpy(draw.shared_constants.data() + 288, &fetch_mode, sizeof(fetch_mode));
    draw.viewport = RenderViewport(0,0,surface.width,surface.height,1,0);
    draw.scissor = RenderRect(0,0,surface.width,surface.height);

    auto target = gpu.device->createTexture(RenderTextureDesc::DepthTarget(surface.width, surface.height,
                                                                          RenderFormat::D32_FLOAT_S8_UINT));
    Check(bool(target), "Restore test depth/stencil allocation failed");
    RenderFramebufferDesc framebuffer_desc;
    framebuffer_desc.depthAttachment = target.get();
    auto framebuffer = gpu.device->createFramebuffer(framebuffer_desc);
    Check(bool(framebuffer), "Restore test framebuffer creation failed");
    auto& commands = gpu.commands;
    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer.get());
    commands->clearDepthStencil(true, true, 1, 0xa7, nullptr, 0);
    commands->end();
    gpu.queue->executeCommandLists(commands.get(), gpu.fence.get());
    const auto before_restore = gpu.Complete();
    Check(depth.RestoreResolvedDepth(config, draw, shaders.pixel_dxil, pixel_constants, *target, *framebuffer) == before_restore + 1,
          "Restore draw did not complete its own fence");

    // D3D12 copies the two planes as distinct full subresources. Plume's
    // generic copy interface does not expose the depth/stencil plane index.
    auto* d3d_device = static_cast<D3D12Device*>(gpu.device.get())->d3d;
    auto* d3d_texture = static_cast<D3D12Texture*>(target.get())->d3d;
    const auto description = d3d_texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> layouts{};
    std::array<UINT64,2> row_sizes{};
    UINT64 bytes = 0;
    d3d_device->GetCopyableFootprints(&description,0,2,0,layouts.data(),nullptr,row_sizes.data(),&bytes);
    Check(bytes && bytes != UINT64_MAX, "Invalid depth/stencil copy footprints");
    const size_t depth_stride = row_sizes[0] / surface.width, stencil_stride = row_sizes[1] / surface.width;
    Check((depth_stride == 4 || depth_stride == 8) && (stencil_stride == 1 || stencil_stride == 8),
          "Unexpected depth/stencil plane packing");
    auto readback = gpu.device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    Check(bool(readback), "Restore test readback allocation failed");
    auto* d3d_buffer = static_cast<D3D12Buffer*>(readback.get())->d3d;
    auto* d3d_list = static_cast<D3D12CommandList*>(commands.get())->d3d;
    commands->begin();
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    for (uint32_t plane = 0; plane < layouts.size(); ++plane) {
        D3D12_TEXTURE_COPY_LOCATION from{d3d_texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.SubresourceIndex = plane;
        D3D12_TEXTURE_COPY_LOCATION to{d3d_buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = layouts[plane];
        d3d_list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }
    commands->end();
    gpu.queue->executeCommandLists(commands.get(), gpu.fence.get());
    gpu.Complete();
    const auto* pixels = static_cast<const uint8_t*>(readback->map());
    Check(pixels != nullptr, "Restore test readback mapping failed");
    size_t match = 0, preserved = 0, covered = 0;
    for (uint32_t y = 0; y < surface.height; ++y) {
        const auto* depth_row = pixels + layouts[0].Offset + size_t(y) * layouts[0].Footprint.RowPitch;
        const auto* stencil_row = pixels + layouts[1].Offset + size_t(y) * layouts[1].Footprint.RowPitch;
        for (uint32_t x = 0; x < surface.width; ++x) {
            float z;
            std::memcpy(&z, depth_row + size_t(x) * depth_stride, sizeof(z));
            preserved += stencil_row[size_t(x) * stencil_stride + (stencil_stride == 8 ? 4 : 0)] == 0xa7;
            // The half-pixel offset places point samples on texel boundaries.
            // Either adjacent texel may win when interpolation rounds the tie.
            bool matches = false;
            for (uint32_t sy : {y, (y + surface.height - 1) % surface.height})
                for (uint32_t sx : {x, (x + surface.width - 1) % surface.width})
                    matches |= std::abs(z - std::max(0.0f, source[size_t(sy) * surface.width + sx] - 1.0e-6f)) <= 1.0e-7f;
            match += matches;
            covered += z > 0 && z < 1;
        }
    }
    readback->unmap();
    std::cout << "Depth restore readback: matched=" << match << ", stencil preserved=" << preserved << ", covered=" << covered << '\n';
    Check(match == source.size() && preserved == source.size() && covered > 1000,
          "Restored depth differs from the sampled source or modified the stencil plane");
}

void ReplayColor(const mc2::native::SceneDepthDraw& prepass, const mc2::native::SceneVertexShader& depth_shader,
                 const mc2::native::SceneColorClear& clear, mc2::native::SceneColorDraw draw,
                 const mc2::native::SceneColorShaders& shaders, const std::filesystem::path& capture,
                 const std::filesystem::path& output, const std::filesystem::path& restore_directory) {
    using namespace mc2::native;
    using namespace plume;
    auto gpu = std::make_shared<NativeDevice>(true);
    const auto& surface = draw.color_surface;
    DepthRenderer depth(ConfigureDepth(prepass.surface, depth_shader), gpu, shaders.depth_resolve_dxil);
    ColorTarget color(surface.width, surface.height, surface.samples, gpu, shaders.resolve_dxil);
    ColorRenderer renderer(ConfigureDepth(surface, shaders.vertex), shaders.pixel_dxil, draw.samplers, depth, color);
    std::array<std::unique_ptr<SampledTexture>, 2> textures;
    std::array<const SampledTexture*, 2> bindings;
    for (uint32_t slot = 0; slot < textures.size(); ++slot) {
        const auto& description = draw.textures[slot];
        std::vector<std::vector<uint8_t>> mips(description.mips.size());
        for (uint32_t level = 0; level < mips.size(); ++level) {
            const auto name = "slot-0" + std::to_string(slot) + ".mip-0" + std::to_string(level);
            const auto source = ReadNativeFile(capture / "textures" / (name + ".raw.bin"));
            std::string error;
            if (!mc2::texture::LinearizeBlockMip(description, level, source, mips[level], error))
                throw std::runtime_error(error);
            Check(mips[level] == ReadNativeFile(capture / "textures" / (name + ".blocks.bin")), "Captured native mip differs");
        }
        const auto& top = description.mips.front();
        const auto format = description.format == 18 ? RenderFormat::BC1_UNORM : RenderFormat::BC3_UNORM;
        textures[slot] = std::make_unique<SampledTexture>(format, top.width, top.height, mips, gpu);
        bindings[slot] = textures[slot].get();
    }
    const auto submit = [&] { return renderer.Render(draw.draw, draw.pixel_constants, bindings); };
    Reject(submit);
    depth.Clear(0);
    const auto depth_result = depth.Render(prepass.draw, true);
    color.Clear(clear.value, &clear.rectangle);
    Reject(submit); // The lower tile has not been initialized.
    draw.draw.scissor = clear.rectangle;
    const auto prior = gpu->completed;
    Check(submit() == prior + 1, "Indexed color draw did not complete its GPU fence");
    Reject([&] { color.ReadPixels(); });
    // Seed the lower tile before readback so a stray write is observable.
    const RenderColor sentinel(0,0,0.25f,0);
    color.Clear(sentinel);
    color.Clear(clear.value, &clear.rectangle);
    submit();
    const auto pixels = color.ReadPixels();
    size_t covered = 0;
    for (size_t i = 0; i < depth_result.pixels.size(); ++i) {
        const auto* pixel = pixels.data() + i * 4;
        const bool upper = i / surface.width < uint32_t(clear.rectangle.bottom);
        Check(!pixel[0] && !pixel[1] && pixel[2] == (upper ? 0 : 0x3400), "Captured color or tile scissor differs");
        Check((pixel[3] != 0) == (upper && depth_result.pixels[i] > 0) &&
              (pixel[3] == 0 || pixel[3] == 0x3800 || pixel[3] == 0x3c00),
              "Captured color coverage differs from the preceding depth draw");
        covered += pixel[3] != 0;
    }
    Check(covered > 1000, "Captured color draw shaded too few pixels");
    const float sky[2][4] = {{1,0.5f,0.25f,1}, {0.25f,0.5f,1,1}};
    std::memcpy(draw.pixel_constants.data() + 4 * 16, sky, sizeof(sky));
    depth.Clear(1);
    submit();
    Check(color.ReadPixels() == pixels, "Depth-rejected indexed color draw changed pixels");
    depth.Clear(0);
    depth.Render(prepass.draw);
    submit();
    const auto lit = color.ReadPixels();
    size_t lit_count = 0;
    for (size_t i = 0; i < lit.size(); i += 4) {
        Check(lit[i+3] == pixels[i+3], "Diagnostic lighting changed alpha coverage");
        if (lit[i+3]) {
            Check(lit[i] && lit[i] < 0x7c00 && lit[i+1] && lit[i+1] < 0x7c00 && lit[i+2] && lit[i+2] < 0x7c00,
                  "Diagnostic lighting did not sample the captured material");
            ++lit_count;
        }
    }
    Check(lit_count == covered, "Lit draw coverage differs");
    // Verify that the material responds to the diffuse texture's gamma flag.
    const uint32_t linear = PackTextureFetchMode(draw.textures[1].swizzle, 0, draw.textures[1].exp_adjust);
    std::memcpy(draw.draw.shared_constants.data() + 292, &linear, sizeof(linear));
    submit();
    const auto without_gamma = color.ReadPixels();
    size_t gamma_changed = 0;
    for (size_t i = 0; i < lit.size(); i += 4)
        gamma_changed += lit[i] != without_gamma[i];
    Check(gamma_changed == covered, "Diffuse texture gamma did not reach the material shader");
    std::vector<uint16_t> upper_color;
    std::vector<float> upper_depth;
    if (!shaders.resolve_dxil.empty()) {
        Reject([&] { color.ReadResolvedPixels(); });
        const auto before_resolve = gpu->completed;
        Reject([&] { color.ResolveAndClear(clear.rectangle, -33); });
        Reject([&] { color.ResolveAndClear(RenderRect(-1,0,1280,512), -3); });
        Check(color.ResolveAndClear(clear.rectangle, -3) == before_resolve + 1,
              "Native resolve and clear did not share one completed submission");
        upper_color = color.ReadResolvedPixels();
        const auto& resolved = upper_color;
        const auto source_after = color.ReadPixels();
        Check(resolved.size() == size_t(surface.width) * clear.rectangle.bottom * 4, "Resolved region extent differs");
        for (size_t i = 0; i < resolved.size(); ++i) {
            // The test material stays in the normalized float16 range after
            // scaling. Multiplication by 1/8 subtracts three from the exponent.
            const uint16_t value = without_gamma[i];
            Check(!value || (value >= 0x1000 && value < 0x7c00), "Resolve fixture is outside the chosen float16 range");
            Check(resolved[i] == (value ? value - 0x0c00 : 0), "Resolved sample average or destination exponent differs");
            Check(source_after[i] == 0, "Resolved source tile was not cleared");
        }
        for (size_t i = resolved.size(); i < source_after.size(); ++i)
            Check(source_after[i] == without_gamma[i], "Resolve cleared outside the selected tile");
        std::cout << "PASS: captured tile resolve matches the D3D12 average scaled by 1/8, with a scoped source clear\n";
    }
    if (!shaders.depth_resolve_dxil.empty()) {
        Reject([&] { depth.ReadResolvedPixels(); });
        auto diagnostic = prepass.draw;
        // Read back the retained depth using a scissor with no covered geometry.
        diagnostic.scissor = RenderRect(0,0,1,1);
        const auto max_before = depth.Render(diagnostic, true).pixels;
        const auto before_resolve = gpu->completed;
        Reject([&] { depth.ResolveAndClear(RenderRect(0,0,1280,721)); });
        Check(depth.ResolveAndClear(clear.rectangle) == before_resolve + 1,
              "Native depth copy and clear did not share one completed submission");
        upper_depth = depth.ReadResolvedPixels();
        const auto& sample_zero = upper_depth;
        const auto max_after = depth.Render(diagnostic, true).pixels;
        Check(sample_zero.size() == size_t(surface.width) * clear.rectangle.bottom, "Resolved depth tile extent differs");
        size_t nonzero = 0, maximum_only = 0;
        for (size_t i = 0; i < sample_zero.size(); ++i) {
            const float value = sample_zero[i], maximum = max_before[i];
            Check(std::isfinite(value) && value >= 0 && max_after[i] == 0, "Invalid resolved value or missing source depth clear");
            const auto step = [](float x) { return std::ldexp(1.0, (x > 0 ? std::max(std::ilogb(x), -14) : -14) - 20); };
            const double spacing = step(value), upper_spacing = step(maximum);
            Check(value / spacing == std::floor(value / spacing) && value <= maximum + 0.5 * upper_spacing,
                  "Resolved value is outside float24 storage or exceeds the largest sample");
            nonzero += value > 0;
            maximum_only += value == 0 && maximum > 0;
        }
        Check(nonzero > 1000, "Depth resolve did not preserve nonzero coverage");
        for (size_t i = sample_zero.size(); i < max_after.size(); ++i)
            Check(max_after[i] == max_before[i], "Depth resolve cleared outside the selected tile");
        std::cout << "PASS: captured depth sample zero and float24 storage, covered=" << nonzero
                  << ", max-only edge pixels=" << maximum_only << '\n';
    }
    if (!upper_color.empty() && !upper_depth.empty()) {
        const RenderRect lower(0,clear.rectangle.bottom,surface.width,surface.height);
        draw.draw.scissor = lower;
        auto probe = prepass.draw;
        probe.scissor = RenderRect(0,0,1,1);
        color.Clear(clear.value, &lower);
        submit();
        const auto lower_pixels = color.ReadPixels();
        const auto lower_max = depth.Render(probe, true).pixels;
        depth.Clear(0, &lower);
        color.Clear(clear.value, &lower);
        submit();
        Check(color.ReadPixels() == lower_pixels && depth.Render(probe, true).pixels == lower_max,
              "Lower opaque replay differs with fresh depth and retained prepass depth");
        color.ResolveAndClear(lower, -3);
        depth.ResolveAndClear(lower);
        const auto full_color = color.ReadResolvedPixels();
        const auto full_depth = depth.ReadResolvedPixels();
        Check(full_color.size() == lower_pixels.size() && full_depth.size() == lower_max.size() &&
              std::equal(upper_color.begin(), upper_color.end(), full_color.begin()) &&
              std::equal(upper_depth.begin(), upper_depth.end(), full_depth.begin()),
              "Lower resolves did not retain the upper outputs at their destination offsets");
        size_t lower_coverage = 0;
        for (size_t i = upper_depth.size(); i < full_depth.size(); ++i) {
            for (size_t channel = 0; channel < 4; ++channel) {
                const size_t index = i * 4 + channel;
                const uint16_t value = lower_pixels[index];
                Check(!value || (value >= 0x1000 && value < 0x7c00), "Lower resolve fixture is outside float16 range");
                Check(full_color[index] == (value ? value - 0x0c00 : 0), "Lower HDR resolve offset or exponent differs");
            }
            Check((full_depth[i] != 0) == (lower_max[i] != 0), "Lower depth resolve coverage differs");
            lower_coverage += full_depth[i] != 0;
        }
        Check(lower_coverage > 1000, "Lower scene replay shaded too few pixels");
        const auto cleared_color = color.ReadPixels();
        const auto cleared_depth = depth.Render(probe, true).pixels;
        Check(std::all_of(cleared_color.begin(), cleared_color.end(), [](uint16_t value) { return value == 0; }) &&
              std::all_of(cleared_depth.begin(), cleared_depth.end(), [](float value) { return value == 0; }),
              "Retained tile source clears did not cover both regions");
        std::cout << "PASS: both captured tiles retained at the original resolve offsets, lower coverage=" << lower_coverage << '\n';
        if (std::filesystem::is_directory(restore_directory))
            CheckDepthRestore(depth, *gpu, prepass.surface, full_depth, restore_directory);
    }
    draw.draw.shared_constants[0] = uint8_t(bindings.size());
    const auto before_rejection = gpu->completed;
    Reject(submit);
    Check(gpu->completed == before_rejection, "Invalid texture binding submitted GPU work");
    Check(!std::filesystem::exists(output), "Color replay output already exists");
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(lit.data()), lit.size() * sizeof(lit.front()));
    Check(bool(file), "Could not write diagnostic color readback");
    std::cout << "PASS: captured first color draw and diagnostic lighting, covered=" << covered
              << ", gamma_changed=" << gamma_changed << ", debug_layer=" << gpu->debug_enabled << '\n';
}
}
int main(int argc,char** argv) try {
    using namespace mc2::native;
    using mc2::capture::LoadBE32;
    Check(argc==3 || argc==4 || argc==6,"Usage: mc2_native_scene_check <depth-capture> <depth-shaders> [color-capture [color-shaders output.rgba16f]]");
    const std::filesystem::path capture(argv[1]);
    const auto shaders=LoadDepthShaders(argv[2]);
    std::array<uint8_t,65536> memory{};
    auto* base=memory.data();
    const auto store=[&](uint32_t at,uint32_t value) {
        base[at]=uint8_t(value>>24); base[at+1]=uint8_t(value>>16); base[at+2]=uint8_t(value>>8); base[at+3]=uint8_t(value);
    };
    const auto copy=[&](uint32_t at,const std::vector<uint8_t>& data) {
        Check(at+data.size()<=memory.size(),"Fixture exceeds memory");
        std::memcpy(base+at,data.data(),data.size());
    };
    constexpr uint32_t device=0x1000,vs=0x7000,vb=0x8000,ib=0x8100,surface=0x8200,vertex_data=0x9000,index_data=0xa000;
    const auto state=ReadNativeFile(capture/"device.bin");
    Check(state.size()==24064,"Invalid captured device"); copy(device,state);
    const auto vertices=ReadNativeFile(capture/"vertices/slot-00.data.bin");
    const auto indices=ReadNativeFile(capture/"index-buffer.data.bin");
    copy(vb,ReadNativeFile(capture/"vertices/slot-00.descriptor.bin"));
    copy(ib,ReadNativeFile(capture/"index-buffer.descriptor.bin"));
    copy(surface,ReadNativeFile(capture/"targets/depth.descriptor.bin"));
    copy(vertex_data,vertices); copy(index_data,indices);
    store(vb+24,vertex_data|3); store(ib+24,index_data);
    store(device+12468,vb); store(device+12444,ib); store(device+12464,surface); store(device+12704,vs);
    store(device+1912,vertex_data|3);
    const auto bind_shader = [&](uint32_t resource, uint32_t physical, const std::vector<uint8_t>& reference, bool vertex) {
        Check(reference.size() >= 12, "Short shader fixture");
        const uint32_t virtual_size = LoadBE32(reference.data() + 4);
        const uint32_t header = resource + (vertex ? 872 : 40);
        Check(header + uint64_t(virtual_size) <= memory.size() && reference.size() >= virtual_size &&
              physical + reference.size() - virtual_size <= memory.size(), "Invalid shader fixture");
        store(resource, vertex ? 6 : 7);
        store(resource + (vertex ? 32 : 24), physical);
        std::memcpy(base + header, reference.data(), virtual_size);
        std::memcpy(base + physical, reference.data() + virtual_size, reference.size() - virtual_size);
    };
    bind_shader(vs, 0xb000, shaders.reference, true);
    PPCContext context{}; context.r3.u32=device; context.r4.u32=4; context.r5.u32=6; context.r6.u32=18;
    const auto original=memory; const auto original_context=context;
    const auto draw=ReadSceneDepthDraw(context,base,shaders);
    Check(draw.draw.vertices==NativeVertexBytes(vertices,2) && draw.draw.indices.size()*4==indices.size(),
          "Native scene snapshot did not preserve geometry");
    for (size_t i=0;i<draw.draw.indices.size();++i)
        Check(draw.draw.indices[i]==LoadBE32(indices.data()+i*4),"Native scene index byte order differs");
    Check(draw.draw.first_index==6 && draw.draw.index_count==18 && draw.surface.width==1280 &&
          draw.surface.height==720 && draw.surface.samples==2,"Native scene draw range or surface differs");
    Check(draw.draw.viewport.x == -0.25f && draw.draw.viewport.y == 0.25f, "Captured viewport jitter was lost");
    Check(memory==original && !std::memcmp(&context,&original_context,sizeof(context)),"Snapshot modified guest state");
    for (const auto [offset,value] : std::array<std::pair<uint32_t,uint32_t>,7>{{
        {10548,0x00700767},{12448,1},{12700,1},{10452,1},{1912,0},{10564,0},{11824,1}}}) {
        store(device+offset,value); Reject([&] { ReadSceneDepthDraw(context,base,shaders); }); memory=original;
    }
    context.r4.u32=0x30; context.r5.u32=0x100; context.f1.f64=0;
    store(0x100,0); store(0x104,0); store(0x108,1280); store(0x10c,720);
    const auto clear=ReadDepthClear(context,base);
    Check(clear.surface==draw.surface && clear.value==0,"Native clear does not refer to the captured draw surface");
    context.f1.f64=1; Reject([&] { ReadDepthClear(context,base); }); context.f1.f64=0;
    store(0x108,640); Reject([&] { ReadDepthClear(context,base); }); store(0x108,1280);
    context.r4.u32=0xf; Reject([&] { ReadDepthClear(context,base); });
    if (argc>=4) {
        const std::filesystem::path color_capture(argv[3]);
        constexpr uint32_t color_surface=0x8300;
        copy(device,ReadNativeFile(color_capture/"device.bin"));
        copy(color_surface,ReadNativeFile(color_capture/"targets/color-0.descriptor.bin"));
        copy(surface,ReadNativeFile(color_capture/"targets/depth.descriptor.bin"));
        for (uint32_t offset:{12448,12732,12736}) store(device+offset,color_surface);
        for (uint32_t offset:{12464,12752}) store(device+offset,surface);
        // The following captured draw has automatic predication. Reconstruct
        // the immediately preceding clear's explicit first-tile selection,
        // observed live and written by the engine's begin-frame routine.
        store(device+12724,1); store(device+12716,2); store(device+10932,2);
        context.r6.u32=0x200;
        for (uint32_t i=0;i<4;++i) store(0x200+i*4,0);
        const auto original_color=memory; const auto color_context=context;
        const auto hdr=ReadSceneColorClear(context,base);
        Check(hdr.surface.width==1280 && hdr.surface.height==720 && hdr.surface.samples==2 &&
              hdr.surface.address==color_surface && hdr.rectangle==plume::RenderRect(0,0,1280,512),
              "Tile clear incorrectly used the physical extent or cleared the second tile");
        Check(memory==original_color && !std::memcmp(&context,&color_context,sizeof(context)),"Color snapshot modified guest state");
        for (const auto [offset,value]:std::array<std::pair<uint32_t,uint32_t>,7>{{
            {13192,512},{12736,0},{12724,0},{12716,8},{12788,719},{10372,0},{11864,1}}}) {
            store(device+offset,value); Reject([&] { ReadSceneColorClear(context,base); }); memory=original_color;
        }
        store(0x200,0x3f800000); Reject([&] { ReadSceneColorClear(context,base); }); memory=original_color;
        store(0x108,640); Reject([&] { ReadSceneColorClear(context,base); });
        if (argc == 6) {
            constexpr uint32_t ps = 0x8400;
            const auto color_shaders = LoadColorShaders(argv[4]);
            memory = original_color;
            store(device + 12724, 0); store(device + 12716, UINT32_MAX); store(device + 10932, 0);
            copy(vb, ReadNativeFile(color_capture / "vertices/slot-00.descriptor.bin"));
            copy(ib, ReadNativeFile(color_capture / "index-buffer.descriptor.bin"));
            copy(vertex_data, ReadNativeFile(color_capture / "vertices/slot-00.data.bin"));
            copy(index_data, ReadNativeFile(color_capture / "index-buffer.data.bin"));
            store(vb + 24, vertex_data | 3); store(ib + 24, index_data);
            store(device + 12468, vb); store(device + 12444, ib); store(device + 1912, vertex_data | 3);
            store(device + 12704, vs); store(device + 12700, ps);
            bind_shader(vs, 0xb000, color_shaders.vertex.reference, true);
            bind_shader(ps, 0xc000, color_shaders.pixel_reference, false);
            context.r4.u32 = 4; context.r5.u32 = 6; context.r6.u32 = 18;
            const auto scene_bytes = memory;
            const auto color_draw = ReadSceneColorDraw(context, base, color_shaders);
            Check(memory == scene_bytes && color_draw.color_surface == hdr.surface && color_draw.tile_depth.height == 512 &&
                  SameScenePositionInputs(draw.draw, color_draw.draw),
                  "First color draw did not preserve its captured geometry and attachments");
            auto altered_draw = color_draw.draw;
            altered_draw.vertex_constants[8 * 16] ^= 1;
            Check(!SameScenePositionInputs(draw.draw, altered_draw), "Changed depth-to-color transform was accepted");
            for (const auto [offset, value] : std::array<std::pair<uint32_t,uint32_t>,5>{{
                    {10552,0}, {10556,15}, {12700,0}, {12724,1}, {1152+12,0}}}) {
                store(device + offset, value);
                Reject([&] { ReadSceneColorDraw(context, base, color_shaders); });
                memory = scene_bytes;
            }
            ReplayColor(draw, shaders, hdr, color_draw, color_shaders, color_capture, argv[5],
                        std::filesystem::path(argv[4]) / "depth-restore");
        }
    }
    std::cout<<"PASS: captured scene input reconstruction, unchanged guest state, clear-to-draw surface identity and unsupported-state rejection\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr<<"Native scene check: "<<error.what()<<'\n'; return 1;
}
