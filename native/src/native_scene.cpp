#include "generated/default/magnacarta2_init.h"
#include "native_scene.h"
#include "native_shader_input.h"
#include "guest_graphics_read.h"
#include "draw_capture.h"
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
    SceneSurface Surface(uint8_t* base) const {
        SceneSurface result;
        for (uint32_t i = 0; i < 4; ++i)
            Require(Word(12448+i*4) == 0, "Color-target scene passes are not implemented");
        result.address = Word(12464);
        std::vector<uint8_t> descriptor;
        Require(ReadGuest(base,result.address,48,descriptor) && (LoadBE32(descriptor.data()) & 15) == 4,
                "Invalid native depth surface");
        const auto surface_info = LoadBE32(descriptor.data()+24), dimensions = LoadBE32(descriptor.data()+36);
        result.width = ((dimensions>>18)&16383)+1; result.height = ((dimensions>>3)&32767)+1;
        result.samples = 1u << ((surface_info>>16)&3);
        Require(result.width == 1280 && result.height == 720 && result.samples == 2 &&
                LoadBE32(descriptor.data()+32) == 17 && (surface_info & 16383) == 1280,
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
}
DepthShaders LoadDepthShaders(const std::filesystem::path& directory) {
    DepthShaders shaders;
    shaders.reference = ReadNativeFile(directory/"vertex-shader.container");
    shaders.dxil = ReadNativeFile(directory/"vertex.dxil");
    shaders.inputs = ReadVertexInputs(shaders.reference);
    Require(shaders.inputs.size() == 3 && shaders.inputs[0].constant_only && shaders.inputs[0].usage == 0 &&
            shaders.inputs[1] == VertexInput{10,0,95,40,0,3,false} &&
            shaders.inputs[2] == VertexInput{5,0,95,40,24,2,false},
            "Scene depth shader input layout differs from the verified shader");
    return shaders;
}
DepthConfiguration ConfigureDepth(const SceneSurface& surface, const DepthShaders& shaders) {
    DepthConfiguration configuration;
    configuration.width=surface.width; configuration.height=surface.height; configuration.sample_count=surface.samples;
    configuration.vertex_shader=shaders.dxil; configuration.inputs=shaders.inputs;
    configuration.cull=plume::RenderCullMode::FRONT;
    return configuration;
}
DepthClear ReadDepthClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.r4.u32 == 0x30, "Only the verified full depth/stencil clear is supported");
    DeviceState state(base,ctx.r3.u32);
    DepthClear clear; clear.surface=state.Surface(base); state.FullViewport(clear.surface);
    std::vector<uint8_t> rect;
    Require(ReadGuest(base,ctx.r5.u32,16,rect) && LoadBE32(rect.data()) == 0 && LoadBE32(rect.data()+4) == 0 &&
            LoadBE32(rect.data()+8) == clear.surface.width && LoadBE32(rect.data()+12) == clear.surface.height,
            "Only the verified full surface clear is supported");
    Require(std::isfinite(ctx.f1.f64) && ctx.f1.f64 == 0, "Scene clear value differs from the verified zero depth/stencil clear");
    clear.value=0;
    return clear;
}
SceneColorClear ReadSceneColorClear(const PPCContext& ctx,uint8_t* base) {
    Require(ctx.r4.u32==0xf,"Only the verified scene color clear is supported");
    DeviceState state(base,ctx.r3.u32);
    Require(state.Word(12448)!=0 && state.Word(12452)==0 && state.Word(12456)==0 && state.Word(12460)==0,
            "Native color clear requires the verified single attachment");
    SceneColorClear clear;
    auto& surface=clear.surface;
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
            "Scene color clear has an unsupported tiling configuration");
    constexpr uint32_t tiles[]={0,0,1280,512,0,512,1280,720};
    for (uint32_t i=0;i<std::size(tiles);++i)
        Require(state.Word(12760+i*4)==tiles[i],"Scene tile rectangles changed");
    // Engine begin-frame sets predicate bit 1 for the first tile before this
    // clear. Other tiles are resolved/cleared later; never clear them early.
    Require(state.Word(12724)==1 && state.Word(12716)==2 && state.Word(10932)==2,
            "Scene color clear uses an unsupported tile predicate");
    surface.width=1280; surface.height=720; surface.samples=2;
    surface.description.assign(descriptor.begin()+24,descriptor.end());
    state.FullViewport(surface);
    std::vector<uint8_t> rect,color;
    Require(ReadGuest(base,ctx.r5.u32,16,rect) && LoadBE32(rect.data())==0 && LoadBE32(rect.data()+4)==0 &&
            LoadBE32(rect.data()+8)==1280 && LoadBE32(rect.data()+12)==720,"Scene color clear rectangle changed");
    Require(ReadGuest(base,ctx.r6.u32 ? ctx.r6.u32 : 0x82247720,16,color),"Unreadable scene clear color");
    for (uint32_t i=0;i<4;++i) {
        clear.value.rgba[i]=std::bit_cast<float>(LoadBE32(color.data()+i*4));
        // Zero is exact in both the original 7e3/2-bit-alpha format and native
        // float16 storage. General HDR quantization/blending is still pending.
        Require(clear.value.rgba[i]==0,"Only the verified zero HDR clear is supported");
    }
    clear.rectangle=plume::RenderRect(0,0,1280,512);
    return clear;
}
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& ctx, uint8_t* base, const DepthShaders& shaders) {
    Require(ctx.r4.u32 == 4, "Only native triangle-list depth draws are supported");
    DeviceState state(base,ctx.r3.u32);
    SceneDepthDraw snapshot; snapshot.surface=state.Surface(base); state.FullViewport(snapshot.surface);
    Require(state.Word(12700) == 0 && state.Word(11824) == 0, "Pixel shaders or declared scene inputs are not implemented");
    Require(state.Word(10548) == 0x00700766 && state.Word(10568) == 0x00018002 && state.Word(10572) == 0x43f &&
            state.Word(10564) == 0x80000 && state.Word(10452) == 0 && state.Float(12672) == 1 && state.Float(12676) == 0,
            "Depth/stencil, culling, clipping, vertex base or viewport state differs from the verified pass");
    Require(ReadBoundShader(base,state.Word(12704),true) == shaders.reference, "Untranslated native scene vertex shader");
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
    auto& draw=snapshot.draw;
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
    const float half_pixel[] = {1.0f/snapshot.surface.width,-1.0f/snapshot.surface.height};
    std::memcpy(draw.shared_constants.data()+264,half_pixel,sizeof(half_pixel));
    draw.viewport=plume::RenderViewport(0,0,float(snapshot.surface.width),float(snapshot.surface.height),1,0);
    draw.scissor=plume::RenderRect(0,0,snapshot.surface.width,snapshot.surface.height);
    return snapshot;
}
}
