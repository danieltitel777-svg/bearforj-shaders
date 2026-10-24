// Elemental Contact v0.11 - "Crimson Desert inspired" water layers (our own code, GPL-3.0).
// Included from ShoreWaves.hlsli (Water.hlsl only). Settings in b13 CD0..CD3 (see EISettings.hlsli / DLL CDBlock).
// All features are additive layers on top of the v0.8 shore waves / foam; each has its own menu toggle.
// Units: Skyrim units (1 m ~ 70 u), g ~ 687 u/s^2, time in seconds. Normals only (water meshes are flat).
#ifndef __EC_CDWATER_HLSLI__
#define __EC_CDWATER_HLSLI__

namespace CDWater
{
	static const float Grav = 687.0;
	static const float TwoPi = 6.2831853;

	// Ripple slope and the water_normals cascade are not in this build.
	// Small wave detail is the Gerstner ripple in WaterWaves.
	float RunAmt()       { return EI3.w > 0.5 ? CD1.x : 0.0; }
	float RunLength()    { return max(CD1.y, 40.0); }
	float RunPeriod()    { return max(CD1.z, 1.5); }
	float BeachSlope()   { return clamp(CD1.w, 0.01, 1.0); }
	float FoamDepthCol() { return EI3.w > 0.5 ? CD2.x : 0.0; }
	float ObstacleAmt()  { return EI3.w > 0.5 ? CD2.y : 0.0; }
	float CrestFoamAmt() { return EI3.w > 0.5 ? CD2.w : 0.0; }
	float CrestDecay()   { return max(CD3.x, 0.2); }

	// ---- Shore surf: waves running toward the shore, depth proxy dist = h / slope.
	// Phase w (t + 2 sqrt(h) / (s sqrt g)): crests travel shoreward with c = sqrt(g h) (slow down in the shallows).
	struct Run
	{
		float2 slope;   // normal xy offset
		float  crest;   // 0..1 crest mask
		float  foam;    // temporal crest foam (build-up + decay) 0..1
		float  env;     // envelope 0..1
	};

	Run ShoreRun(float h, float2 gradH, float2 pAbs, float t, float camDist)
	{
		Run r;
		r.slope = 0; r.crest = 0; r.foam = 0; r.env = 0;
		float amt = RunAmt();
		if (amt <= 0.0 || h <= 0.0)
			return r;
		const float T = RunPeriod();
		const float w = TwoPi / T;
		const float S = BeachSlope();
		const float hMax = max(RunLength() * 0.5, 40.0);       // waves exist in water shallower than ~lambda/2
		const float hBreak = max(hMax * 0.18, 6.0);
		float wob = (ShoreWaves::VNoise(pAbs / 400.0 + t * 0.02) - 0.5) * 1.6;
		float ph = w * (t + 2.0 * sqrt(max(h, 0.0)) / (S * sqrt(Grav))) + wob;
		float c01 = saturate(0.5 + 0.5 * cos(ph));   // v0.13.2: saturate -> pow() never sees a tiny negative (NaN)
		float green = clamp(pow(hMax / max(h, 2.0), 0.25), 1.0, 2.2);   // Green's law growth in the shallows
		r.env = smoothstep(0.0, hBreak, h) * (1.0 - smoothstep(hMax * 0.6, hMax, h)) * green / 2.2;
		float breaking = 1.0 - smoothstep(hBreak, hBreak * 2.5, h);
		float prof = pow(c01, lerp(2.0, 6.0, breaking));
		r.crest = prof * r.env;
		// d(height)/d(h) * grad h: grad h is per terrain facet, so use the model slope S along its direction
		float gl = length(gradH);
		float2 dirDeep = gl > 1e-4 ? gradH / gl : 0;
		float conf = smoothstep(0.01, 0.08, gl);
		float dPhdh = w / (S * sqrt(Grav * max(h, 2.0)));
		float dProf = -lerp(2.0, 6.0, breaking) * pow(c01, lerp(1.0, 5.0, breaking)) * 0.5 * sin(ph);
		float A = 3.0 * amt;                                            // apparent height, units (shading only)
		r.slope = -dirDeep * (A * dProf * dPhdh * S) * r.env * conf * 3.0;
		r.slope = clamp(r.slope, -0.6, 0.6) * (1.0 - smoothstep(3000.0, 7000.0, camDist));
		// (c) temporal crest foam: time since the last crest passed this point, each crest leaves foam that
		// decays (tau = CrestDecay); geometric sum of all previous crests -> build-up toward a steady level.
		float tau = frac(-ph / TwoPi) * T;
		float dk = CrestDecay();
		float steady = 1.0 / max(1.0 - exp(-T / dk), 1e-3);
		float perCrest = r.env * (0.35 + 0.65 * breaking);
		r.foam = saturate(exp(-tau / dk) * steady * perCrest * 0.45) * CrestFoamAmt();
		return r;
	}

	// (a) obstacle foam: a steep bottom right under shallow water = rock / bank / shore edge
	float ObstacleFoam(float h, float gradLen, float2 pAbs, float t)
	{
		float amt = ObstacleAmt();
		if (amt <= 0.0)
			return 0;
		float steep = smoothstep(0.35, 1.5, gradLen);
		float shallowBand = 1.0 - smoothstep(6.0, 45.0, h);
		float n = ShoreWaves::FoamNoise(pAbs * 1.6, t * 1.4);
		return saturate(steep * shallowBand * smoothstep(0.35, 0.7, n + 0.15) * amt);
	}

	// foam colour by depth: thin, water-tinted foam over deep water, bright white in the shallows
	float3 FoamTint(float h)
	{
		float amt = FoamDepthCol();
		float deep = smoothstep(4.0, 80.0, h);
		float3 deepFoam = float3(0.72, 0.84, 0.86);
		float3 shallowFoam = float3(1.00, 0.98, 0.94);
		return lerp(1.0.xxx, lerp(shallowFoam, deepFoam, deep), amt);
	}

}
#endif
