#include "texture_blocks.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <algorithm>
#include <cstring>

namespace mc2::texture {
namespace {
namespace xenos=rex::graphics::xenos;
namespace layout=rex::graphics::texture_util;
constexpr uint64_t kLimit=64*1024*1024;
bool Fail(std::string& error,const char* message) { error=message; return false; }
uint64_t Offset(const BlockTexture& texture,const BlockMip& mip,uint32_t x,uint32_t y) {
    x+=mip.offset_x; y+=mip.offset_y;
    if (texture.tiled)
        return uint32_t(layout::GetTiledOffset2D(int32_t(x),int32_t(y),mip.row_pitch_bytes/texture.block_bytes,
                                               texture.block_bytes==8 ? 3 : 4));
    return uint64_t(y)*mip.row_pitch_bytes+uint64_t(x)*texture.block_bytes;
}
}
bool DescribeBlockTexture(std::span<const uint8_t> bytes,BlockTexture& output,std::string& error) {
    error.clear();
    if (bytes.size()!=24) return Fail(error,"Compressed texture fetch must contain 24 bytes");
    BlockTexture result;
    for (size_t i=0;i<6;++i)
        result.fetch_words[i]=uint32_t(bytes[i*4])<<24|uint32_t(bytes[i*4+1])<<16|
                              uint32_t(bytes[i*4+2])<<8|bytes[i*4+3];
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch,result.fetch_words.data(),sizeof(fetch));
    if (fetch.type!=xenos::FetchConstantType::kTexture || fetch.dimension!=xenos::DataDimension::k2DOrStacked ||
        fetch.stacked || fetch.num_format || !fetch.pitch)
        return Fail(error,"Unsupported compressed texture dimensionality, pitch or number format");
    result.format=uint32_t(fetch.format);
    if (result.format!=18 && result.format!=20) return Fail(error,"Only BC1 and BC3 compressed textures are supported");
    result.block_bytes=result.format==18 ? 8 : 16;
    result.tiled=fetch.tiled; result.endianness=uint32_t(fetch.endianness);
    result.swizzle=fetch.swizzle; result.exp_adjust=fetch.exp_adjust;
    const uint32_t signs[]={uint32_t(fetch.sign_x),uint32_t(fetch.sign_y),uint32_t(fetch.sign_z),uint32_t(fetch.sign_w)};
    for (uint32_t i=0;i<4;++i) {
        const auto selector=(result.swizzle>>(i*3))&7;
        if (selector>=6) return Fail(error,"Reserved compressed texture swizzle");
        if (selector>=4) continue;
        if (signs[selector]==uint32_t(xenos::TextureSign::kGamma)) result.gamma_mask|=1u<<i;
        else if (signs[selector]!=uint32_t(xenos::TextureSign::kUnsigned))
            return Fail(error,"Signed compressed texture sampling is not implemented");
    }
    const uint32_t width=fetch.size_2d.width+1,height=fetch.size_2d.height+1;
    if (uint64_t(width)*height*4>kLimit) return Fail(error,"Compressed texture exceeds capture limits");
    uint32_t base_page=0,mip_page=0;
    layout::GetSubresourcesFromFetchConstant(fetch,nullptr,nullptr,nullptr,&base_page,&mip_page,
                                            &result.mip_min_level,&result.mip_max_level);
    if (!base_page || (result.mip_max_level && !mip_page)) return Fail(error,"Compressed texture has missing storage");
    if (result.mip_max_level>=xenos::kTextureMaxMips) return Fail(error,"Invalid compressed mip range");
    const auto guest=layout::GetGuestTextureLayout(fetch.dimension,fetch.pitch,width,height,1,result.tiled,
                                                   fetch.format,fetch.packed_mips,true,result.mip_max_level);
    uint64_t total=0;
    for (uint32_t level=0;level<=result.mip_max_level;++level) {
        BlockMip mip;
        mip.width=std::max(width>>level,1u); mip.height=std::max(height>>level,1u);
        const auto storage_level=std::min(level,guest.packed_level);
        const auto& storage=level ? guest.mips[storage_level] : guest.base;
        const uint64_t address=uint64_t(level ? mip_page : base_page)*4096+
                               (level ? guest.mip_offsets_bytes[storage_level] : 0);
        mip.row_pitch_bytes=storage.row_pitch_bytes;
        if (!mip.row_pitch_bytes || mip.row_pitch_bytes%result.block_bytes)
            return Fail(error,"Invalid compressed texture row pitch");
        if (fetch.packed_mips) {
            uint32_t z=0;
            layout::GetPackedMipOffset(width,height,1,fetch.format,level,mip.offset_x,mip.offset_y,z);
        }
        const uint32_t columns=(mip.width+3)/4,rows=(mip.height+3)/4;
        uint64_t extent=0;
        if (result.tiled) {
            for (uint32_t y=0;y<rows;++y) for (uint32_t x=0;x<columns;++x)
                extent=std::max(extent,Offset(result,mip,x,y)+result.block_bytes);
        } else extent=Offset(result,mip,columns-1,rows-1)+result.block_bytes;
        total+=uint64_t(columns)*rows*result.block_bytes;
        if (!extent || extent>kLimit || address+extent>0x20000000ull || total>kLimit)
            return Fail(error,"Compressed mip exceeds physical memory or capture limits");
        mip.physical_address=uint32_t(address); mip.source_bytes=uint32_t(extent);
        result.mips.push_back(mip);
    }
    output=std::move(result); return true;
}
bool LinearizeBlockMip(const BlockTexture& texture,uint32_t level,std::span<const uint8_t> guest_bytes,
                       std::vector<uint8_t>& blocks,std::string& error) {
    std::array<uint8_t,24> fetch;
    for (size_t i=0;i<6;++i) for (uint32_t b=0;b<4;++b)
        fetch[i*4+b]=uint8_t(texture.fetch_words[i]>>(24-b*8));
    BlockTexture validated;
    if (!DescribeBlockTexture(fetch,validated,error)) return false;
    if (level>=validated.mips.size()) return Fail(error,"Compressed mip index is out of range");
    const auto& mip=validated.mips[level];
    if (guest_bytes.size()<mip.source_bytes) return Fail(error,"Truncated compressed mip payload");
    const uint32_t columns=(mip.width+3)/4,rows=(mip.height+3)/4;
    std::vector<uint8_t> linear(size_t(columns)*rows*validated.block_bytes);
    constexpr uint32_t endian_xor[]={0,1,3,2};
    for (uint32_t y=0;y<rows;++y) for (uint32_t x=0;x<columns;++x) {
        const auto source=Offset(validated,mip,x,y);
        const auto destination=(size_t(y)*columns+x)*validated.block_bytes;
        for (uint32_t b=0;b<validated.block_bytes;++b)
            linear[destination+b]=guest_bytes[(source+b)^endian_xor[validated.endianness]];
    }
    blocks=std::move(linear); return true;
}
}
