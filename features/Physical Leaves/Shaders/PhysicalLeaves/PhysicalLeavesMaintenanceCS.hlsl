#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"
#include "PhysicalLeaves/PhysicalLeavesShared.hlsli"

// Maintains grounded leaves, validates optional emitter ownership, and wakes
// leaves that intersect the current player capsule into SSPP simulation routes.

struct PhysicalLeafPlayerCapsule
{
	float3 PointA;
	float Radius;
	float3 PointB;
	float Pad;
};

StructuredBuffer<SSPPParticleReference> PersistentInput : register(t0);
StructuredBuffer<SSPPRouteDescriptor> RouteDescriptors : register(t7);
StructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(t8);
StructuredBuffer<SSPPPersistentEmitter> EmitterStates : register(t9);

RWStructuredBuffer<float4> PositionAge : register(u0);
RWStructuredBuffer<float4> VelocityLifetime : register(u1);
RWStructuredBuffer<float4> Appearance : register(u2);
RWStructuredBuffer<uint4> Metadata : register(u3);
RWStructuredBuffer<float4> ContactData : register(u4);
RWStructuredBuffer<SSPPParticleReference> OutputRouteArena : register(u5);
RWStructuredBuffer<PhysicalLeafSidecar> LeafSidecars : register(u6);
RWStructuredBuffer<uint> Counters : register(u7);

// Compact the dominant grounded-survivor path once per thread group. Routing
// each untouched leaf through one global CAS loop serialized large leaf fields.
groupshared uint KeepCount;
groupshared uint KeepBase;
groupshared uint KeepGranted;
groupshared uint KeepArenaBase;

cbuffer PhysicalLeavesMaintenance : register(b4)
{
	uint InputBase;
	uint InputCounterOffset;
	uint KeepRoute;
	uint FreeRoute;

	uint DynamicRouteBase;
	uint DynamicSimulationClass;
	uint PersistentListID;
	uint ParticleCapacity;

	uint PlayerCapsuleCount;
	uint PlayerCollisionEnabled;
	float CapsulePadding;
	float ElapsedSeconds;

	float3 PlayerVelocity;
	float PlayerVelocityInfluence;

	float PlayerHorizontalImpulse;
	float PlayerVerticalImpulse;
	float ParticleCollisionRadiusScale;
	float DepenetrationOffset;

	PhysicalLeafPlayerCapsule PlayerCapsules[1];
};

// Summary: Appends a leaf reference with free-route and quarantine fallback.
// Args:
//   routeID: Preferred lifecycle route.
//   reference: Generation-tagged leaf reference.
// Returns: None.
void QueueRoute(uint routeID, SSPPParticleReference reference)
{
	if (routeID >= SSPP_ROUTE_COUNT)
		return;
	SSPPRouteDescriptor descriptor = RouteDescriptors[routeID];
	uint routeIndex = descriptor.Capacity;
	if (SSPPReserveBoundedCounter(
		Counters, descriptor.CounterOffset, descriptor.Capacity, routeIndex)) {
		OutputRouteArena[descriptor.BaseOffset + routeIndex] = reference;
		return;
	}

	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS], 1u);
	// Capacity pressure is not particle corruption. This maintenance dispatch
	// owns one input reference per particle, so returning a failed keep/wake
	// destination to the global free route is bounded by the allocated pool.
	if (routeID != FreeRoute && FreeRoute < SSPP_ROUTE_COUNT) {
		SSPPRouteDescriptor freeDescriptor = RouteDescriptors[FreeRoute];
		uint freeIndex = freeDescriptor.Capacity;
		if (SSPPReserveBoundedCounter(
			Counters, freeDescriptor.CounterOffset, freeDescriptor.Capacity, freeIndex)) {
			OutputRouteArena[freeDescriptor.BaseOffset + freeIndex] = reference;
			return;
		}
	}
	if (routeID == SSPP_QUARANTINE_ROUTE)
		return;
	SSPPRouteDescriptor quarantine = RouteDescriptors[SSPP_QUARANTINE_ROUTE];
	uint quarantineIndex = quarantine.Capacity;
	if (SSPPReserveBoundedCounter(
		Counters, quarantine.CounterOffset, quarantine.Capacity, quarantineIndex))
		OutputRouteArena[quarantine.BaseOffset + quarantineIndex] = reference;
}

