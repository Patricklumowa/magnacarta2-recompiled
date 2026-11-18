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
    SceneSurface DepthSurface(uint8_t* base, uint32_t height, uint32_t samples = 2) const {
        SceneSurface result;
        result.address = Word(12464);
        std::vector<uint8_t> descriptor;
        Require(ReadGuest(base,result.address,48,descriptor) && (LoadBE32(descriptor.data()) & 15) == 4,
                "Invalid native depth surface");
        const auto surface_info = LoadBE32(descriptor.data()+24), dimensions = LoadBE32(descriptor.data()+36);
        result.width = ((dimensions>>18)&16383)+1; result.height = ((dimensions>>3)&32767)+1;
        result.samples = 1u << ((surface_info>>16)&3);
        Require(result.width == 1280 && result.height == height && result.samples == samples &&
                LoadBE32(descriptor.data()+32) == 17 && (surface_info & 16383) == 1280 &&
                Word(10376) == LoadBE32(descriptor.data()+28),
                "Depth surface differs from the verified float depth/stencil layout");
        result.description.assign(descriptor.begin()+24,descriptor.end());
        return result;
    }
    void FullViewport(const SceneSurface& surface) const {
        Require(Float(12656) == 0 && Float(12660) == 0 && Float(12664) == surface.width &&
                Float(12668) == surface.height && Word(11864) == 0,
                "Scene viewport/scissor differs from the verified pass");
    }
};

void RequireFullClearRect(const PPCContext& ctx, uint8_t* base, const SceneSurface& surface) {
    std::vector<uint8_t> rectangle;
    Require(ReadGuest(base, ctx.r5.u32, 16, rectangle) && LoadBE32(rectangle.data()) == 0 &&
            LoadBE32(rectangle.data() + 4) == 0 && LoadBE32(rectangle.data() + 8) == surface.width &&
            LoadBE32(rectangle.data() + 12) == surface.height, "Only the verified full surface clear rectangle is supported");
}

void RequireSceneTexture(const DeviceState& state, const SceneResolveTexture& source, uint32_t clamp_bits, uint32_t slot = 0) {
    Require(source.address && state.Word(12552 + slot * 4) == source.address, "Fullscreen pass did not bind the scene resolve");
    for (uint32_t i = 0; i < source.fetch_words.size(); ++i) {
        uint32_t expected = source.fetch_words[i];
        if (i == 0) expected |= clamp_bits;
        if (i == 1) expected &= 0x1fffffff;
        Require(state.Word(1152 + slot * 24 + i * 4) == expected, "Fullscreen scene texture fetch differs");
    }
}

void ReadFullscreenConstants(const DeviceState& state, const SceneSurface& surface, DepthDraw& draw,
                             std::vector<uint8_t>& pixel_constants, bool reverse_depth) {
    draw.vertex_constants = NativeVertexBytes(std::span(state.bytes).subspan(1920,4096), 2);
    pixel_constants = NativeVertexBytes(std::span(state.bytes).subspan(6016,4096), 2);
    draw.shared_constants.resize(512);
    const float half_pixel[] = {1.0f / surface.width, -1.0f / surface.height};
    std::memcpy(draw.shared_constants.data() + 264, half_pixel, sizeof(half_pixel));
    const auto mode = PackTextureFetchMode(0x688, 0, 0);
    std::memcpy(draw.shared_constants.data() + 288, &mode, sizeof(mode));
    draw.viewport = plume::RenderViewport(0,0,surface.width,surface.height,reverse_depth ? 1.f : 0.f,reverse_depth ? 0.f : 1.f);
    draw.scissor = plume::RenderRect(0,0,surface.width,surface.height);
}

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

