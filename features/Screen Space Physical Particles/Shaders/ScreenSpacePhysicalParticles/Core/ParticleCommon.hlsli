#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_CORE_COMMON_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_CORE_COMMON_INCLUDED

#include "Common/Math.hlsli"

// Defines the shader-side SSPP ABI shared by all runtime passes and feature
// shaders. Constants, structures, and constant buffers mirror ParticleTypes.h
// and ParticleRuntime frame records exactly.
static const uint SSPP_INVALID_ID = 0xFFFFFFFFu;

static const uint SSPP_SIMULATION_NO_COLLISION = 0u;
static const uint SSPP_SIMULATION_WORLD_WATER = 2u;
static const uint SSPP_SIMULATION_WORLD_PLAYER = 3u;
static const uint SSPP_SIMULATION_CLASS_COUNT = 4u;
static const uint SSPP_MAX_NEWBORN_INITIALIZERS = 8u;
static const uint SSPP_MAX_FEATURES = 32u;

static const uint SSPP_RENDER_CULL_FRUSTUM = 1u << 0u;
static const uint SSPP_RENDER_CULL_DISTANCE = 1u << 1u;

static const uint SSPP_LIFECYCLE_FREE = 0u;
static const uint SSPP_LIFECYCLE_RESERVED = 1u;
static const uint SSPP_LIFECYCLE_DYNAMIC = 2u;
static const uint SSPP_LIFECYCLE_PERSISTENT = 3u;
static const uint SSPP_LIFECYCLE_PENDING_FREE = 4u;
static const uint SSPP_LIFECYCLE_RECYCLING = 5u;
static const uint SSPP_LIFECYCLE_QUARANTINED = 6u;

static const uint SSPP_MAX_PERSISTENT_LISTS = 8u;
static const uint SSPP_MAX_EVENT_ROUTES = 8u;
static const uint SSPP_DYNAMIC_ROUTE_BASE = 0u;
static const uint SSPP_PERSISTENT_ROUTE_BASE = 4u;
static const uint SSPP_FREE_RETURN_ROUTE = 12u;
static const uint SSPP_QUARANTINE_ROUTE = 13u;
static const uint SSPP_ROUTE_COUNT = 14u;

static const uint SSPP_ROUTE_DESTINATION_DYNAMIC = 0u;
static const uint SSPP_ROUTE_DESTINATION_PERSISTENT = 1u;
static const uint SSPP_ROUTE_DESTINATION_QUARANTINE = 3u;

static const uint SSPP_DISTRIBUTION_CONSTANT = 0u;
static const uint SSPP_DISTRIBUTION_UNIFORM = 1u;
static const uint SSPP_DISTRIBUTION_NORMAL = 2u;
static const uint SSPP_DISTRIBUTION_BIAS_LOW = 3u;
static const uint SSPP_DISTRIBUTION_BIAS_HIGH = 4u;

static const uint SSPP_TRAJECTORY_PERCENTILE_HEAVY_TAIL_COUPLED = 1u;

static const uint SSPP_PLACEMENT_DIRECT = 0u;

static const uint SSPP_EMITTER_SHAPE_BOX = 1u;

static const uint SSPP_COLLISION_WORLD_SOLID = 1u << 0u;
static const uint SSPP_COLLISION_WATER = 1u << 1u;
static const uint SSPP_COLLISION_PLAYER = 1u << 2u;

static const uint SSPP_QUERY_PERSPECTIVE_DEPTH = 1u << 0u;
static const uint SSPP_QUERY_SOLID_HEIGHT = 1u << 1u;
static const uint SSPP_QUERY_WATER_HEIGHT = 1u << 2u;
static const uint SSPP_QUERY_PLAYER_CAPSULES = 1u << 3u;

static const uint SSPP_RESPONSE_IGNORE = 0u;
static const uint SSPP_RESPONSE_BOUNCE = 1u;
static const uint SSPP_RESPONSE_SLIDE = 2u;
static const uint SSPP_RESPONSE_SETTLE = 3u;
static const uint SSPP_RESPONSE_STICK = 4u;
static const uint SSPP_RESPONSE_KILL = 5u;
static const uint SSPP_RESPONSE_EMIT_EVENT_AND_KILL = 6u;
static const uint SSPP_COLLISION_RULE_RESET_AGE = 1u << 0u;

