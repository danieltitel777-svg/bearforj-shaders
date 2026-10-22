#include "Common/FrameBuffer.hlsli"

// Converts SSPP's actor-free hardware-depth texture into a logarithmic grayscale
// preview. Clear-depth texels are black; nearer occupied texels are brighter.
Texture2D<float> ActorFreeDepth : register(t0);
RWTexture2D<float4> DebugOutput : register(u0);

// Summary: Writes one actor-free depth preview pixel.
// Args:
//   dispatchID: Output pixel selected by the compute dispatch.
// Returns: None.
[numthreads(8, 8, 1)]
void Main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint outputWidth, outputHeight;
	DebugOutput.GetDimensions(outputWidth, outputHeight);
	if (dispatchID.x >= outputWidth || dispatchID.y >= outputHeight)
		return;

	uint sourceWidth, sourceHeight;
	ActorFreeDepth.GetDimensions(sourceWidth, sourceHeight);
	float2 previewUV = (float2(dispatchID.xy) + 0.5f) / float2(outputWidth, outputHeight);
	float2 sourceUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(previewUV);
	uint2 sourcePixel = min(
		(uint2)(sourceUV * float2(sourceWidth, sourceHeight)),
		uint2(sourceWidth - 1u, sourceHeight - 1u));
	float depth = ActorFreeDepth.Load(int3(sourcePixel, 0));
	if (depth >= 0.9999999f) {
		DebugOutput[dispatchID.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}

	float4 positionCS = float4(
		2.0f * float2(previewUV.x, 1.0f - previewUV.y) - 1.0f,
		depth,
		1.0f);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse[0], positionCS);
	positionMS.xyz /= max(abs(positionMS.w), 1.0e-5f);
	float cameraDistance = max(length(positionMS.xyz), 16.0f);
	float grayscale = 1.0f - saturate(log2(cameraDistance / 16.0f) / 11.0f);
	DebugOutput[dispatchID.xy] = float4(grayscale.xxx, 1.0f);
}