SceneSurface ReadFullscreenColorSurface(const DeviceState& state, uint8_t* base) {
    SceneSurface surface{state.Word(12448),1280,720,1,{}};
    std::vector<uint8_t> descriptor;
    Require(state.Word(12452) == 0 && state.Word(12456) == 0 && state.Word(12460) == 0 &&
            ReadGuest(base, surface.address, 48, descriptor), "Missing fullscreen HDR attachment");
    Require((LoadBE32(descriptor.data()) & 15) == 4 && LoadBE32(descriptor.data() + 24) == 0x14000500 &&
            LoadBE32(descriptor.data() + 28) == 0x302d0 && LoadBE32(descriptor.data() + 36) == 0x13fc1678 &&
            state.Word(10372) == 0x302d0, "Fullscreen HDR attachment layout changed");
    surface.description.assign(descriptor.begin()+24,descriptor.end());
    state.FullViewport(surface);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX,
            "Fullscreen HDR pass needs an untiled surface");
    return surface;
}

SceneSurface ReadBloomColorSurface(const DeviceState& state, uint8_t* base) {
    SceneSurface surface{state.Word(12448),322,182,1,{}};
    state.FullViewport(surface);
    std::vector<uint8_t> descriptor;
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX &&
            state.Word(12452) == 0 && state.Word(12456) == 0 && state.Word(12460) == 0 && state.Word(12464) == 0 &&
            ReadGuest(base, surface.address, 48, descriptor) && (LoadBE32(descriptor.data()) & 15) == 4 &&
            LoadBE32(descriptor.data() + 24) == 0x05800190 && LoadBE32(descriptor.data() + 28) == 0x505a0 &&
            LoadBE32(descriptor.data() + 36) == 0x050405a8 && state.Word(10372) == 0x505a0, "Unsupported bloom target layout");
    surface.description.assign(descriptor.begin()+24,descriptor.end());
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
                              ReadNativeFile(directory / "pixel-7e3.dxil"), {}, {}};
    const auto resolve_path = directory / "color-resolve.dxil";
    if (std::filesystem::exists(resolve_path)) shaders.resolve_dxil = ReadNativeFile(resolve_path);
    const auto depth_resolve_path = directory / "depth-resolve.dxil";
    if (std::filesystem::exists(depth_resolve_path)) shaders.depth_resolve_dxil = ReadNativeFile(depth_resolve_path);
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
    RequireFullClearRect(ctx, base, clear.surface);
    Require(std::isfinite(ctx.f1.f64) && ctx.f1.f64 == 0, "Scene clear value differs from the verified zero depth/stencil clear");
    return clear;
}
SceneSurface ReadPostprocessDepthStencilClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.lr == 0x8238B590 && (ctx.r4.u32 == 0x20 || (ctx.r4.u32 == 0x30 && ctx.f1.f64 == 0)),
            "Unsupported native postprocess depth/stencil clear");
    const DeviceState state(base, ctx.r3.u32);
    const auto surface = state.DepthSurface(base, 720, 1);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX,
            "Native stencil clear requires the untiled surface");
    state.FullViewport(surface);
    RequireFullClearRect(ctx, base, surface);
    // Both flag combinations use stencil replace, ref 0, and write mask 0xff.
    // Only flag 0x30 also writes depth.
    return surface;
}
SceneColorClear ReadSceneColorClear(const PPCContext& ctx,uint8_t* base) {
    Require(ctx.r4.u32==0xf,"Only the verified scene color clear is supported");
    DeviceState state(base,ctx.r3.u32);
    SceneColorClear clear;
    clear.surface = ReadTiledColorSurface(state, base);
    // Begin-frame selects the first tile explicitly before clearing it.
    Require(state.Word(12724)==1 && state.Word(12716)==2 && state.Word(10932)==2,
            "Scene color clear uses an unsupported tile predicate");
    RequireFullClearRect(ctx, base, clear.surface);
    std::vector<uint8_t> color;
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

bool SameScenePositionInputs(const DepthDraw& depth, const DepthDraw& color) {
    // The verified shaders form clip position from c0-c3 and c5-c8. Both
    // receive the same half-pixel offset from the full logical viewport.
    constexpr size_t position_bytes = 9 * 16;
    return depth.vertex_constants.size() >= position_bytes && color.vertex_constants.size() >= position_bytes &&
           std::equal(depth.vertex_constants.begin(), depth.vertex_constants.begin() + 4 * 16,
                      color.vertex_constants.begin()) &&
           std::equal(depth.vertex_constants.begin() + 5 * 16, depth.vertex_constants.begin() + position_bytes,
                      color.vertex_constants.begin() + 5 * 16) &&
           depth.vertices == color.vertices && depth.indices == color.indices &&
           depth.first_index == color.first_index && depth.index_count == color.index_count &&
           depth.viewport == color.viewport;
}

SceneTileResolve ReadSceneTileResolve(const PPCContext& ctx, uint8_t* base) {
    // Color 0, default sample selection (both samples), exponent -3, and
    // clear color after copy. The depth copy selects sample zero and clears depth/stencil.
    const bool color = ctx.lr == 0x826DA2D8 && ctx.r4.u32 == 0xF4000100;
    const bool depth = ctx.lr == 0x826DA314 && ctx.r4.u32 == 0x214;
    Require((color || depth) && ctx.r8.u32 == 0 && ctx.r9.u32 == 0,
            "Unsupported native tiled resolve flags or caller");
    DeviceState state(base, ctx.r3.u32);
    SceneTileResolve resolve;
    resolve.color = ReadTiledColorSurface(state, base);
    resolve.depth = state.DepthSurface(base, 512);
    resolve.copy_depth = depth;
    const uint32_t predicate = state.Word(12716);
    Require(state.Word(12724) == 1 && (predicate == 2 || predicate == 8) && state.Word(10932) == predicate,
            "Native scene resolve requires an explicit tile predicate");
    resolve.tile = predicate == 2 ? 0 : 1;
    const auto tile_state = 12760 + resolve.tile * 16;
    const auto top = state.Word(tile_state + 4), bottom = state.Word(tile_state + 12);
    std::vector<uint8_t> rectangle, origin, clear, destination;
    Require(ReadGuest(base, ctx.r5.u32, 16, rectangle) && LoadBE32(rectangle.data()) == 0 &&
            LoadBE32(rectangle.data() + 4) == top && LoadBE32(rectangle.data() + 8) == resolve.color.width &&
            LoadBE32(rectangle.data() + 12) == bottom, "Native resolve rectangle differs");
    Require(ReadGuest(base, ctx.r7.u32, 8, origin) && LoadBE32(origin.data()) == 0 && LoadBE32(origin.data() + 4) == top,
            "Native resolve destination origin differs");
    if (depth) {
        Require(ctx.f1.f64 == 0 && mc2::capture::GuestRange(ctx.r1.u32, 88) &&
                ReadGuest(base, ctx.r1.u32 + 84, 4, clear) && LoadBE32(clear.data()) == 0,
                "Native depth resolve requires a zero depth and stencil clear");
    } else {
        Require(ReadGuest(base, 0x82247720, 16, clear) &&
                std::all_of(clear.begin(), clear.end(), [](uint8_t byte) { return byte == 0; }),
                "Native color resolve requires the verified zero clear");
    }
    Require(ReadGuest(base, ctx.r6.u32, 52, destination) && (LoadBE32(destination.data()) & 15) == 3,
            "Invalid native resolve destination texture");
    const auto word = [&](uint32_t offset) { return LoadBE32(destination.data() + offset); };
    Require(word(28) == (depth ? 0x8a000002 : 0x8a000156) &&
            (word(32) & 4095) == (depth ? 0x97u : 0x60u) && (word(32) >> 12) != 0 &&
            word(36) == 0x59e4ff && word(40) == 0xd11 && word(44) == 0 && word(48) == 0xa00,
            "Native resolve requires the verified full-size tiled texture destination");
    resolve.destination.address = ctx.r6.u32;
    for (uint32_t i = 0; i < resolve.destination.fetch_words.size(); ++i)
        resolve.destination.fetch_words[i] = word(28 + i * 4);
    resolve.rectangle = plume::RenderRect(0, top, resolve.color.width, bottom);
    resolve.exponent = depth ? 0 : -3;
    return resolve;
}

ScenePassShaders LoadDepthRestoreShaders(const std::filesystem::path& directory) {
    // This shader uses the explicit UP declaration, which supplies the fetch
    // format and stride at draw time.
    return {{ReadNativeFile(directory / "vertex-shader.container"), ReadNativeFile(directory / "vertex.dxil"),
             {{0,0,95,16,0,4}}}, ReadNativeFile(directory / "pixel-shader.container"),
            ReadNativeFile(directory / "pixel.dxil")};
}

SceneDepthRestore ReadSceneDepthRestore(const PPCContext& ctx, uint8_t* base,
        const ScenePassShaders& shaders, const SceneResolveTexture& source) {
    Require(ctx.lr == 0x826DA868 && ctx.r4.u32 == 8 && ctx.r5.u32 == 3 && ctx.r6.u32 == 16,
            "Native depth restore requires the verified user-memory rectangle");
    const DeviceState state(base, ctx.r3.u32);
    SceneDepthRestore restore;
    restore.depth = state.DepthSurface(base, 720, 1);
    state.FullViewport(restore.depth);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX &&
            state.Word(10548) == 0x00700777 && state.Word(10460) == 0 && (state.Word(10556) & 0x1f) == 7 &&
            state.Word(10492) == 0x00ffff00 && state.Word(10496) == 0x00ffff00 && state.Word(10564) == 0x80000 &&
            state.Word(10568) == 0x18002 && state.Word(10572) == 0x43f &&
            state.Float(12672) == 1 && state.Float(12676) == 0 &&
            state.Float(10504) == 640 && state.Float(10508) == 640 && state.Float(10512) == -360 &&
            state.Float(10516) == 360 && state.Float(10520) == -1 && state.Float(10524) == 1,
            "Native depth restore depth/stencil, color mask or viewport state changed");
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference,
            "Untranslated native depth restore shaders");
    std::vector<uint8_t> declaration;
    Require(ReadGuest(base, state.Word(11824), 64, declaration) && (LoadBE32(declaration.data()) & 15) == 5,
            "Invalid native depth restore declaration");
    constexpr uint32_t declaration_words[] = {1,0,0xff000000,0,0,0,0,0,0x001a23a6,0};
    for (uint32_t i = 0; i < std::size(declaration_words); ++i)
        Require(LoadBE32(declaration.data() + 24 + i * 4) == declaration_words[i], "Native restore declaration differs");
    RequireSceneTexture(state, source, 0);
    ReadFullscreenConstants(state, restore.depth, restore.draw, restore.pixel_constants, true);
    return restore;
}

