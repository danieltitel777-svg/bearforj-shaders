// Elemental Contact v0.12 - rain water running over surfaces / stairs (our own code, GPL-3.0).
// World-space mask around the player written by the DLL (RainFlow.cpp), PS t106, R16G16B16A16_FLOAT:
//   R = wetness 0..1 (+2 where the path falls over a step edge), GB = flow direction xy, A = surface z - centre z.
// Settings in ActorGrimeCB: EI11 (centre, half extent, strength), EI12 (darken, gloss, ripples, speed), EI13 (edges, centre z, texel).
// Only bound for world (non-actor) Lighting draws; unbound -> EI11.w = 0 -> nothing happens.
#ifndef __EC_SURFACEFLOW_HLSLI__
#define __EC_SURFACEFLOW_HLSLI__

Texture2D<float4> EIFlowTex : register(t106);
// v0.13 puddles: PS t110 (free in Open Shaders 2.18), R16G16_FLOAT 128x128, same window as EIFlowTex:
//   R = puddle depth 0..1, G = ground z - centre z. EI14 = strength, edge threshold, gloss, darken.
Texture2D<float2> EIPuddleTex : register(t110);

namespace SurfaceFlow
{
	struct Info
	{
		float  wet;    // 0..1
		float2 dir;    // flow direction (xy, normalized or 0)
		float  fall;   // 0..1 near a step-edge fall
	};

	float Strength() { return (EI3.w > 0.5) ? EI11.w : 0.0; }

	// one texel, z-tested (only the surface the water ran over, not bridges / roofs above or floors below)
	float4 Texel(int2 c, float zRel)
	{
		if (any(c < 0) || any(c >= 256))
			return 0;
		float4 v = EIFlowTex.Load(int3(c, 0));
		float zOk = 1.0 - smoothstep(24.0, 56.0, abs(zRel - v.w));
		return float4(v.x, v.y, v.z, zOk);
	}

