#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Validates frame requests and grants slots under global and per-feature budgets.
// A single cooperative group processes priority then rotated feature order,
// reserving the free stack and emitting bounded work chunks entirely on the GPU.
StructuredBuffer<SSPPBurstRequest> FrameRequests : register(t0);
StructuredBuffer<SSPPParticleTemplate> ParticleTemplates : register(t1);
StructuredBuffer<SSPPSimulationProfile> SimulationProfiles : register(t2);
StructuredBuffer<SSPPParticleFeature> Features : register(t3);

RWStructuredBuffer<SSPPPersistentEmitter> EmitterStates : register(u1);
RWStructuredBuffer<SSPPEmissionChunk> EmissionChunks : register(u2);
RWStructuredBuffer<uint> Counters : register(u7);

// Summary: Resolves and validates a request's template and lifecycle destination.
// Args:
//   request: Frame request to validate.
//   simulationClass: Receives the destination simulation class.
//   persistentListID: Receives the persistent-list ID when applicable.
//   customInitialization: Receives the feature initializer ID.
//   persistent: Receives whether the destination is persistent.
// Returns: True when every referenced resource and spawn policy is valid.
bool TryGetDestination(
	SSPPBurstRequest request,
	out uint simulationClass,
	out uint persistentListID,
	out uint customInitialization,
	out bool persistent)
{
	simulationClass = SSPP_INVALID_ID;
	persistentListID = SSPP_INVALID_ID;
	customInitialization = SSPP_INVALID_ID;
	persistent = false;
	if (request.ParticleTemplate >= SSPPParticleTemplateCount)
		return false;

	SSPPParticleTemplate particleTemplate = ParticleTemplates[request.ParticleTemplate];
	if (particleTemplate.SimulationProfile >= SSPPSimulationProfileCount)
		return false;
	if (particleTemplate.LifecyclePhase != SSPP_LIFECYCLE_DYNAMIC &&
		particleTemplate.LifecyclePhase != SSPP_LIFECYCLE_PERSISTENT)
		return false;
	persistent = particleTemplate.LifecyclePhase == SSPP_LIFECYCLE_PERSISTENT;
	if (persistent &&
		particleTemplate.PersistentListID >= SSPPRegisteredPersistentListCount)
		return false;
	if (particleTemplate.SpawnPlacement != request.SpawnPlacement)
		return false;
	if (particleTemplate.CustomInitialization == SSPP_INVALID_ID) {
		if (request.SpawnPlacement != SSPP_PLACEMENT_DIRECT)
			return false;
	} else if (particleTemplate.CustomInitialization >= SSPP_MAX_NEWBORN_INITIALIZERS ||
		(SSPPReadyInitializerMask &
			(1u << particleTemplate.CustomInitialization)) == 0u) {
		return false;
	}

	SSPPSimulationProfile profile = SimulationProfiles[particleTemplate.SimulationProfile];
	simulationClass = profile.SimulationClass;
	persistentListID = particleTemplate.PersistentListID;
	customInitialization = particleTemplate.CustomInitialization;
	return simulationClass < SSPP_SIMULATION_CLASS_COUNT;
}

// Summary: Atomically pops a contiguous range from the global free stack.
// Args:
//   requested: Maximum number of slots to reserve.
//   freeBase: Receives the first reserved free-stack index.
// Returns: Number of reserved slots.
uint ReserveGlobalFree(uint requested, out uint freeBase)
{
	uint observed = Counters[SSPPFreeTopCounter];
	freeBase = observed;
	[allow_uav_condition]
	while (requested != 0u && observed != 0u) {
		uint granted = min(requested, observed);
		freeBase = observed - granted;
		uint original = 0u;
		InterlockedCompareExchange(
			Counters[SSPPFreeTopCounter], observed, freeBase, original);
		if (original == observed)
			return granted;
		observed = original;
	}
	freeBase = 0u;
	return 0u;
}

