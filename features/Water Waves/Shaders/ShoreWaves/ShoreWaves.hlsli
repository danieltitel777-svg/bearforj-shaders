// ShoreWaves for Community Shaders - shoreline Gerstner-style waves + foam.
// Pixel-shader only, no textures, no extra passes. (c) DanyaBunt mod, GPL-3.0 (derived build of CS Water.hlsl).
#ifndef __SHOREWAVES_HLSLI__
#define __SHOREWAVES_HLSLI__

// Bound by the DLL on every water draw (v0.3). Unbound -> zeros -> no waterfalls, v0.2 defaults.
cbuffer ShoreWavesWaterfallCB : register(b13)
{
	float4 WaterfallPoints[16];  // xyz camera-relative impact point, w radius (game units)
	float4 WaterfallInfo;        // x = point count
	float4 EI0, EI1, EI2, EI3;   // v0.3 live menu settings, see EISettings.hlsli
	float4 EI4, EI5;             // v0.4: foam texture, surf (wave height / run-up / period)
	float4 EIwPad6;              // EI6 slot of the shared GPUBlock (blood, unused by water)
	float4 CD0, CD1, CD2, CD3;   // v0.11: Crimson-Desert-inspired layers (CDWater.hlsli)
	float4 ECParallax;           // Bearforj: x on, y foam depth (units), z wave depth (units), w unused
};

// v0.4: 2048^2 seamless foam lace (textures/ElementalContact/foam.dds), bound by the DLL on PS t107.
// Unbound -> EI4.x == 0 -> procedural fallback.
Texture2D<float4> EIFoamTex : register(t107);
// v0.8: 2048^2 tileable detail normal map (textures/ElementalContact/water_normals.dds, BC5 xy), PS t108.
// WaterfallInfo.y = strength (0 = not bound -> procedural), .z = flow speed, .w = 1 when y/z are valid.
Texture2D<float2> EIWaterNormalTex : register(t108);
// Bearforj: height maps derived from foam.dds alpha and from water_normals.dds slopes.
// Data\textures\ElementalContact\foam_height.dds and wave_height.dds. Flat when ECParallax.x is 0.
Texture2D<float4> EIFoamHeightTex : register(t111);
Texture2D<float4> EIWaveHeightTex : register(t112);
float3 gECView = float3(0, 0, 1);

float2 ECParallaxUv(Texture2D<float4> heightTex, float2 uv, float worldPerTile, float depthUnits)
{
	if (ECParallax.x < 0.5 || worldPerTile < 1.0 || depthUnits <= 0.0)
		return uv;
	float3 v = gECView;
	float2 duv = (v.xy / max(abs(v.z), 0.12)) * (depthUnits / worldPerTile);
	float layer = 1.0;
	float2 uvOut = uv;
	float going = 1.0;
	// No break: fxc refuses [unroll] when the exit depends on the height sample.
	[unroll]
	for (int i = 0; i < 6; ++i) {
		float h = heightTex.SampleLevel(Normals01Sampler, uv + duv * (1.0 - layer), 0).r;
		float miss = step(h + 1e-4, layer);
		float stepOn = going * miss;
		layer -= stepOn * (1.0 / 6.0);
		uvOut = lerp(uvOut, uv + duv * (1.0 - layer), stepOn);
		going *= miss;
	}
	return uvOut;
}
#define EI_HAS_CB
#include "ShoreWaves/EISettings.hlsli"

namespace ShoreWaves
{
	// ---- tweakables (edit and clear the shader cache) ----
	static const float MaxDepth      = 120.0;  // game units: waves fade out deeper than this
	static const float CrestSpacing  = 14.0;   // depth (units) between successive crest lines
	static const float Speed         = 0.35;   // crest travel speed toward the shore (contours per second)
	static const float Strength      = 0.22;   // normal tilt strength (kept low: direction is per terrain facet)
	static const float FoamDepth     = 5.0;    // waterline foam band: water shallower than this (~7 cm)
	static const float CrestFoamDepth = 22.0;  // breaking-crest foam only in water shallower than this
	static const float FoamStrength  = 0.70;   // max foam opacity (never fully opaque)
	static const float FoamFadeStart = 2500.0; // distance from camera where foam starts fading
	static const float FoamFadeEnd   = 6000.0;
	static const float3 FoamColor    = float3(0.85, 0.88, 0.90);

