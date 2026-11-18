struct RasterVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

#ifdef MC2_PRESENT_VERTEX
RasterVertex main(uint vertex : SV_VertexID) {
    RasterVertex output;
    output.uv = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
#else
Texture2D<float4> frame_image : register(t0, space0);
SamplerState frame_sampler : register(s0, space1);

float4 main(RasterVertex input) : SV_Target0 {
    return frame_image.Sample(frame_sampler, input.uv);
}
#endif
