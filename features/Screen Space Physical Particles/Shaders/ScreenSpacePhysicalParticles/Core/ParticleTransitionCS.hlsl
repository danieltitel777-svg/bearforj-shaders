// Validates routed particle references and commits them to dynamic, persistent,
// free-return, or quarantine lifecycle destinations. Ordered indirect dispatches
// establish quarantine, free, persistent, then dynamic route precedence.

#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Route descriptors and metadata are read-only. Destination arenas, lifecycle
// state, route overflow references, and counters are owned by this pass.
StructuredBuffer<uint4> ParticleMetadata : register(t1);
StructuredBuffer<SSPPRouteDescriptor> TransitionRouteDescriptors : register(t2);

RWStructuredBuffer<SSPPParticleReference> DynamicDestinationArena : register(u0);
RWStructuredBuffer<SSPPParticleReference> PersistentDestinationArenaA : register(u1);
RWStructuredBuffer<SSPPParticleReference> PersistentDestinationArenaB : register(u2);
RWStructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(u3);
RWStructuredBuffer<SSPPParticleReference> InputRouteArena : register(u4);
RWStructuredBuffer<uint> Counters : register(u7);

groupshared uint GroupDestinationCount;
groupshared uint GroupDestinationBase;
groupshared uint GroupDestinationGranted;
groupshared uint GroupRecycleCount;
groupshared uint GroupRecycleBase;
groupshared uint GroupRecycleGranted;
groupshared uint GroupRecycleRouteBase;
groupshared uint GroupStaleReferences;
groupshared uint GroupDuplicateRoutes;
groupshared uint GroupInvalidDestinations;
groupshared uint GroupQuarantined;
groupshared uint GroupListOverflows;

// Summary: Marks a particle quarantined and records the group-local diagnostic.
// Args:
//   particleID: Particle slot index.
// Returns: None.
void QuarantineParticle(uint particleID)
{
	ParticleAuxiliaryState[particleID].LifecyclePhase = SSPP_LIFECYCLE_QUARANTINED;
	ParticleAuxiliaryState[particleID].PersistentListID = SSPP_INVALID_ID;
	InterlockedAdd(GroupQuarantined, 1u);
}

// Claims free returns before later destination-route dispatches.
[numthreads(64, 1, 1)]
// Summary: Claims valid free-return references and marks their particles pending-free.
// Args:
//   dispatchID: Dispatch-thread index within the free-return route.
//   groupIndex: Thread index used to aggregate lifecycle diagnostics.
// Returns: None.
void ClaimFreeMain(
	uint3 dispatchID : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0u) {
		GroupStaleReferences = 0u;
		GroupDuplicateRoutes = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	SSPPRouteDescriptor descriptor =
		TransitionRouteDescriptors[SSPP_FREE_RETURN_ROUTE];
	uint routeCount = min(Counters[descriptor.CounterOffset], descriptor.Capacity);
	uint routeIndex = dispatchID.x;
	if (routeIndex < routeCount) {
		SSPPParticleReference reference =
			InputRouteArena[descriptor.BaseOffset + routeIndex];
		if (reference.ParticleID >= SSPPCapacity || reference.SlotGeneration == 0u) {
			InterlockedAdd(GroupStaleReferences, 1u);
		} else {
			uint particleID = reference.ParticleID;
			uint4 metadata = ParticleMetadata[particleID];
			SSPPParticleAuxState auxiliary = ParticleAuxiliaryState[particleID];
			if (metadata.w != reference.SlotGeneration ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_FREE ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_RECYCLING ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_QUARANTINED) {
				InterlockedAdd(GroupStaleReferences, 1u);
			} else if (!SSPPClaimRouteEpoch(
				ParticleAuxiliaryState, particleID, SSPPRouteEpoch)) {
				InterlockedAdd(GroupDuplicateRoutes, 1u);
			} else {
				ParticleAuxiliaryState[particleID].LifecyclePhase =
					SSPP_LIFECYCLE_PENDING_FREE;
				ParticleAuxiliaryState[particleID].PersistentListID = SSPP_INVALID_ID;
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex == 0u) {
		if (GroupStaleReferences != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES], GroupStaleReferences);
		if (GroupDuplicateRoutes != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_DUPLICATE_ROUTES], GroupDuplicateRoutes);
	}
}

