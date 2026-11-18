#include "native_color_target.h"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition,const char* error) { if (!condition) throw std::runtime_error(error); }
template<class F> void Reject(F&& action) {
    bool failed=false;
    try { action(); } catch (const std::runtime_error&) { failed=true; }
    Check(failed,"Invalid native color operation was accepted");
}
}
int main() try {
    using namespace mc2::native;
    using namespace plume;
    auto gpu=std::make_shared<NativeDevice>(true);
    for (const auto samples:{1u,2u,4u}) {
        ColorTarget target(5,4,samples,gpu);
        RenderRect left(0,0,2,4),bottom(0,3,5,4),invalid(-1,0,2,4);
        Reject([&] { target.ReadPixels(); });
        const auto prior=gpu->completed;
        Check(target.Clear(RenderColor(0,0,0,0),&left)==prior+1,"Color clear did not advance the native fence");
        Reject([&] { target.ReadPixels(); });
        target.Clear(RenderColor(2,4,0.5f,1));
        target.Clear(RenderColor(8,16,0.25f,0),&left);
        const auto pixels=target.ReadPixels();
        Check(pixels.size()==5*4*4,"Unexpected native HDR readback extent");
        for (uint32_t y=0;y<4;++y) for (uint32_t x=0;x<5;++x) {
            // Exact IEEE half encodings; values above one must survive.
            const std::array<uint16_t,4> expected=x<2 ? std::array<uint16_t,4>{0x4800,0x4c00,0x3400,0} :
                                                       std::array<uint16_t,4>{0x4000,0x4400,0x3800,0x3c00};
            for (uint32_t c=0;c<4;++c)
                Check(pixels[(y*5+x)*4+c]==expected[c],"Native HDR clear, rectangle or MSAA resolve differs");
        }
        ColorTarget second(5,4,samples,gpu);
        second.Clear(RenderColor(0,0,0,0));
        target.Clear(RenderColor(0,0,0,0),&bottom);
        const auto retained=target.ReadPixels();
        for (size_t i=0;i<retained.size();++i)
            Check(retained[i]==(i<5*3*4 ? pixels[i] : 0),"Native color targets did not retain independent contents");
        Reject([&] { target.Clear(RenderColor(0,0,0,0),&invalid); });
        Reject([&] { target.Clear(RenderColor(std::numeric_limits<float>::quiet_NaN(),0,0,0)); });
    }
    Reject([&] { ColorTarget invalid(0,1,1,gpu); });
    Reject([&] { ColorTarget invalid(1,1,3,gpu); });
    ColorTarget fixed(5,4,1,gpu,{},RenderFormat::R16G16B16A16_SNORM);
    constexpr float signed_level = 16383.f / 32767.f;
    fixed.Clear(RenderColor(0,signed_level,-signed_level,1));
    const auto signed_pixels = fixed.ReadPixels();
    const uint16_t expected[] = {0,0x3fff,0xc001,0x7fff};
    for (size_t i = 0; i < signed_pixels.size(); ++i)
        Check(signed_pixels[i] == expected[i % 4], "Native fixed-point clear did not preserve signed values");
    Reject([&] { fixed.Clear(RenderColor(2,0,0,0)); });
    std::cout<<"PASS: HDR values, signed color, partial clears, retained surfaces, 1/2/4-sample resolves and real fences; debug_layer="
             <<gpu->debug_enabled<<", completed="<<gpu->completed<<'\n';
    return 0;
} catch (const std::exception& error) { std::cerr<<"Native color check: "<<error.what()<<'\n'; return 1; }