	Info Sample(float3 posAbs)
	{
		Info o;
		o.wet = 0; o.dir = 0; o.fall = 0;
		if (Strength() <= 0.0)
			return o;
		float2 uv = (posAbs.xy - EI11.xy) / (2.0 * EI11.z) + 0.5;
		if (any(uv <= 0.002) || any(uv >= 0.998))
			return o;
		float2 tc = uv * 256.0 - 0.5;
		int2 c0 = int2(floor(tc));
		float2 f = tc - floor(tc);
		float zRel = posAbs.z - EI13.y;
		float4 t00 = Texel(c0, zRel), t10 = Texel(c0 + int2(1, 0), zRel), t01 = Texel(c0 + int2(0, 1), zRel), t11 = Texel(c0 + int2(1, 1), zRel);
		float4 wts = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y) * float4(t00.w, t10.w, t01.w, t11.w);
		float4 raw = float4(t00.x, t10.x, t01.x, t11.x);
		float4 fl = step(1.5, raw);
		float4 wv = raw - 2.0 * fl;
		o.wet = saturate(dot(wv, wts));
		o.fall = saturate(dot(fl, wts) * 1.5);
		float2 d = t00.yz * wts.x + t10.yz * wts.y + t01.yz * wts.z + t11.yz * wts.w;
		float dl = length(d);
		o.dir = dl > 1e-3 ? d / dl : 0;
		o.wet *= Strength();
		// fade at the mask border
		float2 edgeUv = min(uv, 1.0 - uv);
		o.wet *= smoothstep(0.0, 0.06, min(edgeUv.x, edgeUv.y));
		return o;
	}

	// slope offset for the wet normal: ripples running along the flow, faster on steep surfaces
	float3 RippleNormal(float3 nrm, float3 posAbs, Info fi, float t)
	{
		if (fi.wet <= 0.01 || EI12.z <= 0.0)
			return nrm;
		float2 dir = fi.dir;
		if (dot(dir, dir) < 0.25)
			dir = float2(0.7071, 0.7071);
		float2 perp = float2(-dir.y, dir.x);
		float steep = 1.0 - saturate(nrm.z);
		float spd = (35.0 + 140.0 * steep) * EI12.w;
		float along = dot(posAbs.xy, dir), across = dot(posAbs.xy, perp);
		float wob = EISurf::Noise(float2(across / 7.0, along / 40.0 - t * 0.2)) * 3.0;
		float ph = (along - t * spd) / 7.0 + wob;
		float ph2 = (along * 1.7 - t * spd * 1.3) / 5.0 + wob * 1.3;
		float slopeA = (cos(ph) * 0.6 + cos(ph2) * 0.4) * 0.22 * EI12.z * fi.wet;
		float slopeP = (EISurf::Noise(float2(across / 3.0, (along - t * spd) / 9.0)) - 0.5) * 0.12 * EI12.z * fi.wet;
		return normalize(nrm + float3(dir * slopeA + perp * slopeP, 0.0));
	}

	// step edges: thin film streaks running down the riser + small moving glints. Returns x film, y glint.
	float2 Edge(float3 nrm, float3 posAbs, Info fi, float t)
	{
		float vertical = 1.0 - saturate(abs(nrm.z) * 1.6);
		float lip = saturate(1.0 - abs(nrm.z - 0.7) * 3.0);               // rounded lip of the step
		float edge = fi.fall * max(vertical, lip * 0.6) * saturate(fi.wet * 2.0 + 0.3) * EI13.x;
		if (edge <= 0.01)
			return 0;
		float2 h = normalize(float2(-nrm.y, nrm.x) + 1e-4);              // horizontal tangent of the riser
		float u = dot(posAbs.xy, h);
		float streak = smoothstep(0.45, 0.8, EISurf::Noise(float2(u / 3.0, posAbs.z / 60.0 + t * 0.6)));
		float film = edge * (0.45 + 0.55 * streak);
		float cellU = floor(u / 5.0);
		float cellV = floor(posAbs.z / 7.0 + t * 3.5 + EISurf::Hash(float2(cellU, 1.7)) * 9.0);
		float glint = step(0.92, EISurf::Hash(float2(cellU, cellV))) * edge;
		return float2(saturate(film), saturate(glint));
	}
	// ---- v0.13 puddles ------------------------------------------------------------------------------
	float PuddleTexel(int2 c, float zRel)
	{
		if (any(c < 0) || any(c >= 128))
			return 0;
		float2 v = EIPuddleTex.Load(int3(c, 0));
		return v.x * (1.0 - smoothstep(20.0, 48.0, abs(zRel - v.y)));
	}

	// x = puddle mask (flat water, ragged noisy edge), y = damp rim (darker, glossier, keeps the relief)
	float2 Puddle(float3 posAbs, float nz)
	{
		if (EI3.w <= 0.5 || EI14.x <= 0.0)
			return 0;
		float2 uv = (posAbs.xy - EI11.xy) / (2.0 * EI11.z) + 0.5;
		if (any(uv <= 0.004) || any(uv >= 0.996))
			return 0;
		float2 tc = uv * 128.0 - 0.5;
		int2 c0 = int2(floor(tc));
		float2 f = tc - floor(tc);
		float zRel = posAbs.z - EI13.y;
		float p00 = PuddleTexel(c0, zRel), p10 = PuddleTexel(c0 + int2(1, 0), zRel);
		float p01 = PuddleTexel(c0 + int2(0, 1), zRel), p11 = PuddleTexel(c0 + int2(1, 1), zRel);
		float pd = lerp(lerp(p00, p10, f.x), lerp(p01, p11, f.x), f.y);
		if (pd <= 0.002)
			return 0;
		float nse = EISurf::Noise(posAbs.xy * 0.02) * 0.7 + EISurf::Noise(posAbs.xy * 0.08) * 0.3;
		float v = pd + (nse - 0.5) * 0.35;
		float thr = clamp(EI14.y, 0.1, 0.9);
		float flatF = smoothstep(0.85, 0.95, nz);
		float2 edgeUv = min(uv, 1.0 - uv);
		float fade = smoothstep(0.0, 0.06, min(edgeUv.x, edgeUv.y)) * flatF * saturate(EI14.x);
		float msk = smoothstep(thr - 0.05, thr + 0.05, v) * fade;
		float damp = saturate(smoothstep(0.0, thr, v) * 1.15) * saturate(pd * 8.0) * fade;
		return float2(msk, max(damp, msk));
	}

	// footstep rings (DLL fills up to 4 recent steps in puddles). Returns the slope offset (xy) for a z-up normal.
	float2 StepRing(float4 slot, float2 p)
	{
		if (slot.w >= 3.0 || slot.w < 0.0)
			return 0;
		float2 d = p - slot.xy;
		float dist = length(d);
		float r = slot.w * 70.0 + 4.0;
		float band = saturate(1.0 - abs(dist - r) / 9.0);
		float ring = sin((dist - r) * 0.7) * band * (1.0 - slot.w / 3.0);
		return (dist > 0.01 ? d / dist : 0) * ring * 0.35;
	}

	float2 StepRipples(float3 posAbs)
	{
		return StepRing(EI15, posAbs.xy) + StepRing(EI16, posAbs.xy) + StepRing(EI17, posAbs.xy) + StepRing(EI18, posAbs.xy);
	}
}
#endif
