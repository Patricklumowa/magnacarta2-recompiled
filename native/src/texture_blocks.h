#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mc2::texture {
struct BlockMip {
    uint32_t width=0,height=0,physical_address=0,source_bytes=0,row_pitch_bytes=0;
    uint32_t offset_x=0,offset_y=0;
};
struct BlockTexture {
    std::array<uint32_t,6> fetch_words{};
    uint32_t format=0,block_bytes=0,endianness=0,swizzle=0,gamma_mask=0;
    uint32_t mip_min_level=0,mip_max_level=0;
    int32_t exp_adjust=0;
    bool tiled=false;
    std::vector<BlockMip> mips;
};
// BC1/BC3 retain their compressed blocks for native GPU sampling. Only storage
// order and byte order change here; sampler, swizzle and gamma remain explicit.
bool DescribeBlockTexture(std::span<const uint8_t> fetch_be,BlockTexture& output,std::string& error);
bool LinearizeBlockMip(const BlockTexture& texture,uint32_t level,std::span<const uint8_t> guest_bytes,
                       std::vector<uint8_t>& blocks,std::string& error);
}
