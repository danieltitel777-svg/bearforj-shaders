// Bearforj SSPP runtime v2: simulation of every profile in one dispatch.
#include "ScreenSpacePhysicalParticles/Runtime/SSPPCommon.hlsli"

#define SKYLIGHTING_PROBE_REGISTER t2
#if defined(SSPP_SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

RWStructuredBuffer<Particle> Particles : register(u0);

RWByteAddressBuffer Counters : register(u1);  // [0] dynamic ring cursor
Texture2D<float> SceneDepth : register(t1);

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

float SkyVisibility(float3 a_world)
{
#if defined(SSPP_SKYLIGHTING)
	if (Flags.y <= 0.0)
		return 1.0;
	float3 positionMS = a_world - CameraPos.xyz;
	sh2 sh = Skylighting::SampleNoBias(positionMS);
	return saturate(SphericalHarmonics::Unproject(sh, float3(0, 0, 1)));
#else
	return CameraPos.w > 0.5 ? 0.0 : 1.0;
#endif
}

// 0 none, 1 hit (surface just in front of the particle)
uint Collide(float3 a_world, out float3 a_surface)
{
	a_surface = a_world;
	if (Flags.x <= 0.0)
		return 0;
	float4 clip = mul(ViewProj, float4(a_world - CameraPos.xyz, 1.0));
	if (clip.w <= 1.0)
		return 0;
	float3 ndc = clip.xyz / clip.w;
	if (any(abs(ndc.xy) > 1.0))
		return 0;
	float2 uv = float2(ndc.x, -ndc.y) * 0.5 + 0.5;
	float depth = SceneDepth.Load(int3(uv * ProjParams.zw, 0));
	float sceneZ = ViewDepthFromDevice(depth);
	float particleZ = clip.w;
	if (!(sceneZ > 0.0) || particleZ < sceneZ || particleZ > sceneZ + LightColor.w)
		return 0;
	a_surface = CameraPos.xyz + (a_world - CameraPos.xyz) * (sceneZ / particleZ);
	return 1;
}

float2 OffscreenNdc(float3 a_world)
{
	float4 clip = mul(ViewProj, float4(a_world - CameraPos.xyz, 1.0));
	return clip.w > 1.0 ? clip.xy / clip.w : float2(9, 9);
}

void SpawnDynamic(uint a_kind, float3 a_pos, float3 a_vel, float a_life, float a_size, float4 a_color, uint a_seed)
{
	uint ringSize = (uint)TimeParams.w;
	if (ringSize == 0)
		return;
	uint cursor;
	Counters.InterlockedAdd(0, 1, cursor);
	Particle p;
	p.Position = a_pos;
	p.Age = 0;
	p.Velocity = a_vel;
	p.Life = a_life;
	p.Size = a_size;
	p.Spin = Hash(a_seed) * 6.2831853;
	p.Color = PackColor(a_color);
	p.Kind = a_kind;
	uint slot = SSPPPickSlot(cursor, ringSize);
	if (slot != 0xFFFFFFFFu)
		Particles[slot] = p;
}

// Actors, creatures, gusts, spells and shouts. Resting debris is lifted; airborne particles are pushed and swirled.
void ReactToBodies(inout Particle p, uint kind, inout bool settled)
{
	if ((ReactMask & (1u << kind)) == 0u)
		return;
	float3 push = 0;
	float lift = 0;
	[unroll] for (uint i = 0; i < 8; i++) {
		float radius = InfluencerPos[i].w;
		float strength = InfluencerVel[i].w;
		if (radius < 1.0 || strength <= 0.0)
			continue;
		float3 delta = p.Position - InfluencerPos[i].xyz;
		float horizontal = length(delta.xy);
		if (horizontal > radius || abs(delta.z) > radius * 1.4)
			continue;
		float falloff = 1.0 - horizontal / radius;
		float3 away = horizontal > 1.0 ? float3(delta.xy / horizontal, 0) : float3(1, 0, 0);
		float3 swirl = float3(-away.y, away.x, 0);
		float3 motion = InfluencerVel[i].xyz;
		push += (away * 0.7 + swirl * 0.45) * strength * falloff + motion * falloff * 0.25;
		lift = max(lift, falloff * strength);
	}
	float gust = Wind.w * (0.5 + 0.5 * sin(TimeParams.x * 0.7 + p.Position.x * 0.002 + p.Position.y * 0.0017));
	bool debris = kind != SSPP_RAIN;
	if (ExtraP2.z > 0.5 && debris && settled && gust > (kind == SSPP_LEAF || kind == SSPP_DUST || kind == SSPP_SNOW ? 0.45 : 0.7))
		lift = max(lift, gust * 220.0);
	if (settled) {
		if (lift > 70.0 && debris) {
			p.Kind &= ~SSPP_FLAG_SETTLED;
			p.Velocity = push + float3(Wind.xy * 0.35, 90.0 + lift * 0.35);
			p.Life = max(p.Life, p.Age + 8.0);
			settled = false;
		}
	} else if (dot(push, push) > 1.0) {
		p.Velocity += push * TimeParams.y * 3.0;
	}
}

