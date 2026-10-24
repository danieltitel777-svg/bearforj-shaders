// Maintains one persistent particle list, compacts survivors into its opposite
// ping-pong arena, and routes expired or invalid particles through lifecycle lists.
// The CPU resets the destination counter before dispatching MaintainMain.

#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

static const uint SSPP_MAINTENANCE_NONE = 0u;
static const uint SSPP_MAINTENANCE_KEEP_UNTIL_LIFETIME = 1u;
static const uint SSPP_MAINTENANCE_DEBUG_WAKE = 2u;
// Internal drain policy.
static const uint SSPP_MAINTENANCE_DRAIN = 3u;

// Maintenance reads the current persistent arena and writes particle state,
// the opposite persistent arena, route lists, and their shared counters.
StructuredBuffer<SSPPParticleReference> PersistentInputArena : register(t0);
StructuredBuffer<uint4> ParticleMetadata : register(t1);
StructuredBuffer<SSPPSimulationProfile> SimulationProfiles : register(t2);
StructuredBuffer<float4> ContactData : register(t3);
StructuredBuffer<SSPPRouteDescriptor> MaintenanceRouteDescriptors : register(t7);

RWStructuredBuffer<float4> PositionAge : register(u0);
RWStructuredBuffer<float4> VelocityLifetime : register(u1);
RWStructuredBuffer<SSPPParticleReference> PersistentDestinationArena : register(u2);
RWStructuredBuffer<SSPPParticleAuxState> WritableParticleAuxiliaryState : register(u3);
RWStructuredBuffer<SSPPParticleReference> MaintenanceRouteArena : register(u5);
RWStructuredBuffer<uint> Counters : register(u7);

// Each 64-thread group reserves one contiguous survivor block.
groupshared uint MaintenanceKeepCount;
groupshared uint MaintenanceKeepBase;
groupshared uint MaintenanceKeepGranted;
groupshared uint MaintenanceKeepCapacity;
groupshared uint MaintenanceKeepOwnerFeature;
groupshared uint MaintenanceKeepDestinationValid;

// Summary: Appends a particle reference to a maintenance route with free and quarantine overflow handling.
// Args:
//   routeID: Destination route index.
//   particleID: Particle slot index.
//   slotGeneration: Generation associated with the particle reference.
// Returns: None.
void QueueMaintenanceRoute(
	uint routeID,
	uint particleID,
	uint slotGeneration)
{
	SSPPParticleReference outputReference;
	outputReference.ParticleID = particleID;
	outputReference.SlotGeneration = slotGeneration;
	if (routeID >= SSPP_ROUTE_COUNT) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
		routeID = SSPP_FREE_RETURN_ROUTE;
	}

	SSPPRouteDescriptor descriptor = MaintenanceRouteDescriptors[routeID];
	uint routeIndex = descriptor.Capacity;
	if (SSPPReserveBoundedCounter(
		Counters, descriptor.CounterOffset, descriptor.Capacity, routeIndex)) {
		MaintenanceRouteArena[descriptor.BaseOffset + routeIndex] = outputReference;
		return;
	}

	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS],
		1u);
	// Overflow falls back to the free route, then quarantine.
	if (routeID != SSPP_FREE_RETURN_ROUTE && routeID != SSPP_QUARANTINE_ROUTE) {
		SSPPRouteDescriptor freeDescriptor =
			MaintenanceRouteDescriptors[SSPP_FREE_RETURN_ROUTE];
		uint freeIndex = freeDescriptor.Capacity;
		if (SSPPReserveBoundedCounter(
			Counters, freeDescriptor.CounterOffset, freeDescriptor.Capacity, freeIndex)) {
			MaintenanceRouteArena[
				freeDescriptor.BaseOffset + freeIndex] = outputReference;
			return;
		}
	}

	if (routeID == SSPP_QUARANTINE_ROUTE) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED],
			1u);
		return;
	}
	SSPPRouteDescriptor quarantine =
		MaintenanceRouteDescriptors[SSPP_QUARANTINE_ROUTE];
	uint quarantineIndex = quarantine.Capacity;
	if (SSPPReserveBoundedCounter(
		Counters, quarantine.CounterOffset, quarantine.Capacity, quarantineIndex)) {
		MaintenanceRouteArena[
			quarantine.BaseOffset + quarantineIndex] = outputReference;
	} else {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED],
			1u);
	}
}

