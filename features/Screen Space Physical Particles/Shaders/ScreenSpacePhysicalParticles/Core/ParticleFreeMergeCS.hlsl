// Recycles validated free-route references into the global free stack and
// reconciles global, per-feature, and per-emitter live-particle accounting.

#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Route data is read-only. This pass owns free-stack writes, lifecycle resets,
// emitter live counts, and all associated counters.
StructuredBuffer<SSPPParticleReference> InputRouteArena : register(t0);
StructuredBuffer<SSPPRouteDescriptor> FreeRouteDescriptors : register(t1);

RWStructuredBuffer<uint> FreeIDs : register(u0);
RWStructuredBuffer<uint4> ParticleMetadata : register(u1);
RWStructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(u2);
RWStructuredBuffer<SSPPPersistentEmitter> EmitterStates : register(u3);
RWStructuredBuffer<uint> Counters : register(u7);

// Each 64-thread group reserves one free-stack block and aggregates accounting.
groupshared uint GroupReturnCount;
groupshared uint GroupFreeBase;
groupshared uint GroupFreeGranted;
groupshared uint GroupOwnedCount;
groupshared uint GroupStaleReferences;
groupshared uint GroupDuplicateRoutes;
groupshared uint GroupInvalidProfiles;
groupshared uint GroupOwnerMismatches;
groupshared uint GroupQuarantined;
groupshared uint GroupFeatureCounts[SSPP_MAX_FEATURES];
groupshared uint GroupEmitterKeys[64];
groupshared uint GroupEmitterCounts[64];

// Summary: Atomically decrements an emitter's alive count without underflow.
// Args:
//   emitterIndex: Persistent emitter slot index.
//   requested: Maximum amount to subtract.
// Returns: None.
void DecrementEmitterAliveBounded(uint emitterIndex, uint requested)
{
	uint observed = EmitterStates[emitterIndex].AliveCount;
	[allow_uav_condition]
	while (requested != 0u && observed != 0u) {
		uint decrement = min(requested, observed);
		uint original = 0u;
		InterlockedCompareExchange(
			EmitterStates[emitterIndex].AliveCount,
			observed,
			observed - decrement,
			original);
		if (original == observed)
			return;
		observed = original;
	}
}

// Summary: Adds one returned particle to the group's per-emitter hash table.
// Args:
//   emitterIndex: Persistent emitter slot index.
// Returns: None.
void AggregateEmitter(uint emitterIndex)
{
	uint slot = SSPPHash(emitterIndex) & 63u;
	[unroll]
	for (uint probe = 0u; probe < 64u; ++probe) {
		uint original = SSPP_INVALID_ID;
		InterlockedCompareExchange(
			GroupEmitterKeys[slot], SSPP_INVALID_ID, emitterIndex, original);
		if (original == SSPP_INVALID_ID || original == emitterIndex) {
			InterlockedAdd(GroupEmitterCounts[slot], 1u);
			return;
		}
		slot = (slot + 1u) & 63u;
	}
	// A full 64-entry table cannot reject the group's at-most-64 distinct keys.
	InterlockedAdd(GroupOwnerMismatches, 1u);
}

