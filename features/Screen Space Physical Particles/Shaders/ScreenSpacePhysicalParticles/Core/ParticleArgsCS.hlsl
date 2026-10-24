#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Builds raw D3D11 indirect dispatch/draw records from authoritative bounded
// counters. Each entry point owns only the counter or argument subset it resets.
RWByteAddressBuffer IndirectArguments : register(u0);
RWByteAddressBuffer PreparationArguments : register(u1);
RWStructuredBuffer<uint> Counters : register(u7);
StructuredBuffer<SSPPRenderClass> RenderClasses : register(t0);

[numthreads(64, 1, 1)]
// Summary: Clears per-frame dynamic, render, route, event, and diagnostic counters.
// Args:
//   dispatchID: Counter lane selected by its x component.
// Returns: None.
void ResetCountersMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint index = dispatchID.x;
	if (index < SSPP_SIMULATION_CLASS_COUNT)
		Counters[SSPPOutputCounterBase + index] = 0u;
	if (index < SSPPRenderClassCapacity)
		Counters[SSPPRenderCounterBase + index] = 0u;
	if (index < SSPP_ROUTE_COUNT)
		Counters[SSPPRouteCounterBase + index] = 0u;
	if (index == 0u)
		Counters[SSPPEventCountCounter] = 0u;
	if (index == 0u)
		Counters[SSPPFrameCulledCounter] = 0u;
	if (index < SSPP_RENDER_DIAGNOSTIC_COUNT)
		Counters[SSPPRenderDiagnosticsBase + index] = 0u;
}

[numthreads(64, 1, 1)]
// Summary: Clears only dynamic destination counters after arena reclassification.
// Args:
//   dispatchID: Simulation-class lane selected by its x component.
// Returns: None.
void ResetDynamicCountersMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint simulationClass = dispatchID.x;
	if (simulationClass < SSPP_SIMULATION_CLASS_COUNT)
		Counters[SSPPOutputCounterBase + simulationClass] = 0u;
}

[numthreads(64, 1, 1)]
// Summary: Builds indirect records for lifecycle routes and event consumption.
// Args:
//   dispatchID: Lifecycle-route lane selected by its x component.
// Returns: None.
void BuildRouteArgsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint routeID = dispatchID.x;
	if (routeID >= SSPP_ROUTE_COUNT)
		return;
	if (routeID == 0u) {
		uint eventCount = min(Counters[SSPPEventCountCounter], SSPPEventCapacity);
		PreparationArguments.Store(0u, (eventCount + 63u) / 64u);
		PreparationArguments.Store(4u, 1u);
		PreparationArguments.Store(8u, 1u);
	}

	bool validRoute = routeID < SSPP_SIMULATION_CLASS_COUNT;
	if (routeID >= SSPP_PERSISTENT_ROUTE_BASE && routeID < SSPP_FREE_RETURN_ROUTE)
		validRoute = routeID - SSPP_PERSISTENT_ROUTE_BASE < SSPPRegisteredPersistentListCount;
	validRoute = validRoute || routeID == SSPP_FREE_RETURN_ROUTE ||
		routeID == SSPP_QUARANTINE_ROUTE;
	uint particleCount = validRoute ? min(
		Counters[SSPPRouteCounterBase + routeID], SSPPListSegmentCapacity) : 0u;

	uint argumentBase = routeID * 3u * 4u;
	IndirectArguments.Store(argumentBase, (particleCount + 63u) / 64u);
	IndirectArguments.Store(argumentBase + 4u, 1u);
	IndirectArguments.Store(argumentBase + 8u, 1u);
}

[numthreads(1, 1, 1)]
// Summary: Refreshes the free-return record after transition overflow recovery.
// Args:
//   dispatchID: Unused single-thread dispatch ID.
// Returns: None.
void BuildFreeRouteArgsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint particleCount = min(
		Counters[SSPPRouteCounterBase + SSPP_FREE_RETURN_ROUTE],
		SSPPListSegmentCapacity);
	uint argumentBase = SSPP_FREE_RETURN_ROUTE * 3u * 4u;
	IndirectArguments.Store(argumentBase, (particleCount + 63u) / 64u);
	IndirectArguments.Store(argumentBase + 4u, 1u);
	IndirectArguments.Store(argumentBase + 8u, 1u);
}

