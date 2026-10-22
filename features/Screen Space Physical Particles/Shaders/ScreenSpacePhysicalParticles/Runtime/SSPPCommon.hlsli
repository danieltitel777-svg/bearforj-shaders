#ifndef __SSPP_COMMON_HLSLI__
#define __SSPP_COMMON_HLSLI__

// Bearforj SSPP runtime v2: one shared GPU particle pool for every profile.
// Pool layout: [0, RainEnd) rain, [RainEnd, SnowEnd) snow, [SnowEnd, LeafEnd) ambient leaves,
// [LeafEnd, Total) dynamic ring (splashes, sparks, footstep dust/snow, blood) written by events.

#define SSPP_RAIN 0u
#define SSPP_SNOW 1u
#define SSPP_LEAF 2u
#define SSPP_SPLASH 3u
#define SSPP_SPARK 4u
#define SSPP_DUST 5u
#define SSPP_BLOOD 6u
#define SSPP_EMBER 7u  // fire magic: smoldering embers
#define SSPP_ICE 8u    // frost magic: ice shards

#define SSPP_FLAG_SETTLED 0x100u

struct Particle
{
	float3 Position;  // absolute world
	float Age;
	float3 Velocity;  // units / s
	float Life;       // <= 0: dead
	float Size;
	float Spin;
	uint Color;       // RGBA8
	uint Kind;        // profile | flags
};

cbuffer SSPPFrame : register(b0)
{
	row_major float4x4 ViewProj;  // camera-relative world -> clip (jittered, matches the depth buffer)
	float4 CameraPos;             // xyz absolute camera position, w: in interior (1)
	float4 ProjParams;            // x,y: view z = y / (ndc z - x); z,w: dynamic-res viewport size in pixels
	float4 TimeParams;            // x time, y dt, z frame, w dynamic ring size
	float4 Wind;                  // xyz wind velocity (units/s), w gust strength
	float4 Amounts;               // x rain 0..1, y snow 0..1, z leaves 0..1, w spawn radius
	uint4 Ranges;                 // x RainEnd, y SnowEnd, z LeafEnd, w Total
	float4 RainP;                 // x width, y streak length scale, z brightness, w fall speed
	float4 SnowP;                 // x size, y brightness, z fall speed, w settle time
	float4 LeafP;                 // x size, y brightness, z fall speed, w settle time
	float4 SparkP;                // x lifetime, y size, z brightness, w bounce
	float4 DustP;                 // x size, y brightness, z lifetime, w unused
	float4 BloodP;                // x size, y brightness, z lifetime (stuck), w unused
	float4 SplashP;               // x size, y brightness, z splash chance, w lifetime
	float4 LightColor;            // rgb ambient+sun estimate, w collision thickness
	float4 Flags;                 // x collisions, y sky occlusion, z soft depth range, w spawn height
	float4 ExtraP;                // x spark/ember rest time, y ice rest time, z leaf ground min, w leaf ground max
	float4 ExtraP2;               // x size variation 0..1, y leaf glide (flutter scale), z gust lift 0/1, w fade distance (units)
	float4 ContextA;              // x interior, y deciduous tree density, z conifer density, w cold 0..1
	float4 ContextB;              // x game hour, y location flags (Bearforj::Context), z weather slot, w night
	float4 InfluencerPos[8];      // xyz world, w radius (0 = empty)
	float4 InfluencerVel[8];      // xyz motion, w push strength
	uint ReactMask;               // bit (1 << kind): this profile reacts to actors, creatures, gusts, spells
	uint3 ReactPad;
};

float Hash(uint a_seed)
{
	a_seed ^= 2747636419u;
	a_seed *= 2654435769u;
	a_seed ^= a_seed >> 16;
	a_seed *= 2654435769u;
	a_seed ^= a_seed >> 16;
	a_seed *= 2654435769u;
	return float(a_seed) / 4294967295.0;
}

uint PackColor(float4 c)
{
	uint4 u = uint4(saturate(c) * 255.0 + 0.5);
	return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24);
}

float4 UnpackColor(uint c)
{
	return float4(c & 255u, (c >> 8) & 255u, (c >> 16) & 255u, c >> 24) / 255.0;
}

float ViewDepthFromDevice(float a_depth)
{
	return ProjParams.y / (a_depth - ProjParams.x);
}

#endif