void ReadDepthRestoreVertices(DepthDraw& draw, uint8_t* base, uint32_t address) {
    constexpr float corners[][4] = {{0,0,0,1}, {1,0,0,1}, {0,1,0,1}, {1,1,0,1}};
    std::vector<uint8_t> raw;
    Require(ReadGuest(base, address, 3 * sizeof(corners[0]), raw), "Unreadable native depth restore vertices");
    draw.vertices = NativeVertexBytes(raw, 2);
    Require(std::memcmp(draw.vertices.data(), corners, draw.vertices.size()) == 0, "Native depth restore rectangle changed");
    draw.vertices.resize(sizeof(corners));
    // Expand the captured three-corner rectangle into two triangles.
    std::memcpy(draw.vertices.data() + raw.size(), corners[3], sizeof(corners[3]));
    draw.indices = {0,1,2,2,1,3};
    draw.index_count = uint32_t(draw.indices.size());
}

ScenePassShaders LoadFullscreenColorShaders(const std::filesystem::path& directory, bool grade) {
    auto vertex = LoadVertexShader(directory);
    const std::vector<VertexInput> copy_inputs = {{0,0,95,48,0,4}, {5,0,95,48,16,2}, {10,0,95,48,24,4},
        {10,1,95,48,40,4,false,VertexStorage::UNorm8}};
    const std::vector<VertexInput> grade_inputs = {{0,0,95,32,0,4,true}, {6,0,95,32,0,3},
        {7,0,95,32,12,4,false,VertexStorage::UInt8}, {3,0,95,32,16,4,false,VertexStorage::UInt8},
        {10,0,95,32,20,4,false,VertexStorage::UInt8}};
    Require(vertex.inputs == (grade ? grade_inputs : copy_inputs), "Fullscreen HDR vertex inputs changed");
    return {std::move(vertex), ReadNativeFile(directory / "pixel-shader.container"), ReadNativeFile(directory / "pixel-7e3.dxil")};
}