	struct Result
	{
		float3 normal;   // perturbed normal (z-up)
		float  foam;     // 0..1 (already includes opacity and distance fade)
		float3 foamTint; // foam texture colour (1 = plain)
		float  exposed;  // 0..1 backwash: shallow bottom uncovered (render as wet sand)
		float  shade;    // colour multiplier: >1 lit crest face, <1 trough
	};

	float Hash21(float2 p)
	{
		p = frac(p * float2(123.34, 456.21));
		p += dot(p, p + 45.32);
		return frac(p.x * p.y);
	}

	float VNoise(float2 p)
	{
		float2 i = floor(p), f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), f.x), lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), f.x), f.y);
	}

	// 3-octave value noise, 0..1, smooth (no per-cell blocks)
	float FoamNoise(float2 p, float t)
	{
		float n = VNoise(p / 23.0 + float2(t * 0.11, -t * 0.07)) * 0.5;
		n += VNoise(p / 9.0 - float2(t * 0.19, t * 0.13)) * 0.3;
		n += VNoise(p / 3.5 + float2(-t * 0.31, t * 0.23)) * 0.2;
		return n;
	}

	// v0.4: foam lace from the 2k texture, two world-space layers scrolled by the wave cycle.
	// mask: procedural foam amount 0..1 (where foam should be); returns visible foam 0..1, tint in rgb
	float TexturedFoam(float mask, float2 posAbsXY, float time, float cycle, out float3 tint)
	{
		tint = 1.0;
		if (EI_FoamTexOn < 0.5) {
			float n = FoamNoise(posAbsXY, time);
			return saturate(mask * smoothstep(0.30, 0.65, n + mask * 0.25));
		}
		const float scale = EI_FoamTexScale;
		float2 drift = float2(0.9, 0.4) * cycle * 0.08;  // lace slides a little with every wave
		float2 uv1 = posAbsXY / scale + float2(0.011, -0.007) * time + drift;
		float2 p2 = float2(0.8 * posAbsXY.x - 0.6 * posAbsXY.y, 0.6 * posAbsXY.x + 0.8 * posAbsXY.y);
		float2 uv2 = p2 / (scale * 0.53) + float2(-0.009, 0.013) * time - drift * 1.7;
		uv1 = ECParallaxUv(EIFoamHeightTex, uv1, scale, ECParallax.y);
		uv2 = ECParallaxUv(EIFoamHeightTex, uv2, scale * 0.53, ECParallax.y);
		float4 a = EIFoamTex.Sample(Normals01Sampler, uv1);
		float4 b = EIFoamTex.Sample(Normals01Sampler, uv2);
		float lace = saturate(a.a * 0.7 + b.a * 0.55 + a.a * b.a);
		tint = lerp(a.rgb, b.rgb, 0.5);
		// dense foam (mask ~1) fills in, thin foam keeps only the lace walls
		float thr = lerp(0.55, 0.0, saturate(mask));
		return saturate(smoothstep(thr, thr + 0.35, lace) * saturate(mask * 1.4));
	}

	// waterDepth: vertical water column under this pixel (game units, >= 0, continuous across terrain facets)
	// worldPos  : absolute world position of the water surface pixel; camDist: distance to the camera
	Result Apply(float3 normal, float waterDepth, float3 worldPos, float time, float camDist)
	{
		Result r;
		r.normal = normal;
		r.foam = 0;
		r.foamTint = 1;
		r.exposed = 0;
		r.shade = 1;

		const float waveH = max(EI_WaveHeight, 0.0);
		const float maxDepth = MaxDepth * (0.75 + 0.75 * saturate(waveH));
		float shallow = saturate(1.0 - waterDepth / maxDepth);
		float distFade = 1.0 - smoothstep(FoamFadeStart, FoamFadeEnd, camDist);
		if (shallow <= 0.0 || distFade <= 0.0 || EI_ShoreOn < 0.5)
			return r;

		// One crest reaches the waterline per wave period (EISurf::Cycle == 0), then the swash runs up.
		float tc = time / EI_WavePeriod + EISurf::PhaseShift(worldPos.xy);
		float cycle = frac(tc);
		float wobble = (VNoise(worldPos.xy / 160.0 + time * 0.05) - 0.5) * 0.25;
		float phase = 6.2831853 * (waterDepth / EI_CrestSpacing + tc + wobble);
		float crest = saturate(0.5 + 0.5 * cos(phase));
		// steepening + breaking in shallow water
		float breakDepth = 12.0 + 22.0 * saturate(waveH);
		float breaking = 1.0 - smoothstep(breakDepth * 0.3, breakDepth, waterDepth);
		float shoal = sqrt(shallow) * saturate(waterDepth / 3.0);
		float sharpCrest = pow(crest, lerp(1.5, 5.0, breaking));

		// Normal tilt along the depth gradient (constant per terrain triangle -> faded where unreliable).
		float2 dpx = ddx(worldPos.xy), dpy = ddy(worldPos.xy);
		float ddX = ddx(waterDepth), ddY = ddy(waterDepth);
		float det = dpx.x * dpy.y - dpx.y * dpy.x;
		float2 grad = abs(det) > 1e-6 ? float2(ddX * dpy.y - ddY * dpx.y, ddY * dpx.x - ddX * dpy.x) / det : 0;
		float gradLen = length(grad);
		float confidence = smoothstep(0.02, 0.15, gradLen);
		float2 dir = gradLen > 1e-4 ? grad / gradLen : 0;
		// v0.6: depth derivatives are per terrain facet -> tilt faded out in the last ~10 units (no triangles at the edge)
		float amp = EI_ShoreStrength * (0.6 + 1.4 * waveH) * shoal * confidence * smoothstep(3.0, 12.0, waterDepth);
		r.normal = normalize(float3(normal.xy - dir * sin(phase) * sharpCrest * amp, normal.z));

		// Direction-free shading cue: the wave reads as a lit crest + darker trough even on flat facets.
		r.shade = 1.0 + (sharpCrest - 0.45) * 0.30 * saturate(waveH) * shoal;

		// Swash: front elevation relative to the still surface; u = -depth for water pixels.
		float front = EISurf::Front(worldPos.xy, time);
		float u = -waterDepth;
		float n = FoamNoise(worldPos.xy, time);
		float edgeW = 2.0 + 3.0 * n;
		if (front < 0.0)  // backwash: the shallow bottom between the front and the waterline is uncovered
			r.exposed = smoothstep(front - 0.5, front + 1.5, u);
		float swashLine = (1.0 - smoothstep(0.0, edgeW, abs(u - min(front, 0.0)))) * (front < 0.0 ? 1.0 : saturate(1.0 - cycle * 2.5));

		// Foam: waterline band (strongest during uprush) + breaking crest + swash front line + trail behind backwash.
		float band = (1.0 - smoothstep(0.0, EI_FoamDepth * (0.6 + 0.8 * n), waterDepth)) * lerp(0.55, 1.0, saturate(1.0 - cycle * 2.0));
		float crestFoam = pow(crest, 10.0) * breaking * saturate(waterDepth / 2.0) * saturate(0.4 + waveH);
		float trail = (front < 0.0 && u > front) ? 0.35 * (1.0 - cycle) : 0.0;
		float mask = max(max(band, crestFoam), max(swashLine, trail));
		// v0.6: no foam on the very first unit of water (that is the mesh / terrain intersection line itself);
		// a noise-jittered soft start so the waterline is not traced by a bright jagged line.
		mask *= smoothstep(0.0, 1.0 + 2.0 * n, waterDepth);

		float3 tint;
		float foam = TexturedFoam(mask, worldPos.xy, time, cycle, tint);
		r.foamTint = tint;
		r.foam = saturate(foam) * saturate(EI_FoamStrength * EI_FoamOpacity * 1.2) * distFade;
		return r;
	}
}

