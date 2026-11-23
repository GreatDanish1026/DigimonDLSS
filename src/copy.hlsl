// FSR's output (rgba16f) into a texture of the game's format (r11g11b10f for DLAA, rgba8 when
// upscaling), which is then copied into the game's target.

cbuffer Size : register(b0) {
    uint2 OutSize;
    uint2 pad;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= OutSize.x || id.y >= OutSize.y) return;
    Dst[id.xy] = Src[id.xy];
}
