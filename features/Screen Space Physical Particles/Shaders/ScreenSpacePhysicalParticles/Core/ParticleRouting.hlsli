#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_ROUTING_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_ROUTING_INCLUDED

#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Appends generation-tagged references and exit events to bounded shared arenas.
// Callers own particle-state changes; this include owns atomic reservations,
// overflow recovery, and route/event diagnostics.
StructuredBuffer<SSPPRouteDescriptor> SSPPRouteDescriptors : register(t7);
RWStructuredBuffer<SSPPParticleReference> SSPPRouteArena : register(u5);
RWStructuredBuffer<SSPPParticleExitEvent> SSPPEventArena : register(u6);
RWStructuredBuffer<uint> Counters : register(u7);

// Summary: Attempts one direct append to a lifecycle route.
// Args:
//   routeID: Destination route descriptor.
//   reference: Generation-tagged particle reference.
// Returns: True when the bounded destination accepted the reference.
bool SSPPAppendRouteDirect(uint routeID, SSPPParticleReference reference)
{
	bool success = false;
	if (routeID >= SSPP_ROUTE_COUNT) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
	} else {
		success = SSPPAppendBoundedRoute(
			SSPPRouteArena, Counters, SSPPRouteDescriptors[routeID], reference);
	}
	return success;
}

// Summary: Appends to a route with free-return and quarantine overflow recovery.
// Args:
//   routeID: Preferred destination route.
//   reference: Generation-tagged particle reference.
// Returns: True only when the preferred route accepted the reference.
bool SSPPAppendRoute(uint routeID, SSPPParticleReference reference)
{
	if (SSPPAppendRouteDirect(routeID, reference))
		return true;

	// Overflowed valid references fall back to free, then quarantine.
	if (routeID != SSPP_FREE_RETURN_ROUTE && routeID != SSPP_QUARANTINE_ROUTE &&
		SSPPAppendRouteDirect(SSPP_FREE_RETURN_ROUTE, reference))
		return false;
	if (routeID != SSPP_QUARANTINE_ROUTE &&
		SSPPAppendRouteDirect(SSPP_QUARANTINE_ROUTE, reference))
		return false;
	// Never risk double reuse after quarantine overflow.
	InterlockedAdd(
		Counters[SSPPLifecycleDiagnosticsBase +
			SSPP_LIFECYCLE_DIAGNOSTIC_QUARANTINED],
		1u);
	return false;
}

// Summary: Appends a reference to a validated dynamic simulation-class route.
// Args:
//   reference: Generation-tagged particle reference.
//   simulationClass: Destination simulation class.
// Returns: True when the dynamic destination accepted the reference.
bool SSPPRouteDynamic(SSPPParticleReference reference, uint simulationClass)
{
	if (simulationClass >= SSPP_SIMULATION_CLASS_COUNT) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
		SSPPAppendRoute(SSPP_FREE_RETURN_ROUTE, reference);
		return false;
	}
	return SSPPAppendRoute(SSPP_DYNAMIC_ROUTE_BASE + simulationClass, reference);
}

// Summary: Appends a reference to a validated persistent-list route.
// Args:
//   reference: Generation-tagged particle reference.
//   persistentListID: Destination persistent list.
// Returns: True when the persistent destination accepted the reference.
bool SSPPRoutePersistent(SSPPParticleReference reference, uint persistentListID)
{
	if (persistentListID >= SSPPRegisteredPersistentListCount) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_INVALID_DESTINATIONS],
			1u);
		SSPPAppendRoute(SSPP_FREE_RETURN_ROUTE, reference);
		return false;
	}
	return SSPPAppendRoute(SSPP_PERSISTENT_ROUTE_BASE + persistentListID, reference);
}

// Summary: Appends a reference to the free-return route.
// Args:
//   reference: Generation-tagged particle reference.
// Returns: True when the free-return route accepted the reference.
bool SSPPRouteFree(SSPPParticleReference reference)
{
	return SSPPAppendRoute(SSPP_FREE_RETURN_ROUTE, reference);
}

// Summary: Appends one event to the bounded frame event arena.
// Args:
//   eventRecord: Valid event payload and destination route.
// Returns: True when the event was stored and counted.
bool SSPPAppendExitEvent(SSPPParticleExitEvent eventRecord)
{
	bool stored = false;
	if (eventRecord.EventRoute >= SSPP_MAX_EVENT_ROUTES) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_EVENT_OVERFLOWS],
			1u);
	} else {
		uint eventIndex = 0u;
		InterlockedAdd(Counters[SSPPEventCountCounter], 1u, eventIndex);
		if (eventIndex >= SSPPEventCapacity) {
			InterlockedAdd(
				Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_EVENT_OVERFLOWS],
				1u);
		} else {
			SSPPEventArena[eventIndex] = eventRecord;
			InterlockedAdd(
				Counters[SSPPEventsProducedCounterBase + eventRecord.EventRoute], 1u);
			stored = true;
		}
	}
	return stored;
}

#endif