SceneFullscreenDraw ReadSceneFullscreenDraw(const PPCContext& ctx, uint8_t* base,
        const ScenePassShaders& shaders, const SceneResolveTexture& source, bool grade) {
    Require(grade ? (ctx.lr == 0x827B061C && ctx.r4.u32 == 5 && ctx.r5.u32 == 0 && ctx.r6.u32 == 4) :
        (ctx.lr == 0x826D9CC0 && ctx.r4.u32 == 4 && ctx.r5.u32 == 0 && ctx.r6.u32 == 4 &&
         ctx.r7.u32 == 6 && ctx.r8.u32 == 6 && ctx.r9.u32 == 48), "Unsupported fullscreen HDR draw arguments");
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadFullscreenColorSurface(state, base);
    std::vector<uint8_t> descriptor;
    Require(state.Word(11824) == 0 && state.Word(10452) == 0 && state.Word(10460) == (grade ? 7 : 15) &&
            state.Word(10548) == 0x00700770 && state.Word(10552) == 0x10001 && (state.Word(10556) & 0x1f) == 7 &&
            state.Word(10564) == 0x80000 && state.Word(10568) == (grade ? 0x18002 : 0x18000) && state.Word(10572) == 0x43f &&
            state.Float(12672) == (grade ? 1 : 0) && state.Float(12676) == (grade ? 0 : 1) &&
            state.Float(10504) == 640 && state.Float(10508) == 640 && state.Float(10512) == -360 &&
            state.Float(10516) == 360 && state.Float(10520) == (grade ? -1 : 1) && state.Float(10524) == (grade ? 1 : 0),
            "Fullscreen HDR blend, mask, depth or viewport state changed");
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated fullscreen HDR shaders");
    RequireSceneTexture(state, source, 0x4800);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, grade);
    if (grade) {
        std::vector<uint8_t> raw;
        uint32_t address = 0, size = 0;
        Require(state.bytes[12536] == 8 && ReadGuest(base,state.Word(12468),32,descriptor) &&
                mc2::draw_capture::BufferBytes(descriptor.data(), descriptor.size(), true, address, size) &&
                size == 128 && state.Word(1916) == LoadBE32(descriptor.data() + 28) &&
                (state.Word(1916) & 3) == 2 && ReadGuest(base,address,size,raw), "Invalid color grade vertex buffer");
        const auto vertex_address = LoadBE32(descriptor.data() + 24);
        const auto physical_fetch = (vertex_address & 0x1fffffff) + (((vertex_address >> 20) + 512) & 0x1000);
        Require(state.Word(1912) == physical_fetch, "Color grade vertex binding differs from its buffer");
        pass.draw.vertices = NativeVertexBytes(raw, 2);
        pass.draw.indices = {0,1,2,0,2,3};
        pass.draw.index_count = 6;
    } else {
        for (uint32_t i = 0; i < 16; ++i)
            Require(state.Float(1920 + (5 * 16) + 4 * i) == (i % 5 == 0 ? 1.f : 0.f), "Fullscreen HDR copy transform changed");
    }
    return pass;
}

void ReadFullscreenUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address) {
    std::vector<uint8_t> vertices, indices;
    Require(ReadGuest(base,address,192,vertices) && ReadGuest(base,index_address,24,indices), "Unreadable fullscreen HDR vertices or indices");
    const float half_x = 1.f / draw.viewport.width, half_y = 1.f / draw.viewport.height;
    for (uint32_t i = 0; i < 4; ++i) {
        const auto* position = vertices.data() + i * 48;
        const float x = std::bit_cast<float>(LoadBE32(position)), y = std::bit_cast<float>(LoadBE32(position + 4));
        Require(std::abs(x - ((i & 1 ? 1.f : -1.f) - half_x)) < 1.0e-6f &&
                std::abs(y - ((i & 2 ? -1.f : 1.f) + half_y)) < 1.0e-6f &&
                LoadBE32(position + 8) == 0 && LoadBE32(position + 12) == 0x3f800000, "HDR copy does not cover the full viewport");
    }
    draw.vertices = NativeVertexBytes(vertices, 2);
    draw.indices = {0,1,3,0,3,2};
    for (size_t i = 0; i < draw.indices.size(); ++i)
        Require(LoadBE32(indices.data() + i * 4) == draw.indices[i], "Fullscreen HDR copy indices changed");
    draw.index_count = uint32_t(draw.indices.size());
}

