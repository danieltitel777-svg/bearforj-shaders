#pragma once

#include <algorithm>
#include <cmath>

// Gerstner sum shared by the CPU (camera test, actor wetness) and mirrored in
// features/Water Waves/Shaders/WaterWaves/WaterWaves.hlsli. Keep the two in step.
namespace WaterWaveMath
{
	inline constexpr int kWaveCount = 9;
	inline constexpr float kTwoPi = 6.2831853f;
	inline constexpr float kGravity = 687.0f;
	inline constexpr float kFadeStart = 4000.0f;
	inline constexpr float kFadeEnd = 7000.0f;
	inline constexpr float kNormalFadeStart = 6000.0f;
	inline constexpr float kNormalFadeEnd = 12000.0f;
	inline constexpr float kShoreDeadzone = 4.0f;
	inline constexpr float kSpreads[kWaveCount] = { 0.0f, 0.6f, -0.6f, 1.5f, -1.5f, 2.6f, -2.6f, 0.9f, -1.2f };

	struct Params
	{
		float time = 0.0f;
		float amplitude = 22.0f;
		float chop = 0.6f;
		float windSpeed = 0.0f;
		float windAngle = 0.0f;
		float scale = 1.0f;
		bool enabled = false;
		bool flowmap = false;
	};

	struct Sample
	{
		float height = 0.0f;
		float dx = 0.0f;
		float dy = 0.0f;
		float slopeX = 0.0f;
		float slopeY = 0.0f;
	};

	inline float Smooth(float a_t)
	{
		a_t = std::clamp(a_t, 0.0f, 1.0f);
		return a_t * a_t * (3.0f - 2.0f * a_t);
	}

	// Sky wind is already blended, including by Dynamic Wind Framework when that mod is installed.
	// Calm (near 0) is short ripples. A breeze is the base wave, aimed along the wind. A storm
	// (near 1.2) is a longer, taller wave. Every step is a smoothstep, so weather fades do not pop.
	inline void WindShape(float a_wind, float& a_size, float& a_amp, float& a_storm)
	{
		const float wind = std::clamp(a_wind, 0.0f, 1.5f);
		const float breeze = Smooth((wind - 0.05f) / 0.50f);
		a_storm = Smooth((wind - 0.55f) / 0.65f);
		const float midSize = 0.18f + (1.0f - 0.18f) * breeze;
		const float midAmp = 0.10f + (0.85f - 0.10f) * breeze;
		a_size = midSize + (2.05f - midSize) * a_storm;
		a_amp = midAmp + (1.45f - midAmp) * a_storm;
	}

	// 0 on dry ground and in the 4-unit contact band, 1 once the water is ShoreLength deep.
	inline float ShoreDamp(float a_waterMinusLand, float a_shoreLength)
	{
		const float length = std::max(a_shoreLength, 1.0f);
		return Smooth((a_waterMinusLand - kShoreDeadzone) / length);
	}

	inline float DistanceFade(float a_distance, bool a_normalFade)
	{
		const float start = a_normalFade ? kNormalFadeStart : kFadeStart;
		const float end = a_normalFade ? kNormalFadeEnd : kFadeEnd;
		if (end <= start)
			return a_distance >= end ? 0.0f : 1.0f;
		return 1.0f - Smooth((a_distance - start) / (end - start));
	}

	inline Sample Evaluate(float a_absX, float a_absY, float a_camRelX, float a_camRelY, const Params& a_params, bool a_normalFade)
	{
		Sample sample{};
		if (!a_params.enabled || a_params.flowmap || a_params.amplitude <= 0.0f || a_params.scale <= 0.0f)
			return sample;
		const float distance = std::sqrt(a_camRelX * a_camRelX + a_camRelY * a_camRelY);
		const float fade = DistanceFade(distance, a_normalFade);
		if (fade <= 0.0f)
			return sample;
		float windSize = 1.0f;
		float windAmp = 1.0f;
		float storm = 0.0f;
		WindShape(a_params.windSpeed, windSize, windAmp, storm);
		for (int i = 0; i < kWaveCount; ++i) {
			const float fi = static_cast<float>(i);
			const float lambda = a_params.scale * 420.0f * windSize * std::pow(0.70f, fi);
			const float k = kTwoPi / lambda;
			const float omega = std::sqrt(kGravity * k);
			const float slopeA = 0.095f - 0.006f * fi;
			const float amp = a_params.amplitude * a_params.scale * windAmp * (slopeA / 0.095f) * std::pow(0.62f, fi);
			const float ang = a_params.windAngle + kSpreads[i];
			const float dirX = std::cos(ang);
			const float dirY = std::sin(ang);
			const float phase = k * (dirX * a_absX + dirY * a_absY) - omega * a_params.time;
			const float sn = std::sin(phase);
			const float cs = std::cos(phase);
			const float q = std::min(a_params.chop, 0.75f / (k * amp + 1.0e-4f));
			sample.height += amp * sn;
			sample.dx += dirX * q * amp * cs;
			sample.dy += dirY * q * amp * cs;
			sample.slopeX += dirX * k * amp * cs;
			sample.slopeY += dirY * k * amp * cs;
		}
		sample.height *= fade;
		sample.dx *= fade;
		sample.dy *= fade;
		sample.slopeX *= fade;
		sample.slopeY *= fade;
		return sample;
	}

	// Bilinear land Z. Matches WaterWaves::SampleLandZ in WaterWaves.hlsli.
	// Returns -1e8 when the grid is missing or the point is outside it (open water).
	inline float SampleLandZ(float a_absX, float a_absY, float a_originX, float a_originY, float a_cell, int a_cells, const float* a_land)
	{
		if (!a_land || a_cell <= 1.0f || a_cells <= 1)
			return -1.0e8f;
		const float px = (a_absX - a_originX) / a_cell;
		const float py = (a_absY - a_originY) / a_cell;
		const float n = static_cast<float>(a_cells);
		if (px < 0.0f || py < 0.0f || px > n - 1.0f || py > n - 1.0f)
			return -1.0e8f;
		const float x0f = std::floor(px);
		const float y0f = std::floor(py);
		const float fx = px - x0f;
		const float fy = py - y0f;
		const int x0 = static_cast<int>(x0f);
		const int y0 = static_cast<int>(y0f);
		const int x1 = std::min(x0 + 1, a_cells - 1);
		const int y1 = std::min(y0 + 1, a_cells - 1);
		const float g00 = a_land[y0 * a_cells + x0];
		const float g10 = a_land[y0 * a_cells + x1];
		const float g01 = a_land[y1 * a_cells + x0];
		const float g11 = a_land[y1 * a_cells + x1];
		const float g0 = g00 + (g10 - g00) * fx;
		const float g1 = g01 + (g11 - g01) * fx;
		return g0 + (g1 - g0) * fy;
	}
}
