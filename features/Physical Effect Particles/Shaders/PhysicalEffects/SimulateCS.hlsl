#include "PhysicalEffects/Common.hlsli"

RWStructuredBuffer<PhysFx::Particle> Particles : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= PhysFx::kMaxParticles)
		return;
	PhysFx::Particle p = Particles[id.x];
	if (p.Lifetime <= 0.0)
		return;

	float dt = PhysFx::DeltaTime;
	p.Age += dt;
	if (p.Age >= p.Lifetime || any(isnan(p.Position))) {
		p.Lifetime = 0.0;
		Particles[id.x] = p;
		return;
	}

	float drag = p.Kind == PhysFx::KindShock ? 6.0 : (p.Kind == PhysFx::KindEmber ? 1.2 : 1.8);
	p.Velocity *= exp(-drag * dt);
	float gravity = PhysFx::Gravity;
	if (p.Kind == PhysFx::KindEmber)
		gravity *= lerp(-0.35, 1.0, saturate(p.Age / max(p.Lifetime, 0.05)));
	else if (p.Kind == PhysFx::KindShock)
		gravity *= 0.15;
	p.Velocity.z -= gravity * dt;
	p.Position += p.Velocity * dt;
	Particles[id.x] = p;
}
