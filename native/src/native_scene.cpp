#include "generated/default/magnacarta2_init.h"
#include "native_scene.h"
#include "native_shader_input.h"
#include "native_sampled_texture.h"
#include "guest_graphics_read.h"
#include "draw_capture.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace mc2::native {
namespace {
using mc2::capture::ReadGuest;
using mc2::capture::LoadBE32;
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
struct DeviceState {
    std::vector<uint8_t> bytes;
    DeviceState(uint8_t* base, uint32_t address) {
        Require(ReadGuest(base,address,24064,bytes), "Unreadable native scene device");
    }
    uint32_t Word(uint32_t offset) const { return LoadBE32(bytes.data()+offset); }
    float Float(uint32_t offset) const { return std::bit_cast<float>(Word(offset)); }
    void NoColorAttachments() const {
        for (uint32_t i = 0; i < 4; ++i)
            Require(Word(12448+i*4) == 0, "This native depth pass requires no color attachments");
    }
    SceneSurface DepthSurface(uint8_t* base, uint32_t height) const {
        SceneSurface result;
        result.address = Word(12464);
        std::vector<uint8_t> descriptor;
        Require(ReadGuest(base,result.address,48,descriptor) && (LoadBE32(descriptor.data()) & 15) == 4,
                "Invalid native depth surface");
        const auto surface_info = LoadBE32(descriptor.data()+24), dimensions = LoadBE32(descriptor.data()+36);
        result.width = ((dimensions>>18)&16383)+1; result.height = ((dimensions>>3)&32767)+1;
        result.samples = 1u << ((surface_info>>16)&3);
        Require(result.width == 1280 && result.height == height && result.samples == 2 &&
                LoadBE32(descriptor.data()+32) == 17 && (surface_info & 16383) == 1280 &&
                Word(10376) == LoadBE32(descriptor.data()+28),
                "Depth surface differs from the verified two-sample float format");
        result.description.assign(descriptor.begin()+24,descriptor.end());
        return result;
    }
    void FullViewport(const SceneSurface& surface) const {
        Require(Float(12656) == 0 && Float(12660) == 0 && Float(12664) == surface.width &&
                Float(12668) == surface.height && Word(11864) == 0,
                "Scene viewport/scissor differs from the verified pass");
    }
};

SceneVertexShader LoadVertexShader(const std::filesystem::path& directory) {
    auto reference = ReadNativeFile(directory / "vertex-shader.container");
    auto inputs = ReadVertexInputs(reference);
    return {std::move(reference), ReadNativeFile(directory / "vertex.dxil"), std::move(inputs)};
}

SceneSurface ReadTiledColorSurface(const DeviceState& state, uint8_t* base) {
    Require(state.Word(12448)!=0 && state.Word(12452)==0 && state.Word(12456)==0 && state.Word(12460)==0,
            "Native scene requires the verified single color attachment");
    SceneSurface surface;
    surface.address=state.Word(12448);
    std::vector<uint8_t> descriptor;
    Require(ReadGuest(base,surface.address,48,descriptor) && (LoadBE32(descriptor.data())&15)==4,
            "Invalid native color surface");
    const auto info=LoadBE32(descriptor.data()+24), color_info=LoadBE32(descriptor.data()+28);
    const auto dimensions=LoadBE32(descriptor.data()+36);
    Require(((dimensions>>18)&16383)+1==1280 && ((dimensions>>3)&32767)+1==512 &&
            (info&16383)==1280 && ((info>>16)&3)==1 && ((color_info>>16)&1023)==3 &&
            (LoadBE32(descriptor.data()+40)&63)==63 && state.Word(10372)==color_info,
            "Scene color surface differs from the verified two-sample HDR format");
    Require((state.bytes[10940]&0x20)!=0 && state.Word(12756)==2 && state.Word(13196)==2 &&
            state.Word(13188)==1280 && state.Word(13192)==720 &&
            state.Word(12732)==surface.address && state.Word(12736)==surface.address &&
            state.Word(12740)==0 && state.Word(12744)==0 && state.Word(12748)==0 &&
            state.Word(12464)!=0 && state.Word(12752)==state.Word(12464),
            "Native scene has an unsupported tiling configuration");
    constexpr uint32_t tiles[]={0,0,1280,512,0,512,1280,720};
    for (uint32_t i=0;i<std::size(tiles);++i)
        Require(state.Word(12760+i*4)==tiles[i],"Scene tile rectangles changed");
    surface.width=1280; surface.height=720; surface.samples=2;
    surface.description.assign(descriptor.begin()+24,descriptor.end());
    state.FullViewport(surface);
    return surface;
}

DepthDraw ReadGeometry(const PPCContext& ctx, uint8_t* base, const DeviceState& state,
                       const SceneSurface& surface, const SceneVertexShader& shader) {
    Require(ctx.r4.u32 == 4 && state.Word(11824) == 0,
            "Native scene requires the verified triangle-list vertex inputs");
    Require(state.Word(10548) == 0x00700766 && state.Word(10568) == 0x00018002 && state.Word(10572) == 0x43f &&
            state.Word(10564) == 0x80000 && state.Word(10452) == 0 && state.Float(12672) == 1 && state.Float(12676) == 0,
            "Depth/stencil, culling, clipping, vertex base or viewport state differs from the verified pass");
    Require(ReadBoundShader(base,state.Word(12704),true) == shader.reference, "Untranslated native scene vertex shader");
    Require(uint32_t(state.bytes[12536])*4 == 40, "Native scene vertex stride changed");
    std::vector<uint8_t> vertex_descriptor, index_descriptor, raw;
    uint32_t address=0,size=0;
    Require(ReadGuest(base,state.Word(12468),32,vertex_descriptor) &&
            mc2::draw_capture::BufferBytes(vertex_descriptor.data(),vertex_descriptor.size(),true,address,size) &&
            ReadGuest(base,address,size,raw), "Invalid scene vertex buffer");
    const auto vertex_address=LoadBE32(vertex_descriptor.data()+24);
    const auto physical_fetch=(vertex_address & 0x1fffffff) + (((vertex_address>>20)+512)&0x1000);
    Require(state.Word(1912) == physical_fetch && state.Word(1916) == LoadBE32(vertex_descriptor.data()+28),
            "Bound vertex fetch differs from its resource descriptor");
    DepthDraw draw;
    draw.vertices=NativeVertexBytes(raw,LoadBE32(vertex_descriptor.data()+28)&3);
    Require(ReadGuest(base,state.Word(12444),32,index_descriptor) &&
            (LoadBE32(index_descriptor.data()) & 0xf000000f) == 0xc0000002 &&
            mc2::draw_capture::BufferBytes(index_descriptor.data(),index_descriptor.size(),false,address,size) &&
            ReadGuest(base,address,size,raw), "Unsupported native scene index buffer");
    Require(raw.size()%4 == 0, "Invalid 32-bit scene index extent");
    draw.indices.resize(raw.size()/4);
    for (size_t i=0;i<draw.indices.size();++i) draw.indices[i]=LoadBE32(raw.data()+i*4);
    draw.first_index=ctx.r5.u32; draw.index_count=ctx.r6.u32;
    draw.vertex_constants=NativeVertexBytes(std::span(state.bytes).subspan(1920,4096),2);
    draw.shared_constants.resize(512);
    const float half_pixel[] = {1.0f/surface.width,-1.0f/surface.height};
    std::memcpy(draw.shared_constants.data()+264,half_pixel,sizeof(half_pixel));
    const float x_scale = state.Float(10504), y_scale = state.Float(10512);
    const float x = state.Float(10508) - x_scale, y = state.Float(10516) + y_scale;
    Require(x_scale == float(surface.width) / 2 && y_scale == -float(surface.height) / 2 &&
            std::abs(x) <= 0.5f && std::abs(y) <= 0.5f && state.Float(10520) == -1 && state.Float(10524) == 1,
            "Native scene cached viewport transform differs");
    draw.viewport=plume::RenderViewport(x,y,float(surface.width),float(surface.height),1,0);
    draw.scissor=plume::RenderRect(0,0,surface.width,surface.height);
    return draw;
}
}

