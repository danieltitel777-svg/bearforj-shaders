// Bearforj SSPP runtime v2: CPU events (footsteps, hits, sparks) -> particles in the dynamic ring.
#include "ScreenSpacePhysicalParticles/Runtime/SSPPCommon.hlsli"

struct EmitEvent
{
	float3 Position;
	uint Kind;
	float3 Direction;  // main direction (normalized), length 0 = radial
	uint Count;
	float4 Params;     // x speed, y spread 0..1, z size, w lifetime
	float4 Color;
};

StructuredBuffer<EmitEvent> Events : register(t0);
RWStructuredBuffer<Particle> Particles : register(u0);

RWByteAddressBuffer Counters : register(u1);

cbuffer EmitCB : register(b1)
{
	uint EventCount;
	uint EmitFrame;
	uint2 EmitPad;
};

// Shared limits rule (Bearforj 0.6): a new effect particle never overwrites a visible one.
// Probe 4 ring slots; take a dead one, else the one closest to the end of its fade (<0.25 s left);
// otherwise drop the new particle - old ones only ever leave by dissolving.
uint SSPPPickSlot(uint a_cursor, uint a_ring)
{
	uint best = 0xFFFFFFFFu;
	float bestLeft = 0.25;
	[unroll] for (uint k = 0; k < 4; k++) {
		uint slot = Ranges.z + (a_cursor * 4u + k) % a_ring;
		Particle o = Particles[slot];
		float left = o.Life - o.Age;
		if (o.Life <= 0.0 || left <= 0.0)
			return slot;
		if (left < bestLeft) {
			bestLeft = left;
			best = slot;
		}
	}
	return best;
}

[numthreads(64, 1, 1)] void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
	if (gid.x >= EventCount)
		return;
	EmitEvent e = Events[gid.x];
	uint ringSize = (uint)TimeParams.w;
	if (ringSize == 0)
		return;
	for (uint i = gtid.x; i < e.Count; i += 64) {
		uint seed = gid.x * 7349u + i * 389u + EmitFrame * 15485863u;
		float3 r = float3(Hash(seed), Hash(seed + 1u), Hash(seed + 2u)) * 2.0 - 1.0;
		float3 dir = dot(e.Direction, e.Direction) > 0.01 ? normalize(e.Direction + r * e.Params.y) : normalize(r + float3(0, 0, 0.6));
		float speed = e.Params.x * lerp(0.4, 1.0, Hash(seed + 3u));
		uint cursor;
		Counters.InterlockedAdd(0, 1, cursor);
		Particle p;
		p.Position = e.Position + r * float3(4, 4, 1);
		p.Age = 0;
		p.Velocity = dir * speed;
		p.Life = e.Params.w * lerp(0.6, 1.2, Hash(seed + 4u));
		p.Size = e.Params.z * lerp(1.0 - ExtraP2.x, 1.0 + ExtraP2.x, Hash(seed + 5u));
		p.Spin = Hash(seed + 6u) * 6.2831853;
		p.Color = PackColor(e.Color);
		p.Kind = e.Kind;
		uint slot = SSPPPickSlot(cursor, ringSize);
		if (slot != 0xFFFFFFFFu)
			Particles[slot] = p;
	}
}
