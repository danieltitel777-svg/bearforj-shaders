#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_NEWBORN_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_NEWBORN_INCLUDED

#include "Common/FrameBuffer.hlsli"
#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Exposes the fixed NewbornInitializationContext bindings and safe helpers used
// by feature-owned newborn passes. Each accepted newborn must complete to one
// lifecycle route or return to the free route.
StructuredBuffer<SSPPBurstRequest> SSPPNewbornRequests : register(t0);
StructuredBuffer<SSPPEmissionChunk> SSPPNewbornChunks : register(t1);
StructuredBuffer<uint> SSPPNewbornFreeIDs : register(t2);
StructuredBuffer<SSPPParticleTemplate> SSPPNewbornTemplates : register(t3);
Texture2D<float> SceneDepth : register(t4);
Texture2D<float> SSPPNewbornSolidHeight : register(t5);
Texture2D<float> SSPPNewbornWaterHeight : register(t6);
StructuredBuffer<SSPPRouteDescriptor> SSPPNewbornRouteDescriptors : register(t7);
StructuredBuffer<SSPPSimulationProfile> SSPPNewbornSimulationProfiles : register(t8);

RWStructuredBuffer<float4> SSPPNewbornPositionAge : register(u0);
RWStructuredBuffer<float4> SSPPNewbornVelocityLifetime : register(u1);
RWStructuredBuffer<float4> SSPPNewbornAppearance : register(u2);
RWStructuredBuffer<uint4> SSPPNewbornMetadata : register(u3);
RWStructuredBuffer<float4> SSPPNewbornContactData : register(u4);
RWStructuredBuffer<SSPPParticleReference> SSPPNewbornRouteArena : register(u5);
RWStructuredBuffer<SSPPParticleAuxState> SSPPNewbornAuxiliaryState : register(u6);
RWStructuredBuffer<uint> Counters : register(u7);

cbuffer SSPPNewbornCollisionFrame : register(b1)
{
	uint SceneDepthAvailable;
	uint SolidHeightAvailable;
	uint WaterHeightAvailable;
	uint PlayerCapsuleCount;

	float2 SolidHeightCenter;
	float SolidHeightHalfExtent;
	float SolidHeightTexelSize;

	float2 WaterHeightCenter;
	float WaterHeightHalfExtent;
	float WaterHeightTexelSize;

	float3 PlayerVelocity;
	uint CollisionRuleCount;
};

#include "ScreenSpacePhysicalParticles/ScreenSpaceCollision.hlsli"
#include "ScreenSpacePhysicalParticles/HeightFieldCollision.hlsli"

// Summary: Validates and resolves one custom-initialization work item.
// Args:
//   chunkIndex: Emission chunk index.
//   localOrdinal: Particle ordinal within that chunk.
//   expectedInitializer: Initializer ID owned by the calling shader.
//   chunk: Receives the validated chunk.
//   request: Receives the source request.
//   particleTemplate: Receives the source particle template.
//   reference: Receives the allocated generation-tagged particle reference.
// Returns: True when every index, ID, and generation is valid for this initializer.
bool SSPPLoadNewborn(
	uint chunkIndex,
	uint localOrdinal,
	uint expectedInitializer,
	out SSPPEmissionChunk chunk,
	out SSPPBurstRequest request,
	out SSPPParticleTemplate particleTemplate,
	out SSPPParticleReference reference)
{
	chunk = (SSPPEmissionChunk)0;
	request = (SSPPBurstRequest)0;
	particleTemplate = (SSPPParticleTemplate)0;
	reference.ParticleID = SSPP_INVALID_ID;
	reference.SlotGeneration = 0u;
	uint chunkCount = min(Counters[SSPPEmissionChunkCounter], SSPPMaxEmissionChunks);
	if (chunkIndex >= chunkCount)
		return false;
	chunk = SSPPNewbornChunks[chunkIndex];
	if (chunk.CustomInitialization != expectedInitializer ||
		localOrdinal >= chunk.Count ||
		chunk.RequestIndex >= min(Counters[SSPPFrameRequestCounter], SSPPMaxFrameRequests))
		return false;
	request = SSPPNewbornRequests[chunk.RequestIndex];
	if (request.ParticleTemplate >= SSPPParticleTemplateCount)
		return false;
	particleTemplate = SSPPNewbornTemplates[request.ParticleTemplate];
	uint freeIndex = chunk.FreeBase + localOrdinal;
	if (freeIndex >= SSPPCapacity)
		return false;
	uint particleID = SSPPNewbornFreeIDs[freeIndex];
	if (particleID >= SSPPCapacity)
		return false;
	reference.ParticleID = particleID;
	reference.SlotGeneration = SSPPNewbornMetadata[particleID].w;
	return reference.SlotGeneration != 0u;
}

// Summary: Appends one initialized newborn to a bounded lifecycle route.
// Args:
//   routeID: Dynamic, persistent, or free-return route.
//   reference: Allocated generation-tagged particle reference.
// Returns: True when the route accepted the reference.
bool SSPPAppendNewbornRoute(uint routeID, SSPPParticleReference reference)
{
	if (routeID >= SSPP_ROUTE_COUNT)
		return false;
	return SSPPAppendBoundedRoute(
		SSPPNewbornRouteArena,
		Counters,
		SSPPNewbornRouteDescriptors[routeID],
		reference);
}

// Summary: Returns an uninitialized or rejected newborn to the free route.
// Args:
//   reference: Allocated generation-tagged particle reference.
// Returns: None.
void SSPPRejectNewborn(SSPPParticleReference reference)
{
	SSPPAppendNewbornRoute(SSPP_FREE_RETURN_ROUTE, reference);
}

// Summary: Routes a successfully initialized newborn to its planned lifecycle.
// Args:
//   chunk: Emission plan containing dynamic or persistent destination.
//   reference: Initialized generation-tagged particle reference.
// Returns: None.
void SSPPCompleteNewborn(
	SSPPEmissionChunk chunk,
	SSPPParticleReference reference)
{
	uint routeID = (chunk.Flags & SSPP_PLAN_FLAG_PERSISTENT) != 0u ?
		SSPP_PERSISTENT_ROUTE_BASE + chunk.PersistentListID :
		SSPP_DYNAMIC_ROUTE_BASE + chunk.SimulationClass;
	if (!SSPPAppendNewbornRoute(routeID, reference))
		SSPPRejectNewborn(reference);
}

#endif
