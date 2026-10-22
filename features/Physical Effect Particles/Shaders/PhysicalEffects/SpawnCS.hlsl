#include "PhysicalEffects/Common.hlsli"

StructuredBuffer<PhysFx::FxEvent> Events : register(t0);
RWStructuredBuffer<PhysFx::Particle> Particles : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThread : SV_GroupThreadID)
{
	if (groupId.x >= PhysFx::EventCount)
		return;
	PhysFx::FxEvent e = Events[groupId.x];
	uint n = min(e.Count, PhysFx::SlotsPerEvent);
	if (groupThread.x >= n)
		return;

	uint slot = (e.FirstSlot + groupThread.x) % PhysFx::kMaxParticles;
	uint h0 = PhysFx::Hash(e.Seed, slot);
	uint h1 = PhysFx::Hash(e.Seed, slot + 19u);
	uint h2 = PhysFx::Hash(e.Seed, slot + 91u);
	float u0 = PhysFx::U01(h0);
	float u1 = PhysFx::U01(h1);
	float u2 = PhysFx::U01(h2);

	float3 axis = e.Direction;
	if (dot(axis, axis) < 1e-4)
		axis = float3(0.0, 0.0, 1.0);
	axis = normalize(axis);
	float3 helper = abs(axis.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 side = normalize(cross(axis, helper));
	float3 up = cross(side, axis);
	float ang = u0 * 6.2831853;
	float cone = sqrt(u1) * e.Spread;
	float s, c;
	sincos(ang, s, c);
	float3 dir = normalize(axis + (side * c + up * s) * cone);

	float speed = e.Speed * lerp(0.55, 1.25, u2);
	float3 velocity = dir * speed;
	if (e.Kind == PhysFx::KindEmber)
		velocity.z += e.Speed * lerp(0.35, 0.9, u0);
	else if (e.Kind == PhysFx::KindFrost)
		velocity.z -= e.Speed * 0.15;
	else if (e.Kind == PhysFx::KindShock)
		velocity *= 1.6;

	PhysFx::Particle p;
	p.Position = e.Position + (side * c + up * s) * (u1 * 8.0);
	p.Age = 0.0;
	p.Velocity = velocity;
	p.Lifetime = e.Lifetime * PhysFx::LifetimeScale * lerp(0.75, 1.15, u2);
	p.Color = e.Color.rgb;
	p.Size = e.Size * PhysFx::SizeScale * lerp(0.7, 1.35, u0);
	p.Kind = e.Kind;
	p.Brightness = e.Brightness * PhysFx::BrightnessScale;
	p.Pad0 = 0.0;
	p.Pad1 = 0.0;
	Particles[slot] = p;
}