[numthreads(64, 1, 1)]
// Summary: Moves one route's references into bounded lifecycle destinations and quarantines unrecoverable overflow.
// Args:
//   dispatchID: Dispatch-thread index within the selected route.
//   groupIndex: Thread index used for group reservations and diagnostics.
// Returns: None.
void TransitionMain(
	uint3 dispatchID : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0u) {
		GroupDestinationCount = 0u;
		GroupDestinationBase = 0u;
		GroupDestinationGranted = 0u;
		GroupRecycleCount = 0u;
		GroupRecycleBase = 0u;
		GroupRecycleGranted = 0u;
		GroupRecycleRouteBase = 0u;
		GroupStaleReferences = 0u;
		GroupDuplicateRoutes = 0u;
		GroupInvalidDestinations = 0u;
		GroupQuarantined = 0u;
		GroupListOverflows = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	uint dispatchRoute = SSPPDispatchRoute;
	SSPPRouteDescriptor descriptor = (SSPPRouteDescriptor)0;
	uint routeCount = 0u;
	if (dispatchRoute < SSPP_ROUTE_COUNT) {
		descriptor = TransitionRouteDescriptors[dispatchRoute];
		routeCount = min(Counters[descriptor.CounterOffset], descriptor.Capacity);
	}

	bool destinationCandidate = false;
	bool recycleCandidate = false;
	bool quarantineCandidate = false;
	uint particleID = SSPP_INVALID_ID;
	SSPPParticleReference reference;
	reference.ParticleID = SSPP_INVALID_ID;
	reference.SlotGeneration = 0u;

	uint routeIndex = dispatchID.x;
	if (dispatchRoute < SSPP_ROUTE_COUNT && routeIndex < routeCount) {
		reference = InputRouteArena[descriptor.BaseOffset + routeIndex];
		if (reference.ParticleID >= SSPPCapacity || reference.SlotGeneration == 0u) {
			InterlockedAdd(GroupStaleReferences, 1u);
		} else {
			particleID = reference.ParticleID;
			uint4 metadata = ParticleMetadata[particleID];
			SSPPParticleAuxState auxiliary = ParticleAuxiliaryState[particleID];
			if (metadata.w != reference.SlotGeneration ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_FREE ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_RECYCLING ||
				auxiliary.LifecyclePhase == SSPP_LIFECYCLE_QUARANTINED) {
				InterlockedAdd(GroupStaleReferences, 1u);
			} else if (!SSPPClaimRouteEpoch(
				ParticleAuxiliaryState, particleID, SSPPRouteEpoch)) {
				InterlockedAdd(GroupDuplicateRoutes, 1u);
			} else if (descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_DYNAMIC) {
				if (descriptor.DestinationIndex < SSPP_SIMULATION_CLASS_COUNT)
					destinationCandidate = true;
				else {
					InterlockedAdd(GroupListOverflows, 1u);
					recycleCandidate = true;
				}
			} else if (descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_PERSISTENT) {
				if (descriptor.OwnerFeature != auxiliary.OwnerFeature) {
					InterlockedAdd(GroupInvalidDestinations, 1u);
					recycleCandidate = true;
				} else if (descriptor.DestinationIndex >= SSPPRegisteredPersistentListCount ||
					descriptor.DestinationIndex >= SSPP_MAX_PERSISTENT_LISTS) {
					InterlockedAdd(GroupListOverflows, 1u);
					recycleCandidate = true;
				} else {
					destinationCandidate = true;
				}
			} else if (descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_QUARANTINE) {
				quarantineCandidate = true;
			} else {
				InterlockedAdd(GroupInvalidDestinations, 1u);
				quarantineCandidate = true;
			}
		}
	}

	uint destinationLocalIndex = 0u;
	if (destinationCandidate)
		InterlockedAdd(GroupDestinationCount, 1u, destinationLocalIndex);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u && GroupDestinationCount != 0u) {
		uint counterOffset = 0u;
		uint destinationCapacity = min(descriptor.Capacity, SSPPListSegmentCapacity);
		if (descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_DYNAMIC) {
			counterOffset = SSPPOutputCounterBase + descriptor.DestinationIndex;
		} else {
			uint parity =
				(SSPPPersistentDestinationParityMask >> descriptor.DestinationIndex) & 1u;
			uint counterBase = parity != 0u ?
				SSPPPersistentCounterBaseB : SSPPPersistentCounterBaseA;
			counterOffset = counterBase + descriptor.DestinationIndex;
		}
		GroupDestinationGranted = SSPPReserveBoundedBlock(
			Counters,
			counterOffset,
			destinationCapacity,
			GroupDestinationCount,
			GroupDestinationBase);
	}
	GroupMemoryBarrierWithGroupSync();

	if (destinationCandidate) {
		if (destinationLocalIndex < GroupDestinationGranted) {
			uint destinationIndex = GroupDestinationBase + destinationLocalIndex;
			if (descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_DYNAMIC) {
				DynamicDestinationArena[
					SSPPListAddress(descriptor.DestinationIndex, destinationIndex)] = reference;
				ParticleAuxiliaryState[particleID].LifecyclePhase = SSPP_LIFECYCLE_DYNAMIC;
				ParticleAuxiliaryState[particleID].PersistentListID = SSPP_INVALID_ID;
			} else {
				uint address = SSPPListAddress(descriptor.DestinationIndex, destinationIndex);
				uint parity =
					(SSPPPersistentDestinationParityMask >> descriptor.DestinationIndex) & 1u;
				if (parity != 0u)
					PersistentDestinationArenaB[address] = reference;
				else
					PersistentDestinationArenaA[address] = reference;
				ParticleAuxiliaryState[particleID].LifecyclePhase = SSPP_LIFECYCLE_PERSISTENT;
				ParticleAuxiliaryState[particleID].PersistentListID = descriptor.DestinationIndex;
			}
		} else {
			InterlockedAdd(GroupListOverflows, 1u);
			recycleCandidate = true;
		}
	}

	uint recycleLocalIndex = 0u;
	if (recycleCandidate)
		InterlockedAdd(GroupRecycleCount, 1u, recycleLocalIndex);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u && GroupRecycleCount != 0u) {
		SSPPRouteDescriptor freeDescriptor =
			TransitionRouteDescriptors[SSPP_FREE_RETURN_ROUTE];
		GroupRecycleRouteBase = freeDescriptor.BaseOffset;
		GroupRecycleGranted = SSPPReserveBoundedBlock(
			Counters,
			freeDescriptor.CounterOffset,
			min(freeDescriptor.Capacity, SSPPListSegmentCapacity),
			GroupRecycleCount,
			GroupRecycleBase);
	}
	GroupMemoryBarrierWithGroupSync();

	if (recycleCandidate) {
		if (recycleLocalIndex < GroupRecycleGranted) {
			InputRouteArena[
				GroupRecycleRouteBase + GroupRecycleBase + recycleLocalIndex] = reference;
			ParticleAuxiliaryState[particleID].LifecyclePhase = SSPP_LIFECYCLE_PENDING_FREE;
			ParticleAuxiliaryState[particleID].PersistentListID = SSPP_INVALID_ID;
		} else {
			QuarantineParticle(particleID);
		}
	}
	if (quarantineCandidate)
		QuarantineParticle(particleID);

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex == 0u) {
		if (GroupStaleReferences != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES], GroupStaleReferences);
		if (GroupDuplicateRoutes != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_DUPLICATE_ROUTES], GroupDuplicateRoutes);
		if (GroupInvalidDestinations != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS], GroupInvalidDestinations);
		if (GroupQuarantined != 0u)
			InterlockedAdd(Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED], GroupQuarantined);
		if (GroupListOverflows != 0u)
			InterlockedAdd(Counters[SSPPDiagnosticsBase +
				SSPP_DIAGNOSTIC_LIST_OVERFLOWS], GroupListOverflows);
		if (GroupDestinationGranted != 0u) {
			uint diagnostic = descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_DYNAMIC ?
				SSPP_LIFECYCLE_DIAGNOSTIC_DYNAMIC_TRANSITIONS + descriptor.DestinationIndex :
				SSPP_LIFECYCLE_DIAGNOSTIC_PERSISTENT_TRANSITIONS + descriptor.DestinationIndex;
			InterlockedAdd(
				Counters[SSPPLifecycleDiagnosticsBase + diagnostic],
				GroupDestinationGranted);
		}
	}
}