SceneVertexShader LoadDepthShaders(const std::filesystem::path& directory) {
    auto shader = LoadVertexShader(directory);
    const std::vector<VertexInput> expected = {{0,0,95,40,0,4,true}, {10,0,95,40,0,3}, {5,0,95,40,24,2}};
    Require(shader.inputs == expected, "Scene depth shader input layout differs from the verified shader");
    return shader;
}
SceneColorShaders LoadColorShaders(const std::filesystem::path& directory) {
    auto vertex = LoadVertexShader(directory);
    const std::vector<VertexInput> expected = {
        {0,0,95,40,0,4,true}, {6,0,95,40,0,3}, {7,0,95,40,12,4,false,VertexStorage::UInt8},
        {3,0,95,40,16,4,false,VertexStorage::UInt8}, {10,0,95,40,20,4,false,VertexStorage::UInt8}, {5,0,95,40,24,2}
    };
    Require(vertex.inputs == expected, "Scene color shader input layout differs from the verified shader");
    SceneColorShaders shaders{std::move(vertex), ReadNativeFile(directory / "pixel-shader.container"),
                              ReadNativeFile(directory / "pixel-7e3.dxil"), {}};
    const auto resolve_path = directory / "color-resolve.dxil";
    if (std::filesystem::exists(resolve_path)) shaders.resolve_dxil = ReadNativeFile(resolve_path);
    return shaders;
}
DepthConfiguration ConfigureDepth(const SceneSurface& surface, const SceneVertexShader& shader) {
    DepthConfiguration configuration;
    configuration.width=surface.width; configuration.height=surface.height; configuration.sample_count=surface.samples;
    configuration.vertex_shader=shader.dxil; configuration.inputs=shader.inputs;
    configuration.cull=plume::RenderCullMode::FRONT;
    return configuration;
}
DepthClear ReadDepthClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.r4.u32 == 0x30, "Only the verified full depth/stencil clear is supported");
    DeviceState state(base,ctx.r3.u32);
    state.NoColorAttachments();
    DepthClear clear; clear.surface=state.DepthSurface(base,720); state.FullViewport(clear.surface);
    std::vector<uint8_t> rect;
    Require(ReadGuest(base,ctx.r5.u32,16,rect) && LoadBE32(rect.data()) == 0 && LoadBE32(rect.data()+4) == 0 &&
            LoadBE32(rect.data()+8) == clear.surface.width && LoadBE32(rect.data()+12) == clear.surface.height,
            "Only the verified full surface clear is supported");
    Require(std::isfinite(ctx.f1.f64) && ctx.f1.f64 == 0, "Scene clear value differs from the verified zero depth/stencil clear");
    return clear;
}
SceneColorClear ReadSceneColorClear(const PPCContext& ctx,uint8_t* base) {
    Require(ctx.r4.u32==0xf,"Only the verified scene color clear is supported");
    DeviceState state(base,ctx.r3.u32);
    SceneColorClear clear;
    clear.surface = ReadTiledColorSurface(state, base);
    // Begin-frame selects the first tile explicitly before clearing it.
    Require(state.Word(12724)==1 && state.Word(12716)==2 && state.Word(10932)==2,
            "Scene color clear uses an unsupported tile predicate");
    std::vector<uint8_t> rect,color;
    Require(ReadGuest(base,ctx.r5.u32,16,rect) && LoadBE32(rect.data())==0 && LoadBE32(rect.data()+4)==0 &&
            LoadBE32(rect.data()+8)==1280 && LoadBE32(rect.data()+12)==720,"Scene color clear rectangle changed");
    Require(ReadGuest(base,ctx.r6.u32 ? ctx.r6.u32 : 0x82247720,16,color),"Unreadable scene clear color");
    for (uint32_t i=0;i<4;++i) {
        clear.value.rgba[i]=std::bit_cast<float>(LoadBE32(color.data()+i*4));
        Require(clear.value.rgba[i]==0,"Only the verified zero HDR clear is supported");
    }
    clear.rectangle=plume::RenderRect(0,0,1280,512);
    return clear;
}
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& ctx, uint8_t* base, const SceneVertexShader& shader) {
    DeviceState state(base, ctx.r3.u32);
    state.NoColorAttachments();
    const auto surface = state.DepthSurface(base,720);
    state.FullViewport(surface);
    Require(state.Word(12700) == 0, "Native depth pass has a bound pixel shader");
    return {surface, ReadGeometry(ctx, base, state, surface, shader)};
}
SceneColorDraw ReadSceneColorDraw(const PPCContext& ctx, uint8_t* base, const SceneColorShaders& shaders) {
    DeviceState state(base, ctx.r3.u32);
    SceneColorDraw snapshot;
    snapshot.color_surface = ReadTiledColorSurface(state, base);
    snapshot.tile_depth = state.DepthSurface(base,512);
    Require(state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX && state.Word(10932) == 0 &&
            state.Word(12728) == 2, "Native color draw requires the verified automatic tile predicate");
    // Full color write, copy blend, and no alpha test or alpha-to-coverage.
    Require(state.Word(10460) == 0xf && state.Word(10552) == 0x10001 && (state.Word(10556) & 0x1f) == 7,
            "Native color draw has unsupported color write, blend or alpha state");
    Require(ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference,
            "Untranslated native scene pixel shader");
    snapshot.draw = ReadGeometry(ctx, base, state, snapshot.color_surface, shaders.vertex);
    snapshot.pixel_constants = NativeVertexBytes(std::span(state.bytes).subspan(6016,4096),2);
    for (uint32_t slot = 0; slot < snapshot.textures.size(); ++slot) {
        auto& texture = snapshot.textures[slot];
        std::string error;
        if (!mc2::texture::DescribeBlockTexture(std::span(state.bytes).subspan(1152 + slot * 24, 24), texture, error))
            throw std::runtime_error(error);
        const auto& words = texture.fetch_words;
        Require((words[0] & 0x0007fc00) == 0 && words[3] == 0x06280d10 && words[4] == 0x983 &&
                (words[5] & 0x1ff) == 0 && texture.format == (slot ? 20u : 18u) &&
                texture.mip_min_level == 0 && texture.mip_max_level == 6,
                "Native scene block texture or sampler state changed");
        // Both fetch instructions use the bound filter settings. The reference
        // D3D12 sampler selects anisotropic filtering with a 4:1 limit for this state.
        auto& sampler = snapshot.samplers[slot];
        sampler.anisotropyEnabled = true;
        sampler.maxAnisotropy = 4;
        sampler.maxLOD = float(texture.mip_max_level);
        auto* shared = snapshot.draw.shared_constants.data();
        const auto mode = PackTextureFetchMode(texture.swizzle, texture.gamma_mask, texture.exp_adjust);
        std::memcpy(shared + slot * 4, &slot, sizeof(slot));
        std::memcpy(shared + 192 + slot * 4, &slot, sizeof(slot));
        std::memcpy(shared + 288 + slot * 4, &mode, sizeof(mode));
    }
    return snapshot;
}

