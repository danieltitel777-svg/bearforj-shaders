#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_PHYSICAL_LEAVES_SHARED_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_PHYSICAL_LEAVES_SHARED_INCLUDED

#include "Common/Math.hlsli"

static const uint PHYSICAL_LEAF_STATE_UNINITIALIZED = 0u;
static const uint PHYSICAL_LEAF_STATE_AIRBORNE = 1u;
static const uint PHYSICAL_LEAF_STATE_GROUNDED = 2u;
static const uint PHYSICAL_LEAF_FLAG_STANDALONE_FIELD = 1u << 0u;

struct PhysicalLeafSidecar
{
	float3 HomePosition;
	float BaseRotation;
	float3 HomeNormal;
	float AngularVelocity;
	uint Generation;
	uint State;
	float2 GroundTilt;
};

#endif