// Summary: Adds rejected particles to one feature's cumulative drop counter.
// Args:
//   featureID: Registered feature counter lane.
//   amount: Number of rejected particles.
// Returns: None.
void CountFeatureDrop(uint featureID, uint amount)
{
	InterlockedAdd(Counters[SSPPFeatureDroppedCounterBase + featureID], amount);
}

// Summary: Validates, budgets, grants, and chunks one frame emission request.
// Args:
//   requestIndex: Request's index in the frame arena.
//   request: Request payload.
//   validFeature: Whether owner feature is registered.
// Returns: None.
void PlanRequest(
	uint requestIndex,
	SSPPBurstRequest request,
	bool validFeature)
{
	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_BURST_REQUESTS],
		1u);
	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_REQUESTED_PARTICLES],
		request.Count);
	if (!validFeature) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_DROPPED_PARTICLES],
			request.Count);
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			request.Count);
		if (request.OwnerEmitter != SSPP_INVALID_ID) {
			InterlockedAdd(
				Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
				request.Count);
		}
		return;
	}
	InterlockedAdd(
		Counters[SSPPFeatureRequestedCounterBase + request.OwnerFeature],
		request.Count);
	if (SSPPRetiringOwnerFeature != SSPP_INVALID_ID &&
		request.OwnerFeature == SSPPRetiringOwnerFeature) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_DROPPED_PARTICLES],
			request.Count);
		if (request.OwnerEmitter != SSPP_INVALID_ID) {
			InterlockedAdd(
				Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
				request.Count);
		}
		CountFeatureDrop(request.OwnerFeature, request.Count);
		return;
	}

	if (request.Count == 0u) {
		return;
	}

	uint simulationClass;
	uint persistentListID;
	uint customInitialization;
	bool persistent;
	if (!TryGetDestination(
		request, simulationClass, persistentListID, customInitialization, persistent)) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_DROPPED_PARTICLES],
			request.Count);
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			request.Count);
		if (request.OwnerEmitter != SSPP_INVALID_ID) {
			InterlockedAdd(
				Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
				request.Count);
		}
		CountFeatureDrop(request.OwnerFeature, request.Count);
		return;
	}

	bool validOwner = false;
	if (request.OwnerEmitter != SSPP_INVALID_ID) {
		if (request.OwnerEmitter < SSPPMaxEmitters) {
			SSPPPersistentEmitter owner = EmitterStates[request.OwnerEmitter];
			validOwner = owner.State == SSPP_EMITTER_STATE_ACTIVE &&
				owner.Generation == request.OwnerEmitterGeneration;
		}
		if (!validOwner) {
			InterlockedAdd(
				Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_OWNER_MISMATCHES],
				1u);
			InterlockedAdd(
				Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
				request.Count);
			InterlockedAdd(
				Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_DROPPED_PARTICLES],
				request.Count);
			CountFeatureDrop(request.OwnerFeature, request.Count);
			return;
		}
	}

	SSPPParticleFeature feature = Features[request.OwnerFeature];
	uint maximumLive = feature.MaximumLiveParticles == 0u ?
		SSPPCapacity : min(feature.MaximumLiveParticles, SSPPCapacity);
	uint featureLiveCounter = SSPPFeatureLiveCounterBase + request.OwnerFeature;
	uint ignoredFeatureReservationBase = 0u;
	uint featureReserved = SSPPReserveBoundedBlock(
		Counters,
		featureLiveCounter,
		maximumLive,
		request.Count,
		ignoredFeatureReservationBase);
	uint budgetClamped = request.Count - featureReserved;
	if (budgetClamped != 0u) {
		InterlockedAdd(
			Counters[SSPPFeatureBudgetClampedCounterBase + request.OwnerFeature],
			budgetClamped);
	}

	uint freeBase = 0u;
	uint granted = ReserveGlobalFree(featureReserved, freeBase);
	if (granted < featureReserved)
		SSPPDecrementBoundedCounter(
			Counters, featureLiveCounter, featureReserved - granted);

	uint chunkCount =
		(granted + SSPPEmissionChunkSize - 1u) / SSPPEmissionChunkSize;
	uint chunkBase = 0u;
	// Capacity plus one partial chunk per request bounds the chunk arena.
	InterlockedAdd(Counters[SSPPEmissionChunkCounter], chunkCount, chunkBase);

	uint planFlags = 0u;
	if (validOwner)
		planFlags |= SSPP_PLAN_FLAG_VALID_OWNER;
	if (persistent)
		planFlags |= SSPP_PLAN_FLAG_PERSISTENT;

	for (uint chunkOrdinal = 0u; chunkOrdinal < chunkCount; ++chunkOrdinal) {
		uint firstOrdinal = chunkOrdinal * SSPPEmissionChunkSize;
		SSPPEmissionChunk chunk;
		chunk.RequestIndex = requestIndex;
		chunk.FreeBase = freeBase + firstOrdinal;
		chunk.FirstOrdinal = firstOrdinal;
		chunk.Count = min(SSPPEmissionChunkSize, granted - firstOrdinal);
		chunk.SimulationClass = simulationClass;
		chunk.PersistentListID = persistentListID;
		chunk.CustomInitialization = customInitialization;
		chunk.Flags = planFlags;
		EmissionChunks[chunkBase + chunkOrdinal] = chunk;
	}

	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_GRANTED_PARTICLES],
		granted);
	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_DROPPED_PARTICLES],
		request.Count - granted);
	InterlockedAdd(
		Counters[SSPPFeatureGrantedCounterBase + request.OwnerFeature],
		granted);
	CountFeatureDrop(request.OwnerFeature, request.Count - granted);

	if (validOwner) {
		InterlockedAdd(EmitterStates[request.OwnerEmitter].AliveCount, granted);
		InterlockedAdd(Counters[SSPPOwnedParticleCounter], granted);
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_GRANTED],
			granted);
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
			request.Count - granted);
	}

	if (granted != 0u) {
		uint previousAllocated;
		InterlockedAdd(Counters[SSPPAllocatedCounter], granted, previousAllocated);
		InterlockedMax(Counters[SSPPHighWaterCounter], previousAllocated + granted);
	}
}

