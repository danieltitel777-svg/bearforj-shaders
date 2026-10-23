#ifndef __WATER_WAVES_HLSLI__
#define __WATER_WAVES_HLSLI__

// Mirror of src/Features/WaterWavesMath.h. Same 9 waves, same fades, same shore deadzone.
// SharedData::WaveScale is 0 when the feature is off. Wind speed and angle ride in
// WindFieldTransitionData.z/w (those components are otherwise unused).
// ShoreGap is the land Z grid filled by WaterWaves::UpdateShoreGrid. Shore damp reads that
// grid, not the depth buffer, so the shoreline does not move when the camera does.

Texture2D<float> ShoreGap : register(t59);

namespace WaterWaves
{
	static const float kTwoPi = 6.2831853;
	static const float kGravity = 687.0;
	static const float kFadeStart = 4000.0;
	static const float kFadeEnd = 7000.0;
	static const float kNormalFadeStart = 6000.0;
	static const float kNormalFadeEnd = 12000.0;
	static const float kShoreDeadzone = 4.0;
	static const float kSpreads[9] = { 0.0, 0.6, -0.6, 1.5, -1.5, 2.6, -2.6, 0.9, -1.2 };

	// Same bilinear as WaterWaveMath::SampleLandZ. -1e8 means "no land" (open water).
	float SampleLandZ(float2 absXY)
	{
		if (SharedData::ShoreCellSize <= 1.0)
			return -1.0e8;
		float2 p = (absXY - float2(SharedData::ShoreOriginX, SharedData::ShoreOriginY)) / SharedData::ShoreCellSize;
		float n = SharedData::ShoreCellCount;
		if (any(p < 0.0) || any(p > n - 1.0))
			return -1.0e8;
		float2 i0f = floor(p);
		float2 f = p - i0f;
		float2 i1f = min(i0f + 1.0, n - 1.0);
		int2 i0 = int2(i0f);
		int2 i1 = int2(i1f);
		float g00 = ShoreGap.Load(int3(i0.x, i0.y, 0));
		float g10 = ShoreGap.Load(int3(i1.x, i0.y, 0));
		float g01 = ShoreGap.Load(int3(i0.x, i1.y, 0));
		float g11 = ShoreGap.Load(int3(i1.x, i1.y, 0));
		return lerp(lerp(g00, g10, f.x), lerp(g01, g11, f.x), f.y);
	}

	// 0 when the land is within 4 units of this water plane. Independent of the camera.
	float ShoreDampGPU(float3 plane, uint eye)
	{
		float2 absXY = plane.xy + FrameBuffer::CameraPosAdjust[eye].xy;
		float landZ = SampleLandZ(absXY);
		float waterZ = plane.z + FrameBuffer::CameraPosAdjust[eye].z;
		float gap = landZ < -1.0e7 ? 1.0e6 : (waterZ - landZ);
		float shore = max(SharedData::WaveShore, 1.0);
		float t = saturate((gap - kShoreDeadzone) / shore);
		return t * t * (3.0 - 2.0 * t);
	}

	struct WaveResponse
	{
		float amplitude;
		float chop;
		float length;
		float spread;
		float storm;
	};

	WaveResponse LerpResponse(WaveResponse a, WaveResponse b, float t)
	{
		t = saturate(t);
		t = t * t * (3.0 - 2.0 * t);
		WaveResponse o;
		o.amplitude = lerp(a.amplitude, b.amplitude, t);
		o.chop = lerp(a.chop, b.chop, t);
		o.length = lerp(a.length, b.length, t);
		o.spread = lerp(a.spread, b.spread, t);
		o.storm = lerp(a.storm, b.storm, t);
		return o;
	}

	// Same anchors as WaterWaveMath::WaveResponseAt.
	WaveResponse WaveResponseAt(float wind)
	{
		wind = clamp(wind, 0.0, 1.5);
		WaveResponse calm = { 0.08, 0.22, 0.40, 1.20, 0.0 };
		WaveResponse breeze = { 0.72, 0.80, 1.00, 0.55, 0.08 };
		WaveResponse storm = { 1.55, 1.20, 1.30, 0.32, 1.0 };
		if (wind <= 0.42)
			return LerpResponse(calm, breeze, wind / 0.42);
		return LerpResponse(breeze, storm, (wind - 0.42) / (1.05 - 0.42));
	}

	struct WaveSample
	{
		float3 offset;
		float2 slope;
		float jacobian;
	};

	WaveSample Eval(float2 absXY, float2 camRelXY, bool normalFade)
	{
		WaveSample s = (WaveSample)0;
		s.jacobian = 1.0;
		if (SharedData::WaveScale <= 0.0 || SharedData::WaveAmplitude <= 0.0)
			return s;
		float dist = length(camRelXY);
		float fadeStart = normalFade ? kNormalFadeStart : kFadeStart;
		float fadeEnd = normalFade ? kNormalFadeEnd : kFadeEnd;
		float fade = 1.0 - smoothstep(fadeStart, fadeEnd, dist);
		if (fade <= 0.0)
			return s;
		WaveResponse weather = WaveResponseAt(SharedData::WindFieldTransitionData.z);
		float angle = SharedData::WindFieldTransitionData.w;
		float jxx = 1.0;
		float jyy = 1.0;
		float jxy = 0.0;
		[unroll] for (int i = 0; i < 9; i++)
		{
			float lambda = SharedData::WaveScale * weather.length * 420.0 * pow(0.70, (float)i);
			float k = kTwoPi / lambda;
			float omega = sqrt(kGravity * k);
			float slopeA = 0.095 - 0.006 * (float)i;
			float amp = SharedData::WaveAmplitude * SharedData::WaveScale * weather.amplitude * (slopeA / 0.095) * pow(0.62, (float)i);
			float ang = angle + kSpreads[i] * weather.spread;
			float dirX = cos(ang);
			float dirY = sin(ang);
			float phase = k * (dirX * absXY.x + dirY * absXY.y) - omega * SharedData::Timer;
			float sn = sin(phase);
			float cs = cos(phase);
			float q = min(SharedData::WaveChop * weather.chop, 0.85 / (k * amp + 1e-4));
			float wa = k * amp;
			s.offset.z += amp * sn;
			s.offset.xy += float2(dirX, dirY) * (q * amp * cs);
			s.slope += float2(dirX, dirY) * (wa * cs);
			jxx -= q * dirX * dirX * wa * sn;
			jyy -= q * dirY * dirY * wa * sn;
			jxy -= q * dirX * dirY * wa * sn;
		}
		s.offset *= fade;
		s.slope *= fade;
		s.jacobian = lerp(1.0, jxx * jyy - jxy * jxy, fade);
		return s;
	}

