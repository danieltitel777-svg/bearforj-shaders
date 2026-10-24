#include "Common/FrameBuffer.hlsli"
#include "ScreenSpacePhysicalParticles/ProceduralQuad.hlsli"

// Draws one world-particle render class. Mode 0 tints magic sparks from the
// packed spell color. Modes 1 and 2 pick a slice from the per-kind texture array.
// Every kind fades in over the first 28% of its life and fades out after 50%.

cbuffer RenderSettings : register(b0)
{
	uint Mode;
	uint VisibleSegmentBase;
	uint ParticleCapacity;
	uint RenderSettingsPad;
};

#if defined(VSHADER)
StructuredBuffer<float4> PositionAge : register(t0);
StructuredBuffer<float4> VelocityLifetime : register(t1);
StructuredBuffer<float4> Appearance : register(t2);
StructuredBuffer<uint4> Metadata : register(t3);
StructuredBuffer<float4> ContactData : register(t4);
StructuredBuffer<uint> VisibleParticleIDs : register(t5);

struct VertexOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	float Opacity : TEXCOORD1;
	float Variation : TEXCOORD2;
	float GeneralScalar : TEXCOORD3;
};

// Summary: Expands one visible world particle into a camera-facing quad.
// Args:
//   vertexID: Procedural triangle-strip corner index.
//   instanceID: Particle instance within the visible SSPP segment.
// Returns: Clip-space vertex, texture coordinates, and fade.
VertexOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VertexOutput output = (VertexOutput)0;
	uint visibleIndex = VisibleSegmentBase + instanceID;
	uint particleID = VisibleParticleIDs[visibleIndex];
	if (particleID >= ParticleCapacity)
		return output;

	float4 positionAge = PositionAge[particleID];
	float4 velocityLifetime = VelocityLifetime[particleID];
	float4 appearance = Appearance[particleID];
	float life = saturate(positionAge.w / max(velocityLifetime.w, 0.0001f));
	float fadeIn = saturate(life / 0.28f);
	float fadeOut = 1.0f - saturate((life - 0.5f) / 0.5f);
	float fade = fadeIn * fadeOut;

	float rotation = appearance.z * Math::TAU;
	float sineValue;
	float cosineValue;
	sincos(rotation, sineValue, cosineValue);
	float2 corner = GetProceduralQuadCorner(vertexID);
	float3 viewRight = mul(FrameBuffer::CameraViewInverse, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz;
	float3 viewUp = mul(FrameBuffer::CameraViewInverse, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz;
	float3 quadRight = viewRight * cosineValue + viewUp * sineValue;
	float3 quadUp = -viewRight * sineValue + viewUp * cosineValue;
	float radius = max(appearance.x, 0.0f);
	float3 cameraRelative = positionAge.xyz - FrameBuffer::CameraPosAdjust.xyz;
	float3 vertexPosition = cameraRelative + quadRight * corner.x * radius + quadUp * corner.y * radius;
	output.Position = mul(FrameBuffer::CameraViewProj, float4(vertexPosition, 1.0f));
	output.TexCoord = corner * 0.5f + 0.5f;
	output.Opacity = saturate(appearance.y) * fade;
	output.Variation = appearance.z;
	output.GeneralScalar = Mode == 3u ?
		floor(fmod(max(positionAge.w, 0.0f) * 6.0f, 4.0f)) :
		appearance.w;
	return output;
}
#endif

#if defined(PSHADER)
Texture2DArray<float4> ParticleSlices : register(t2);
SamplerState ParticleSampler : register(s0);

struct VertexOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	float Opacity : TEXCOORD1;
	float Variation : TEXCOORD2;
	float GeneralScalar : TEXCOORD3;
};

// Summary: Shades one world particle from its texture array and fade.
// Args:
//   input: Rasterized texture coordinates, fade, and appearance.
// Returns: Alpha-blended or emissive particle color.
float4 PSMain(VertexOutput input) : SV_TARGET
{
	uint width;
	uint height;
	uint sliceCount;
	ParticleSlices.GetDimensions(width, height, sliceCount);
	sliceCount = max(sliceCount, 1u);
	float slice = Mode == 3u ?
		round(input.GeneralScalar) :
		floor(saturate(input.Variation) * sliceCount) % sliceCount;

	float4 texel = ParticleSlices.Sample(ParticleSampler, float3(input.TexCoord, slice));
	float alpha = texel.a * input.Opacity;
	clip(alpha - 0.04f);

	if (Mode == 0u) {
		uint packed = (uint)round(input.GeneralScalar);
		float3 tint = float3(
			((packed >> 16u) & 255u) / 255.0f,
			((packed >> 8u) & 255u) / 255.0f,
			(packed & 255u) / 255.0f);
		return float4(texel.rgb * tint, alpha);
	}

	if (Mode == 2u)
		return float4(texel.rgb, alpha);
	return float4(texel.rgb, alpha);
}
#endif