plume::RenderSamplerDesc FullscreenColorSampler() {
    plume::RenderSamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = plume::RenderFilter::NEAREST;
    sampler.mipmapMode = plume::RenderMipmapMode::NEAREST;
    sampler.addressU = sampler.addressV = plume::RenderTextureAddressMode::CLAMP;
    sampler.maxLOD = 0;
    return sampler;
}

SceneSurface ReadFullscreenResolve(const PPCContext& ctx, uint8_t* base, const SceneResolveTexture& destination) {
    Require(ctx.lr == 0x827B3694 && ctx.r4.u32 == 0xf4000000 && !ctx.r5.u32 && !ctx.r7.u32 && !ctx.r8.u32 && !ctx.r9.u32,
            "Unsupported fullscreen resolve flags or region");
    const DeviceState state(base, ctx.r3.u32);
    const auto surface = ReadFullscreenColorSurface(state, base);
    std::vector<uint8_t> descriptor;
    Require(ctx.r6.u32 == destination.address && ReadGuest(base, ctx.r6.u32, 52, descriptor) &&
            (LoadBE32(descriptor.data()) & 15) == 3, "Fullscreen resolve did not reuse the scene texture");
    for (uint32_t i = 0; i < destination.fetch_words.size(); ++i)
        Require(LoadBE32(descriptor.data() + 28 + i * 4) == destination.fetch_words[i], "Fullscreen resolve texture layout changed");
    return surface;
}

SceneColorClear ReadBloomClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.lr == 0x8238B590 && ctx.r4.u32 == 0xf, "Unsupported bloom color clear arguments");
    const DeviceState state(base, ctx.r3.u32);
    SceneColorClear clear;
    clear.surface = ReadBloomColorSurface(state, base);
    RequireFullClearRect(ctx, base, clear.surface);
    std::vector<uint8_t> value;
    Require(ReadGuest(base, ctx.r6.u32 ? ctx.r6.u32 : 0x82247720, 16, value) &&
            std::all_of(value.begin(), value.end(), [](uint8_t byte) { return byte == 0; }), "Bloom target clear requires zero");
    clear.value = plume::RenderColor(0,0,0,0);
    clear.rectangle = plume::RenderRect(0,0,clear.surface.width,clear.surface.height);
    return clear;
}