SceneColorResolve ReadSceneColorResolve(const PPCContext& ctx, uint8_t* base) {
    // Color 0, default sample selection (both samples), exponent -3, and
    // clear color after copy. The depth resolve has a different contract.
    Require(ctx.lr == 0x826DA2D8 && ctx.r4.u32 == 0xF4000100 && ctx.r8.u32 == 0 && ctx.r9.u32 == 0,
            "Unsupported native tiled resolve flags or caller");
    DeviceState state(base, ctx.r3.u32);
    SceneColorResolve resolve;
    resolve.source = ReadTiledColorSurface(state, base);
    Require(state.Word(12724) == 1 && state.Word(12716) == 2 && state.Word(10932) == 2,
            "Native color resolve requires the first tile predicate");
    std::vector<uint8_t> rectangle, origin, clear, destination;
    Require(ReadGuest(base, ctx.r5.u32, 16, rectangle) && LoadBE32(rectangle.data()) == 0 &&
            LoadBE32(rectangle.data() + 4) == 0 && LoadBE32(rectangle.data() + 8) == 1280 &&
            LoadBE32(rectangle.data() + 12) == 512, "Native color resolve rectangle differs");
    Require(ReadGuest(base, ctx.r7.u32, 8, origin) && LoadBE32(origin.data()) == 0 && LoadBE32(origin.data() + 4) == 0,
            "Native color resolve destination origin differs");
    Require(ReadGuest(base, 0x82247720, 16, clear) &&
            std::all_of(clear.begin(), clear.end(), [](uint8_t byte) { return byte == 0; }),
            "Native color resolve requires the verified zero clear");
    Require(ReadGuest(base, ctx.r6.u32, 52, destination) && (LoadBE32(destination.data()) & 15) == 3,
            "Invalid native color resolve destination texture");
    const auto word = [&](uint32_t offset) { return LoadBE32(destination.data() + offset); };
    Require(word(28) == 0x8a000156 && (word(32) & 4095) == 0x60 && (word(32) >> 12) != 0 &&
            word(36) == 0x59e4ff && word(40) == 0xd11 && word(44) == 0 && word(48) == 0xa00,
            "Native color resolve requires the verified full-size tiled RGBA16F destination");
    resolve.destination = ctx.r6.u32;
    resolve.rectangle = plume::RenderRect(0,0,1280,512);
    resolve.exponent = -3;
    return resolve;
}
}
