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

	// --- settings (zeros when the DLL did not write the block -> everything off)
	float RippleAmt()    { return EI3.w > 0.5 ? CD0.x : 0.0; }
	float RippleScale()  { return max(CD0.y, 0.1); }
	float RippleSpeed()  { return CD0.z; }
	float NormalTexAmt() { return (EI3.w > 0.5 && WaterfallInfo.y > 0.0) ? CD0.w : 0.0; }
	float RunAmt()       { return EI3.w > 0.5 ? CD1.x : 0.0; }
	float RunLength()    { return max(CD1.y, 40.0); }
	float RunPeriod()    { return max(CD1.z, 1.5); }
	float BeachSlope()   { return clamp(CD1.w, 0.01, 1.0); }
	float FoamDepthCol() { return EI3.w > 0.5 ? CD2.x : 0.0; }
	float ObstacleAmt()  { return EI3.w > 0.5 ? CD2.y : 0.0; }
	float QuietAmt()     { return EI3.w > 0.5 ? CD2.z : 0.0; }
	float CrestFoamAmt() { return EI3.w > 0.5 ? CD2.w : 0.0; }
	float CrestDecay()   { return max(CD3.x, 0.2); }
	float CausticsAmt()  { return EI3.w > 0.5 ? CD3.y : 0.0; }
	float FogTintAmt()   { return EI3.w > 0.5 ? CD3.z : 0.0; }
	float CausticsMode() { return CD3.w; }  // 0 = only when Open Shaders caustics are off, 1 = always

	float Hash11(float n) { return frac(sin(n * 127.1) * 43758.5453); }

	// finite-depth dispersion w = sqrt(g k tanh(k h))
	float Omega(float k, float h) { return sqrt(Grav * k * tanh(max(k * h, 0.05))); }

	// ---- Stage 1: sum of directional Gerstner waves, returns the normal xy offset (slopes)
	float2 RippleSlope(float2 pAbs, float t, float h, float camDist)
	{
		float amt = RippleAmt();
		float2 sl = 0;
		if (amt <= 0.0)
			return sl;
		// pixel footprint -> fade waves that would alias
		float foot = max(length(ddx(pAbs)), length(ddy(pAbs)));
		const float baseDir = 0.6 + 0.35 * sin(t * 0.004);  // slow wind veer
		// v0.13.2: 9 waves in three sets - long swell (narrow spread), mid wind waves, short cross chop
		[unroll] for (int i = 0; i < 9; ++i) {
			float fi = (float)i;
			float lam = RippleScale() * (420.0 * pow(0.70, fi));            // 420 .. 24 units
			float spread = (i < 3) ? 0.6 : ((i < 6) ? 1.5 : 2.6);
			float ang = baseDir + (Hash11(fi + 3.0) - 0.5) * spread + ((i >= 6) ? 1.2 : 0.0);
			float2 dirW = float2(cos(ang), sin(ang));
			float k = TwoPi / lam;
			float w = Omega(k, h) * RippleSpeed();
			float slopeA = (0.095 - 0.006 * fi);                             // k*A, sum ~0.64 < 1 (no loops)
			float th = k * dot(dirW, pAbs) - w * t + Hash11(fi * 7.1) * TwoPi;
			float fadeAlias = 1.0 - smoothstep(lam * 0.12, lam * 0.35, foot);
			sl -= dirW * slopeA * cos(th) * fadeAlias;
		}
		// calmer in very shallow water and far away
		return sl * amt * smoothstep(0.0, 10.0, h) * (1.0 - smoothstep(4000.0, 9000.0, camDist));
	}

	// optional FFT-like layer: the tileable detail normal texture (t108) at two extra scales, crossing directions
	float2 TexSlope(float2 pAbs, float t)
	{
		float amt = NormalTexAmt();
		if (amt <= 0.0)
			return 0;
		float2 uv1 = pAbs / 1700.0 + float2(0.010, 0.004) * t;
		float2 q = float2(0.8 * pAbs.x + 0.6 * pAbs.y, -0.6 * pAbs.x + 0.8 * pAbs.y);
		float2 uv2 = q / 610.0 + float2(-0.006, 0.013) * t;
		uv1 = ECParallaxUv(EIWaveHeightTex, uv1, 1700.0, ECParallax.z);
		uv2 = ECParallaxUv(EIWaveHeightTex, uv2, 610.0, ECParallax.z);
		float2 a = EIWaterNormalTex.Sample(Normals01Sampler, uv1) * 2.0 - 1.0;
		float2 b = EIWaterNormalTex.Sample(Normals01Sampler, uv2) * 2.0 - 1.0;
		float2 q2 = float2(-0.5 * pAbs.x + 0.87 * pAbs.y, -0.87 * pAbs.x - 0.5 * pAbs.y);
		float2 uv3 = q2 / 190.0 + float2(0.017, -0.011) * t;
		uv3 = ECParallaxUv(EIWaveHeightTex, uv3, 190.0, ECParallax.z);
		float2 c = EIWaterNormalTex.Sample(Normals01Sampler, uv3) * 2.0 - 1.0;  // v0.13.2 crisp fine cascade
		return (a * 0.5 + b * 0.35 + c * 0.25) * 0.4 * amt;
	}

	// ---- Stage 2: shore waves running toward the shore, depth proxy dist = h / slope.
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

	// (b) quiet-zone foam (shader approximation of a baked mask): slow flow + shallow-medium depth -> foam gathers
	// in eddies / calm bays. flow = flow-map vector (0 when the water has no flow map).
	float QuietFoam(float h, float2 flow, bool hasFlow, float2 pAbs, float t)
	{
		float amt = QuietAmt();
		if (amt <= 0.0 || !hasFlow)
			return 0;
		float fs = length(flow);
		float calm = 1.0 - smoothstep(0.08, 0.40, fs);
		float nearBand = smoothstep(2.0, 10.0, h) * (1.0 - smoothstep(60.0, 180.0, h));
		float2 drift = flow * t * 6.0;
		float n = ShoreWaves::VNoise((pAbs - drift) / 70.0) * 0.6 + ShoreWaves::VNoise((pAbs - drift) / 19.0) * 0.4;
		return saturate(calm * nearBand * smoothstep(0.55, 0.85, n) * amt * 0.8);
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

	// ---- Stage 3: projected caustics + underwater fog tint on the refracted bottom (above-water view)
	float CausticPattern(float2 p, float t)
	{
		float c = 0.0;
		[unroll] for (int i = 0; i < 2; ++i) {
			float sc = (i == 0) ? 1.0 : 1.9;
			float2 q = p * sc + float2(t * 0.21, -t * 0.17) * (1.0 + (float)i * 0.4);
			float v = sin(q.x + 1.3 * sin(q.y * 1.2 + t * 0.6)) + sin(q.y + 1.3 * sin(q.x * 1.4 - t * 0.5));
			c += pow(saturate(1.0 - abs(v) * 0.55), 7.0) * ((i == 0) ? 1.0 : 0.6);
		}
		return c;
	}

	float3 Bottom(float3 refrCol, float h, float3 surfAbs, float3 refrDir, float3 sunDir, float3 sunCol,
		float t, bool osCaustics, float3 deepCol)
	{
		// v0.13.2: refrDir can be degenerate (refraction sample above the water plane) -> fall back to straight down
		float3 rd = (all(isfinite(refrDir)) && dot(refrDir, refrDir) > 1e-4) ? refrDir : float3(0, 0, -1);
		float path = min(h / max(abs(rd.z), 0.35), 1500.0);
		float3 col = refrCol;
		refrDir = rd;
		float ca = CausticsAmt();
		if (ca > 0.0 && (!osCaustics || CausticsMode() > 0.5) && h > 0.5) {
			float2 bottomXY = surfAbs.xy + refrDir.xy * path;
			float sz = max(sunDir.z, 0.25);
			float2 proj = bottomXY + sunDir.xy / sz * h;                      // where the sun ray entered the surface
			float pat = CausticPattern(proj / 55.0, t);
			float depthFade = smoothstep(0.5, 8.0, h) * (1.0 - smoothstep(180.0, 900.0, h));
			float sun = saturate(sunDir.z * 2.0) * saturate(dot(sunCol, float3(0.3, 0.5, 0.2)));
			col *= 1.0 + pat * depthFade * sun * ca * 0.9;
		}
		float fa = FogTintAmt();
		if (fa > 0.0) {
			// v0.13.2 FIX (orange shallows): v0.11 added in-scatter as deepCol * (1 - tr) per channel. Red is absorbed
			// fastest, so (1 - tr) was largest in red and the "fog" painted the bottom orange whenever the deep colour
			// was not strongly blue (night / weather presets). In-scatter now uses ONE scalar weight and is tinted by
			// the water deep colour only; absorption alone shifts the bottom toward blue-green.
			float3 sigma = float3(0.0045, 0.0018, 0.0012) * fa;               // per unit: red fades first
			float3 tr = exp(-sigma * path);
			float inscat = 1.0 - dot(tr, float3(0.2, 0.4, 0.4));
			float3 dc = saturate(deepCol);
			col = col * tr + dc * inscat * 0.5;
		}
		return all(isfinite(col)) ? max(col, 0.0) : refrCol;
	}
}
#endif
