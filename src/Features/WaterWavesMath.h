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
		// 1 on flat water. Drops where a crest folds (Tessendorf Jacobian).
		float jacobian = 1.0f;
	};

	// Weather scales the nine waves. Calm is a low ripple, a breeze runs along the wind,
	// a storm is a longer steeper swell. Smooth() on each segment so a weather blend does not pop.
	struct WaveResponse
	{
		float amplitude = 1.0f;
		float chop = 1.0f;
		float length = 1.0f;
		float spread = 1.0f;
		float storm = 0.0f;
	};

	inline float Smooth(float a_t);

	inline WaveResponse LerpResponse(const WaveResponse& a_a, const WaveResponse& a_b, float a_t)
	{
		const float t = Smooth(a_t);
		WaveResponse out;
		out.amplitude = a_a.amplitude + (a_b.amplitude - a_a.amplitude) * t;
		out.chop = a_a.chop + (a_b.chop - a_a.chop) * t;
		out.length = a_a.length + (a_b.length - a_a.length) * t;
		out.spread = a_a.spread + (a_b.spread - a_a.spread) * t;
		out.storm = a_a.storm + (a_b.storm - a_a.storm) * t;
		return out;
	}

	inline WaveResponse WaveResponseAt(float a_wind)
	{
		const float wind = std::clamp(a_wind, 0.0f, 1.5f);
		const WaveResponse calm{ 0.08f, 0.22f, 0.40f, 1.20f, 0.0f };
		const WaveResponse breeze{ 0.72f, 0.80f, 1.00f, 0.55f, 0.08f };
		const WaveResponse storm{ 1.55f, 1.20f, 1.30f, 0.32f, 1.0f };
		constexpr float kBreeze = 0.42f;
		constexpr float kStorm = 1.05f;
		if (wind <= kBreeze)
			return LerpResponse(calm, breeze, wind / kBreeze);
		return LerpResponse(breeze, storm, (wind - kBreeze) / (kStorm - kBreeze));
	}

	struct WaveRgb
	{
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
	};

	// Per-unit Beer-Lambert extinction. Clear water eats red first (turquoise shallows).
	// Murk raises every channel so the water goes brown-grey instead of a flat blue.
	inline WaveRgb WaveExtinction(float a_murk)
	{
		const float murk = std::clamp(a_murk, 0.0f, 1.0f);
		const WaveRgb clearSigma{ 0.0085f, 0.0042f, 0.0016f };
		const WaveRgb murkSigma{ 0.0160f, 0.0145f, 0.0125f };
		return WaveRgb{
			clearSigma.r + (murkSigma.r - clearSigma.r) * murk,
			clearSigma.g + (murkSigma.g - clearSigma.g) * murk,
			clearSigma.b + (murkSigma.b - clearSigma.b) * murk
		};
	}

	inline WaveRgb WaveScatter(float a_vertical, float a_murk, WaveRgb a_shallow, WaveRgb a_deep)
	{
		const float depthT = Smooth(std::clamp(a_vertical / 400.0f, 0.0f, 1.0f));
		const float murk = std::clamp(a_murk, 0.0f, 1.0f);
		const WaveRgb clearShallow{ 0.05f, 0.28f, 0.26f };
		const WaveRgb clearDeep{ 0.015f, 0.05f, 0.07f };
		const WaveRgb mudShallow{ 0.11f, 0.12f, 0.075f };
		const WaveRgb mudDeep{ 0.045f, 0.050f, 0.032f };
		WaveRgb clear{
			clearShallow.r + (clearDeep.r - clearShallow.r) * depthT,
			clearShallow.g + (clearDeep.g - clearShallow.g) * depthT,
			clearShallow.b + (clearDeep.b - clearShallow.b) * depthT
		};
		WaveRgb mud{
			mudShallow.r + (mudDeep.r - mudShallow.r) * depthT,
			mudShallow.g + (mudDeep.g - mudShallow.g) * depthT,
			mudShallow.b + (mudDeep.b - mudShallow.b) * depthT
		};
		WaveRgb scatter{
			clear.r + (mud.r - clear.r) * murk,
			clear.g + (mud.g - clear.g) * murk,
			clear.b + (mud.b - clear.b) * murk
		};
		const WaveRgb authored{
			a_shallow.r + (a_deep.r - a_shallow.r) * depthT,
			a_shallow.g + (a_deep.g - a_shallow.g) * depthT,
			a_shallow.b + (a_deep.b - a_shallow.b) * depthT
		};
		const float lum = std::max(authored.r * 0.25f + authored.g * 0.50f + authored.b * 0.25f, 1.0e-3f);
		const float relR = 1.0f + (authored.r / lum - 1.0f) * 0.35f;
		const float relG = 1.0f + (authored.g / lum - 1.0f) * 0.35f;
		const float relB = 1.0f + (authored.b / lum - 1.0f) * 0.35f;
		scatter.r *= relR;
		scatter.g *= relG;
		scatter.b *= relB;
		return scatter;
	}

	// Scene colour times transmission, plus a dark scatter. Path 0 returns the scene unchanged.
	inline WaveRgb WaveAbsorb(WaveRgb a_scene, float a_path, float a_vertical, float a_murk, WaveRgb a_shallow, WaveRgb a_deep)
	{
		const float path = std::max(a_path, 0.0f);
		const WaveRgb sigma = WaveExtinction(a_murk);
		const float tr = std::exp(-sigma.r * path);
		const float tg = std::exp(-sigma.g * path);
		const float tb = std::exp(-sigma.b * path);
		const WaveRgb scatter = WaveScatter(a_vertical, a_murk, a_shallow, a_deep);
		return WaveRgb{
			a_scene.r * tr + scatter.r * (1.0f - tr),
			a_scene.g * tg + scatter.g * (1.0f - tg),
			a_scene.b * tb + scatter.b * (1.0f - tb)
		};
	}

	inline float WaveOpacity(float a_path, float a_murk)
	{
		const float path = std::max(a_path, 0.0f);
		const WaveRgb sigma = WaveExtinction(a_murk);
		const float see = 0.30f * std::exp(-sigma.r * path) + 0.40f * std::exp(-sigma.g * path) + 0.30f * std::exp(-sigma.b * path);
		return std::clamp(1.0f - see, 0.0f, 1.0f);
	}

	inline float Smooth(float a_t)
	{
		a_t = std::clamp(a_t, 0.0f, 1.0f);
		return a_t * a_t * (3.0f - 2.0f * a_t);
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
		const WaveResponse weather = WaveResponseAt(a_params.windSpeed);
		float jxx = 1.0f;
		float jyy = 1.0f;
		float jxy = 0.0f;
		for (int i = 0; i < kWaveCount; ++i) {
			const float fi = static_cast<float>(i);
			const float lambda = a_params.scale * weather.length * 420.0f * std::pow(0.70f, fi);
			const float k = kTwoPi / lambda;
			const float omega = std::sqrt(kGravity * k);
			const float slopeA = 0.095f - 0.006f * fi;
			const float amp = a_params.amplitude * a_params.scale * weather.amplitude * (slopeA / 0.095f) * std::pow(0.62f, fi);
			const float ang = a_params.windAngle + kSpreads[i] * weather.spread;
			const float dirX = std::cos(ang);
			const float dirY = std::sin(ang);
			const float phase = k * (dirX * a_absX + dirY * a_absY) - omega * a_params.time;
			const float sn = std::sin(phase);
			const float cs = std::cos(phase);
			const float q = std::min(a_params.chop * weather.chop, 0.85f / (k * amp + 1.0e-4f));
			const float wa = k * amp;
			sample.height += amp * sn;
			sample.dx += dirX * q * amp * cs;
			sample.dy += dirY * q * amp * cs;
			sample.slopeX += dirX * wa * cs;
			sample.slopeY += dirY * wa * cs;
			jxx -= q * dirX * dirX * wa * sn;
			jyy -= q * dirY * dirY * wa * sn;
			jxy -= q * dirX * dirY * wa * sn;
		}
		sample.height *= fade;
		sample.dx *= fade;
		sample.dy *= fade;
		sample.slopeX *= fade;
		sample.slopeY *= fade;
		sample.jacobian = 1.0f + ((jxx * jyy - jxy * jxy) - 1.0f) * fade;
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
