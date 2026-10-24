#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Expands granted emission chunks into stable particle slots. The pass initializes
// canonical SoA state and routes common newborns; feature-owned initializers receive
// reserved newborns without an initial lifecycle reference.
StructuredBuffer<SSPPBurstRequest> FrameRequests : register(t0);
StructuredBuffer<SSPPEmissionChunk> EmissionChunks : register(t1);
StructuredBuffer<uint> FreeIDs : register(t2);
StructuredBuffer<SSPPParticleTemplate> ParticleTemplates : register(t3);
StructuredBuffer<SSPPRouteDescriptor> RouteDescriptors : register(t7);

RWStructuredBuffer<float4> PositionAge : register(u0);
RWStructuredBuffer<float4> VelocityLifetime : register(u1);
RWStructuredBuffer<float4> Appearance : register(u2);
RWStructuredBuffer<uint4> Metadata : register(u3);
RWStructuredBuffer<float4> ContactData : register(u4);
RWStructuredBuffer<SSPPParticleReference> OutputRouteArena : register(u5);
RWStructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(u6);
RWStructuredBuffer<uint> Counters : register(u7);

static const float SSPP_DEGREES_TO_RADIANS = 0.017453292519943295f;
static const float SSPP_TRAJECTORY_NORMAL_PERCENTILE = 1.90f;
static const float SSPP_TRAJECTORY_UNIFORM_PERCENTILE = 2.05f;

groupshared float3 GroupForward;
groupshared float3 GroupRight;
groupshared float3 GroupVertical;
groupshared float3 GroupSpawnBoundsMin;
groupshared float3 GroupSpawnBoundsMax;

// Summary: Generates two independent standard-normal samples with Box-Muller.
// Args:
//   seed: Burst or emitter seed.
//   particleOrdinal: Particle ordinal within the request.
//   generation: Stable-slot generation.
//   streamBase: First random-property stream.
// Returns: Pair of standard-normal variates.
float2 SSPPSampleStandardNormalPair(
	uint seed,
	uint particleOrdinal,
	uint generation,
	uint streamBase)
{
	float u1 = max(
		SSPPRandom01(seed, particleOrdinal, generation, streamBase),
		1.0e-7f);
	float u2 = SSPPRandom01(seed, particleOrdinal, generation, streamBase + 1u);
	float radius = sqrt(-2.0f * log(u1));
	float angle = Math::TAU * u2;
	float sine;
	float cosine;
	sincos(angle, sine, cosine);
	return radius * float2(cosine, sine);
}

// Summary: Samples the unbounded shape used by coupled heavy-tail trajectories.
// Args:
//   distribution: Property distribution selecting symmetric or biased shape.
//   seed: Burst or emitter seed.
//   particleOrdinal: Particle ordinal within the request.
//   generation: Stable-slot generation.
//   propertyIndex: Trajectory-property stream selector.
// Returns: Clamped shape value centered near zero.
float SSPPSampleHeavyTailShape(
	SSPPScalarDistribution distribution,
	uint seed,
	uint particleOrdinal,
	uint generation,
	uint propertyIndex)
{
	if (distribution.Type == SSPP_DISTRIBUTION_CONSTANT)
		return 0.0f;

	uint streamBase = 32u + propertyIndex * 8u;
	float shape = 0.0f;
	if (distribution.Type == SSPP_DISTRIBUTION_UNIFORM) {
		shape = (SSPPRandom01(
			seed, particleOrdinal, generation, streamBase) - 0.5f) *
			(2.0f * SSPP_TRAJECTORY_NORMAL_PERCENTILE / 0.90f);
	} else {
		float2 gaussian = SSPPSampleStandardNormalPair(
			seed, particleOrdinal, generation, streamBase);
		if (distribution.Type == SSPP_DISTRIBUTION_NORMAL) {
			shape = gaussian.x;
		} else if (distribution.Type == SSPP_DISTRIBUTION_BIAS_LOW ||
			distribution.Type == SSPP_DISTRIBUTION_BIAS_HIGH) {
			static const float skew = 0.70f;
			float skewed = skew * abs(gaussian.x) +
				sqrt(1.0f - skew * skew) * gaussian.y;
			shape = distribution.Type == SSPP_DISTRIBUTION_BIAS_LOW ? -skewed : skewed;
		} else {
			shape = (SSPPRandom01(
				seed, particleOrdinal, generation, streamBase) - 0.5f) *
				(2.0f * SSPP_TRAJECTORY_NORMAL_PERCENTILE / 0.90f);
		}
	}

	float component = SSPPRandom01(
		seed, particleOrdinal, generation, streamBase + 2u);
	shape *= component < 0.85f ? 1.0f : (component < 0.97f ? 1.8f : 3.0f);
	return clamp(shape, -8.0f, 8.0f);
}

