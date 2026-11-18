// Original synthetic shader; no game inputs. Compile with the patched
// XenosRecomp/shader_common.h include directory and HLSL 2021.
#include "shader_common.h"
cbuffer SharedConstants : register(b2, space4) { DEFINE_SHARED_CONSTANTS(); };
ByteAddressBuffer inputs : register(t0, space5);
RWByteAddressBuffer results : register(u0, space5);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint address = id.x * 32;
    float4 value = asfloat(inputs.Load4(address));
    uint mode = inputs.Load(address + 16);
    uint slot = inputs.Load(address + 20);
    uint outputAddress = id.x * 48;
    results.Store4(outputAddress, asuint(applyTextureFetch(value, mode)));
    results.Store4(outputAddress + 16, asuint(applyTextureFetch(value, TEXTURE_FETCH_MODE(slot))));
    results.Store4(outputAddress + 32, asuint(quantizeColor7e3(value)));
}
