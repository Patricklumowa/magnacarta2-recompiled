#include "generated/default/magnacarta2_init.h"
#include "native_color_target.h"
#include "native_sampled_texture.h"
#include "native_scene.h"
#include "native_shader_input.h"
#include "shader_capture.h"
#include <array>
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

void ReplayColor(const mc2::native::SceneDepthDraw& prepass, const mc2::native::SceneVertexShader& depth_shader,
                 const mc2::native::SceneColorClear& clear, mc2::native::SceneColorDraw draw,
                 const mc2::native::SceneColorShaders& shaders, const std::filesystem::path& capture,
                 const std::filesystem::path& output) {
    using namespace mc2::native;
    using namespace plume;
    auto gpu = std::make_shared<NativeDevice>(true);
    const auto& surface = draw.color_surface;
    DepthRenderer depth(ConfigureDepth(prepass.surface, depth_shader), gpu);
    ColorTarget color(surface.width, surface.height, surface.samples, gpu);
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
        Check((pixel[3] != 0) == (upper && depth_result.pixels[i] > 0) && pixel[3] < 0x7c00,
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
                  color_draw.draw.vertices == draw.draw.vertices && color_draw.draw.indices == draw.draw.indices &&
                  color_draw.draw.viewport.x == draw.draw.viewport.x && color_draw.draw.viewport.y == draw.draw.viewport.y,
                  "First color draw did not preserve its captured geometry and attachments");
            for (const auto [offset, value] : std::array<std::pair<uint32_t,uint32_t>,5>{{
                    {10552,0}, {10556,15}, {12700,0}, {12724,1}, {1152+12,0}}}) {
                store(device + offset, value);
                Reject([&] { ReadSceneColorDraw(context, base, color_shaders); });
                memory = scene_bytes;
            }
            ReplayColor(draw, shaders, hdr, color_draw, color_shaders, color_capture, argv[5]);
        }
    }
    std::cout<<"PASS: captured scene input reconstruction, unchanged guest state, clear-to-draw surface identity and unsupported-state rejection\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr<<"Native scene check: "<<error.what()<<'\n'; return 1;
}