[numthreads(64, 1, 1)]
// Summary: Validates free returns, recycles slots, and updates ownership accounting.
// Args:
//   dispatchID: Dispatch-thread index within the free-return route.
//   groupIndex: Thread index used for block reservation and accounting aggregation.
// Returns: None.
void MergeFreeReturnsMain(
	uint3 dispatchID : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0u) {
		GroupReturnCount = 0u;
		GroupFreeBase = 0u;
		GroupFreeGranted = 0u;
		GroupOwnedCount = 0u;
		GroupStaleReferences = 0u;
		GroupDuplicateRoutes = 0u;
		GroupInvalidProfiles = 0u;
		GroupOwnerMismatches = 0u;
		GroupQuarantined = 0u;
	}
	if (groupIndex < SSPP_MAX_FEATURES)
		GroupFeatureCounts[groupIndex] = 0u;
	GroupEmitterKeys[groupIndex] = SSPP_INVALID_ID;
	GroupEmitterCounts[groupIndex] = 0u;
	GroupMemoryBarrierWithGroupSync();

	SSPPRouteDescriptor descriptor =
		FreeRouteDescriptors[SSPP_FREE_RETURN_ROUTE];
	uint returnCount = min(Counters[descriptor.CounterOffset], descriptor.Capacity);
	uint returnIndex = dispatchID.x;
	bool validReturn = false;
	uint particleID = SSPP_INVALID_ID;
	SSPPParticleReference reference;
	reference.ParticleID = SSPP_INVALID_ID;
	reference.SlotGeneration = 0u;
	SSPPParticleAuxState auxiliary = (SSPPParticleAuxState)0;

	if (returnIndex < returnCount) {
		reference = InputRouteArena[descriptor.BaseOffset + returnIndex];
		if (reference.ParticleID >= SSPPCapacity || reference.SlotGeneration == 0u) {
			InterlockedAdd(GroupStaleReferences, 1u);
		} else {
			particleID = reference.ParticleID;
			uint4 metadata = ParticleMetadata[particleID];
			auxiliary = ParticleAuxiliaryState[particleID];
			if (metadata.w != reference.SlotGeneration ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_FREE ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_RECYCLING ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_QUARANTINED) {
				InterlockedAdd(GroupStaleReferences, 1u);
			} else {
				// Pending-free references were claimed by the transition pass.
				bool claimed = auxiliary.LifecyclePhase == SSPP_LIFECYCLE_PENDING_FREE ?
					auxiliary.LastRouteEpoch == SSPPRouteEpoch :
					SSPPClaimRouteEpoch(ParticleAuxiliaryState, particleID, SSPPRouteEpoch);
				if (!claimed) {
					InterlockedAdd(GroupDuplicateRoutes, 1u);
				} else {
					uint originalPhase = SSPP_LIFECYCLE_FREE;
					InterlockedCompareExchange(
						ParticleAuxiliaryState[particleID].LifecyclePhase,
						auxiliary.LifecyclePhase,
						SSPP_LIFECYCLE_RECYCLING,
						originalPhase);
					if (originalPhase == auxiliary.LifecyclePhase)
						validReturn = true;
					else
						InterlockedAdd(GroupStaleReferences, 1u);
				}
			}
		}
	}

	uint localReturnIndex = 0u;
	if (validReturn)
		InterlockedAdd(GroupReturnCount, 1u, localReturnIndex);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		GroupFreeGranted = SSPPReserveBoundedBlock(
			Counters,
			SSPPFreeTopCounter,
			SSPPCapacity,
			GroupReturnCount,
			GroupFreeBase);
	}
	GroupMemoryBarrierWithGroupSync();

	if (validReturn && localReturnIndex >= GroupFreeGranted) {
		ParticleAuxiliaryState[particleID].LifecyclePhase = SSPP_LIFECYCLE_QUARANTINED;
		ParticleAuxiliaryState[particleID].PersistentListID = SSPP_INVALID_ID;
		InterlockedAdd(GroupQuarantined, 1u);
		validReturn = false;
	}

	if (validReturn) {
		if (auxiliary.EmitterIndex != SSPP_INVALID_ID) {
			InterlockedAdd(GroupOwnedCount, 1u);
			if (auxiliary.EmitterIndex >= SSPPMaxEmitters) {
				InterlockedAdd(GroupOwnerMismatches, 1u);
			} else {
				SSPPPersistentEmitter emitter = EmitterStates[auxiliary.EmitterIndex];
				if (emitter.State != SSPP_EMITTER_STATE_ACTIVE ||
					emitter.Generation != auxiliary.EmitterGeneration)
					InterlockedAdd(GroupOwnerMismatches, 1u);
				else
					AggregateEmitter(auxiliary.EmitterIndex);
			}
		}

		if (auxiliary.OwnerFeature < SSPPRegisteredFeatureCount)
			InterlockedAdd(GroupFeatureCounts[auxiliary.OwnerFeature], 1u);
		else
			InterlockedAdd(GroupInvalidProfiles, 1u);

		// Emission advances the preserved generation when reusing this slot.
		ParticleMetadata[particleID] = uint4(
			SSPP_INVALID_ID,
			SSPP_INVALID_ID,
			SSPP_INVALID_ID,
			reference.SlotGeneration);

		auxiliary.EmitterIndex = SSPP_INVALID_ID;
		auxiliary.EmitterGeneration = 0u;
		auxiliary.OwnerFeature = 0u;
		auxiliary.FeaturePayloadID = 0u;
		auxiliary.LifecyclePhase = SSPP_LIFECYCLE_FREE;
		auxiliary.PersistentListID = SSPP_INVALID_ID;
		auxiliary.LastRouteEpoch = SSPPRouteEpoch;
		auxiliary.Flags = 0u;
		ParticleAuxiliaryState[particleID] = auxiliary;
		FreeIDs[GroupFreeBase + localReturnIndex] = particleID;
	}
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		if (GroupFreeGranted != 0u)
			SSPPDecrementBoundedCounter(Counters, SSPPAllocatedCounter, GroupFreeGranted);
		if (GroupOwnedCount != 0u)
			SSPPDecrementBoundedCounter(Counters, SSPPOwnedParticleCounter, GroupOwnedCount);
	}
	if (groupIndex < SSPP_MAX_FEATURES) {
		uint requested = GroupFeatureCounts[groupIndex];
		if (requested != 0u) {
			uint decremented = SSPPDecrementBoundedCounter(
				Counters, SSPPFeatureLiveCounterBase + groupIndex, requested);
			if (decremented < requested)
				InterlockedAdd(GroupStaleReferences, requested - decremented);
		}
	}
	uint emitterIndex = GroupEmitterKeys[groupIndex];
	if (emitterIndex != SSPP_INVALID_ID)
		DecrementEmitterAliveBounded(emitterIndex, GroupEmitterCounts[groupIndex]);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		if (GroupStaleReferences != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES], GroupStaleReferences);
		if (GroupDuplicateRoutes != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_DUPLICATE_ROUTES], GroupDuplicateRoutes);
		if (GroupInvalidProfiles != 0u)
			InterlockedAdd(Counters[SSPPDiagnosticsBase +
				SSPP_DIAGNOSTIC_INVALID_PROFILES], GroupInvalidProfiles);
		if (GroupOwnerMismatches != 0u)
			InterlockedAdd(Counters[SSPPEmitterDiagnosticsBase +
				SSPP_EMITTER_DIAGNOSTIC_OWNER_MISMATCHES], GroupOwnerMismatches);
		if (GroupQuarantined != 0u) {
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED], GroupQuarantined);
			InterlockedAdd(Counters[SSPPDiagnosticsBase +
				SSPP_DIAGNOSTIC_LIST_OVERFLOWS], GroupQuarantined);
		}
		if (GroupFreeGranted != 0u) {
			InterlockedAdd(Counters[SSPPDiagnosticsBase +
				SSPP_DIAGNOSTIC_RECYCLED_PARTICLES], GroupFreeGranted);
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_FREE_RETURNS], GroupFreeGranted);
		}
	}
}