ScenePassShaders LoadBloomShaders(const std::filesystem::path& directory) {
    auto vertex = LoadVertexShader(directory);
    const std::vector<VertexInput> inputs = {{0,0,95,32,0,4}, {5,0,95,32,16,2}};
    Require(vertex.inputs == inputs, "Bloom vertex layout changed");
    return {std::move(vertex), ReadNativeFile(directory / "pixel-shader.container"), ReadNativeFile(directory / "pixel-fixed16.dxil")};
}

SceneFullscreenDraw ReadBloomDraw(const PPCContext& ctx, uint8_t* base, const ScenePassShaders& shaders,
        const SceneResolveTexture& color, const SceneResolveTexture& depth) {
    Require(ctx.lr == 0x826D9CC0 && ctx.r4.u32 == 4 && ctx.r5.u32 == 0 && ctx.r6.u32 == 4 && ctx.r7.u32 == 6 &&
            ctx.r8.u32 == 1 && ctx.r9.u32 == 32, "Unsupported indexed bloom draw arguments");
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadBloomColorSurface(state, base);
    Require(state.Word(11824) == 0 && state.Word(10452) == 0 && state.Word(10460) == 15 &&
            state.Word(10548) == 0x00700770 && state.Word(10552) == 0x10001 && (state.Word(10556) & 0x1f) == 7 &&
            state.Word(10564) == 0x80000 && state.Word(10568) == 0x18000 && state.Word(10572) == 0x43f &&
            state.Float(12672) == 0 && state.Float(12676) == 1 &&
            state.Float(10504) == 161 && state.Float(10508) == 161 && state.Float(10512) == -91 &&
            state.Float(10516) == 91 && state.Float(10520) == 1 && state.Float(10524) == 0,
            "Unsupported bloom viewport, write mask or blend state");
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated bloom shaders");
    RequireSceneTexture(state, depth, 0x4800);
    RequireSceneTexture(state, color, 0x4800, 1);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, false);
    const uint32_t slot = 1, mode = PackTextureFetchMode(0x688,0,0);
    auto* shared = pass.draw.shared_constants.data();
    std::memcpy(shared + 4, &slot, sizeof(slot));
    std::memcpy(shared + 196, &slot, sizeof(slot));
    std::memcpy(shared + 292, &mode, sizeof(mode));
    return pass;
}

void ReadBloomUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address) {
    std::vector<uint8_t> vertices, indices;
    Require(ReadGuest(base,address,128,vertices) && ReadGuest(base,index_address,12,indices), "Unreadable bloom vertices or indices");
    draw.vertices = NativeVertexBytes(vertices, 2);
    draw.indices = {0,1,3,0,3,2};
    for (size_t i = 0; i < draw.indices.size(); ++i)
        Require((uint32_t(indices[i*2]) << 8 | indices[i*2+1]) == draw.indices[i], "Bloom triangle indices changed");
    draw.index_count = uint32_t(draw.indices.size());
}

SceneSurface ReadBloomResolve(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.lr == 0x827B3694 && ctx.r4.u32 == 0 && !ctx.r5.u32 && !ctx.r7.u32 && !ctx.r8.u32 && !ctx.r9.u32,
            "Unsupported bloom resolve flags or region");
    const DeviceState state(base, ctx.r3.u32);
    const auto surface = ReadBloomColorSurface(state, base);
    std::vector<uint8_t> descriptor;
    Require(ReadGuest(base, ctx.r6.u32, 52, descriptor) && (LoadBE32(descriptor.data()) & 15) == 3,
            "Invalid bloom resolve destination");
    const auto word = [&](uint32_t offset) { return LoadBE32(descriptor.data() + offset); };
    Require(word(28) == 0x82c00002 && (word(32) & 4095) == 0x5a && (word(32) >> 12) != 0 &&
            word(36) == 0x16a141 && word(40) == 0xd10 && word(44) == 0 && word(48) == 0x200,
            "Bloom resolve requires the verified 322x182 RGBA16_UNORM texture");
    return surface;
}
}
