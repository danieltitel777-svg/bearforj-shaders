#include "ScreenSpacePhysicalParticles/Core/ParticleNewborn.hlsli"

// Initializes feature-owned leaf newborns on solid height-field or perspective
// surfaces before handing them to SSPP's persistent lifecycle.

cbuffer PhysicalLeavesNewborn : register(b4)
{
	uint ExpectedInitializer;
	float MinimumGroundNormalZ;
	float GroundSearchBelow;
	float GroundSearchAbove;

	float ScreenXYTolerance;
	float SurfaceOffset;
	float2 NewbornPad;
};

static const float PHYSICAL_LEAVES_GOLDEN_RATIO_CONJUGATE = 0.61803398875f;

// Summary: Samples a ground-like surface from the orthographic solid field.
// Args:
//   worldXY: World-space horizontal sample position.
//   surfacePosition: Receives the reconstructed world position.
//   surfaceNormal: Receives the reconstructed upward normal.
// Returns: True when a valid ground sample exists.
bool SampleHeightFieldGround(
	float2 worldXY,
	out float3 surfacePosition,
	out float3 surfaceNormal)
{
	return SSPP_SampleHeightField(
		SSPPNewbornSolidHeight,
		SolidHeightAvailable,
		SolidHeightCenter,
		SolidHeightHalfExtent,
		SolidHeightTexelSize,
		MinimumGroundNormalZ,
		worldXY,
		surfacePosition,
		surfaceNormal);
}

// Summary: Samples actor-free perspective depth near a requested leaf position.
// Args:
//   guidePosition: World-space point used for projection.
//   requestedXY: Requested horizontal leaf position.
//   request: Burst bounds used to reject unrelated surfaces.
//   surfacePosition: Receives the reconstructed world position.
//   surfaceNormal: Receives the reconstructed upward normal.
// Returns: True when nearby ground-like depth is available.
bool SamplePerspectiveGround(
	float3 guidePosition,
	float2 requestedXY,
	SSPPBurstRequest request,
	out float3 surfacePosition,
	out float3 surfaceNormal)
{
	surfacePosition = guidePosition;
	surfaceNormal = float3(0.0f, 0.0f, 1.0f);
	if (SceneDepthAvailable == 0u)
		return false;

	float3 cameraRelative = guidePosition - FrameBuffer::CameraPosAdjust.xyz;
	float4 clipPosition = mul(
		FrameBuffer::CameraViewProj,
		float4(cameraRelative, 1.0f));
	if (clipPosition.w <= 0.01f)
		return false;
	float2 uv = clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f;
	if (FrameBuffer::IsOutsideFrame(uv))
		return false;
	uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);

	uint width;
	uint height;
	SceneDepth.GetDimensions(width, height);
	if (width < 2u || height < 2u)
		return false;
	uint2 dimensions = uint2(width, height);
	uint2 maximumPixel = dimensions - 1u;
	uint2 pixel = min(uint2(uv * dimensions), maximumPixel);
	float depth = SceneDepth.Load(int3(pixel, 0));
	if (depth >= DEPTH_CLEAR_THRESHOLD)
		return false;

	surfacePosition = ReconstructScenePosition(pixel, depth, dimensions);
	float3 boundsMinimum = min(request.SpawnBoundsMin, request.SpawnBoundsMax);
	float3 boundsMaximum = max(request.SpawnBoundsMin, request.SpawnBoundsMax);
	if (length(surfacePosition.xy - requestedXY) > ScreenXYTolerance ||
		surfacePosition.z < boundsMinimum.z - GroundSearchBelow ||
		surfacePosition.z > boundsMaximum.z + GroundSearchAbove)
		return false;
	if (!ReconstructSceneNormal(pixel, depth, surfacePosition, dimensions, surfaceNormal))
		return false;
	if (surfaceNormal.z < 0.0f)
		surfaceNormal = -surfaceNormal;
	return surfaceNormal.z >= MinimumGroundNormalZ;
}

