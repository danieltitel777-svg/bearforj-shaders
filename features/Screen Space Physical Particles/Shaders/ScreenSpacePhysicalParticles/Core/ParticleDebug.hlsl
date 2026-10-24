#include "Common/FrameBuffer.hlsli"
#include "ScreenSpacePhysicalParticles/ProceduralQuad.hlsli"

// Renders the built-in stress-test class directly from stable particle state and
// its class-specific visible-ID segment.
StructuredBuffer<float4> PositionAge : register(t0);
StructuredBuffer<float4> Appearance : register(t2);
StructuredBuffer<uint> RenderIndices : register(t5);

struct DebugVertexOutput
{
	float4 Position : SV_POSITION;
	float Opacity : TEXCOORD0;
};

// Summary: Expands one visible particle into a camera-facing procedural quad.
// Args:
//   vertexID: Procedural quad vertex index.
//   instanceID: Index within the debug class's visible-ID segment.
// Returns: Clip-space vertex and saturated particle opacity.
DebugVertexOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	uint particleID = RenderIndices[instanceID];
	float4 positionAge = PositionAge[particleID];
	float4 appearance = Appearance[particleID];
	float2 corner = GetProceduralQuadCorner(vertexID);

	float3 viewRight = mul(
		FrameBuffer::CameraViewInverse,
		float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz;
	float3 viewUp = mul(
		FrameBuffer::CameraViewInverse,
		float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz;
	float3 cameraRelative = positionAge.xyz - FrameBuffer::CameraPosAdjust.xyz;
	float radius = max(appearance.x, 0.0f);
	float3 vertexPosition = cameraRelative +
		viewRight * corner.x * radius +
		viewUp * corner.y * radius;

	DebugVertexOutput output;
	output.Position = mul(
		FrameBuffer::CameraViewProj,
		float4(vertexPosition, 1.0f));
	output.Opacity = saturate(appearance.y);
	return output;
}

// Summary: Shades a debug particle as a translucent white quad.
// Args:
//   input: Interpolated vertex output.
// Returns: White RGBA using particle opacity.
float4 PSMain(DebugVertexOutput input) : SV_TARGET
{
	return float4(1.0f, 1.0f, 1.0f, input.Opacity);
}