// Summary: Routes a maintained particle to its simulation-class dynamic list.
// Args:
//   particleID: Particle slot index.
//   slotGeneration: Generation associated with the particle reference.
//   simulationClass: Dynamic simulation-list index.
// Returns: None.
void RouteMaintenanceDynamic(
	uint particleID,
	uint slotGeneration,
	uint simulationClass)
{
	if (simulationClass >= SSPP_SIMULATION_CLASS_COUNT) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
		QueueMaintenanceRoute(
			SSPP_FREE_RETURN_ROUTE, particleID, slotGeneration);
		return;
	}
	QueueMaintenanceRoute(
		SSPP_DYNAMIC_ROUTE_BASE + simulationClass, particleID, slotGeneration);
}

// Summary: Routes a maintained particle to the free-return list.
// Args:
//   particleID: Particle slot index.
//   slotGeneration: Generation associated with the particle reference.
// Returns: None.
void RouteMaintenanceFree(uint particleID, uint slotGeneration)
{
	QueueMaintenanceRoute(
		SSPP_FREE_RETURN_ROUTE, particleID, slotGeneration);
}

// Summary: Records one rejected stale persistent reference.
// Args: None.
// Returns: None.
void CountStaleReference()
{
	InterlockedAdd(
		Counters[SSPPLifecycleDiagnosticsBase +
			SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES],
		1u);
}

// Summary: Validates a persistent-list reference against slot generation and lifecycle ownership.
// Args:
//   reference: Persistent particle reference to validate.
//   particleID: Receives the referenced particle slot.
//   auxiliary: Receives the particle lifecycle state when the slot generation is current.
// Returns: True when the reference belongs to the maintained persistent list.
bool ValidatePersistentReference(
	SSPPParticleReference reference,
	out uint particleID,
	out SSPPParticleAuxState auxiliary)
{
	particleID = reference.ParticleID;
	auxiliary.EmitterIndex = SSPP_INVALID_ID;
	auxiliary.EmitterGeneration = 0u;
	auxiliary.OwnerFeature = 0u;
	auxiliary.FeaturePayloadID = 0u;
	auxiliary.LifecyclePhase = SSPP_LIFECYCLE_FREE;
	auxiliary.PersistentListID = SSPP_INVALID_ID;
	auxiliary.LastRouteEpoch = 0u;
	auxiliary.Flags = 0u;
	if (particleID >= SSPPCapacity || reference.SlotGeneration == 0u)
		return false;
	if (ParticleMetadata[particleID].w != reference.SlotGeneration)
		return false;
	auxiliary = WritableParticleAuxiliaryState[particleID];
	return auxiliary.LifecyclePhase == SSPP_LIFECYCLE_PERSISTENT &&
		auxiliary.PersistentListID == SSPPMaintenanceList;
}

// Summary: Claims a persistent survivor for the current routing epoch.
// Args:
//   particleID: Particle slot index.
// Returns: True when this invocation acquired the epoch claim.
bool ClaimPersistentKeep(uint particleID)
{
	if (SSPPClaimRouteEpoch(
			WritableParticleAuxiliaryState, particleID, SSPPRouteEpoch))
		return true;
	InterlockedAdd(
		Counters[SSPPLifecycleDiagnosticsBase +
			SSPP_LIFECYCLE_DIAGNOSTIC_DUPLICATE_ROUTES],
		1u);
	return false;
}

// Summary: Reinitializes a sleeping debug particle and routes it back to dynamic simulation.
// Args:
//   particleID: Particle slot index.
//   slotGeneration: Generation associated with the particle reference.
//   metadata: Particle profile and generation metadata.
//   contact: Stored contact normal and payload.
// Returns: None.
void RouteDebugWake(
	uint particleID,
	uint slotGeneration,
	uint4 metadata,
	float4 contact)
{
	if (metadata.x >= SSPPSimulationProfileCount) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		RouteMaintenanceFree(particleID, slotGeneration);
		return;
	}
	SSPPSimulationProfile profile = SimulationProfiles[metadata.x];
	if (profile.SimulationClass >= SSPP_SIMULATION_CLASS_COUNT) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		RouteMaintenanceFree(particleID, slotGeneration);
		return;
	}

	float3 normal = SSPPSafeNormalize(contact.xyz, float3(0.0f, 0.0f, 1.0f));
	float angle = SSPPRandom01(
		particleID,
		SSPPRouteEpoch,
		slotGeneration,
		0u) * Math::TAU;
	float sineValue = 0.0f;
	float cosineValue = 1.0f;
	sincos(angle, sineValue, cosineValue);
	float3 lateral = float3(cosineValue, sineValue, 0.0f);
	float3 wakeDirection = SSPPSafeNormalize(normal + lateral * 0.35f,
		float3(0.0f, 0.0f, 1.0f));
	float4 velocityLifetime = VelocityLifetime[particleID];
	velocityLifetime.xyz = wakeDirection * 96.0f;
	VelocityLifetime[particleID] = velocityLifetime;
	PositionAge[particleID].w = 0.0f;
	RouteMaintenanceDynamic(
		particleID, slotGeneration, profile.SimulationClass);
}

