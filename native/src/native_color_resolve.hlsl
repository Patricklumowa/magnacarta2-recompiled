Texture2DMS<float4, 2> sourceColor : register(t0);
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
    // Both samples contribute. The float16 destination applies the resolve
    // exponent to all four channels, including the stored two-bit alpha.
    float4 average = (sourceColor.Load(pixel, 0) + sourceColor.Load(pixel, 1)) * 0.5;
    resolvedColor[pixel] = average * scale;
}