// Summary: Selects perspective ground when visible, otherwise orthographic ground.
// Args:
//   worldXY: Requested horizontal leaf position.
//   request: Burst bounds used for vertical validation.
//   surfacePosition: Receives the selected world position.
//   surfaceNormal: Receives the selected upward normal.
// Returns: True when either collision provider supplies valid ground.
bool FindGround(
	float2 worldXY,
	SSPPBurstRequest request,
	out float3 surfacePosition,
	out float3 surfaceNormal)
{
	float3 heightPosition = 0.0f;
	float3 heightNormal = float3(0.0f, 0.0f, 1.0f);
	bool heightValid = SampleHeightFieldGround(
		worldXY, heightPosition, heightNormal);
	float3 boundsMinimum = min(request.SpawnBoundsMin, request.SpawnBoundsMax);
	float3 boundsMaximum = max(request.SpawnBoundsMin, request.SpawnBoundsMax);
	float guideZ = heightValid ? heightPosition.z : boundsMinimum.z;
	if (SamplePerspectiveGround(
		float3(worldXY, guideZ),
		worldXY,
		request,
		surfacePosition,
		surfaceNormal))
		return true;

	surfacePosition = heightPosition;
	surfaceNormal = heightNormal;
	return heightValid &&
		heightPosition.z >= boundsMinimum.z - GroundSearchBelow &&
		heightPosition.z <= boundsMaximum.z + GroundSearchAbove;
}

[numthreads(64, 1, 1)]
// Summary: Places one granted leaf newborn on a sampled ground surface.
// Args:
//   groupID: Emission-chunk index.
//   groupThreadID: Lane distributing newborns within the chunk.
// Returns: None.
void InitializeMain(
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID)
{
	for (uint localOrdinal = groupThreadID.x;
		localOrdinal < SSPPEmissionChunkSize;
		localOrdinal += 64u) {
		SSPPEmissionChunk chunk;
		SSPPBurstRequest request;
		SSPPParticleTemplate particleTemplate;
		SSPPParticleReference reference;
		if (!SSPPLoadNewborn(
			groupID.x,
			localOrdinal,
			ExpectedInitializer,
			chunk,
			request,
			particleTemplate,
			reference))
			continue;

		uint ordinal = chunk.FirstOrdinal + localOrdinal;
		float count = max((float)request.Count, 1.0f);
		float sequence = ((float)ordinal + 0.5f) / count;
		float2 jitter = float2(
			SSPPRandom01(request.Seed, ordinal, reference.SlotGeneration, 16u),
			SSPPRandom01(request.Seed, ordinal, reference.SlotGeneration, 17u));
		float2 distribution = frac(
			float2(sequence, sequence * PHYSICAL_LEAVES_GOLDEN_RATIO_CONJUGATE) +
			jitter);
		float2 boundsMinimum = min(
			request.SpawnBoundsMin.xy, request.SpawnBoundsMax.xy);
		float2 boundsMaximum = max(
			request.SpawnBoundsMin.xy, request.SpawnBoundsMax.xy);
		float2 worldXY = lerp(boundsMinimum, boundsMaximum, distribution);

		float3 surfacePosition;
		float3 surfaceNormal;
		if (!FindGround(
			worldXY, request, surfacePosition, surfaceNormal) ||
			!all(isfinite(surfacePosition)) || !all(isfinite(surfaceNormal))) {
			SSPPRejectNewborn(reference);
			continue;
		}

		float4 positionAge = SSPPNewbornPositionAge[reference.ParticleID];
		float4 velocityLifetime =
			SSPPNewbornVelocityLifetime[reference.ParticleID];
		positionAge.xyz = surfacePosition + surfaceNormal * max(SurfaceOffset, 0.0f);
		positionAge.w = 0.0f;
		// Grounded leaves retain their support normal until maintenance either
		// initializes the generation-tagged sidecar or wakes them into flight.
		velocityLifetime.xyz = surfaceNormal;
		SSPPNewbornPositionAge[reference.ParticleID] = positionAge;
		SSPPNewbornVelocityLifetime[reference.ParticleID] = velocityLifetime;
		SSPPNewbornContactData[reference.ParticleID] = float4(surfaceNormal, 0.0f);
		SSPPCompleteNewborn(chunk, reference);
	}
}
