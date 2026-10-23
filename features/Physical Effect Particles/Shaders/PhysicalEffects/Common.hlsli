// Bearforj: small spark / ember / frost / shock pool. Not the removed SSPP simulation.
#ifndef __PHYSICAL_EFFECTS_COMMON_HLSLI__
#define __PHYSICAL_EFFECTS_COMMON_HLSLI__

namespace PhysFx
{
	static const uint SlotsPerEvent = 48;
	static const uint kMaxParticles = 4096;

	static const uint KindSpark = 0;
	static const uint KindEmber = 1;
	static const uint KindFrost = 2;
	static const uint KindShock = 3;

	struct Particle
	{
		float3 Position;
		float Age;
		float3 Velocity;
		float Lifetime;
		float3 Color;
		float Size;
		uint Kind;
		float Brightness;
		float Pad0;
		float Pad1;
	};

	struct FxEvent
	{
		float3 Position;
		uint FirstSlot;
		float3 Direction;
		uint Kind;
		float Speed;
		float Spread;
		float Size;
		float Lifetime;
		float4 Color;
		uint Count;
		uint Seed;
		float Brightness;
		float Pad;
	};

	cbuffer PhysFxFrame : register(b0)
	{
		float4 CameraPosAdjust : packoffset(c0);
		float DeltaTime : packoffset(c1.x);
		float Gravity : packoffset(c1.y);
		uint EventCount : packoffset(c1.z);
		uint ParticleCapacity : packoffset(c1.w);
		float SizeScale : packoffset(c2.x);
		float LifetimeScale : packoffset(c2.y);
		float BrightnessScale : packoffset(c2.z);
		float PadFrame : packoffset(c2.w);
	};

	uint Hash(uint a, uint b)
	{
		uint x = a * 1664525u + b + 1013904223u;
		x ^= x >> 16;
		x *= 2246822519u;
		return x;
	}

	float U01(uint x)
	{
		return (x & 0x00FFFFFFu) / 16777216.0;
	}
}

#endif
