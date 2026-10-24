#include "Common/FrameBuffer.hlsli"
#include "PhysicalLeaves/PhysicalLeavesShared.hlsli"
#include "ScreenSpacePhysicalParticles/ProceduralQuad.hlsli"

// Renders SSPP-visible leaves from the neutral leaf atlas.
// appearance.w is frame 01 of the species. Variation picks frames 01-04.
// RegionTint multiplies neutral leaves. Needle slices stay untinted.
// Grounded leaves rest at a stable random tilt; airborne leaves billboard.

cbuffer RenderSettings : register(b0)
{
	uint DebugWhiteQuads;
	uint VisibleSegmentBase;
	uint ParticleCapacity;
	uint NeedleSliceBase;
	float3 RegionTint;
	float RegionTintPad;
};

#if defined(VSHADER)
StructuredBuffer<float4> PositionAge : register(t0);
StructuredBuffer<float4> VelocityLifetime : register(t1);
StructuredBuffer<float4> Appearance : register(t2);
StructuredBuffer<uint4> Metadata : register(t3);
StructuredBuffer<float4> ContactData : register(t4);
StructuredBuffer<uint> VisibleParticleIDs : register(t5);
StructuredBuffer<PhysicalLeafSidecar> LeafSidecars : register(t6);

struct VertexOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	float Opacity : TEXCOORD1;
	float Slice : TEXCOORD2;
	float Variation : TEXCOORD3;
};

// Summary: Expands one visible leaf into a surface-aligned or camera-facing quad.
// Args:
//   vertexID: Procedural triangle-strip corner index.
//   instanceID: Leaf instance within the visible SSPP segment.
// Returns: Clip-space vertex and atlas inputs.
VertexOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VertexOutput output = (VertexOutput)0;
	uint visibleIndex = VisibleSegmentBase + instanceID;
	uint particleID = VisibleParticleIDs[visibleIndex];
	if (particleID >= ParticleCapacity)
		return output;

	float4 positionAge = PositionAge[particleID];
	float4 appearance = Appearance[particleID];
	uint generation = Metadata[particleID].w;
	PhysicalLeafSidecar sidecar = LeafSidecars[particleID];
	float rotation = appearance.z * Math::TAU;
	bool sidecarValid = sidecar.Generation == generation;
	if (sidecarValid) {
		rotation = sidecar.BaseRotation;
		if (sidecar.State == PHYSICAL_LEAF_STATE_AIRBORNE)
			rotation += sidecar.AngularVelocity * max(positionAge.w, 0.0f);
	}

	float2 corner = GetProceduralQuadCorner(vertexID);
	output.TexCoord = corner * 0.5f + 0.5f;
	float sineValue;
	float cosineValue;
	sincos(rotation, sineValue, cosineValue);

	float3 quadRight;
	float3 quadUp;
	float3 supportNormal = 0.0f;
	float supportLiftScale = 0.0f;
	if (sidecarValid && sidecar.State == PHYSICAL_LEAF_STATE_GROUNDED) {
		supportNormal = normalize(
			all(isfinite(sidecar.HomeNormal)) &&
			dot(sidecar.HomeNormal, sidecar.HomeNormal) > 1.0e-6f ?
				sidecar.HomeNormal : float3(0.0f, 0.0f, 1.0f));
		float3 referenceAxis = abs(supportNormal.z) < 0.999f ?
			float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
		float3 surfaceRight = normalize(cross(referenceAxis, supportNormal));
		float3 surfaceUp = cross(supportNormal, surfaceRight);
		quadRight = surfaceRight * cosineValue + surfaceUp * sineValue;
		float3 planarUp = -surfaceRight * sineValue + surfaceUp * cosineValue;
		float tiltSine = sidecar.GroundTilt.x;
		float tiltCosine = 1.0f + sidecar.GroundTilt.y;
		quadUp = planarUp * tiltCosine + supportNormal * tiltSine;
		supportLiftScale = abs(tiltSine);
	} else {
		float3 viewRight = mul(
			FrameBuffer::CameraViewInverse,
			float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz;
		float3 viewUp = mul(
			FrameBuffer::CameraViewInverse,
			float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz;
		quadRight = viewRight * cosineValue + viewUp * sineValue;
		quadUp = -viewRight * sineValue + viewUp * cosineValue;
	}
	float radius = max(appearance.x, 0.0f);
	float3 cameraRelative = positionAge.xyz - FrameBuffer::CameraPosAdjust.xyz;
	float3 vertexPosition = cameraRelative +
		supportNormal * supportLiftScale * radius +
		quadRight * corner.x * radius +
		quadUp * corner.y * radius;
	output.Position = mul(
		FrameBuffer::CameraViewProj,
		float4(vertexPosition, 1.0f));
	output.Opacity = saturate(appearance.y);
	output.Slice = appearance.w;
	output.Variation = appearance.z;
	return output;
}
#endif

#if defined(PSHADER)
Texture2DArray<float4> LeafSlices : register(t2);
SamplerState LeafSampler : register(s0);

struct VertexOutput
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	float Opacity : TEXCOORD1;
	float Slice : TEXCOORD2;
	float Variation : TEXCOORD3;
};

// Summary: Shades one leaf fragment from the texture array or debug-white material.
// Args:
//   input: Rasterized leaf UV and opacity.
// Returns: Leaf color and alpha coverage.
float4 PSMain(VertexOutput input) : SV_TARGET
{
	if (DebugWhiteQuads != 0u)
		return float4(1.0f, 1.0f, 1.0f, input.Opacity);

	float frame = min(floor(saturate(input.Variation) * 4.0f), 3.0f);
	float slice = max(round(input.Slice) + frame, 0.0f);
	float4 leaf = LeafSlices.Sample(LeafSampler, float3(saturate(input.TexCoord), slice));
	float3 tint = slice >= (float)NeedleSliceBase ? float3(1.0f, 1.0f, 1.0f) : RegionTint;
	leaf.rgb *= tint;
	leaf.a *= input.Opacity;
	clip(leaf.a - 0.05f);
	return leaf;
}
#endif