// Summary: Validates the persistent emitter recorded by a leaf.
// Args:
//   auxiliary: Leaf lifecycle and emitter ownership state.
// Returns: True while the matching emitter generation remains active.
bool EmitterStillOwns(SSPPParticleAuxState auxiliary)
{
	if ((auxiliary.Flags & PHYSICAL_LEAF_FLAG_STANDALONE_FIELD) != 0u)
		return true;
	uint emitterCount;
	uint emitterStride;
	EmitterStates.GetDimensions(emitterCount, emitterStride);
	if (auxiliary.EmitterIndex == SSPP_INVALID_ID ||
		auxiliary.EmitterIndex >= emitterCount)
		return false;
	SSPPPersistentEmitter emitter = EmitterStates[auxiliary.EmitterIndex];
	return emitter.State == SSPP_EMITTER_STATE_ACTIVE &&
		emitter.Generation == auxiliary.EmitterGeneration;
}

// Summary: Tests a leaf sphere against the current player capsule.
// Args:
//   position: Leaf center in world space.
//   particleRadius: Leaf collision radius.
//   contactNormal: Receives the outward capsule normal.
//   penetration: Receives overlap depth.
// Returns: True when the leaf overlaps the capsule.
bool FindCapsuleContact(
	float3 position,
	float particleRadius,
	out float3 contactNormal,
	out float penetration)
{
	contactNormal = float3(0.0f, 0.0f, 1.0f);
	penetration = 0.0f;
	if (PlayerCapsuleCount == 0u)
		return false;

	PhysicalLeafPlayerCapsule capsule = PlayerCapsules[0];
	float3 segment = capsule.PointB - capsule.PointA;
	float segmentLengthSquared = dot(segment, segment);
	float segmentParameter = segmentLengthSquared > 1.0e-6f ?
		saturate(dot(position - capsule.PointA, segment) / segmentLengthSquared) : 0.0f;
	float3 delta = position - (capsule.PointA + segment * segmentParameter);
	float distanceSquared = dot(delta, delta);
	float collisionRadius = max(capsule.Radius, 0.0f) +
		max(CapsulePadding, 0.0f) + max(particleRadius, 0.0f);
	if (distanceSquared >= collisionRadius * collisionRadius)
		return false;

	float distanceValue = sqrt(max(distanceSquared, 1.0e-8f));
	contactNormal = distanceSquared > 1.0e-8f ?
		delta / distanceValue : float3(0.0f, 0.0f, 1.0f);
	penetration = collisionRadius - distanceValue;
	return true;
}