// Diagnostics are consecutive counters beginning at FrameConstants::diagnosticsBase.
static const uint SSPP_DIAGNOSTIC_BURST_REQUESTS = 0u;
static const uint SSPP_DIAGNOSTIC_REQUESTED_PARTICLES = 1u;
static const uint SSPP_DIAGNOSTIC_GRANTED_PARTICLES = 2u;
static const uint SSPP_DIAGNOSTIC_DROPPED_PARTICLES = 3u;
static const uint SSPP_DIAGNOSTIC_RECYCLED_PARTICLES = 4u;
static const uint SSPP_DIAGNOSTIC_INVALID_PROFILES = 5u;
static const uint SSPP_DIAGNOSTIC_LIST_OVERFLOWS = 6u;
static const uint SSPP_DIAGNOSTIC_EVENT_OVERFLOWS = 7u;
static const uint SSPP_DIAGNOSTIC_COLLISION_QUERIES = 8u;
static const uint SSPP_DIAGNOSTIC_COLLISION_HITS = 11u;
static const uint SSPP_DIAGNOSTIC_COLLISION_RESPONSES = 14u;
static const uint SSPP_DIAGNOSTIC_INVALID_COLLISION_RULES = 21u;

struct SSPPScalarDistribution
{
	float2 Range;
	uint Type;
	float Shape;
};

struct SSPPSimulationProfile
{
	float3 Acceleration;
	float Drag;

	float Restitution;
	float Friction;
	float SettleSpeed;
	float SurfaceOffset;

	uint CollisionMask;
	uint QueryPolicy;
	uint SimulationClass;
	uint ResponseTableOffset;
};

struct SSPPCollisionRule
{
	uint Response;
	uint DestinationListID;
	uint NextSimulationProfileID;
	uint NextRenderClassID;

	uint EventRoute;
	uint EventPayloadID;
	float RestitutionOverride;
	float FrictionOverride;

	float MinimumParticleAge;
	float CollisionRadiusScale;
	float SeparationOffset;
	float MinimumSettleNormalZ;

	float NextLifetimeOverride;
	uint Flags;
	uint2 Pad;
};

struct SSPPParticleTemplate
{
	uint SimulationProfile;
	uint RenderClass;
	uint Pad0;
	uint LifecyclePhase;

	uint PersistentListID;
	uint CustomInitialization;
	uint SpawnPlacement;
	uint Flags;
};

struct SSPPParticleFeature
{
	uint MaximumLiveParticles;
	uint3 Pad;
};

struct SSPPBurstRequest
{
	float3 Origin;
	float Strength;

	float3 Forward;
	uint Seed;

	float3 Right;
	uint Count;

	float3 Vertical;
	uint ParticleTemplate;

	SSPPScalarDistribution InitialSpeed;
	SSPPScalarDistribution HorizontalAngleDegrees;
	SSPPScalarDistribution VerticalAngleDegrees;
	SSPPScalarDistribution Lifetime;
	SSPPScalarDistribution Radius;
	SSPPScalarDistribution Opacity;

	float GeneralScalar;
	uint Priority;
	uint PayloadID;
	uint OwnerEmitter;

	uint OwnerEmitterGeneration;
	uint OwnerFeature;
	uint Flags;
	uint TrajectoryPolicy;

	float3 SpawnBoundsMin;
	uint SpawnShape;

	float3 SpawnBoundsMax;
	uint SpawnPlacement;
};

struct SSPPEmissionChunk
{
	uint RequestIndex;
	uint FreeBase;
	uint FirstOrdinal;
	uint Count;

	uint SimulationClass;
	uint PersistentListID;
	uint CustomInitialization;
	uint Flags;
};

struct SSPPParticleReference
{
	uint ParticleID;
	uint SlotGeneration;
};

struct SSPPParticleAuxState
{
	uint EmitterIndex;
	uint EmitterGeneration;
	uint OwnerFeature;
	uint FeaturePayloadID;

	uint LifecyclePhase;
	uint PersistentListID;
	uint LastRouteEpoch;
	uint Flags;
};