// Summary: Maps a heavy-tail shape into a percentile-defined property range.
// Args:
//   distribution: Output range and distribution type.
//   shape: Standardized heavy-tail sample.
// Returns: Property value whose configured bounds approximate outer percentiles.
float SSPPMapHeavyTailRange(SSPPScalarDistribution distribution, float shape)
{
	if (distribution.Type == SSPP_DISTRIBUTION_CONSTANT)
		return distribution.Range.x;
	float center = (distribution.Range.x + distribution.Range.y) * 0.5f;
	float halfRange = (distribution.Range.y - distribution.Range.x) * 0.5f;
	float percentile = distribution.Type == SSPP_DISTRIBUTION_UNIFORM ?
		SSPP_TRAJECTORY_UNIFORM_PERCENTILE : SSPP_TRAJECTORY_NORMAL_PERCENTILE;
	return center + halfRange * shape / percentile;
}

// Summary: Constructs a normalized launch direction from sampled angles.
// Args:
//   trajectoryPolicy: Independent-angle or coupled heavy-tail policy.
//   forward: Orthonormal launch-frame forward axis.
//   right: Orthonormal launch-frame right axis.
//   vertical: Orthonormal launch-frame vertical axis.
//   horizontalDegrees: Sampled horizontal angle in degrees.
//   verticalDegrees: Sampled vertical angle in degrees.
// Returns: Normalized world-space launch direction.
float3 BuildEmissionDirection(
	uint trajectoryPolicy,
	float3 forward,
	float3 right,
	float3 vertical,
	float horizontalDegrees,
	float verticalDegrees)
{
	if (trajectoryPolicy == SSPP_TRAJECTORY_PERCENTILE_HEAVY_TAIL_COUPLED) {
		float horizontalAngle =
			clamp(horizontalDegrees, -80.0f, 80.0f) * SSPP_DEGREES_TO_RADIANS;
		float verticalAngle =
			clamp(verticalDegrees, -80.0f, 80.0f) * SSPP_DEGREES_TO_RADIANS;
		return SSPPSafeNormalize(
			forward + right * tan(horizontalAngle) + vertical * tan(verticalAngle),
			forward);
	}

	float horizontalSine;
	float horizontalCosine;
	float verticalSine;
	float verticalCosine;
	sincos(horizontalDegrees * SSPP_DEGREES_TO_RADIANS, horizontalSine, horizontalCosine);
	sincos(verticalDegrees * SSPP_DEGREES_TO_RADIANS, verticalSine, verticalCosine);
	float3 horizontalDirection =
		forward * horizontalCosine + right * horizontalSine;
	return SSPPSafeNormalize(
		horizontalDirection * verticalCosine + vertical * verticalSine,
		forward);
}

// Summary: Samples a point or box emitter's world-space spawn position.
// Args:
//   request: Source request containing shape, origin, and seed.
//   particleOrdinal: Particle ordinal within the request.
//   generation: Stable-slot generation.
//   boundsMinimum: Ordered box minimum.
//   boundsMaximum: Ordered box maximum.
// Returns: Request origin for points, or a deterministic point inside the box.
float3 BuildSpawnPosition(
	SSPPBurstRequest request,
	uint particleOrdinal,
	uint generation,
	float3 boundsMinimum,
	float3 boundsMaximum)
{
	if (request.SpawnShape != SSPP_EMITTER_SHAPE_BOX)
		return request.Origin;
	float3 sample = float3(
		SSPPRandom01(request.Seed, particleOrdinal, generation, 16u),
		SSPPRandom01(request.Seed, particleOrdinal, generation, 17u),
		SSPPRandom01(request.Seed, particleOrdinal, generation, 18u));
	return lerp(boundsMinimum, boundsMaximum, sample);
}

