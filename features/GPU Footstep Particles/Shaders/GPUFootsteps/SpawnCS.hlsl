// Bearforj: footstep particle spawning. Particle look and counts follow Bottled Shaders'
// FootstepSpawnCS; ground colour comes from a per-surface palette (no G-buffer reads), and
// water/mud are driven by the CPU-side water depth and Wetness Effects state.
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "GPUFootsteps/Common.hlsli"

StructuredBuffer<GPUFootsteps::FootstepEvent> Events : register(t0);
RWStructuredBuffer<GPUFootsteps::Particle> Particles : register(u0);

groupshared uint gsCounts[GPUFootsteps::TypeCount];
groupshared float3 gsGroundColor;
groupshared float3 gsSnowColor;
groupshared float3 gsWaterColor;
groupshared float gsSnowWeight;
groupshared float gsWetness;

float3 GetSurfaceColor(uint surface)
{
	switch (surface) {
	case GPUFootsteps::SurfaceDirt:
		return float3(0.33, 0.26, 0.19);
	case GPUFootsteps::SurfaceGravel:
		return float3(0.42, 0.40, 0.37);
	case GPUFootsteps::SurfaceSand:
		return float3(0.62, 0.55, 0.42);
	case GPUFootsteps::SurfaceGrass:
		return float3(0.30, 0.29, 0.19);
	case GPUFootsteps::SurfaceMud:
		return float3(0.21, 0.16, 0.11);
	case GPUFootsteps::SurfaceSnow:
		return float3(0.88, 0.90, 0.94);
	case GPUFootsteps::SurfaceIce:
		return float3(0.78, 0.85, 0.92);
	case GPUFootsteps::SurfaceAsh:
		return float3(0.40, 0.39, 0.38);
	default:
		return float3(0.42, 0.41, 0.40);
	}
}

void GetSurfaceCounts(uint surface, out float counts[GPUFootsteps::TypeCount])
{
	[unroll] for (uint i = 0; i < GPUFootsteps::TypeCount; i++)
		counts[i] = 0.0;

	switch (surface) {
	case GPUFootsteps::SurfaceDirt:
		counts[GPUFootsteps::TypeDust] = 6.0;
		counts[GPUFootsteps::TypeClod] = 5.0;
		break;
	case GPUFootsteps::SurfaceGravel:
		counts[GPUFootsteps::TypeDust] = 3.0;
		counts[GPUFootsteps::TypeClod] = 6.0;
		break;
	case GPUFootsteps::SurfaceSand:
		counts[GPUFootsteps::TypeDust] = 7.0;
		counts[GPUFootsteps::TypeClod] = 7.0;
		break;
	case GPUFootsteps::SurfaceGrass:
		counts[GPUFootsteps::TypeDust] = 2.0;
		counts[GPUFootsteps::TypeClod] = 2.0;
		break;
	case GPUFootsteps::SurfaceMud:
		// Bearforj: heavier, wetter mud with a few droplets
		counts[GPUFootsteps::TypeClod] = 12.0;
		counts[GPUFootsteps::TypeDroplet] = 3.0;
		break;
	case GPUFootsteps::SurfaceSnow:
		counts[GPUFootsteps::TypeSnow] = 8.0;
		counts[GPUFootsteps::TypeClod] = 7.0;
		break;
	case GPUFootsteps::SurfaceIce:
		counts[GPUFootsteps::TypeSnow] = 2.0;
		counts[GPUFootsteps::TypeClod] = 3.0;
		break;
	case GPUFootsteps::SurfaceAsh:
		counts[GPUFootsteps::TypeDust] = 10.0;
		break;
	case GPUFootsteps::SurfaceWater:
		counts[GPUFootsteps::TypeDroplet] = 16.0;
		counts[GPUFootsteps::TypeMist] = 3.0;
		break;
	default:
		break;
	}
}

bool IsLooseSurface(uint surface)
{
	return surface == GPUFootsteps::SurfaceDirt || surface == GPUFootsteps::SurfaceSand ||
	       surface == GPUFootsteps::SurfaceGrass || surface == GPUFootsteps::SurfaceMud ||
	       surface == GPUFootsteps::SurfaceGravel;
}