struct SSPPRouteDescriptor
{
	uint BaseOffset;
	uint Capacity;
	uint CounterOffset;
	uint DestinationKind;

	uint DestinationIndex;
	uint OwnerFeature;
	uint Flags;
	uint Pad;
};

struct SSPPParticleExitEvent
{
	float3 Position;
	float Radius;

	float3 Velocity;
	float Amount;

	float3 SurfaceNormal;
	float FeatureScalar;

	uint EventRoute;
	uint EventPayloadID;
	uint CollisionChannel;
	uint Flags;
};

struct SSPPRenderClass
{
	uint Enabled;
	uint VertexCountPerInstance;
	uint StartVertexLocation;
	uint PreparationThreads;

	float CullRadiusScale;
	float MaximumDistance;
	uint CullingFlags;
	uint Pad;
};

// Exact mirror of ParticleRuntime::FrameConstants.
cbuffer SSPPFrameConstants : register(b0)
{
	uint SSPPCapacity;
	uint SSPPListSegmentCapacity;
	uint SSPPInputCounterBase;
	uint SSPPOutputCounterBase;

	uint SSPPDispatchClass;
	uint SSPPFramePad0;
	uint SSPPSimulationProfileCount;
	uint SSPPParticleTemplateCount;

	float SSPPDeltaTime;
	uint SSPPRenderClassCount;
	uint SSPPRenderCounterBase;
	uint SSPPFrameCulledCounter;

	uint SSPPFreeTopCounter;
	uint SSPPAllocatedCounter;
	uint SSPPHighWaterCounter;
	uint SSPPDiagnosticsBase;

	uint SSPPRegisteredFeatureCount;
	uint SSPPFeatureLiveCounterBase;
	uint SSPPFeatureRequestedCounterBase;
	uint SSPPFeatureGrantedCounterBase;

	uint SSPPFeatureDroppedCounterBase;
	uint SSPPFeatureBudgetClampedCounterBase;
	uint2 SSPPFeatureCounterPad;
};

// Exact 64-byte mirror of ParticleRuntime::LifecycleFrameConstants at b3.
cbuffer SSPPLifecycleFrameConstants : register(b3)
{
	uint SSPPRouteCounterBase;
	uint SSPPEventCountCounter;
	uint SSPPEventCapacity;
	uint SSPPRouteEpoch;

	uint SSPPPersistentCounterBaseA;
	uint SSPPPersistentCounterBaseB;
	uint SSPPPersistentDestinationParityMask;
	uint SSPPRegisteredPersistentListCount;

	uint SSPPEventsProducedCounterBase;
	uint SSPPEventsConsumedCounterBase;
	uint SSPPLifecycleDiagnosticsBase;
	uint SSPPDispatchRoute;

	uint SSPPMaintenanceList;
	uint SSPPMaintenancePolicy;
	float SSPPMaintenanceElapsed;
	uint SSPPRetiringOwnerFeature;
};

static const uint SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES = 0u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_DUPLICATE_ROUTES = 1u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS = 2u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED = 3u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_FREE_RETURNS = 4u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_MAINTENANCE_RUNS = 5u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_DYNAMIC_TRANSITIONS = 6u;
static const uint SSPP_LIFECYCLE_DIAGNOSTIC_PERSISTENT_TRANSITIONS = 10u;

// Exact 16-byte mirror of ParticleRuntime::RenderFrameConstants at b4.
cbuffer SSPPRenderFrameConstants : register(b4)
{
	uint SSPPRenderDiagnosticsBase;
	uint SSPPRegisteredRenderClassCount;
	uint SSPPRenderClassCapacity;
	uint SSPPRenderFramePad;
};

static const uint SSPP_RENDER_DIAGNOSTIC_CANDIDATES = 0u;
static const uint SSPP_RENDER_DIAGNOSTIC_FRUSTUM_CULLED = 1u;
static const uint SSPP_RENDER_DIAGNOSTIC_DISTANCE_CULLED = 2u;
static const uint SSPP_RENDER_DIAGNOSTIC_DISABLED_OR_MISSING = 3u;
static const uint SSPP_RENDER_DIAGNOSTIC_INVALID_CLASS = 4u;
static const uint SSPP_RENDER_DIAGNOSTIC_STALE_REFERENCES = 5u;
static const uint SSPP_RENDER_DIAGNOSTIC_BIN_OVERFLOWS = 6u;
static const uint SSPP_RENDER_DIAGNOSTIC_COUNT = 7u;

