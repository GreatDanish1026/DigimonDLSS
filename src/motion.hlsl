// DLSS inputs from what the game's TAA (ps40e6b08f) reads: motion vectors rebuilt from linear depth
// and the camera (the game has no velocity buffer: its TAA reprojects by depth, camera motion only),
// a depth copy DLSS can read, and a bias mask from the ZPrePass flags.
//
// b0 is the game's own TAA constant range ($Params, bound from its ring buffer). The maths copies
// the TAA's instruction by instruction (its disassembly, see docs/FINDINGS.md):
//   ray   = front + ndc.x * right.w * right.xyz + ndc.y * up.w * up.xyz
//   world = cameraPosition + ray * linearDepth
//   prev  = (dot(world, column 0/1/3 of TemporalAAPrevViewProj))  -> uv = xy / w * 0.5 + 0.5, y flipped

cbuffer Params : register(b0) {
    float4 DiffuseColor;           // 0
    float4 wCameraPosition;        // 16 (xyz)
    float4 wCameraRightVector;     // 32 (w: tan of half the horizontal FOV)
    float4 wCameraUpVector;        // 48 (w: tan of half the vertical FOV)
    float4 wCameraFrontVector;     // 64 (xyz)
    float4 wCameraProjJitter;      // 80 (the game's own, zero)
    float4 ColorTextureSize;       // 96
    float4 TemporalAAParams[7];    // 112
    float4 CorrectHistoryViewParams;  // 224
    float4 PrevViewProj[4];        // 240: cb0[15..18], read by column as the TAA does
};

cbuffer Ours : register(b1) {
    float2 RenderSize;   // the render rectangle (top left of the textures)
    float2 JitterPx;     // where this frame's samples sit relative to pixel centres, render pixels, y down
    uint   MaskFlags;    // 1: write the bias mask
    float  FsrNear;      // FSR's depth is near / linear depth (reversed Z, infinite far plane)
    uint2  pad;
};

Texture2D<float> LinearDepth : register(t0);  // the TAA's t1 (r32f)
Texture2D<uint2> ZPrePass    : register(t1);  // the TAA's t4 (rg8uint flags)

RWTexture2D<float2> MotionOut : register(u0);  // render pixels, current -> previous, jitter removed
RWTexture2D<float>  DepthOut  : register(u1);
RWTexture2D<float>  MaskOut   : register(u2);
RWTexture2D<float>  FsrDepthOut : register(u3);  // FSR: reversed infinite depth

float4 PrevColumn(uint c) {
    return float4(PrevViewProj[0][c], PrevViewProj[1][c], PrevViewProj[2][c], PrevViewProj[3][c]);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)RenderSize.x || id.y >= (uint)RenderSize.y) return;
    const float depth = LinearDepth[id.xy];

    // The surface this pixel shows sits at the pixel centre plus the jitter.
    const float2 surface = (float2(id.xy) + 0.5 + JitterPx) / RenderSize;
    const float2 ndc = float2(surface.x * 2.0 - 1.0, 1.0 - surface.y * 2.0);
    const float3 ray = wCameraFrontVector.xyz + ndc.x * wCameraRightVector.w * wCameraRightVector.xyz +
                       ndc.y * wCameraUpVector.w * wCameraUpVector.xyz;
    const float4 world = float4(wCameraPosition.xyz + ray * depth, 1.0);

    const float x = dot(world, PrevColumn(0));
    const float y = dot(world, PrevColumn(1));
    const float w = dot(world, PrevColumn(3));
    float2 prev = float2(x, y) / w * 0.5 + 0.5;
    prev.y = 1.0 - prev.y;

    MotionOut[id.xy] = w > 1e-6 ? (prev - surface) * RenderSize : float2(0, 0);
    DepthOut[id.xy] = depth;
    FsrDepthOut[id.xy] = depth > FsrNear ? FsrNear / depth : 1.0;
    if (MaskFlags & 1) {
        // Objects the game keeps out of its TAA ((flags & 3) >= 2): DLSS trusts this frame there.
        MaskOut[id.xy] = (ZPrePass[id.xy].x & 3) >= 2 ? 1.0 : 0.0;
    }
}
