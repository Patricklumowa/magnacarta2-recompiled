#include "native_sampled_texture.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

using namespace plume;
namespace mc2::native {
namespace {
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
struct SampledTexture::Impl {
    struct Mip {
        uint32_t width,height,rows,row_bytes,pitch;
        size_t offset;
    };
    std::shared_ptr<NativeDevice> gpu;
    RenderFormat format;
    uint32_t block_bytes;
    std::vector<Mip> mips;
    size_t transfer_bytes=0;
    std::unique_ptr<RenderTexture> texture;

    RenderTextureCopyLocation Footprint(const RenderBuffer* buffer,const Mip& mip) const {
        // plume's rowWidth is in texels, even for block-compressed formats.
        return RenderTextureCopyLocation::PlacedFootprint(buffer,format,mip.width,mip.height,1,
                                                          mip.pitch/block_bytes*4,mip.offset);
    }
    Impl(RenderFormat type,uint32_t width,uint32_t height,std::span<const std::vector<uint8_t>> blocks,
         std::shared_ptr<NativeDevice> context) : gpu(std::move(context)),format(type) {
        Require(gpu && width && height && width<=8192 && height<=8192,"Invalid native sampled texture extent");
        // Unaligned top-level BC extents require an optional D3D12 feature.
        Require(!(width%4) && !(height%4),"Native BC top-level extent must be a multiple of four");
        Require(format==RenderFormat::BC1_UNORM || format==RenderFormat::BC3_UNORM,"Unsupported native block format");
        Require(!blocks.empty() && blocks.size()<=std::bit_width(std::max(width,height)),"Invalid native mip count");
        block_bytes=format==RenderFormat::BC1_UNORM ? 8 : 16;
        for (uint32_t level=0;level<blocks.size();++level) {
            Mip mip{};
            mip.width=std::max(width>>level,1u); mip.height=std::max(height>>level,1u);
            mip.rows=(mip.height+3)/4; mip.row_bytes=(mip.width+3)/4*block_bytes;
            Require(blocks[level].size()==size_t(mip.row_bytes)*mip.rows,"Native BC mip payload extent differs");
            mip.pitch=(mip.row_bytes+255)&~255u;
            mip.offset=(transfer_bytes+511)&~size_t(511);
            transfer_bytes=mip.offset+size_t(mip.pitch)*mip.rows;
            Require(transfer_bytes<=64*1024*1024,"Native BC upload exceeds the bounded texture size");
            mips.push_back(mip);
        }
        texture=gpu->device->createTexture(RenderTextureDesc::Texture2D(width,height,uint32_t(mips.size()),format));
        auto upload=gpu->device->createBuffer(RenderBufferDesc::UploadBuffer(transfer_bytes));
        Require(texture && upload,"Native BC texture allocation failed");
        auto* mapped=static_cast<uint8_t*>(upload->map());
        Require(mapped!=nullptr,"Native BC upload mapping failed");
        std::memset(mapped,0,transfer_bytes);
        for (uint32_t level=0;level<mips.size();++level) {
            const auto& mip=mips[level];
            for (uint32_t row=0;row<mip.rows;++row)
                std::memcpy(mapped+mip.offset+size_t(row)*mip.pitch,
                            blocks[level].data()+size_t(row)*mip.row_bytes,mip.row_bytes);
        }
        upload->unmap();
        auto& commands=gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture.get(),RenderTextureLayout::COPY_DEST));
        for (uint32_t level=0;level<mips.size();++level)
            commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get(),level),Footprint(upload.get(),mips[level]));
        commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(texture.get(),RenderTextureLayout::SHADER_READ));
        commands->end();
        gpu->queue->executeCommandLists(commands.get(),gpu->fence.get());
        // The temporary upload can be released only after actual completion.
        gpu->Complete();
    }
    std::vector<std::vector<uint8_t>> ReadBlocks() {
        auto readback=gpu->device->createBuffer(RenderBufferDesc::ReadbackBuffer(transfer_bytes));
        Require(bool(readback),"Native BC readback allocation failed");
        auto& commands=gpu->commands;
        commands->begin();
        commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture.get(),RenderTextureLayout::COPY_SOURCE));
        for (uint32_t level=0;level<mips.size();++level)
            commands->copyTextureRegion(Footprint(readback.get(),mips[level]),RenderTextureCopyLocation::Subresource(texture.get(),level));
        commands->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(texture.get(),RenderTextureLayout::SHADER_READ));
        commands->end();
        gpu->queue->executeCommandLists(commands.get(),gpu->fence.get()); gpu->Complete();
        std::vector<std::vector<uint8_t>> blocks(mips.size());
        for (size_t level=0;level<mips.size();++level)
            blocks[level].resize(size_t(mips[level].row_bytes)*mips[level].rows);
        const auto* mapped=static_cast<const uint8_t*>(readback->map());
        Require(mapped!=nullptr,"Native BC readback mapping failed");
        for (size_t level=0;level<mips.size();++level) {
            const auto& mip=mips[level];
            for (uint32_t row=0;row<mip.rows;++row)
                std::memcpy(blocks[level].data()+size_t(row)*mip.row_bytes,
                            mapped+mip.offset+size_t(row)*mip.pitch,mip.row_bytes);
        }
        readback->unmap();
        return blocks;
    }
};
SampledTexture::SampledTexture(RenderFormat format,uint32_t width,uint32_t height,
                               std::span<const std::vector<uint8_t>> mips,std::shared_ptr<NativeDevice> gpu)
    : impl_(std::make_unique<Impl>(format,width,height,mips,std::move(gpu))) {}
SampledTexture::~SampledTexture()=default;
const RenderTexture* SampledTexture::Texture() const { return impl_->texture.get(); }
std::vector<std::vector<uint8_t>> SampledTexture::ReadBlocks() { return impl_->ReadBlocks(); }
}
