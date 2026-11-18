#ifdef MC2_SINGLE_SAMPLE
Texture2D<float4> sourceColor : register(t0);
#else
Texture2DMS<float4, 2> sourceColor : register(t0);
#endif
RWTexture2D<float4> resolvedColor : register(u0);

cbuffer ResolveParameters : register(b0)
{
    uint2 origin;
    uint2 extent;
    float scale;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= extent))
        return;
    uint2 pixel = origin + id.xy;
#ifdef MC2_SINGLE_SAMPLE
    float4 color = sourceColor.Load(int3(pixel, 0));
#else
    float4 color = (sourceColor.Load(pixel, 0) + sourceColor.Load(pixel, 1)) * 0.5;
#endif
    // Apply the resolve exponent to all four channels.
    resolvedColor[pixel] = color * scale;
}
