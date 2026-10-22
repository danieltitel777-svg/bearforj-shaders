// Advances active persistent emitters and emits at most one bounded burst
// request per emitter for the frame's shared emission planner.

#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Handles and profiles are read-only. This pass owns emitter accumulator and
// request-ordinal updates, frame-request appends, and emitter diagnostics.
StructuredBuffer<SSPPEmitterHandle> ActiveEmitterHandles : register(t0);
StructuredBuffer<SSPPEmissionProfile> EmissionProfiles : register(t1);

RWStructuredBuffer<SSPPPersistentEmitter> EmitterStates : register(u0);
RWStructuredBuffer<SSPPBurstRequest> FrameRequests : register(u1);
RWStructuredBuffer<uint> Counters : register(u7);

[numthreads(64, 1, 1)]
// Summary: Updates one active emitter and appends its rate or density-maintenance request.
// Args:
//   dispatchID: Dispatch-thread index into the active-emitter handle list.
// Returns: None.
void UpdateEmittersMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint activeIndex = dispatchID.x;
	if (activeIndex >= SSPPActiveEmitterCount)
		return;

	SSPPEmitterHandle handle = ActiveEmitterHandles[activeIndex];
	uint emitterIndex = handle.EmitterIndex;
	if (emitterIndex >= SSPPMaxEmitters)
		return;

	SSPPPersistentEmitter emitter = EmitterStates[emitterIndex];
	if (emitter.State != SSPP_EMITTER_STATE_ACTIVE ||
		emitter.Generation != handle.EmitterGeneration)
		return;

	InterlockedAdd(Counters[SSPPActiveEmitterCounter], 1u);
	if ((emitter.Flags & SSPP_EMITTER_FLAG_ENABLED) == 0u) {
		EmitterStates[emitterIndex] = emitter;
		return;
	}

	if (emitter.EmissionProfile >= SSPPEmissionProfileCount) {
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_INVALID_COMMANDS],
			1u);
		return;
	}
	if (emitter.OwnerFeature >= SSPPRegisteredFeatureCount) {
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_INVALID_COMMANDS],
			1u);
		return;
	}

	SSPPEmissionProfile profile = EmissionProfiles[emitter.EmissionProfile];
	float accumulated = max(emitter.SpawnAccumulator, 0.0f) +
		max(emitter.ParticlesPerSecond, 0.0f) * max(SSPPEmitterDeltaTime, 0.0f);
	uint rateCount = (uint)min(floor(accumulated), (float)SSPPCapacity);
	emitter.SpawnAccumulator = accumulated - (float)rateCount;

	uint densityCount = 0u;
	if ((emitter.Flags & SSPP_EMITTER_FLAG_MAINTAIN_DENSITY) != 0u &&
		emitter.TargetAlive > emitter.AliveCount) {
		densityCount = emitter.TargetAlive - emitter.AliveCount;
	}

	uint requestedBeforeBudget = max(rateCount, densityCount);
	uint maximumAlive = emitter.MaximumAlive == 0u ? SSPPCapacity :
		min(emitter.MaximumAlive, SSPPCapacity);
	uint remainingBudget = maximumAlive > emitter.AliveCount ?
		maximumAlive - emitter.AliveCount : 0u;
	uint requested = min(requestedBeforeBudget, remainingBudget);
	if (requested < requestedBeforeBudget) {
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_BUDGET_CLAMPS],
			requestedBeforeBudget - requested);
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
			requestedBeforeBudget - requested);
	}

	if (requested == 0u) {
		EmitterStates[emitterIndex] = emitter;
		return;
	}

	InterlockedAdd(
		Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_REQUESTS],
		1u);
	InterlockedAdd(
		Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_REQUESTED],
		requested);

	uint requestIndex = SSPPMaxFrameRequests;
	if (!SSPPReserveBoundedCounter(
		Counters, SSPPFrameRequestCounter, SSPPMaxFrameRequests, requestIndex)) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_BURST_REQUESTS],
			1u);
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_REQUESTED_PARTICLES],
			requested);
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_DROPPED_PARTICLES],
			requested);
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_REQUEST_OVERFLOWS],
			1u);
		InterlockedAdd(
			Counters[SSPPEmitterDiagnosticsBase + SSPP_EMITTER_DIAGNOSTIC_DROPPED],
			requested);
		InterlockedAdd(
			Counters[SSPPFeatureRequestedCounterBase + emitter.OwnerFeature],
			requested);
		InterlockedAdd(
			Counters[SSPPFeatureDroppedCounterBase + emitter.OwnerFeature],
			requested);
		EmitterStates[emitterIndex] = emitter;
		return;
	}

	SSPPBurstRequest request;
	request.Origin = emitter.Origin;
	request.Strength = emitter.Strength;
	request.Forward = emitter.Forward;
	request.Seed = SSPPHash(emitter.Seed ^ emitter.RequestOrdinal * 0x9E3779B9u);
	request.Right = emitter.Right;
	request.Count = requested;
	request.Vertical = emitter.Vertical;
	request.ParticleTemplate = emitter.ParticleTemplate;
	request.InitialSpeed = profile.InitialSpeed;
	request.HorizontalAngleDegrees = profile.HorizontalAngleDegrees;
	request.VerticalAngleDegrees = profile.VerticalAngleDegrees;
	request.Lifetime = profile.Lifetime;
	request.Radius = profile.Radius;
	request.Opacity = profile.Opacity;
	request.GeneralScalar = profile.GeneralScalar;
	request.Priority = emitter.Priority;
	request.PayloadID = emitter.OwnerCell;
	request.OwnerEmitter = emitterIndex;
	request.OwnerEmitterGeneration = emitter.Generation;
	request.OwnerFeature = emitter.OwnerFeature;
	request.Flags = profile.Flags;
	request.TrajectoryPolicy = profile.TrajectoryPolicy;
	request.SpawnBoundsMin = emitter.BoundsMin;
	request.SpawnShape = profile.SpawnShape;
	request.SpawnBoundsMax = emitter.BoundsMax;
	request.SpawnPlacement = profile.SpawnPlacement;
	FrameRequests[requestIndex] = request;

	emitter.RequestOrdinal += 1u;
	EmitterStates[emitterIndex] = emitter;
}