[numthreads(64, 1, 1)]
// Summary: Cooperatively plans all requests in priority and rotated feature order.
// Args:
//   groupIndex: Lane distributing request indices within the single group.
// Returns: None.
void PlanMain(uint groupIndex : SV_GroupIndex)
{
	uint requestCount = min(Counters[SSPPFrameRequestCounter], SSPPMaxFrameRequests);
	uint featureCount = min(SSPPRegisteredFeatureCount, SSPP_MAX_FEATURES);

	[loop]
	for (uint priority = 4u; priority-- > 0u;) {
		if (featureCount != 0u) {
			uint firstFeature = (SSPPPlanningFeature + priority) % featureCount;
			[loop]
			for (uint ordinal = 0u; ordinal < featureCount; ++ordinal) {
				uint featureID = (firstFeature + ordinal) % featureCount;
				[loop]
				for (uint requestIndex = groupIndex;
					 requestIndex < requestCount;
					 requestIndex += 64u) {
					SSPPBurstRequest request = FrameRequests[requestIndex];
					if (min(request.Priority, 3u) == priority &&
						request.OwnerFeature == featureID) {
						PlanRequest(requestIndex, request, true);
					}
				}
				DeviceMemoryBarrierWithGroupSync();
			}
		}

		// Count invalid owners after valid features at the same priority.
		[loop]
		for (uint requestIndex = groupIndex;
			 requestIndex < requestCount;
			 requestIndex += 64u) {
			SSPPBurstRequest request = FrameRequests[requestIndex];
			if (min(request.Priority, 3u) == priority &&
				request.OwnerFeature >= featureCount) {
				PlanRequest(requestIndex, request, false);
			}
		}
		DeviceMemoryBarrierWithGroupSync();
	}
}