void PlanFootstep(GPUFootsteps::FootstepEvent footstep)
{
	uint surface = footstep.Surface;
	float wetness = (GPUFootsteps::FrameFlags & GPUFootsteps::FrameFlagWetness) ? saturate(GPUFootsteps::Wetness) : 0.0;
	float snowWeight = (surface == GPUFootsteps::SurfaceSnow || surface == GPUFootsteps::SurfaceIce) ? 1.0 : 0.0;

	float counts[GPUFootsteps::TypeCount];
	GetSurfaceCounts(surface, counts);

	float dryness = 1.0 - smoothstep(0.1, 0.5, wetness);
	counts[GPUFootsteps::TypeDust] *= dryness;
	counts[GPUFootsteps::TypeSnow] *= 1.0 - 0.5 * smoothstep(0.3, 0.9, wetness);
	if (IsLooseSurface(surface))
		counts[GPUFootsteps::TypeClod] += 4.0 * wetness;
	counts[GPUFootsteps::TypeDroplet] += 6.0 * smoothstep(0.5, 1.0, wetness) * (1.0 - snowWeight);

	// Bearforj: shallow water scales splashes with depth (puddle -> knee-deep)
	if (surface == GPUFootsteps::SurfaceWater) {
		float depth = saturate(footstep.WaterDepth / 40.0);
		counts[GPUFootsteps::TypeDroplet] *= lerp(0.6, 1.6, depth);
		counts[GPUFootsteps::TypeMist] *= lerp(0.5, 2.0, depth);
	}

	float strength = footstep.Strength * ((footstep.Flags & GPUFootsteps::EventFlagLanding) ? 1.6 : 1.0);
	counts[GPUFootsteps::TypeDust] *= GPUFootsteps::DustIntensity * strength;
	counts[GPUFootsteps::TypeClod] *= lerp(surface == GPUFootsteps::SurfaceMud ? GPUFootsteps::MudIntensity : GPUFootsteps::DustIntensity, GPUFootsteps::SnowIntensity, snowWeight) * strength;
	counts[GPUFootsteps::TypeSnow] *= GPUFootsteps::SnowIntensity * strength;
	counts[GPUFootsteps::TypeDroplet] *= GPUFootsteps::SplashIntensity * strength;
	counts[GPUFootsteps::TypeMist] *= GPUFootsteps::SplashIntensity * strength;

	uint remaining = min(footstep.SlotCount, GPUFootsteps::SlotsPerEvent);
	[unroll] for (uint type = 0; type < GPUFootsteps::TypeCount; type++)
	{
		uint count = min((uint)round(max(counts[type], 0.0)), remaining);
		gsCounts[type] = count;
		remaining -= count;
	}

	float3 groundColor = Color::Diffuse(GetSurfaceColor(surface));
	float3 snowColor = Color::Diffuse(GetSurfaceColor(surface == GPUFootsteps::SurfaceIce ? GPUFootsteps::SurfaceIce : GPUFootsteps::SurfaceSnow));

	gsGroundColor = groundColor;
	gsSnowColor = snowColor;
	gsWaterColor = surface == GPUFootsteps::SurfaceMud ? lerp(groundColor, Color::Diffuse(float3(0.5, 0.45, 0.4)), 0.4) : Color::Diffuse(float3(0.80, 0.83, 0.86));
	gsSnowWeight = snowWeight;
	gsWetness = wetness;
}

uint ResolveType(uint localIndex, out bool alive)
{
	uint start = 0;
	[unroll] for (uint type = 0; type < GPUFootsteps::TypeCount; type++)
	{
		uint end = start + gsCounts[type];
		if (localIndex < end) {
			alive = true;
			return type;
		}
		start = end;
	}
	alive = false;
	return 0;
}

