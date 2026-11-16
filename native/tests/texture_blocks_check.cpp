#include "texture_blocks.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const char* error) { if (!value) throw std::runtime_error(error); }
std::array<uint8_t,24> Fetch(uint32_t width,uint32_t height,uint32_t format,bool tiled,uint32_t endian=0,
                            bool packed=false,uint32_t max_mip=0,uint32_t pitch=4) {
    const uint32_t words[]={2u|(pitch<<22)|(uint32_t(tiled)<<31),0x100000u|format|(endian<<6),
        (width-1)|((height-1)<<13),0x688u<<1,max_mip<<6,0x200000u|(1u<<9)|(uint32_t(packed)<<11)};
    std::array<uint8_t,24> result{};
    for (size_t i=0;i<6;++i) for (uint32_t b=0;b<4;++b) result[i*4+b]=uint8_t(words[i]>>(24-b*8));
    return result;
}
}
int main() try {
    using namespace mc2::texture;
    std::string error;
    BlockTexture texture;
    std::vector<uint8_t> blocks;
    for (auto format:{18u,20u}) for (uint32_t endian=0;endian<4;++endian) {
        Check(DescribeBlockTexture(Fetch(9,7,format,false,endian),texture,error),error.c_str());
        const auto size=format==18 ? 8u : 16u;
        const auto row_pitch=32*size; // Fetch pitch is 128 texels = 32 BC blocks.
        const auto& mip=texture.mips[0];
        Check(mip.width==9 && mip.height==7 && mip.row_pitch_bytes==row_pitch && mip.source_bytes==row_pitch+3*size,
              "Linear compressed texture included unused row padding");
        std::vector<uint8_t> raw(mip.source_bytes);
        for (uint32_t y=0;y<2;++y) for (uint32_t b=0;b<3*size;++b) raw[y*row_pitch+b]=uint8_t(y*64+b);
        Check(LinearizeBlockMip(texture,0,raw,blocks,error),error.c_str());
        constexpr uint32_t xor_bytes[]={0,1,3,2};
        Check(blocks.size()==6*size,"Wrong compressed linear extent");
        for (uint32_t y=0;y<2;++y) for (uint32_t b=0;b<3*size;++b)
            Check(blocks[y*3*size+b]==uint8_t(y*64+(b^xor_bytes[endian])),"Compressed block byte order differs");
        const auto before=blocks;
        auto altered=texture; altered.mips.clear(); altered.block_bytes=1;
        Check(LinearizeBlockMip(altered,0,raw,blocks,error) && blocks==before,"Caller fields overrode canonical fetch bounds");
        raw.pop_back();
        Check(!LinearizeBlockMip(texture,0,raw,blocks,error) && blocks==before,"Truncated mip changed output");
        Check(!LinearizeBlockMip(texture,1,raw,blocks,error),"Out-of-range mip was accepted");
    }
    for (auto format:{18u,20u}) for (bool tiled:{false,true}) {
        Check(DescribeBlockTexture(Fetch(64,64,format,tiled,1,true,15),texture,error),error.c_str());
        Check(texture.mips.size()==7 && texture.mip_max_level==6,"Mip range did not match texture size");
        const std::array<std::array<uint32_t,2>,7> offsets{{{0,0},{0,0},{4,0},{2,0},{1,0},{0,2},{0,1}}};
        for (uint32_t level=0;level<7;++level) {
            const auto& mip=texture.mips[level];
            Check(mip.width==(64u>>level) && mip.height==(64u>>level) && mip.offset_x==offsets[level][0] &&
                  mip.offset_y==offsets[level][1],"Packed BC mip dimensions/offsets differ");
            Check(level ? mip.physical_address>=0x200000 : mip.physical_address==0x100000,"Base and mip storage were mixed");
            if (level>2) Check(mip.physical_address==texture.mips[2].physical_address,"Packed levels do not share storage");
            std::vector<uint8_t> raw(mip.source_bytes,uint8_t(level+1));
            Check(LinearizeBlockMip(texture,level,raw,blocks,error),error.c_str());
            Check(blocks.size()==size_t((mip.width+3)/4)*((mip.height+3)/4)*texture.block_bytes &&
                  std::all_of(blocks.begin(),blocks.end(),[&](auto b){return b==level+1;}),"Mip output extent/data differs");
        }
    }
    for (auto format:{18u,20u}) {
        Check(DescribeBlockTexture(Fetch(132,132,format,true,0,false,0,8),texture,error),error.c_str());
        const auto size=texture.block_bytes, tile_bytes=32*32*size;
        // Fixed coordinates exercise microtile byte interleaving and transitions
        // between 32x32-block tiles. No address helper builds this fixture.
        const uint32_t points[][3]={{0,0,0},{1,0,size==8 ? 8u : 32u},{0,1,16},
            {1,1,size==8 ? 24u : 48u},{32,0,tile_bytes},{0,32,tile_bytes*2},{32,32,tile_bytes*3}};
        std::vector<uint8_t> raw(texture.mips[0].source_bytes);
        for (uint32_t i=0;i<std::size(points);++i) raw.at(points[i][2])=uint8_t(i+1);
        Check(LinearizeBlockMip(texture,0,raw,blocks,error),error.c_str());
        for (uint32_t i=0;i<std::size(points);++i)
            Check(blocks[(points[i][1]*33+points[i][0])*size]==i+1,"Compressed micro/macro tile addressing differs");
        Check(DescribeBlockTexture(Fetch(16,16,format,true,0,true,15),texture,error),error.c_str());
        Check(texture.mips.size()==5 && texture.mips[0].offset_x==4 && texture.mips[0].physical_address==0x100000 &&
              texture.mips[1].offset_x==2 && texture.mips[1].physical_address==0x200000 &&
              texture.mips[4].physical_address==0x200000,"Packed base and mip tails were conflated");
    }
    auto fetch=Fetch(64,64,20,true,1,true,6); fetch[3]|=0xfc;
    Check(DescribeBlockTexture(fetch,texture,error) && texture.gamma_mask==7,"Compressed RGB gamma metadata was lost");
    const auto prior=texture.fetch_words;
    Check(!DescribeBlockTexture(Fetch(64,64,6,false),texture,error) && texture.fetch_words==prior,"Unsupported format changed output");
    fetch=Fetch(64,64,18,false); fetch[3]|=4;
    Check(!DescribeBlockTexture(fetch,texture,error),"Signed BC sampling was accepted as UNORM");
    std::cout<<"PASS: BC1/BC3 blocks, all endian modes, partial blocks, packed mips, separate storage, gamma metadata and bounds\n";
    return 0;
} catch (const std::exception& error) { std::cerr<<"Texture block check: "<<error.what()<<'\n'; return 1; }
