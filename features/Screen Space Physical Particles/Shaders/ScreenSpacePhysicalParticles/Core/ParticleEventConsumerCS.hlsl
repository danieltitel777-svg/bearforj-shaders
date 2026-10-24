#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Validates the shared exit-event ABI and counts events consumed by one
// route. Feature-owned consumers receive the same bounded event arena.
StructuredBuffer<SSPPParticleExitEvent> ExitEvents : register(t0);
RWStructuredBuffer<uint> Counters : register(u7);

[numthreads(64, 1, 1)]
// Summary: Validates and counts one exit event for the selected route.
// Args:
//   dispatchID: Candidate event index in its x component.
// Returns: None.
void CountEventsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	if (SSPPDispatchRoute >= SSPP_MAX_EVENT_ROUTES)
		return;

	uint eventCount = min(Counters[SSPPEventCountCounter], SSPPEventCapacity);
	uint eventIndex = dispatchID.x;
	if (eventIndex >= eventCount)
		return;

	SSPPParticleExitEvent eventRecord = ExitEvents[eventIndex];
	if (eventRecord.EventRoute >= SSPP_MAX_EVENT_ROUTES ||
		!all(isfinite(eventRecord.Position)) ||
		!isfinite(eventRecord.Radius) || eventRecord.Radius < 0.0f ||
		!all(isfinite(eventRecord.Velocity)) ||
		!isfinite(eventRecord.Amount) ||
		!all(isfinite(eventRecord.SurfaceNormal)) ||
		!isfinite(eventRecord.FeatureScalar)) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
		return;
	}

	if (eventRecord.EventRoute == SSPPDispatchRoute) {
		InterlockedAdd(
			Counters[SSPPEventsConsumedCounterBase + SSPPDispatchRoute],
			1u);
	}
}