void Respawn(inout Particle p, uint id, uint kind, float amount, float fallSpeed)
{
	uint seed = id * 7919u + (uint)TimeParams.z * 104729u;
	// Spawn chance per frame keeps the column density proportional to the weather amount.
	if (Hash(seed) > amount * 0.25) {
		p.Life = 0;
		return;
	}
	float radius = Amounts.w * sqrt(Hash(seed + 1u));
	float angle = Hash(seed + 2u) * 6.2831853;
	float height = lerp(-300.0, Flags.w, Hash(seed + 3u));
	// lead the spawn column into the wind so drops arrive above the camera
	float3 upwind = -Wind.xyz * (Flags.w / max(fallSpeed, 50.0)) * 0.5;
	p.Position = CameraPos.xyz + float3(cos(angle) * radius, sin(angle) * radius, height) + float3(upwind.xy, 0);
	p.Age = 0;
	p.Velocity = float3(Wind.xy, -fallSpeed);
	p.Life = kind == SSPP_RAIN ? 6.0 : (kind == SSPP_SNOW ? 40.0 : 60.0);  // leaves glide long, then rest
	p.Spin = Hash(seed + 4u) * 6.2831853;
	p.Kind = kind;
	if (kind == SSPP_RAIN) {
		p.Size = RainP.x * lerp(1.0 - ExtraP2.x * 0.6, 1.0 + ExtraP2.x * 0.6, Hash(seed + 5u));
		p.Color = PackColor(float4(0.75, 0.8, 0.88, 1));
	} else if (kind == SSPP_SNOW) {
		p.Size = SnowP.x * lerp(1.0 - ExtraP2.x, 1.0 + ExtraP2.x, Hash(seed + 5u));
		p.Color = PackColor(float4(1, 1, 1, 1));
	} else {
		p.Size = LeafP.x * lerp(1.0 - ExtraP2.x * 0.8, 1.0 + ExtraP2.x * 0.8, Hash(seed + 5u));
		float t = Hash(seed + 6u);
		float3 leaf = t < 0.4 ? float3(0.55, 0.32, 0.08) : (t < 0.7 ? float3(0.62, 0.48, 0.12) : (t < 0.9 ? float3(0.3, 0.38, 0.1) : float3(0.45, 0.12, 0.05)));
		// plan step 3: conifer areas drop small needles/twigs instead of broad leaves (share from tree context)
		float coniferShare = ContextA.z / max(ContextA.y + ContextA.z, 1e-3);
		if (Hash(seed + 7u) < coniferShare) {
			p.Size *= 0.35;
			leaf = lerp(float3(0.28, 0.22, 0.1), float3(0.22, 0.28, 0.12), Hash(seed + 8u));
		}
		p.Color = PackColor(float4(leaf, 1));
		p.Position.z = CameraPos.z + lerp(150.0, 900.0, Hash(seed + 3u));
	}
	if (SkyVisibility(p.Position) < 0.35)
		p.Life = 0;  // under a roof: no precipitation here
}