[numthreads(64, 1, 1)]
// Summary: Builds dynamic-simulation and persistent-maintenance dispatch records.
// Args:
//   dispatchID: Simulation-class or persistent-list lane.
// Returns: None.
void BuildSimulationArgsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint index = dispatchID.x;
	if (index < SSPP_SIMULATION_CLASS_COUNT) {
		uint particleCount = min(
			Counters[SSPPInputCounterBase + index],
			SSPPListSegmentCapacity);
		uint argumentBase = index * 3u * 4u;
		IndirectArguments.Store(argumentBase, (particleCount + 63u) / 64u);
		IndirectArguments.Store(argumentBase + 4u, 1u);
		IndirectArguments.Store(argumentBase + 8u, 1u);
	}
	if (index < SSPPRegisteredPersistentListCount) {
		uint parity = (SSPPPersistentDestinationParityMask >> index) & 1u;
		uint counterBase = parity != 0u ?
			SSPPPersistentCounterBaseB : SSPPPersistentCounterBaseA;
		uint particleCount = min(
			Counters[counterBase + index], SSPPListSegmentCapacity);
		uint argumentBase = index * 3u * 4u;
		PreparationArguments.Store(argumentBase, (particleCount + 63u) / 64u);
		PreparationArguments.Store(argumentBase + 4u, 1u);
		PreparationArguments.Store(argumentBase + 8u, 1u);
	}
}

[numthreads(64, 1, 1)]
// Summary: Builds renderer preparation/draw records and aggregate culling counts.
// Args:
//   dispatchID: Render-class lane selected by its x component.
// Returns: None.
void BuildRenderArgsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint renderClass = dispatchID.x;
	if (renderClass == 0u) {
		uint candidateCount = 0u;
		[unroll]
		for (uint simulationClass = 0u;
			simulationClass < SSPP_SIMULATION_CLASS_COUNT;
			++simulationClass) {
			candidateCount += min(
				Counters[SSPPOutputCounterBase + simulationClass],
				SSPPListSegmentCapacity);
		}
		[loop]
		for (uint listID = 0u;
			listID < min(SSPPRegisteredPersistentListCount, SSPP_MAX_PERSISTENT_LISTS);
			++listID) {
			uint parity = (SSPPPersistentDestinationParityMask >> listID) & 1u;
			uint counterBase = parity != 0u ?
				SSPPPersistentCounterBaseB : SSPPPersistentCounterBaseA;
			candidateCount += min(
				Counters[counterBase + listID], SSPPListSegmentCapacity);
		}

		uint visibleCount = 0u;
		[loop]
		for (uint classID = 0u;
			classID < min(SSPPRegisteredRenderClassCount, SSPPRenderClassCapacity);
			++classID) {
			visibleCount += min(
				Counters[SSPPRenderCounterBase + classID],
				SSPPListSegmentCapacity);
		}
		Counters[SSPPRenderDiagnosticsBase + SSPP_RENDER_DIAGNOSTIC_CANDIDATES] =
			candidateCount;
		Counters[SSPPFrameCulledCounter] =
			candidateCount - min(candidateCount, visibleCount);
	}
	if (renderClass >= SSPPRegisteredRenderClassCount)
		return;

	uint visibleCount = min(
		Counters[SSPPRenderCounterBase + renderClass],
		SSPPListSegmentCapacity);
	SSPPRenderClass descriptor = RenderClasses[renderClass];
	uint argumentBase = renderClass * 4u * 4u;
	IndirectArguments.Store(argumentBase, descriptor.VertexCountPerInstance);
	IndirectArguments.Store(argumentBase + 4u, visibleCount);
	IndirectArguments.Store(argumentBase + 8u, descriptor.StartVertexLocation);
	// Instance IDs remain local; renderers apply their visible-segment base once.
	IndirectArguments.Store(argumentBase + 12u, 0u);

	uint preparationBase = renderClass * 3u * 4u;
	uint preparationGroups = descriptor.PreparationThreads == 0u ? 0u :
		(visibleCount + descriptor.PreparationThreads - 1u) /
		descriptor.PreparationThreads;
	PreparationArguments.Store(preparationBase, preparationGroups);
	PreparationArguments.Store(preparationBase + 4u, 1u);
	PreparationArguments.Store(preparationBase + 8u, 1u);
}