[numthreads(1, 1, 1)]
// Summary: Clears the destination counter for the maintained persistent list.
// Args:
//   dispatchID: Dispatch-thread index; only thread zero performs the reset.
// Returns: None.
void ResetPersistentDestinationMain(uint3 dispatchID : SV_DispatchThreadID)
{
	if (dispatchID.x != 0u ||
		SSPPMaintenanceList >= SSPPRegisteredPersistentListCount ||
		SSPPMaintenanceList >= SSPP_MAX_PERSISTENT_LISTS)
		return;

	uint destinationParity =
		(SSPPPersistentDestinationParityMask >> SSPPMaintenanceList) & 1u;
	uint destinationCounterBase = destinationParity != 0u ?
		SSPPPersistentCounterBaseB : SSPPPersistentCounterBaseA;
	Counters[destinationCounterBase + SSPPMaintenanceList] = 0u;
	InterlockedAdd(
		Counters[SSPPLifecycleDiagnosticsBase +
			SSPP_LIFECYCLE_DIAGNOSTIC_MAINTENANCE_RUNS],
		1u);
}

[numthreads(64, 1, 1)]
// Summary: Applies maintenance policy, compacts survivors, and routes particles leaving a persistent list.
// Args:
//   dispatchID: Dispatch-thread index within the maintained list.
//   groupIndex: Thread index used for group compaction and counter aggregation.
// Returns: None.
void MaintainMain(
	uint3 dispatchID : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	// The uniform list guard precedes every group synchronization point.
	if (SSPPMaintenanceList >= SSPPRegisteredPersistentListCount ||
		SSPPMaintenanceList >= SSPP_MAX_PERSISTENT_LISTS)
		return;

	if (groupIndex == 0u) {
		MaintenanceKeepCount = 0u;
		MaintenanceKeepBase = 0u;
		MaintenanceKeepGranted = 0u;
		MaintenanceKeepCapacity = 0u;
		MaintenanceKeepOwnerFeature = SSPP_INVALID_ID;
		MaintenanceKeepDestinationValid = 0u;
		SSPPRouteDescriptor descriptor = MaintenanceRouteDescriptors[
			SSPP_PERSISTENT_ROUTE_BASE + SSPPMaintenanceList];
		MaintenanceKeepCapacity = min(
			descriptor.Capacity, SSPPListSegmentCapacity);
		MaintenanceKeepOwnerFeature = descriptor.OwnerFeature;
		MaintenanceKeepDestinationValid =
			descriptor.DestinationKind == SSPP_ROUTE_DESTINATION_PERSISTENT &&
			descriptor.DestinationIndex == SSPPMaintenanceList ? 1u : 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	// The parity mask names the destination; input uses the opposite arena.
	uint destinationParity =
		(SSPPPersistentDestinationParityMask >> SSPPMaintenanceList) & 1u;
	uint inputParity = destinationParity ^ 1u;
	uint counterBase = inputParity != 0u ?
		SSPPPersistentCounterBaseB : SSPPPersistentCounterBaseA;
	uint particleCount = min(
		Counters[counterBase + SSPPMaintenanceList],
		SSPPListSegmentCapacity);
	uint listIndex = dispatchID.x;
	SSPPParticleReference reference;
	reference.ParticleID = SSPP_INVALID_ID;
	reference.SlotGeneration = 0u;
	uint particleID = SSPP_INVALID_ID;
	SSPPParticleAuxState auxiliary;
	auxiliary.EmitterIndex = SSPP_INVALID_ID;
	auxiliary.EmitterGeneration = 0u;
	auxiliary.OwnerFeature = 0u;
	auxiliary.FeaturePayloadID = 0u;
	auxiliary.LifecyclePhase = SSPP_LIFECYCLE_FREE;
	auxiliary.PersistentListID = SSPP_INVALID_ID;
	auxiliary.LastRouteEpoch = 0u;
	auxiliary.Flags = 0u;
	bool keep = false;
	if (listIndex < particleCount) {
		reference = PersistentInputArena[
			SSPPListAddress(SSPPMaintenanceList, listIndex)];
		if (!ValidatePersistentReference(reference, particleID, auxiliary)) {
			CountStaleReference();
		} else if (SSPPMaintenancePolicy == SSPP_MAINTENANCE_DRAIN) {
			RouteMaintenanceFree(particleID, reference.SlotGeneration);
		} else {
			float4 positionAge = PositionAge[particleID];
			float4 velocityLifetime = VelocityLifetime[particleID];
			float elapsed = max(SSPPMaintenanceElapsed, 0.0f);
			if (!all(isfinite(positionAge)) || !all(isfinite(velocityLifetime)) ||
				!isfinite(elapsed)) {
				RouteMaintenanceFree(particleID, reference.SlotGeneration);
			} else if (SSPPMaintenancePolicy == SSPP_MAINTENANCE_NONE) {
				keep = true;
			} else {
				positionAge.w = max(positionAge.w, 0.0f) + elapsed;
				PositionAge[particleID] = positionAge;
				float lifetime = velocityLifetime.w;
				if (lifetime <= 0.0f || positionAge.w >= lifetime) {
					RouteMaintenanceFree(
						particleID, reference.SlotGeneration);
				} else if (SSPPMaintenancePolicy ==
					SSPP_MAINTENANCE_KEEP_UNTIL_LIFETIME) {
					keep = true;
				} else if (SSPPMaintenancePolicy ==
					SSPP_MAINTENANCE_DEBUG_WAKE) {
					float wakeAge = min(1.0f, lifetime * 0.5f);
					if (positionAge.w >= wakeAge) {
						RouteDebugWake(
							particleID,
							reference.SlotGeneration,
							ParticleMetadata[particleID],
							ContactData[particleID]);
					} else {
						keep = true;
					}
				} else {
					InterlockedAdd(
						Counters[SSPPLifecycleDiagnosticsBase +
							SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
						1u);
					RouteMaintenanceFree(
						particleID, reference.SlotGeneration);
				}
			}
		}
	}

	// Direct compaction preserves owner and route-epoch validation.
	if (keep && (MaintenanceKeepDestinationValid == 0u ||
		auxiliary.OwnerFeature != MaintenanceKeepOwnerFeature)) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
		RouteMaintenanceFree(particleID, reference.SlotGeneration);
		keep = false;
	}
	if (keep)
		keep = ClaimPersistentKeep(particleID);

	uint localKeepIndex = 0u;
	if (keep)
		InterlockedAdd(MaintenanceKeepCount, 1u, localKeepIndex);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		uint destinationCounterBase = destinationParity != 0u ?
			SSPPPersistentCounterBaseB : SSPPPersistentCounterBaseA;
		MaintenanceKeepGranted = SSPPReserveBoundedBlock(
			Counters,
			destinationCounterBase + SSPPMaintenanceList,
			MaintenanceKeepCapacity,
			MaintenanceKeepCount,
			MaintenanceKeepBase);
		if (MaintenanceKeepGranted != 0u) {
			InterlockedAdd(
				Counters[SSPPLifecycleDiagnosticsBase +
					SSPP_LIFECYCLE_DIAGNOSTIC_PERSISTENT_TRANSITIONS +
					SSPPMaintenanceList],
				MaintenanceKeepGranted);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (!keep)
		return;
	if (localKeepIndex < MaintenanceKeepGranted) {
		PersistentDestinationArena[SSPPListAddress(
			SSPPMaintenanceList,
			MaintenanceKeepBase + localKeepIndex)] = reference;
		return;
	}

	// Reservation failure after bounded compaction indicates corrupted counters.
	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS], 1u);
	WritableParticleAuxiliaryState[particleID].LifecyclePhase =
		SSPP_LIFECYCLE_QUARANTINED;
	WritableParticleAuxiliaryState[particleID].PersistentListID = SSPP_INVALID_ID;
	InterlockedAdd(
		Counters[SSPPLifecycleDiagnosticsBase +
			SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED],
		1u);
}
