#include "Common/FrameBuffer.hlsli"
#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"
#include "ScreenSpacePhysicalParticles/ParticleCulling.hlsli"

// Validates generation-tagged active-list references, applies per-class culling,
// and appends visible stable IDs to fixed render-class segments.
StructuredBuffer<SSPPParticleReference> LifecycleIndexArena : register(t0);
StructuredBuffer<float4> PositionAge : register(t1);
StructuredBuffer<float4> Appearance : register(t2);
StructuredBuffer<uint4> Metadata : register(t3);
StructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(t4);
StructuredBuffer<SSPPRenderClass> RenderClasses : register(t5);

RWStructuredBuffer<uint> RenderIndexArena : register(u0);
RWStructuredBuffer<uint> Counters : register(u7);

// Summary: Validates and classifies one lifecycle reference into a render segment.
// Args:
//   reference: Generation-tagged stable particle reference.
//   expectedLifecycle: Required dynamic or persistent lifecycle phase.
//   expectedPersistentList: Required list ID for persistent particles.
// Returns: None.
void ClassifyReference(
	SSPPParticleReference reference,
	uint expectedLifecycle,
	uint expectedPersistentList)
{
	uint particleID = reference.ParticleID;
	if (particleID >= SSPPCapacity) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES],
			1u);
		InterlockedAdd(
			Counters[SSPPRenderDiagnosticsBase +
				SSPP_RENDER_DIAGNOSTIC_STALE_REFERENCES],
			1u);
		return;
	}
	uint4 metadata = Metadata[particleID];
	SSPPParticleAuxState auxiliary = ParticleAuxiliaryState[particleID];
	if (reference.SlotGeneration == 0u || metadata.w != reference.SlotGeneration ||
		auxiliary.LifecyclePhase != expectedLifecycle ||
		(expectedLifecycle == SSPP_LIFECYCLE_PERSISTENT &&
			auxiliary.PersistentListID != expectedPersistentList)) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES],
			1u);
		InterlockedAdd(
			Counters[SSPPRenderDiagnosticsBase +
				SSPP_RENDER_DIAGNOSTIC_STALE_REFERENCES],
			1u);
		return;
	}

	uint renderClass = metadata.y;
	if (renderClass >= SSPPRegisteredRenderClassCount ||
		renderClass >= SSPPRenderClassCapacity) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		InterlockedAdd(
			Counters[SSPPRenderDiagnosticsBase +
				SSPP_RENDER_DIAGNOSTIC_INVALID_CLASS],
			1u);
		return;
	}
	SSPPRenderClass descriptor = RenderClasses[renderClass];
	if (descriptor.Enabled == 0u || descriptor.VertexCountPerInstance == 0u) {
		InterlockedAdd(
			Counters[SSPPRenderDiagnosticsBase +
				SSPP_RENDER_DIAGNOSTIC_DISABLED_OR_MISSING],
			1u);
		return;
	}

	float4 positionAge = PositionAge[particleID];
	float4 appearance = Appearance[particleID];
	if (!all(isfinite(positionAge)) || !all(isfinite(appearance))) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		return;
	}
	float radius = max(appearance.x * max(descriptor.CullRadiusScale, 0.0f), 0.0f);
	float3 cameraRelative = positionAge.xyz - FrameBuffer::CameraPosAdjust[0].xyz;
	if ((descriptor.CullingFlags & SSPP_RENDER_CULL_DISTANCE) != 0u &&
		descriptor.MaximumDistance > 0.0f) {
		float distanceLimit = descriptor.MaximumDistance + radius;
		if (dot(cameraRelative, cameraRelative) > distanceLimit * distanceLimit) {
			InterlockedAdd(
				Counters[SSPPRenderDiagnosticsBase +
					SSPP_RENDER_DIAGNOSTIC_DISTANCE_CULLED],
				1u);
			return;
		}
	}
	if ((descriptor.CullingFlags & SSPP_RENDER_CULL_FRUSTUM) != 0u &&
		!IsSphereVisible(positionAge.xyz, radius)) {
		InterlockedAdd(
			Counters[SSPPRenderDiagnosticsBase +
				SSPP_RENDER_DIAGNOSTIC_FRUSTUM_CULLED],
			1u);
		return;
	}

	uint renderIndex;
	InterlockedAdd(
		Counters[SSPPRenderCounterBase + renderClass],
		1u,
		renderIndex);
	if (renderIndex >= SSPPListSegmentCapacity) {
		InterlockedAdd(
			Counters[SSPPRenderCounterBase + renderClass],
			0xFFFFFFFFu);
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_LIST_OVERFLOWS],
			1u);
		InterlockedAdd(
			Counters[SSPPRenderDiagnosticsBase +
				SSPP_RENDER_DIAGNOSTIC_BIN_OVERFLOWS],
			1u);
		return;
	}

	RenderIndexArena[SSPPListAddress(renderClass, renderIndex)] = particleID;
}

[numthreads(64, 1, 1)]
// Summary: Classifies one compact dynamic active-list segment.
// Args:
//   dispatchID: Index within the selected simulation-class segment.
// Returns: None.
void ClassifyMain(uint3 dispatchID : SV_DispatchThreadID)
{
	if (SSPPDispatchClass >= SSPP_SIMULATION_CLASS_COUNT)
		return;
	uint inputCount = min(
		Counters[SSPPInputCounterBase + SSPPDispatchClass],
		SSPPListSegmentCapacity);
	if (dispatchID.x >= inputCount)
		return;
	ClassifyReference(
		LifecycleIndexArena[SSPPListAddress(SSPPDispatchClass, dispatchID.x)],
		SSPP_LIFECYCLE_DYNAMIC,
		SSPP_INVALID_ID);
}

[numthreads(64, 1, 1)]
// Summary: Classifies one compact persistent-list segment.
// Args:
//   dispatchID: Index within the selected persistent-list segment.
// Returns: None.
void ClassifyPersistentMain(uint3 dispatchID : SV_DispatchThreadID)
{
	if (SSPPMaintenanceList >= SSPPRegisteredPersistentListCount)
		return;
	uint inputCount = min(
		Counters[SSPPInputCounterBase + SSPPMaintenanceList],
		SSPPListSegmentCapacity);
	if (dispatchID.x >= inputCount)
		return;
	ClassifyReference(
		LifecycleIndexArena[SSPPListAddress(SSPPMaintenanceList, dispatchID.x)],
		SSPP_LIFECYCLE_PERSISTENT,
		SSPPMaintenanceList);
}