// Summary: Applies a deterministic 32-bit integer avalanche hash.
// Args:
//   value: Input integer.
// Returns: Hashed integer.
uint SSPPHash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7FEB352Du;
	value ^= value >> 15u;
	value *= 0x846CA68Bu;
	value ^= value >> 16u;
	return value;
}

// Summary: Generates one deterministic particle-property sample in [0, 1).
// Args:
//   seed: Burst or emitter seed.
//   particleOrdinal: Particle ordinal within its request.
//   generation: Stable-slot generation.
//   stream: Independent property-stream identifier.
// Returns: Pseudorandom scalar in [0, 1).
float SSPPRandom01(uint seed, uint particleOrdinal, uint generation, uint stream)
{
	uint value = seed;
	value ^= particleOrdinal * 0x9E3779B9u;
	value ^= generation * 0x85EBCA6Bu;
	value ^= stream * 0xC2B2AE35u;
	return (float)(SSPPHash(value) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

// Summary: Samples a configured constant, uniform, normal, or biased range.
// Args:
//   distribution: Range, distribution type, and bias shape.
//   seed: Burst or emitter seed.
//   particleOrdinal: Particle ordinal within its request.
//   generation: Stable-slot generation.
//   stream: Independent property-stream identifier.
// Returns: Deterministic sample within distribution.Range.
float SSPPSampleDistribution(
	SSPPScalarDistribution distribution,
	uint seed,
	uint particleOrdinal,
	uint generation,
	uint stream)
{
	if (distribution.Type == SSPP_DISTRIBUTION_CONSTANT)
		return distribution.Range.x;

	float sample = SSPPRandom01(seed, particleOrdinal, generation, stream * 4u);
	if (distribution.Type == SSPP_DISTRIBUTION_NORMAL) {
		// Four bounded uniform samples approximate a finite normal distribution.
		sample = (sample +
			SSPPRandom01(seed, particleOrdinal, generation, stream * 4u + 1u) +
			SSPPRandom01(seed, particleOrdinal, generation, stream * 4u + 2u) +
			SSPPRandom01(seed, particleOrdinal, generation, stream * 4u + 3u)) * 0.25f;
	} else if (distribution.Type == SSPP_DISTRIBUTION_BIAS_LOW) {
		sample = pow(sample, 1.0f + max(distribution.Shape, 0.0f));
	} else if (distribution.Type == SSPP_DISTRIBUTION_BIAS_HIGH) {
		sample = 1.0f - pow(1.0f - sample, 1.0f + max(distribution.Shape, 0.0f));
	}

	return lerp(distribution.Range.x, distribution.Range.y, saturate(sample));
}

// Summary: Normalizes a vector without producing NaN for near-zero input.
// Args:
//   value: Vector to normalize.
//   fallback: Result used when value has negligible length.
// Returns: Normalized value or fallback.
float3 SSPPSafeNormalize(float3 value, float3 fallback)
{
	float lengthSquared = dot(value, value);
	return lengthSquared > 1.0e-8f ? value * rsqrt(lengthSquared) : fallback;
}

// Summary: Resolves a local list index into the shared segmented arena.
// Args:
//   listID: Segment identifier.
//   listIndex: Index local to that segment.
// Returns: Linear arena address.
uint SSPPListAddress(uint listID, uint listIndex)
{
	return listID * SSPPListSegmentCapacity + listIndex;
}

// Summary: Advances a slot generation while reserving zero as invalid.
// Args:
//   generation: Current generation.
// Returns: Next nonzero generation.
uint SSPPNextGeneration(uint generation)
{
	uint next = generation + 1u;
	return next == 0u ? 1u : next;
}

// Summary: Atomically increments a counter without exceeding its limit.
// Args:
//   counters: Counter buffer containing counterIndex.
//   counterIndex: Counter element to reserve.
//   limit: Exclusive maximum value.
//   reservedIndex: Receives the pre-increment value on success.
// Returns: True when one index was reserved.
bool SSPPReserveBoundedCounter(
	RWStructuredBuffer<uint> counters,
	uint counterIndex,
	uint limit,
	out uint reservedIndex)
{
	reservedIndex = limit;
	uint observed = counters[counterIndex];
	[allow_uav_condition]
	while (observed < limit) {
		uint original;
		InterlockedCompareExchange(counters[counterIndex], observed, observed + 1u, original);
		if (original == observed) {
			reservedIndex = observed;
			return true;
		}
		observed = original;
	}
	return false;
}

// Summary: Atomically reserves a bounded contiguous range from an increasing counter.
// Args:
//   counters: Counter buffer containing counterIndex.
//   counterIndex: Counter element to increment.
//   limit: Exclusive counter limit.
//   requested: Maximum requested range length.
//   reservationBase: Receives the first reserved index or limit on failure.
// Returns: Number of reserved indices.
uint SSPPReserveBoundedBlock(
	RWStructuredBuffer<uint> counters,
	uint counterIndex,
	uint limit,
	uint requested,
	out uint reservationBase)
{
	reservationBase = limit;
	uint observed = counters[counterIndex];
	[allow_uav_condition]
	while (requested != 0u && observed < limit) {
		uint granted = min(requested, limit - observed);
		uint original;
		InterlockedCompareExchange(
			counters[counterIndex], observed, observed + granted, original);
		if (original == observed) {
			reservationBase = observed;
			return granted;
		}
		observed = original;
	}
	return 0u;
}

// Summary: Atomically subtracts a bounded amount without underflowing a counter.
// Args:
//   counters: Counter buffer containing counterIndex.
//   counterIndex: Counter element to decrement.
//   requested: Maximum amount to subtract.
// Returns: Amount removed from the counter.
uint SSPPDecrementBoundedCounter(
	RWStructuredBuffer<uint> counters,
	uint counterIndex,
	uint requested)
{
	uint observed = counters[counterIndex];
	[allow_uav_condition]
	while (requested != 0u && observed != 0u) {
		uint decrement = min(requested, observed);
		uint original;
		InterlockedCompareExchange(
			counters[counterIndex], observed, observed - decrement, original);
		if (original == observed)
			return decrement;
		observed = original;
	}
	return 0u;
}

// Summary: Claims a particle once for a routing epoch.
// Args:
//   auxiliaryState: Particle lifecycle state containing route epochs.
//   particleID: Stable particle slot to claim.
//   routeEpoch: Current routing epoch.
// Returns: True when this invocation acquired the epoch claim.
bool SSPPClaimRouteEpoch(
	RWStructuredBuffer<SSPPParticleAuxState> auxiliaryState,
	uint particleID,
	uint routeEpoch)
{
	uint observed = auxiliaryState[particleID].LastRouteEpoch;
	[allow_uav_condition]
	while (observed != routeEpoch) {
		uint original;
		InterlockedCompareExchange(
			auxiliaryState[particleID].LastRouteEpoch, observed, routeEpoch, original);
		if (original == observed)
			return true;
		observed = original;
	}
	return false;
}

// Summary: Appends a particle reference through a bounded route descriptor.
// Args:
//   routeArena: Destination reference arena.
//   counters: Counter buffer containing the route counter and diagnostics.
//   descriptor: Route bounds and counter location.
//   reference: Generation-tagged particle reference to append.
// Returns: True when the route accepted the reference.
bool SSPPAppendBoundedRoute(
	RWStructuredBuffer<SSPPParticleReference> routeArena,
	RWStructuredBuffer<uint> counters,
	SSPPRouteDescriptor descriptor,
	SSPPParticleReference reference)
{
	uint routeIndex = 0u;
	InterlockedAdd(counters[descriptor.CounterOffset], 1u, routeIndex);
	bool accepted = routeIndex < descriptor.Capacity;
	if (accepted) {
		routeArena[descriptor.BaseOffset + routeIndex] = reference;
	} else {
		InterlockedAdd(
			counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS], 1u);
	}
	return accepted;
}

#endif
