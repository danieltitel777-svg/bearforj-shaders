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

	struct WaveSample
	{
		float3 offset;
		float2 slope;
	};

	WaveSample Eval(float2 absXY, float2 camRelXY, bool normalFade)
	{
		WaveSample s = (WaveSample)0;
		if (SharedData::WaveScale <= 0.0 || SharedData::WaveAmplitude <= 0.0)
			return s;
		float dist = length(camRelXY);
		float fadeStart = normalFade ? kNormalFadeStart : kFadeStart;
		float fadeEnd = normalFade ? kNormalFadeEnd : kFadeEnd;
		float fade = 1.0 - smoothstep(fadeStart, fadeEnd, dist);
		if (fade <= 0.0)
			return s;
		float wind = clamp(SharedData::WindFieldTransitionData.z, 0.0, 1.5);
		float windAmp = 0.35 + wind * wind;
		float angle = SharedData::WindFieldTransitionData.w;
		[unroll] for (int i = 0; i < 9; i++)
		{
			float lambda = SharedData::WaveScale * 420.0 * pow(0.70, (float)i);
			float k = kTwoPi / lambda;
			float omega = sqrt(kGravity * k);
			float slopeA = 0.095 - 0.006 * (float)i;
			float amp = SharedData::WaveAmplitude * SharedData::WaveScale * windAmp * (slopeA / 0.095) * pow(0.62, (float)i);
			float ang = angle + kSpreads[i];
			float dirX = cos(ang);
			float dirY = sin(ang);
			float phase = k * (dirX * absXY.x + dirY * absXY.y) - omega * SharedData::Timer;
			float sn = sin(phase);
			float cs = cos(phase);
			float q = min(SharedData::WaveChop, 0.75 / (k * amp + 1e-4));
			s.offset.z += amp * sn;
			s.offset.xy += float2(dirX, dirY) * (q * amp * cs);
			s.slope += float2(dirX, dirY) * (k * amp * cs);
		}
		s.offset *= fade;
		s.slope *= fade;
		return s;
	}

	float3 Offset(float3 plane, uint eye, bool normalFade)
	{
#if defined(FLOWMAP)
		return 0.0.xxx;
#else
		if (SharedData::WaveScale <= 0.0)
			return 0.0.xxx;
		float2 absXY = plane.xy + FrameBuffer::CameraPosAdjust[eye].xy;
		WaveSample s = Eval(absXY, plane.xy, normalFade);
		float damp = normalFade ? 1.0 : ShoreDampGPU(plane, eye);
		return s.offset * damp;
#endif
	}

	float2 Slope(float3 plane, uint eye)
	{
#if defined(FLOWMAP)
		return 0.0.xx;
#else
		if (SharedData::WaveScale <= 0.0)
			return 0.0.xx;
		float2 absXY = plane.xy + FrameBuffer::CameraPosAdjust[eye].xy;
		WaveSample s = Eval(absXY, plane.xy, true);
		return s.slope * ShoreDampGPU(plane, eye);
#endif
	}
}
#endif
