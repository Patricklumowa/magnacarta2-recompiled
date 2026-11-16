#include "generated/default/magnacarta2_init.h"
#include "native_scene.h"
#include "native_shader_input.h"
#include "shader_capture.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
template<class F> void Reject(F&& action) {
    bool rejected=false; try { action(); } catch (const std::runtime_error&) { rejected=true; }
    Check(rejected,"Unsupported native scene state was accepted");
}
}
int main(int argc,char** argv) try {
    using namespace mc2::native;
    using mc2::capture::LoadBE32;
    Check(argc==3,"Usage: mc2_native_scene_check <captured-depth-draw-directory> <verified-depth-shader-directory>");
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
    const auto virtual_size=LoadBE32(shaders.reference.data()+4);
    store(vs,6); store(vs+32,0xb000);
    Check(vs+872ull+virtual_size<memory.size() && shaders.reference.size()>=virtual_size &&
          0xb000ull+shaders.reference.size()-virtual_size<=memory.size(),"Invalid shader fixture");
    std::memcpy(base+vs+872,shaders.reference.data(),virtual_size);
    std::memcpy(base+0xb000,shaders.reference.data()+virtual_size,shaders.reference.size()-virtual_size);
    PPCContext context{}; context.r3.u32=device; context.r4.u32=4; context.r5.u32=6; context.r6.u32=18;
    const auto original=memory; const auto original_context=context;
    const auto draw=ReadSceneDepthDraw(context,base,shaders);
    Check(draw.draw.vertices==NativeVertexBytes(vertices,2) && draw.draw.indices.size()*4==indices.size(),
          "Native scene snapshot did not preserve geometry");
    for (size_t i=0;i<draw.draw.indices.size();++i)
        Check(draw.draw.indices[i]==LoadBE32(indices.data()+i*4),"Native scene index byte order differs");
    Check(draw.draw.first_index==6 && draw.draw.index_count==18 && draw.surface.width==1280 &&
          draw.surface.height==720 && draw.surface.samples==2,"Native scene draw range or surface differs");
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
    std::cout<<"PASS: captured scene input reconstruction, unchanged guest state, clear-to-draw surface identity and unsupported-state rejection\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr<<"Native scene check: "<<error.what()<<'\n'; return 1;
}