[numthreads(256, 1, 1)] void main(uint3 tid : SV_DispatchThreadID)
{
	uint id = tid.x;
	if (id >= Ranges.w)
		return;
	Particle p = Particles[id];
	float dt = TimeParams.y;

	uint kind = id < Ranges.x ? SSPP_RAIN : (id < Ranges.y ? SSPP_SNOW : (id < Ranges.z ? SSPP_LEAF : (p.Kind & 0xFFu)));
	bool ambient = id < Ranges.z;

	if (p.Life <= 0.0 || p.Age >= p.Life) {
		if (ambient) {
			float amount = kind == SSPP_RAIN ? Amounts.x : (kind == SSPP_SNOW ? Amounts.y : Amounts.z);
			float fall = kind == SSPP_RAIN ? RainP.w : (kind == SSPP_SNOW ? SnowP.z : LeafP.z);
			Respawn(p, id, kind, amount, fall);
		} else {
			p.Life = 0;
		}
		Particles[id] = p;
		return;
	}
	if (dt <= 0.0)
		return;

	p.Age += dt;
	bool settled = (p.Kind & SSPP_FLAG_SETTLED) != 0;
	float3 rel = p.Position - CameraPos.xyz;
	if (ambient && (dot(rel.xy, rel.xy) > Amounts.w * Amounts.w * 1.6 || rel.z < -1500.0)) {
		p.Life = min(p.Life, p.Age + 0.75);  // left the camera column: dissolve, then recycle (no popping)
	}

	if (!settled) {
		uint seed = id * 31u + (uint)p.Spin * 977u;
		float gust = 1.0 + Wind.w * sin(TimeParams.x * 0.7 + p.Position.x * 0.002 + p.Position.y * 0.0017);
		float3 wind = Wind.xyz * gust;
		if (kind == SSPP_RAIN) {
			float3 target = float3(wind.xy, -RainP.w);
			p.Velocity = lerp(p.Velocity, target, saturate(dt * 4.0));
		} else if (kind == SSPP_SNOW) {
			float t = TimeParams.x * 1.3 + p.Spin * 5.0;
			float3 flutter = float3(sin(t), cos(t * 0.8), 0) * 35.0;
			float3 target = float3(wind.xy * 0.9, -SnowP.z) + flutter;
			p.Velocity = lerp(p.Velocity, target, saturate(dt * 1.5));
			p.Spin += dt * 1.7;
		} else if (kind == SSPP_LEAF) {
			float t = TimeParams.x * 2.1 + p.Spin * 3.0;
			// gliding fall: side-to-side swing with lift at the end of each swing (falling-leaf pendulum)
			float swing = sin(t);
			float3 flutter = float3(swing * 90.0, cos(t * 0.7) * 70.0, (abs(swing) * 2.0 - 1.0) * 40.0) * ExtraP2.y;
			float3 target = float3(wind.xy * 1.2, -LeafP.z / max(ExtraP2.y, 0.25)) + flutter;
			p.Velocity = lerp(p.Velocity, target, saturate(dt * 1.2));
			p.Spin += dt * (2.0 + Hash(seed) * 3.0);
		} else if (kind == SSPP_DUST) {
			p.Velocity *= saturate(1.0 - dt * 2.5);
			p.Velocity += float3(wind.xy * 0.4, 12.0) * dt;
		} else {
			// splash, spark, blood: ballistic with gravity and a little drag
			float g = kind == SSPP_SPARK ? 0.6 : (kind == SSPP_EMBER ? (p.Age < 0.4 ? -0.15 : 0.25) : 1.0);  // embers rise a moment, then drift down
			p.Velocity.z -= 980.0 * dt * g;
			p.Velocity *= saturate(1.0 - dt * (kind == SSPP_SPARK ? 0.6 : (kind == SSPP_EMBER ? 1.2 : 0.3)));
			if (kind == SSPP_EMBER)
				p.Velocity.xy += wind.xy * 0.5 * dt;
			if (kind == SSPP_ICE)
				p.Spin += dt * 8.0;
		}

		float3 next = p.Position + p.Velocity * dt;
		float3 surface;
		if (kind != SSPP_DUST && Collide(next, surface)) {
			if (kind == SSPP_RAIN) {
				uint seed2 = id * 131u + (uint)TimeParams.z;
				if (Hash(seed2) < SplashP.z) {
					[unroll] for (uint i = 0; i < 3; i++) {
						float a = Hash(seed2 + i * 3u + 1u) * 6.2831853;
						float s = lerp(60.0, 140.0, Hash(seed2 + i * 3u + 2u));
						SpawnDynamic(SSPP_SPLASH, surface + float3(0, 0, 1), float3(cos(a) * s, sin(a) * s, lerp(120.0, 220.0, Hash(seed2 + i))), SplashP.w, SplashP.x, float4(0.8, 0.85, 0.9, 1), seed2 + i);
					}
				}
				p.Life = 0;
			} else if (kind == SSPP_SPARK || kind == SSPP_EMBER || kind == SSPP_ICE) {
				// bounce + roll; once too slow to bounce, rest on the ground and cool / melt (ExtraP rest times)
				float bounce = kind == SSPP_ICE ? 0.3 : (kind == SSPP_EMBER ? 0.2 : SparkP.w);
				float vz = abs(p.Velocity.z) * bounce;
				p.Velocity = float3(p.Velocity.xy * 0.55, vz);
				next = surface + float3(0, 0, 1);
				if (vz < 40.0) {
					p.Velocity = 0;
					p.Kind |= SSPP_FLAG_SETTLED;
					uint s2 = id * 977u;
					float rest = kind == SSPP_ICE ? ExtraP.y : ExtraP.x;
					p.Spin = rest;  // remember the rest length for the cooling ramp
					p.Life = p.Age + rest * lerp(0.8, 1.2, Hash(s2));
				}
			} else if (kind == SSPP_SPLASH) {
				p.Life = 0;
			} else {
				// snow, leaves, blood settle on the surface and fade after their settle time
				next = surface + float3(0, 0, 0.5);
				p.Velocity = 0;
				p.Kind |= SSPP_FLAG_SETTLED;
				float settle = kind == SSPP_SNOW ? SnowP.w : (kind == SSPP_LEAF ? lerp(ExtraP.z, ExtraP.w, Hash(id * 613u + (uint)TimeParams.z)) : BloodP.z);
				p.Life = p.Age + settle;
			}
		}
		if (ambient && kind != SSPP_LEAF && SkyVisibility(next) < 0.2)
			p.Life = kind == SSPP_RAIN ? 0 : min(p.Life, p.Age + 0.5);  // under a roof edge: rain streak ends, snow dissolves
		p.Position = next;
	} else if (any(abs(OffscreenNdc(p.Position)) > 1.05)) {
		// resting particles off screen can't be depth-tested: dissolve them (removal order: far, off screen, oldest)
		p.Life = min(p.Life, p.Age + 1.5);
	}
	ReactToBodies(p, kind, settled);
	Particles[id] = p;
}
