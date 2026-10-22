#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_EMITTER_COMMON_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_EMITTER_COMMON_INCLUDED

#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Defines the emitter command/state ABI, emitter frame constants, and the common
// bounded-counter primitive used by emitter and emission-planning passes.
static const uint SSPP_EMITTER_COMMAND_CREATE = 0u;
static const uint SSPP_EMITTER_COMMAND_UPDATE = 1u;
static const uint SSPP_EMITTER_COMMAND_DESTROY = 2u;

static const uint SSPP_EMITTER_STATE_FREE = 0u;
static const uint SSPP_EMITTER_STATE_ACTIVE = 1u;

static const uint SSPP_EMITTER_FLAG_ENABLED = 1u << 0u;
static const uint SSPP_EMITTER_FLAG_MAINTAIN_DENSITY = 1u << 1u;

static const uint SSPP_PLAN_FLAG_VALID_OWNER = 1u << 0u;
static const uint SSPP_PLAN_FLAG_PERSISTENT = 1u << 1u;

// Consecutive counters beginning at EmitterFrameConstants::EmitterDiagnosticsBase.
static const uint SSPP_EMITTER_DIAGNOSTIC_CREATES = 0u;
static const uint SSPP_EMITTER_DIAGNOSTIC_UPDATES = 1u;
static const uint SSPP_EMITTER_DIAGNOSTIC_DESTROYS = 2u;
static const uint SSPP_EMITTER_DIAGNOSTIC_REQUESTS = 3u;
static const uint SSPP_EMITTER_DIAGNOSTIC_REQUESTED = 4u;
static const uint SSPP_EMITTER_DIAGNOSTIC_GRANTED = 5u;
static const uint SSPP_EMITTER_DIAGNOSTIC_DROPPED = 6u;
static const uint SSPP_EMITTER_DIAGNOSTIC_INVALID_COMMANDS = 7u;
static const uint SSPP_EMITTER_DIAGNOSTIC_REQUEST_OVERFLOWS = 8u;
static const uint SSPP_EMITTER_DIAGNOSTIC_BUDGET_CLAMPS = 10u;
static const uint SSPP_EMITTER_DIAGNOSTIC_OWNER_MISMATCHES = 11u;

struct SSPPEmissionProfile
{
	SSPPScalarDistribution InitialSpeed;
	SSPPScalarDistribution HorizontalAngleDegrees;
	SSPPScalarDistribution VerticalAngleDegrees;
	SSPPScalarDistribution Lifetime;
	SSPPScalarDistribution Radius;
	SSPPScalarDistribution Opacity;

	float GeneralScalar;
	uint SpawnShape;
	uint SpawnPlacement;
	uint Flags;

	uint TrajectoryPolicy;
	uint3 Pad;
};

struct SSPPPersistentEmitter
{
	float3 Origin;
	float Strength;

	float3 Forward;
	uint Seed;

	float3 Right;
	uint ParticleTemplate;

	float3 Vertical;
	uint EmissionProfile;

	float3 BoundsMin;
	float ParticlesPerSecond;

	float3 BoundsMax;
	float SpawnAccumulator;

	uint TargetAlive;
	uint MaximumAlive;
	uint AliveCount;
	uint Generation;

	uint Priority;
	uint OwnerFeature;
	uint OwnerCell;
	uint RequestOrdinal;

	uint Flags;
	uint State;
	uint2 Pad;
};

struct SSPPEmitterCommand
{
	SSPPPersistentEmitter Emitter;
	uint EmitterIndex;
	uint Operation;
	uint2 Pad;
};

struct SSPPEmitterHandle
{
	uint EmitterIndex;
	uint EmitterGeneration;
};

// Exact mirror of ParticleRuntime::EmitterFrameConstants at b2.
cbuffer SSPPEmitterFrameConstants : register(b2)
{
	uint SSPPMaxEmitters;
	uint SSPPActiveEmitterCount;
	uint SSPPCPURequestCount;
	uint SSPPEmitterCommandCount;

	uint SSPPMaxFrameRequests;
	uint SSPPMaxEmissionChunks;
	uint SSPPFrameRequestCounter;
	uint SSPPEmissionChunkCounter;

	uint SSPPActiveEmitterCounter;
	uint SSPPOwnedParticleCounter;
	uint SSPPEmitterDiagnosticsBase;
	uint SSPPPlanningPriority;

	uint SSPPEmissionChunkSize;
	uint SSPPEmissionProfileCount;
	float SSPPEmitterDeltaTime;
	uint SSPPReadyInitializerMask;

	uint SSPPPlanningFeature;
	uint3 SSPPEmitterFramePad;
};

#endif
