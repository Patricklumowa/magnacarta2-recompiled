#include "native_sampled_texture.h"
#include <plume_d3d12.h>
#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition,const char* error) { if (!condition) throw std::runtime_error(error); }
template<class F> void Reject(F&& action) {
    bool failed=false;
    try { action(); } catch (const std::runtime_error&) { failed=true; }
    Check(failed,"Invalid native texture was accepted");
}
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Check(bool(file),"Cannot open local BC mip fixture");
    const auto size=file.tellg();
    Check(size>0 && uint64_t(size)<=16*1024*1024,"Invalid BC mip fixture extent");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    Check(bool(file),"Short BC mip fixture read"); return bytes;
}
void CheckFootprints(const mc2::native::SampledTexture& texture,const mc2::native::NativeDevice& gpu,
                     uint32_t mip_count,uint32_t block_bytes) {
    const auto* native=static_cast<const plume::D3D12Texture*>(texture.Texture());
    const auto desc=native->d3d->GetDesc();
    Check(desc.MipLevels==mip_count,"GPU texture does not expose the entire mip chain");
    // Ask D3D12 independently for each actual subresource's copy footprint.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mip_count);
    std::vector<UINT> rows(mip_count);
    std::vector<UINT64> row_bytes(mip_count);
    UINT64 size=0;
    auto* device=static_cast<plume::D3D12Device*>(gpu.device.get())->d3d;
    device->GetCopyableFootprints(&desc,0,mip_count,0,footprints.data(),rows.data(),row_bytes.data(),&size);
    Check(size!=UINT64_MAX,"D3D12 rejected the mip copy layout");
    for (uint32_t level=0;level<mip_count;++level) {
        const auto width=std::max(uint32_t(desc.Width)>>level,1u),height=std::max(desc.Height>>level,1u);
        const auto& footprint=footprints[level];
        Check(footprint.Offset%512==0 && footprint.Footprint.Width==(width+3)/4*4 &&
              footprint.Footprint.Height==(height+3)/4*4 && rows[level]==(height+3)/4 &&
              row_bytes[level]==uint64_t((width+3)/4)*block_bytes && footprint.Footprint.RowPitch%256==0,
              "Native copy assumptions differ from D3D12's subresource footprints");
    }
}
}
int main(int argc,char** argv) try {
    using namespace mc2::native;
    using namespace plume;
    Check(argc==1 || argc==2,"Usage: mc2_native_texture_check [first-color-capture/textures]");
    auto gpu=std::make_shared<NativeDevice>(true);
    for (auto format:{RenderFormat::BC1_UNORM,RenderFormat::BC3_UNORM}) {
        const auto block_bytes=format==RenderFormat::BC1_UNORM ? 8u : 16u;
        for (const auto size:{std::array{4u,4u},std::array{8u,12u},std::array{132u,68u},std::array{64u,64u}}) {
            const auto count=std::bit_width(std::max(size[0],size[1]));
            std::vector<std::vector<uint8_t>> blocks(count);
            for (uint32_t level=0;level<count;++level) {
                const auto width=std::max(size[0]>>level,1u),height=std::max(size[1]>>level,1u);
                blocks[level].resize(size_t((width+3)/4)*((height+3)/4)*block_bytes);
                for (size_t i=0;i<blocks[level].size();++i)
                    blocks[level][i]=uint8_t(i*17+(i/7)*11+level*31+block_bytes);
            }
            const auto expected=blocks;
            const auto prior=gpu->completed;
            SampledTexture texture(format,size[0],size[1],blocks,gpu);
            Check(gpu->completed==prior+1,"Texture upload did not complete its real GPU fence");
            CheckFootprints(texture,*gpu,count,block_bytes);
            for (auto& mip:blocks) std::fill(mip.begin(),mip.end(),0x39);
            SampledTexture second(format,size[0],size[1],blocks,gpu);
            Check(texture.ReadBlocks()==expected && second.ReadBlocks()==blocks,"Native BC mip roundtrip differs");
            Check(texture.ReadBlocks()==expected,"Native BC texture did not retain independent GPU contents");
        }
    }
    std::vector<std::vector<uint8_t>> faces;
    for (uint8_t face=0;face<6;++face) {
        faces.emplace_back(32,uint8_t(2*face+1));
        faces.emplace_back(8,uint8_t(2*face+2));
    }
    SampledTexture cube(RenderFormat::BC1_UNORM,8,8,faces,gpu,6);
    const auto cube_desc=static_cast<const D3D12Texture*>(cube.Texture())->d3d->GetDesc();
    Check(cube.CubeView() && cube_desc.DepthOrArraySize==6 && cube_desc.MipLevels==2 && cube.ReadBlocks()==faces,
          "Native cubemap face and mip roundtrip differs");
    std::cout<<"PASS: all six cubemap faces and two mips byte-exact after GPU upload/readback\n";
    const std::vector<std::vector<uint8_t>> one{{0,1,2,3,4,5,6,7}};
    const auto prior=gpu->completed;
    Reject([&] { SampledTexture invalid(RenderFormat::BC1_UNORM,0,4,one,gpu); });
    Reject([&] { SampledTexture invalid(RenderFormat::BC1_UNORM,7,4,one,gpu); });
    Reject([&] { SampledTexture invalid(RenderFormat::BC1_UNORM,8196,4,one,gpu); });
    Reject([&] { SampledTexture invalid(RenderFormat::BC1_UNORM,4,4,{},gpu); });
    Reject([&] { SampledTexture invalid(RenderFormat::BC3_UNORM,4,4,one,gpu); });
    Reject([&] { SampledTexture invalid(RenderFormat::R8G8B8A8_UNORM,4,4,one,gpu); });
    Reject([&] { SampledTexture invalid(RenderFormat::BC1_UNORM,4,4,one,nullptr); });
    auto too_many=one; too_many.resize(4,one.front());
    Reject([&] { SampledTexture invalid(RenderFormat::BC1_UNORM,4,4,too_many,gpu); });
    Check(gpu->completed==prior,"Rejected texture advanced the GPU submission timeline");
    if (argc==2) {
        for (uint32_t slot=0;slot<2;++slot) {
            std::vector<std::vector<uint8_t>> blocks;
            for (uint32_t level=0;level<7;++level)
                blocks.push_back(Read(std::filesystem::path(argv[1])/(
                    "slot-0"+std::to_string(slot)+".mip-0"+std::to_string(level)+".blocks.bin")));
            SampledTexture texture(slot ? RenderFormat::BC3_UNORM : RenderFormat::BC1_UNORM,64,64,blocks,gpu);
            Check(texture.ReadBlocks()==blocks,"Captured BC mip payload changed during GPU upload/readback");
        }
        std::cout<<"PASS: both captured scene textures, all 14 mip levels byte-exact after GPU upload/readback\n";
    }
    std::cout<<"PASS: BC1/BC3, non-square mips, partial edge blocks, tiny mip tails, retained contents, input rejection and fences; debug_layer="
             <<gpu->debug_enabled<<", completed="<<gpu->completed<<'\n';
    return 0;
} catch (const std::exception& error) { std::cerr<<"Native texture check: "<<error.what()<<'\n'; return 1; }
