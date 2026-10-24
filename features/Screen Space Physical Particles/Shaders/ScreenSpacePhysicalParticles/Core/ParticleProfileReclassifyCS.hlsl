#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Rebuilds compact dynamic active lists after a simulation profile changes class.
// Stable particle state and ownership remain unchanged; invalid references route free.
StructuredBuffer<SSPPParticleReference> InputReferences : register(t0);
StructuredBuffer<SSPPSimulationProfile> SimulationProfiles : register(t1);
StructuredBuffer<uint4> ParticleMetadata : register(t2);
StructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(t8);

RWStructuredBuffer<SSPPParticleReference> DynamicDestinationArena : register(u0);

#include "ScreenSpacePhysicalParticles/Core/ParticleRouting.hlsli"

[numthreads(64, 1, 1)]
// Summary: Reclassifies one validated dynamic reference from its current profile.
// Args:
//   dispatchID: Index within the selected source-class segment.
// Returns: None.
void ReclassifyMain(uint3 dispatchID : SV_DispatchThreadID)
{
	if (SSPPDispatchClass >= SSPP_SIMULATION_CLASS_COUNT)
		return;
	uint inputCount = min(
		Counters[SSPPInputCounterBase + SSPPDispatchClass],
		SSPPListSegmentCapacity);
	if (dispatchID.x >= inputCount)
		return;

	SSPPParticleReference reference =
		InputReferences[SSPPListAddress(SSPPDispatchClass, dispatchID.x)];
	if (reference.ParticleID >= SSPPCapacity || reference.SlotGeneration == 0u) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES],
			1u);
		return;
	}

	uint particleID = reference.ParticleID;
	uint4 metadata = ParticleMetadata[particleID];
	SSPPParticleAuxState auxiliary = ParticleAuxiliaryState[particleID];
	if (metadata.w != reference.SlotGeneration ||
		auxiliary.LifecyclePhase != SSPP_LIFECYCLE_DYNAMIC) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES],
			1u);
		return;
	}

	if (metadata.x >= SSPPSimulationProfileCount) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		SSPPRouteFree(reference);
		return;
	}
	uint destinationClass = SimulationProfiles[metadata.x].SimulationClass;
	if (destinationClass >= SSPP_SIMULATION_CLASS_COUNT) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		SSPPRouteFree(reference);
		return;
	}

	uint destinationIndex = SSPPListSegmentCapacity;
	if (!SSPPReserveBoundedCounter(
		Counters,
		SSPPOutputCounterBase + destinationClass,
		SSPPListSegmentCapacity,
		destinationIndex)) {
		// A full capacity-sized segment indicates duplicate or corrupt references.
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS],
			1u);
		SSPPRouteFree(reference);
		return;
	}
	DynamicDestinationArena[
		SSPPListAddress(destinationClass, destinationIndex)] = reference;
}