	struct Displaced
	{
		float3 offset;
		float2 slope;
		float jacobian;
	};

	Displaced Displace(float3 plane, uint eye, bool normalFade)
	{
		Displaced d = (Displaced)0;
		d.jacobian = 1.0;
#if !defined(FLOWMAP)
		if (SharedData::WaveScale <= 0.0)
			return d;
		float2 absXY = plane.xy + FrameBuffer::CameraPosAdjust[eye].xy;
		WaveSample s = Eval(absXY, plane.xy, normalFade);
		float damp = ShoreDampGPU(plane, eye);
		d.offset = s.offset * damp;
		d.slope = s.slope * damp;
		d.jacobian = lerp(1.0, s.jacobian, damp);
#endif
		return d;
	}

	// Patchy foam mask. Hash period is far longer than a texture tile.
	float Hash21(float2 p)
	{
		float2 q = frac(p * float2(0.1031, 0.11369));
		q += dot(q, q.yx + 19.19);
		return frac(q.x * q.y);
	}

	float FoamNoise(float2 xz, float time)
	{
		float2 p = xz * 0.017 + time * float2(0.15, 0.07);
		float2 i = floor(p);
		float2 f = frac(p);
		float2 u = f * f * (3.0 - 2.0 * f);
		float a = Hash21(i);
		float b = Hash21(i + float2(1, 0));
		float c = Hash21(i + float2(0, 1));
		float d = Hash21(i + float2(1, 1));
		float n = lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
		float2 p2 = xz * 0.041 - time * float2(0.11, 0.19);
		float2 i2 = floor(p2);
		float2 f2 = frac(p2);
		float2 u2 = f2 * f2 * (3.0 - 2.0 * f2);
		float n2 = lerp(lerp(Hash21(i2 + 3.1), Hash21(i2 + float2(4.1, 3.1)), u2.x), lerp(Hash21(i2 + float2(3.1, 4.1)), Hash21(i2 + 4.1), u2.x), u2.y);
		return saturate(n * 0.65 + n2 * 0.35);
	}

	// Two incommensurate sines. Their beat is long enough that the ripple does not show a tile.
	float2 RippleGradient(float2 xz, float time, float2 wind)
	{
		float2 p = xz + wind * time * 18.0;
		float a = dot(p, float2(0.073, 0.049)) + time * 1.1;
		float b = dot(p, float2(-0.041, 0.091)) - time * 0.8;
		return float2(0.073, 0.049) * cos(a) * 0.55 + float2(-0.041, 0.091) * cos(b) * 0.35;
	}

	float3 Offset(float3 plane, uint eye, bool normalFade)
	{
		return Displace(plane, eye, normalFade).offset;
	}

	float2 Slope(float3 plane, uint eye)
	{
		return Displace(plane, eye, true).slope;
	}

	// Mirrors WaterWaveMath::WaveExtinction / WaveScatter / WaveAbsorb / WaveOpacity.
	float3 Extinction(float murk)
	{
		murk = saturate(murk);
		float3 clearSigma = float3(0.0085, 0.0042, 0.0016);
		float3 murkSigma = float3(0.0160, 0.0145, 0.0125);
		return lerp(clearSigma, murkSigma, murk);
	}

	float3 Scatter(float vertical, float murk, float3 shallowC, float3 deepC)
	{
		float depthT = saturate(vertical / 400.0);
		depthT = depthT * depthT * (3.0 - 2.0 * depthT);
		murk = saturate(murk);
		float3 clearC = lerp(float3(0.05, 0.28, 0.26), float3(0.015, 0.05, 0.07), depthT);
		float3 mudC = lerp(float3(0.11, 0.12, 0.075), float3(0.045, 0.050, 0.032), depthT);
		float3 scatter = lerp(clearC, mudC, murk);
		float3 authored = lerp(shallowC, deepC, depthT);
		float lum = max(dot(authored, float3(0.25, 0.50, 0.25)), 1e-3);
		float3 rel = 1.0 + (authored / lum - 1.0) * 0.35;
		return scatter * rel;
	}

	float3 Absorb(float3 scene, float path, float vertical, float murk, float3 shallowC, float3 deepC)
	{
		path = max(path, 0.0);
		float3 sigma = Extinction(murk);
		float3 transmit = exp(-sigma * path);
		float3 scatter = Scatter(vertical, murk, shallowC, deepC);
		return scene * transmit + scatter * (1.0 - transmit);
	}

	float Opacity(float path, float murk)
	{
		path = max(path, 0.0);
		float3 sigma = Extinction(murk);
		float3 transmit = exp(-sigma * path);
		float see = dot(transmit, float3(0.30, 0.40, 0.30));
		return saturate(1.0 - see);
	}
}
#endif
