#ifdef MC2_SINGLE_SAMPLE
Texture2D<float> sourceDepth : register(t0);
#else
Texture2DMS<float, 2> sourceDepth : register(t0);
#endif
RWTexture2D<float> resolvedDepth : register(u0);

cbuffer ResolveParameters : register(b0)
{
    uint2 origin;
    uint2 extent;
};

// Depth export stores the decoded values of the console's unsigned 20e4
// format. The rounding follows ReXGlue/Xenia's Float32To20e4/Float20e4To32.
// Copyright (c) 2020 Ben Vanik; (c) 2026 Tom Clay. BSD-3-Clause.
// see THIRD_PARTY_NOTICES.md.
float quantizeDepth(float depth)
{
    if (!(depth > 0.0))
        return 0.0;
    uint bits = asuint(saturate(depth));
    uint exponent = bits >> 23;
    uint rebased = exponent < 113
        ? ((bits & 0x7fffffu) | 0x800000u) >> min(113u - exponent, 24u)
        : bits - (112u << 23);
    uint code = (rebased + 3u + ((rebased >> 3) & 1u)) >> 3;
    uint mantissa = code & 0xfffffu;
    uint exponent20e4 = code >> 20;
    return exponent20e4 ? asfloat(((exponent20e4 + 112u) << 23) | (mantissa << 3))
                        : float(mantissa) * (1.0 / 17179869184.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= extent))
        return;
    uint2 pixel = origin + id.xy;
#ifdef MC2_SINGLE_SAMPLE
    resolvedDepth[pixel] = quantizeDepth(sourceDepth.Load(int3(pixel, 0)));
#else
    resolvedDepth[pixel] = quantizeDepth(sourceDepth.Load(pixel, 0));
#endif
}
