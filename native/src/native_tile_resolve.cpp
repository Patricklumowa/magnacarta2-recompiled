#include "native_tile_resolve.h"
#include <stdexcept>

using namespace plume;
namespace mc2::native {
namespace {
struct ResolveParameters {
    uint32_t x, y, width, height;
    float scale;
};
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

TileResolve::TileResolve(NativeDevice& gpu, RenderTexture& source, uint32_t width, uint32_t height,
                         RenderFormat format, std::span<const uint8_t> bytecode, const RenderTextureView* source_view)
    : gpu_(gpu), source_(source) {
    auto& device = gpu_.device;
    output_ = device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, format,
        RenderTextureFlag::UNORDERED_ACCESS));
    shader_ = device->createShader(bytecode.data(), bytecode.size(), "main", RenderShaderFormat::DXIL);
    const RenderDescriptorRange ranges[] = {
        {RenderDescriptorRangeType::TEXTURE, 0, 1}, {RenderDescriptorRangeType::READ_WRITE_TEXTURE, 0, 1}
    };
    const RenderDescriptorSetDesc set_desc(ranges, uint32_t(std::size(ranges)));
    descriptors_ = device->createDescriptorSet(set_desc);
    Require(output_ && shader_ && descriptors_, "Native tile resolve resource creation failed");
    descriptors_->setTexture(0, &source_, RenderTextureLayout::SHADER_READ, source_view);
    descriptors_->setTexture(1, output_.get(), RenderTextureLayout::GENERAL);
    const RenderPushConstantRange parameters(0, 0, 0, sizeof(ResolveParameters), RenderShaderStageFlag::COMPUTE);
    RenderPipelineLayoutDesc layout;
    layout.descriptorSetDescs = &set_desc; layout.descriptorSetDescsCount = 1;
    layout.pushConstantRanges = &parameters; layout.pushConstantRangesCount = 1;
    layout_ = device->createPipelineLayout(layout);
    Require(bool(layout_), "Native tile resolve layout creation failed");
    pipeline_ = device->createComputePipeline(RenderComputePipelineDesc(layout_.get(), shader_.get(), 8, 8, 1));
    Require(bool(pipeline_), "Native tile resolve pipeline creation failed");
}

void TileResolve::Record(const RenderRect& rectangle, float scale) {
    const ResolveParameters parameters = {uint32_t(rectangle.left), uint32_t(rectangle.top),
        uint32_t(rectangle.right - rectangle.left), uint32_t(rectangle.bottom - rectangle.top), scale};
    auto& commands = gpu_.commands;
    const RenderTextureBarrier barriers[] = {
        {&source_, RenderTextureLayout::SHADER_READ}, {output_.get(), RenderTextureLayout::GENERAL}
    };
    commands->barriers(RenderBarrierStage::COMPUTE, barriers, uint32_t(std::size(barriers)));
    commands->setComputePipelineLayout(layout_.get());
    commands->setPipeline(pipeline_.get());
    commands->setComputeDescriptorSet(descriptors_.get(), 0);
    commands->setComputePushConstants(0, &parameters);
    commands->dispatch((parameters.width + 7) / 8, (parameters.height + 7) / 8, 1);
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(output_.get(), RenderTextureLayout::SHADER_READ));
}
}