[numthreads(64, 1, 1)] void main(uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID) {
	uint eventIndex = groupID.x;
	uint localIndex = threadID.x;
	if (eventIndex >= GPUFootsteps::EventCount)
		return;

	GPUFootsteps::FootstepEvent footstep = Events[eventIndex];

	if (localIndex == 0)
		PlanFootstep(footstep);

	GroupMemoryBarrierWithGroupSync();

	if (localIndex >= footstep.SlotCount)
		return;

	uint slot = (footstep.FirstSlot + localIndex) % GPUFootsteps::MaxParticles;
	GPUFootsteps::Particle particle = (GPUFootsteps::Particle)0;

	bool alive;
	uint type = ResolveType(localIndex, alive);
	if (!alive) {
		Particles[slot] = particle;
		return;
	}

	uint seed = footstep.Seed * 0x9E3779B9u + localIndex * 0x632BE5ABu;
	float r0 = GPUFootsteps::Hash(seed, 0);
	float r1 = GPUFootsteps::Hash(seed, 1);
	float r2 = GPUFootsteps::Hash(seed, 2);
	float r3 = GPUFootsteps::Hash(seed, 3);
	float r4 = GPUFootsteps::Hash(seed, 4);
	float r5 = GPUFootsteps::Hash(seed, 5);
	float r6 = GPUFootsteps::Hash(seed, 6);

	float scale = clamp(footstep.Scale, 0.25, 4.0);
	float strength = clamp(footstep.Strength, 0.2, 3.0);
	float angle = r0 * Math::TAU;
	float2 radial = float2(cos(angle), sin(angle));
	float speed = length(footstep.Velocity.xy);
	float2 moveDirection = speed > 5.0 ? footstep.Velocity.xy / speed : footstep.Forward;
	float speedFactor = saturate(speed / 350.0);
	float2 kick = -moveDirection * speedFactor;

	float3 position = footstep.Position + float3(radial * r1 * 5.0 * scale, 1.5 * scale);
	float3 velocity = float3(footstep.Velocity.xy * 0.15, 0.0);
	float3 color = gsGroundColor;
	float size = 1.0;
	float growth = 1.0;
	float lifetime = 1.0;
	float opacity = 1.0;
	float groundZ = footstep.Position.z;
	uint flags = 0;

	if (type == GPUFootsteps::TypeDust) {
		velocity += float3(radial * lerp(15.0, 45.0, r2) * strength + kick * 25.0, lerp(8.0, 30.0, r3) * strength);
		size = lerp(4.0, 7.0, r4) * scale;
		growth = lerp(2.2, 3.4, r5);
		lifetime = lerp(1.1, 2.0, r6);
		opacity = lerp(0.18, 0.32, r3);
		float luminance = Color::RGBToLuminance(gsGroundColor);
		color = lerp(gsGroundColor, luminance.xxx, 0.25) * lerp(1.0, 1.2, r2);
		position.z += 2.0 * scale;
	} else if (type == GPUFootsteps::TypeSnow) {
		velocity += float3(radial * lerp(20.0, 55.0, r2) * strength + kick * 30.0, lerp(20.0, 60.0, r3) * strength);
		size = lerp(3.5, 6.0, r4) * scale;
		growth = lerp(1.8, 2.6, r5);
		lifetime = lerp(0.9, 1.6, r6);
		opacity = lerp(0.35, 0.55, r3);
		color = gsSnowColor;
		flags = 0x100u;
		position.z += 2.0 * scale;
	} else if (type == GPUFootsteps::TypeClod) {
		bool snowy = r5 < gsSnowWeight;
		velocity += float3(radial * lerp(15.0, 55.0, r2) + kick * lerp(30.0, 90.0, r3), lerp(50.0, 140.0, r4) * strength);
		size = (snowy ? lerp(0.7, 1.8, r6) : lerp(0.5, 1.3, r6)) * scale;
		lifetime = lerp(0.9, 1.6, r1);
		opacity = 1.0;
		color = snowy ? gsSnowColor * lerp(0.9, 1.0, r2) : gsGroundColor * lerp(0.5, 0.8, r2) * lerp(1.0, 0.75, gsWetness);
		flags = snowy ? 0x100u : 0u;
	} else if (type == GPUFootsteps::TypeDroplet) {
		velocity += float3(radial * lerp(40.0, 130.0, r2) + kick * 40.0, lerp(90.0, 220.0, r3) * strength);
		position.xy = footstep.Position.xy + radial * lerp(2.0, 8.0, r1) * scale;
		size = lerp(0.35, 0.7, r4) * scale;
		lifetime = lerp(0.35, 0.7, r6);
		opacity = lerp(0.5, 0.8, r5);
		color = gsWaterColor;
	} else {
		velocity += float3(radial * lerp(15.0, 40.0, r2), lerp(15.0, 35.0, r3));
		size = lerp(3.0, 5.0, r4) * scale;
		growth = lerp(1.8, 2.6, r5);
		lifetime = lerp(0.45, 0.8, r6);
		opacity = lerp(0.12, 0.2, r1);
		color = gsWaterColor * 1.1;
	}

	particle.Position = position;
	particle.Age = 0.0;
	particle.Velocity = velocity;
	particle.Lifetime = lifetime * GPUFootsteps::LifetimeScale;
	particle.Color = max(color, 0.0);
	particle.Size = size * GPUFootsteps::SizeScale;
	particle.Type = type | flags;
	particle.GroundZ = groundZ;
	particle.Growth = growth;
	particle.Opacity = saturate(opacity * GPUFootsteps::Opacity);

	Particles[slot] = particle;
}
