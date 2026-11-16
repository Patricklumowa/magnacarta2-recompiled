#include "native_sampled_texture.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F&& action) {
    bool failed=false;
    try { action(); } catch (const std::runtime_error&) { failed=true; }
    Check(failed,"Invalid texture interpretation was accepted");
}
uint32_t Swizzle(uint32_t x,uint32_t y,uint32_t z,uint32_t w) { return x|(y<<3)|(z<<6)|(w<<9); }
std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Check(bool(file),"Cannot open synthetic texture-fetch shader");
    const auto size=file.tellg();
    Check(size>0 && uint64_t(size)<=1024*1024,"Invalid synthetic shader extent");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    Check(bool(file),"Short synthetic shader read"); return bytes;
}
struct Input { std::array<float,4> value; uint32_t mode,slot,padding[2]{}; };
static_assert(sizeof(Input)==32);
std::array<float,4> Reference(const std::array<float,4>& input,uint32_t mode) {
    if (!(mode&0x80000000u)) return input;
    std::array<float,4> output;
    for (uint32_t c=0;c<4;++c) {
        const auto selector=(mode>>(c*3))&7;
        double value=selector<4 ? input[selector] : selector==5 ? 1 : 0;
        if (mode&(1u<<(12+c))) {
            // Independent double-precision evaluation of the four code ranges.
            const std::array<float,3> boundaries{64.f/255,96.f/255,192.f/255};
            const std::array<int,4> slopes{1,2,4,8},intercepts{0,-64,-256,-1024};
            uint32_t piece=0;
            for (float boundary:boundaries) piece+=value>=boundary;
            value=std::isnan(value) ? 0 : std::clamp(value,0.0,1.0);
            value=value*255*slopes[piece]+intercepts[piece];
            value=(value+std::trunc(value*slopes[piece]/1024))/1023;
        }
        int exponent=(mode>>16)&63;
        if (exponent>=32) exponent-=64;
        output[c]=float(std::ldexp(value,exponent));
    }
    return output;
}
}
int main(int argc,char** argv) try {
    using namespace mc2::native;
    using namespace plume;
    Check(argc==2,"Usage: mc2_native_fetch_check <texture_fetch_check.dxil>");
    Check(PackTextureFetchMode(0x688,7,0)==0x80007688u,"Native fetch constant layout differs");
    Reject([] { PackTextureFetchMode(0x688,16,0); });
    Reject([] { PackTextureFetchMode(0x688,0,-33); });
    Reject([] { PackTextureFetchMode(0x688,0,32); });
    Reject([] { PackTextureFetchMode(4096,0,0); });
    Reject([] { PackTextureFetchMode(Swizzle(0,1,2,6),0,0); });
    Reject([] { PackTextureFetchMode(Swizzle(0,1,2,5),8,0); });
    std::array<uint32_t,16> modes;
    for (uint32_t i=0;i<12;++i) modes[i]=PackTextureFetchMode(0x688,i,i>=8 ? int(i-8) : 0);
    modes[12]=PackTextureFetchMode(Swizzle(2,1,0,3),7,-32);
    modes[13]=PackTextureFetchMode(Swizzle(4,5,0,1),12,31);
    modes[14]=PackTextureFetchMode(Swizzle(3,3,3,3),15,-4);
    modes[15]=0;
    std::vector<float> values;
    for (uint32_t i=0;i<256;++i) values.push_back(float(i)/255);
    for (float boundary:{64.f/255,96.f/255,192.f/255}) {
        values.push_back(std::nextafter(boundary,0.f));
        values.push_back(std::nextafter(boundary,1.f));
    }
    values.insert(values.end(),{-1,2,std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()});
    std::vector<Input> cases;
    for (uint32_t i=0;i<values.size();++i) for (uint32_t slot=0;slot<modes.size();++slot) {
        Input input{{values[i],values[(i+59)%values.size()],values[(i+137)%values.size()],values[(i+203)%values.size()]},
                    modes[slot],(slot+7)%16};
        if (slot==15) input.mode=0x7fffffffu; // Disabled despite nonzero lower bits.
        cases.push_back(input);
    }
    while (cases.size()%64) cases.push_back(cases.front());
    auto gpu=std::make_shared<NativeDevice>(true);
    auto& device=gpu->device;
    const auto byte_size=cases.size()*sizeof(Input);
    auto input=device->createBuffer(RenderBufferDesc::UploadBuffer(byte_size,RenderBufferFlag::STORAGE));
    auto constants=device->createBuffer(RenderBufferDesc::UploadBuffer(512,RenderBufferFlag::CONSTANT));
    auto output=device->createBuffer(RenderBufferDesc::DefaultBuffer(byte_size,RenderBufferFlag::STORAGE|RenderBufferFlag::UNORDERED_ACCESS));
    auto readback=device->createBuffer(RenderBufferDesc::ReadbackBuffer(byte_size));
    Check(input && constants && output && readback,"Native fetch test allocation failed");
    auto* mapped=input->map(); Check(mapped!=nullptr,"Native fetch input mapping failed");
    std::memcpy(mapped,cases.data(),byte_size); input->unmap();
    mapped=constants->map(); Check(mapped!=nullptr,"Native fetch constants mapping failed");
    std::memset(mapped,0,512); std::memcpy(static_cast<uint8_t*>(mapped)+288,modes.data(),sizeof(modes)); constants->unmap();
    const RenderDescriptorRange buffer_ranges[]={{RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER,0,1},
                                                {RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER,0,1}};
    const RenderDescriptorRange constant_range(RenderDescriptorRangeType::CONSTANT_BUFFER,2,1);
    std::array<RenderDescriptorSetDesc,6> sets{};
    sets[4]=RenderDescriptorSetDesc(&constant_range,1); sets[5]=RenderDescriptorSetDesc(buffer_ranges,2);
    auto constant_set=device->createDescriptorSet(sets[4]),buffer_set=device->createDescriptorSet(sets[5]);
    Check(constant_set && buffer_set,"Native fetch test descriptor allocation failed");
    constant_set->setBuffer(0,constants.get(),512);
    buffer_set->setBuffer(0,input.get(),byte_size); buffer_set->setBuffer(1,output.get(),byte_size);
    RenderPipelineLayoutDesc layout_desc;
    layout_desc.descriptorSetDescs=sets.data(); layout_desc.descriptorSetDescsCount=sets.size();
    auto layout=device->createPipelineLayout(layout_desc);
    const auto bytes=Read(argv[1]);
    auto shader=device->createShader(bytes.data(),bytes.size(),"main",RenderShaderFormat::DXIL);
    Check(layout && shader,"Native fetch test shader creation failed");
    auto pipeline=device->createComputePipeline(RenderComputePipelineDesc(layout.get(),shader.get(),64,1,1));
    Check(bool(pipeline),"Native fetch test pipeline creation failed");
    auto& commands=gpu->commands;
    commands->begin();
    commands->barriers(RenderBarrierStage::COMPUTE,RenderBufferBarrier(output.get(),RenderBufferAccess::WRITE));
    commands->setComputePipelineLayout(layout.get()); commands->setPipeline(pipeline.get());
    commands->setComputeDescriptorSet(constant_set.get(),4); commands->setComputeDescriptorSet(buffer_set.get(),5);
    commands->dispatch(uint32_t(cases.size()/64),1,1);
    commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(output.get(),RenderBufferAccess::READ));
    commands->copyBufferRegion(readback.get(),output.get(),byte_size);
    commands->end(); gpu->queue->executeCommandLists(commands.get(),gpu->fence.get()); gpu->Complete();
    const auto* actual=static_cast<const float*>(readback->map());
    Check(actual!=nullptr,"Native fetch test readback mapping failed");
    size_t mismatches=0; double max_relative=0;
    for (size_t i=0;i<cases.size();++i) for (uint32_t test=0;test<2;++test) {
        const auto mode=test ? modes[cases[i].slot] : cases[i].mode;
        const auto expected=Reference(cases[i].value,mode);
        for (uint32_t c=0;c<4;++c) {
            const float result=actual[i*8+test*4+c];
            const auto relative=std::abs(double(result)-expected[c])/std::max(1e-30,std::abs(double(expected[c])));
            bool matches;
            if (!(mode&0x80000000u)) matches=std::bit_cast<uint32_t>(result)==std::bit_cast<uint32_t>(expected[c]);
            else if (std::isnan(expected[c])) matches=std::isnan(result);
            else if (std::isinf(expected[c])) matches=result==expected[c];
            else { matches=std::isfinite(result) && (relative<2e-6 || std::abs(result-expected[c])<1e-30);
                   max_relative=std::max(max_relative,relative); }
            if (!matches && mismatches++<8)
                std::cerr<<"Fetch mismatch case="<<i<<", test="<<test<<", channel="<<c<<", mode="<<mode
                         <<", expected="<<expected[c]<<", actual="<<result<<'\n';
        }
    }
    readback->unmap(); Check(!mismatches,"Native GPU fetch interpretation differs from CPU reference");
    std::cout<<"PASS: "<<cases.size()*2<<" GPU fetch cases, gamma boundaries, swizzle/constants, signed exponent, disabled bypass and all 16 shared slots; max_relative="
             <<max_relative<<", debug_layer="<<gpu->debug_enabled<<'\n';
    return 0;
} catch (const std::exception& error) { std::cerr<<"Native fetch check: "<<error.what()<<'\n'; return 1; }