[numthreads(64, 1, 1)]
// Summary: Maintains one persistent leaf and routes grounded, woken, or expired state.
// Args:
//   dispatchID: Persistent-list element index.
// Returns: None.
void MaintainMain(
	uint3 dispatchID : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0u) {
		KeepCount = 0u;
		KeepBase = 0u;
		KeepGranted = 0u;
		KeepArenaBase = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	uint inputCount = min(Counters[InputCounterOffset], ParticleCapacity);
	SSPPParticleReference reference;
	reference.ParticleID = SSPP_INVALID_ID;
	reference.SlotGeneration = 0u;
	bool keep = false;
	uint routeID = SSPP_INVALID_ID;

	if (dispatchID.x < inputCount) {
		reference = PersistentInput[InputBase + dispatchID.x];
		if (reference.ParticleID < ParticleCapacity && reference.SlotGeneration != 0u) {
			uint particleID = reference.ParticleID;
			uint4 metadata = Metadata[particleID];
			SSPPParticleAuxState auxiliary = ParticleAuxiliaryState[particleID];
			if (metadata.w == reference.SlotGeneration &&
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_PERSISTENT &&
				auxiliary.PersistentListID == PersistentListID) {
				if (!EmitterStillOwns(auxiliary)) {
					routeID = FreeRoute;
				} else {
					float4 positionAge = PositionAge[particleID];
					float4 velocityLifetime = VelocityLifetime[particleID];
					float4 appearance = Appearance[particleID];
					float4 contactData = ContactData[particleID];
					if (!all(isfinite(positionAge)) || !all(isfinite(velocityLifetime)) ||
						!all(isfinite(appearance)) || !all(isfinite(contactData))) {
						routeID = FreeRoute;
					} else {
						PhysicalLeafSidecar sidecar = LeafSidecars[particleID];
						bool newGeneration = sidecar.Generation != reference.SlotGeneration;
						bool newlyGrounded = !newGeneration &&
							sidecar.State == PHYSICAL_LEAF_STATE_AIRBORNE;
						if (newGeneration || newlyGrounded ||
							sidecar.State == PHYSICAL_LEAF_STATE_UNINITIALIZED) {
							float3 groundNormal = SSPPSafeNormalize(
								contactData.xyz, float3(0.0f, 0.0f, 1.0f));
							sidecar.HomePosition = positionAge.xyz;
							sidecar.HomeNormal = groundNormal;
							sidecar.BaseRotation = newGeneration ?
								frac(appearance.z) * Math::TAU :
								sidecar.BaseRotation +
									sidecar.AngularVelocity * max(positionAge.w, 0.0f);
							float tiltAngle = lerp(
								-0.13962634f,
								0.13962634f,
								SSPPRandom01(
									asuint(appearance.z),
									particleID,
									reference.SlotGeneration,
									31u));
							float tiltCosine;
							sincos(tiltAngle, sidecar.GroundTilt.x, tiltCosine);
							sidecar.GroundTilt.y = tiltCosine - 1.0f;
							sidecar.AngularVelocity = 0.0f;
							sidecar.Generation = reference.SlotGeneration;
							sidecar.State = PHYSICAL_LEAF_STATE_GROUNDED;
							positionAge.w = 0.0f;
							PositionAge[particleID] = positionAge;
							LeafSidecars[particleID] = sidecar;
						}

						float3 contactNormal;
						float penetration;
						if (PlayerCollisionEnabled == 0u || PlayerCapsuleCount == 0u ||
							ElapsedSeconds <= 0.0f ||
							!FindCapsuleContact(
								positionAge.xyz,
								max(appearance.x, 0.0f) *
									max(ParticleCollisionRadiusScale, 0.0f),
								contactNormal,
								penetration)) {
							keep = true;
						} else {
							float2 horizontal = contactNormal.xy;
							if (dot(horizontal, horizontal) <= 1.0e-6f)
								horizontal = PlayerVelocity.xy;
							if (dot(horizontal, horizontal) <= 1.0e-6f) {
								float angle = SSPPRandom01(
									particleID,
									reference.SlotGeneration,
									SSPPRouteEpoch,
									17u) * Math::TAU;
								horizontal = float2(cos(angle), sin(angle));
							} else {
								horizontal = normalize(horizontal);
							}

							positionAge.xy += horizontal *
								(penetration + max(DepenetrationOffset, 0.0f));
							positionAge.w = 0.0f;
							velocityLifetime.xyz =
								float3(horizontal * max(PlayerHorizontalImpulse, 0.0f),
									max(PlayerVerticalImpulse, 0.0f)) +
								PlayerVelocity * max(PlayerVelocityInfluence, 0.0f);
							sidecar.BaseRotation +=
								sidecar.AngularVelocity * max(positionAge.w, 0.0f);
							sidecar.AngularVelocity = lerp(
								-8.0f,
								8.0f,
								SSPPRandom01(
									particleID,
									reference.SlotGeneration,
									SSPPRouteEpoch,
									23u));
							sidecar.State = PHYSICAL_LEAF_STATE_AIRBORNE;
							PositionAge[particleID] = positionAge;
							VelocityLifetime[particleID] = velocityLifetime;
							LeafSidecars[particleID] = sidecar;
							routeID = DynamicRouteBase + DynamicSimulationClass;
						}
					}
				}
			}
		}
	}

	if (!keep && routeID != SSPP_INVALID_ID)
		QueueRoute(routeID, reference);

	uint localKeepIndex = 0u;
	if (keep)
		InterlockedAdd(KeepCount, 1u, localKeepIndex);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u && KeepCount != 0u && KeepRoute < SSPP_ROUTE_COUNT) {
		SSPPRouteDescriptor descriptor = RouteDescriptors[KeepRoute];
		KeepArenaBase = descriptor.BaseOffset;
		KeepGranted = SSPPReserveBoundedBlock(
			Counters,
			descriptor.CounterOffset,
			descriptor.Capacity,
			KeepCount,
			KeepBase);
	}
	GroupMemoryBarrierWithGroupSync();

	if (!keep)
		return;
	if (localKeepIndex < KeepGranted) {
		OutputRouteArena[KeepArenaBase + KeepBase + localKeepIndex] = reference;
		return;
	}

	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS], 1u);
	QueueRoute(FreeRoute, reference);
}