// Summary: Appends one common newborn to a bounded initial lifecycle route.
// Args:
//   routeID: Planned dynamic, persistent, or free-return route.
//   reference: Newly initialized generation-tagged slot reference.
// Returns: True when the route accepted the reference.
bool AppendEmissionRoute(uint routeID, SSPPParticleReference reference)
{
	if (routeID >= SSPP_ROUTE_COUNT)
		return false;
	return SSPPAppendBoundedRoute(
		OutputRouteArena, Counters, RouteDescriptors[routeID], reference);
}

[numthreads(64, 1, 1)]
// Summary: Expands one granted chunk into initialized particle slots.
// Args:
//   groupID: Emission-chunk index in its x component.
//   groupThreadID: Lane distributing particles within the chunk.
// Returns: None.
void EmitMain(
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID)
{
	uint chunkIndex = groupID.x;
	SSPPEmissionChunk chunk = EmissionChunks[chunkIndex];
	SSPPBurstRequest request = FrameRequests[chunk.RequestIndex];
	SSPPParticleTemplate particleTemplate = ParticleTemplates[request.ParticleTemplate];
	if (groupThreadID.x == 0u) {
		GroupForward = SSPPSafeNormalize(request.Forward, float3(0.0f, 1.0f, 0.0f));
		float3 referenceUp = abs(GroupForward.z) < 0.999f ?
			float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
		float3 fallbackRight = SSPPSafeNormalize(
			cross(GroupForward, referenceUp), float3(1.0f, 0.0f, 0.0f));
		GroupRight = request.Right - GroupForward * dot(request.Right, GroupForward);
		GroupRight = SSPPSafeNormalize(GroupRight, fallbackRight);
		GroupVertical = request.Vertical -
			GroupForward * dot(request.Vertical, GroupForward) -
			GroupRight * dot(request.Vertical, GroupRight);
		GroupVertical = SSPPSafeNormalize(GroupVertical, cross(GroupRight, GroupForward));
		GroupSpawnBoundsMin = min(request.SpawnBoundsMin, request.SpawnBoundsMax);
		GroupSpawnBoundsMax = max(request.SpawnBoundsMin, request.SpawnBoundsMax);
	}
	GroupMemoryBarrierWithGroupSync();

	// Each lane initializes a bounded strided subset of its chunk.
	for (uint localOrdinal = groupThreadID.x;
		localOrdinal < chunk.Count;
		localOrdinal += 64u) {
		uint ordinal = chunk.FirstOrdinal + localOrdinal;
		uint particleID = FreeIDs[chunk.FreeBase + localOrdinal];
		if (particleID >= SSPPCapacity) {
			InterlockedAdd(
				Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS],
				1u);
			continue;
		}

		uint generation = SSPPNextGeneration(Metadata[particleID].w);
		float speed = 0.0f;
		float horizontalDegrees = 0.0f;
		float verticalDegrees = 0.0f;
		if (request.TrajectoryPolicy == SSPP_TRAJECTORY_PERCENTILE_HEAVY_TAIL_COUPLED) {
			float speedShape = SSPPSampleHeavyTailShape(
				request.InitialSpeed, request.Seed, ordinal, generation, 0u);
			float horizontalShape = SSPPSampleHeavyTailShape(
				request.HorizontalAngleDegrees, request.Seed, ordinal, generation, 1u);
			float verticalShape = SSPPSampleHeavyTailShape(
				request.VerticalAngleDegrees, request.Seed, ordinal, generation, 2u);
			speed = max(SSPPMapHeavyTailRange(request.InitialSpeed, speedShape), 0.0f);
			horizontalDegrees = SSPPMapHeavyTailRange(
				request.HorizontalAngleDegrees, horizontalShape);
			verticalDegrees = SSPPMapHeavyTailRange(
				request.VerticalAngleDegrees, verticalShape);
			float angularPeriphery = length(float2(horizontalShape, verticalShape));
			float peripheralSpeedScale = lerp(
				1.0f, 0.78f, saturate((angularPeriphery - 1.0f) / 4.0f));
			float speedNoise = lerp(
				0.90f,
				1.10f,
				SSPPRandom01(request.Seed, ordinal, generation, 35u));
			speed *= peripheralSpeedScale * speedNoise;
		} else {
			speed = max(SSPPSampleDistribution(
				request.InitialSpeed, request.Seed, ordinal, generation, 0u), 0.0f);
			horizontalDegrees = SSPPSampleDistribution(
				request.HorizontalAngleDegrees, request.Seed, ordinal, generation, 1u);
			verticalDegrees = SSPPSampleDistribution(
				request.VerticalAngleDegrees, request.Seed, ordinal, generation, 2u);
		}
		float lifetime = max(SSPPSampleDistribution(
			request.Lifetime, request.Seed, ordinal, generation, 3u), 1.0e-4f);
		float radius = max(SSPPSampleDistribution(
			request.Radius, request.Seed, ordinal, generation, 4u), 0.0f);
		float opacity = saturate(SSPPSampleDistribution(
			request.Opacity, request.Seed, ordinal, generation, 5u));
		float variation = SSPPRandom01(request.Seed, ordinal, generation, 24u);
		float3 direction = BuildEmissionDirection(
			request.TrajectoryPolicy,
			GroupForward,
			GroupRight,
			GroupVertical,
			horizontalDegrees,
			verticalDegrees);
		float3 spawnPosition = BuildSpawnPosition(
			request,
			ordinal,
			generation,
			GroupSpawnBoundsMin,
			GroupSpawnBoundsMax);
		bool finiteParticle = isfinite(speed) && isfinite(horizontalDegrees) &&
			isfinite(verticalDegrees) && isfinite(lifetime) && isfinite(radius);
		if (!finiteParticle) {
			speed = 0.0f;
			lifetime = 0.0001f;
			radius = 0.0f;
			opacity = 0.0f;
			direction = float3(0.0f, 0.0f, 1.0f);
			spawnPosition = float3(0.0f, 0.0f, 0.0f);
			InterlockedAdd(
				Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
				1u);
		}

		PositionAge[particleID] = float4(spawnPosition, 0.0f);
		VelocityLifetime[particleID] = float4(
			direction * speed * max(request.Strength, 0.0f),
			lifetime);
		Appearance[particleID] = float4(
			radius, opacity, variation, request.GeneralScalar);
		Metadata[particleID] = uint4(
			particleTemplate.SimulationProfile,
			particleTemplate.RenderClass,
			0u,
			generation);
		ContactData[particleID] = 0.0f;

		SSPPParticleAuxState auxiliary;
		auxiliary.EmitterIndex = (chunk.Flags & SSPP_PLAN_FLAG_VALID_OWNER) != 0u ?
			request.OwnerEmitter : SSPP_INVALID_ID;
		auxiliary.EmitterGeneration = (chunk.Flags & SSPP_PLAN_FLAG_VALID_OWNER) != 0u ?
			request.OwnerEmitterGeneration : 0u;
		auxiliary.OwnerFeature = request.OwnerFeature;
		auxiliary.FeaturePayloadID = request.PayloadID;
		auxiliary.LifecyclePhase = SSPP_LIFECYCLE_RESERVED;
		auxiliary.PersistentListID = SSPP_INVALID_ID;
		auxiliary.LastRouteEpoch = 0u;
		auxiliary.Flags = request.Flags;
		ParticleAuxiliaryState[particleID] = auxiliary;

		SSPPParticleReference reference;
		reference.ParticleID = particleID;
		reference.SlotGeneration = generation;
		uint routeID = (chunk.Flags & SSPP_PLAN_FLAG_PERSISTENT) != 0u ?
			SSPP_PERSISTENT_ROUTE_BASE + chunk.PersistentListID :
			SSPP_DYNAMIC_ROUTE_BASE + chunk.SimulationClass;
		// Feature initializers exclusively own their newborns' first lifecycle route.
		if (!finiteParticle) {
			AppendEmissionRoute(SSPP_FREE_RETURN_ROUTE, reference);
		} else if (chunk.CustomInitialization == SSPP_INVALID_ID) {
			// Keep side-effecting appends out of short-circuit expressions for FXC.
			if (!AppendEmissionRoute(routeID, reference))
				AppendEmissionRoute(SSPP_FREE_RETURN_ROUTE, reference);
		}
	}
}