// ===================== River waves / rock foam / waterfall foam (added) =====================
// Optional waterfall impact points, bound by the ActorGrime/DTD fork DLL (BSWaterShader hook).
// Unbound -> zeros -> WaterfallInfo.x == 0 -> no effect.
// (ShoreWavesWaterfallCB moved to the top of this file in v0.3)

namespace ShoreWaves
{
	static const float RiverMinSpeed   = 0.15;  // flow below this -> no river waves
	static const float RiverFastSpeed  = 0.85;
	static const float RockBand        = 5.0;   // ~10 cm contact band (game units of depth)

	float ValueNoise2(float2 p)
	{
		float2 i = floor(p), f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), f.x), lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), f.x), f.y);
	}

	float2 Rotate(float2 v, float a)
	{
		float s = sin(a), c = cos(a);
		return float2(c * v.x - s * v.y, s * v.x + c * v.y);
	}

	// v0.4-v0.7 river waves, kept ONLY for their foam (unchanged look); their normal is no longer used.
	// flow: world-space flow vector decoded from the CS flowmap (length ~ 0..1.4)
	Result RiverWavesV07(float3 normal, float2 flow, float3 worldPos, float time)
	{
		Result r;
		r.normal = normal;
		r.foam = 0;
		float speed = length(flow);
		float strength = smoothstep(RiverMinSpeed, RiverFastSpeed, speed);
		const float riverMul = EI_River;
		if (strength <= 0.0 || riverMul <= 0.001)
			return r;
		float2 dir = flow / max(speed, 1e-4);
		float fast = smoothstep(0.6, 1.0, speed);
		float tilt = lerp(0.9, 1.2, fast) * strength * riverMul;

		static const float  lens[4]   = { 60.0, 140.0, 300.0, 95.0 };
		static const float  angles[4] = { 0.0, 0.45, -0.6, 1.1 };
		static const float  speeds[4] = { 1.6, 1.1, 0.7, 1.3 };
		static const float  amps[4]   = { 0.30, 0.35, 0.25, 0.10 };

		float2 slope = 0;
		float crestMax = 0;
		float pn = ValueNoise2(worldPos.xy / 180.0 + time * 0.05) * 6.2831853;
		[unroll] for (int i = 0; i < 4; i++) {
			float2 d = Rotate(dir, angles[i]);
			float k = 6.2831853 / lens[i];
			float ph = dot(worldPos.xy, d) * k - time * speeds[i] * (1.0 + 2.0 * speed) + pn * (0.35 + 0.15 * i);
			float s = sin(ph), c = cos(ph);
			float sharp = lerp(2.0, 4.0, fast);
			float crest = pow(saturate(0.5 + 0.5 * s), sharp);
			float dcrest = c * pow(saturate(0.5 + 0.5 * s), sharp - 1.0) * sharp * 0.5;
			slope += d * dcrest * amps[i];
			crestMax = max(crestMax, crest * amps[i] / 0.35);
		}
		slope *= tilt;
		r.normal = normalize(float3(normal.xy - slope, normal.z));

		float n = ValueNoise2(worldPos.xy / 9.0 - dir * time * 40.0);
		r.foam = saturate(saturate(pow(crestMax, 6.0) * fast * (0.6 + 0.4 * n)) * 0.7 * riverMul);
		return r;
	}

	// v0.8 detail normals: two scales of the 2K tileable normal map scrolled in different directions (lakes, sea).
	// Absolute world position / tile size keeps the pattern continuous across water cells (no per-cell squares).
	// SampleGrad: explicit gradients (world-position derivatives taken in uniform flow) so the samples are legal
	// inside FXC dynamic branches; gScale = uv units per world unit of this layer.
	float2 SampleDetail(float2 uv, float gScale, float4 dxy)
	{
		return EIWaterNormalTex.SampleGrad(Normals01Sampler, uv, dxy.xy * gScale, dxy.zw * gScale) * 2.0 - 1.0;
	}

	// flow: world-space flow vector decoded from the CS flowmap (length ~ 0..1.4)
	// v0.8: the v0.4-v0.7 river waves used phase = dot(worldPos, flowDir) * k. worldPos is absolute (1e5 units), so the
	// tiny direction changes between flow-map texels turned into huge phase jumps -> square blocks following the flow-map
	// grid. Now: classic two-phase flow-map advection of the detail texture (UV offset = flow * phase, cross-faded),
	// which is continuous no matter how the flow changes, plus a calm two-layer drift where there is no flow.
	Result RiverWaves(float3 normal, float2 flow, float3 worldPos, float time)
	{
		Result r;
		r.normal = normal;
		r.foam = 0;
		r.foamTint = 1;
		r.exposed = 0;
		r.shade = 1;
		const float4 eiDxy = float4(ddx(worldPos.xy), ddy(worldPos.xy));
		const float strengthTex = WaterfallInfo.w > 0.5 ? WaterfallInfo.y : 0.0;
		const float flowSpeed = WaterfallInfo.w > 0.5 ? WaterfallInfo.z : 1.0;
		float speed = length(flow);
		const float riverMul = EI_River;
		float2 slope = 0;
		[branch] if (strengthTex > 0.001)
		{
			// still water: two layers, ~5 m and ~1.7 m tiles, slow wind drift
			float2 p = worldPos.xy;
			float2 a = SampleDetail(p / 512.0 + float2(0.011, 0.007) * time, 1.0 / 512.0, eiDxy);
			float2 b = SampleDetail(p / 173.0 + float2(-0.017, 0.013) * time, 1.0 / 173.0, eiDxy);
			float calm = 1.0 - smoothstep(0.05, 0.5, speed);
			slope = (a * 0.55 + b * 0.45) * 0.35 * calm;
			// flowing water: advect along the flow, two phases half a cycle apart, triangle cross-fade
			[branch] if (speed > 0.05 && riverMul > 0.001)
			{
				float cycle = 1.0;
				float t = time * 0.25 * flowSpeed;
				float ph0 = frac(t), ph1 = frac(t + 0.5);
				float w0 = 1.0 - abs(1.0 - 2.0 * ph0);
				float2 adv = flow * 0.35;
				float2 uv = p / 220.0;
				float2 f0 = SampleDetail(uv - adv * ph0 * cycle, 1.0 / 220.0, eiDxy);
				float2 f1 = SampleDetail(uv - adv * ph1 * cycle + 0.5, 1.0 / 220.0, eiDxy);
				float2 fl = f0 * w0 + f1 * (1.0 - w0);
				float strength = smoothstep(RiverMinSpeed * 0.5, RiverFastSpeed, speed);
				slope += fl * lerp(0.35, 0.9, strength) * riverMul * (1.0 - calm * 0.5);
			}
			slope *= strengthTex;
		}
		r.normal = normalize(float3(normal.xy + slope, normal.z));
		r.foam = RiverWavesV07(normal, flow, worldPos, time).foam;  // foam identical to v0.7
		if (strengthTex <= 0.001)
			r.normal = RiverWavesV07(normal, flow, worldPos, time).normal;  // no texture: old behaviour
		return r;
	}

	// Foam where fast water hits rocks: shallow contact band, denser upstream, short tail downstream.
	float RockFoam(float waterDepth, float2 flow, float3 worldPos, float time)
	{
		float speed = length(flow);
		float strong = smoothstep(0.45, 0.9, speed);
		if (strong <= 0.0)
			return 0.0;
		float2 dir = flow / max(speed, 1e-4);
		float band = 1.0 - smoothstep(0.0, RockBand, waterDepth);
		// advected noise, cells respawn every 1.5 s with crossfade
		const float period = 1.5;
		float t0 = time / period;
		float a = frac(t0);
		float2 adv = worldPos.xy - dir * time * 60.0 * speed;
		float n0 = ValueNoise2(adv / 5.0 + Hash21(floor(t0).xx) * 37.0);
		float n1 = ValueNoise2(adv / 5.0 + Hash21(floor(t0 + 0.5).xx + 0.5) * 37.0);
		float w0 = 1.0 - abs(a * 2.0 - 1.0);
		float n = lerp(n1, n0, w0);
		return saturate(saturate(band * strong * smoothstep(0.45, 0.8, n)) * 0.6 * EI_RockFoam);
	}

	// Boiling foam patches around waterfall impact points (camera-relative positions).
	float WaterfallFoam(float3 posRel, float3 worldPos, float time, out float2 turb)
	{
		turb = 0;
		uint count = (uint)min(WaterfallInfo.x, 16.0);
		float foam = 0;
		[loop] for (uint i = 0; i < count; i++) {
			float4 p = WaterfallPoints[i];
			float radius = max(p.w, 1.0);
			float2 dxy = posRel.xy - p.xy;
			float d = length(dxy);
			if (d > radius * 1.6 || abs(posRel.z - p.z) > 200.0)
				continue;
			float boil = ValueNoise2(worldPos.xy / 14.0 + float2(time * 0.7, -time * 0.9)) * 0.6 +
			             ValueNoise2(worldPos.xy / 5.0 - time * 1.7) * 0.4;
			float core = 1.0 - smoothstep(radius * 0.35, radius * 1.4, d + boil * radius * 0.4);
			foam = max(foam, core * (0.65 + 0.35 * boil));
			turb += (dxy / max(d, 1e-3)) * core * (boil - 0.5) * 0.8;  // outward boiling bumps
		}
		turb *= EI_WaterfallFoam;
		return saturate(foam * EI_WaterfallFoam);
	}
}
#include "ShoreWaves/CDWater.hlsli"
#endif
