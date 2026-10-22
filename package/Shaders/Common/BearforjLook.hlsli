#ifndef __BEARFORJ_LOOK_HLSLI__
#define __BEARFORJ_LOOK_HLSLI__

#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"

// Bearforj "Crimson" grade. Filmic toe + soft shoulder, warm highlights, cool shadows.
// `linearInput` is the space of `color`. The result is returned in that same space.
// Highlights above 1 (HDR display mapping) keep their luminance; the grade still tints them.
float3 BearforjCrimsonGrade(float3 lin)
{
	const float3 hdr = min(lin, 0.96) / (1.0 - min(lin, 0.96));
	const float w = SharedData::bearforjLook.Warmth;
	float y = Color::RGBToLuminance(hdr);
	const float yc = 0.18 * pow(max(y, 1e-6) / 0.18, 1.06);
	const float A = 0.22, B = 0.30, C = 0.10, D = 0.22, E = 0.012, F = 0.30;
	const float ex = 2.2;
	float2 v = float2(yc * ex, 8.0);
	float2 hc = ((v * (A * v + C * B) + D * E) / (v * (A * v + B) + D * F)) - E / F;
	float yo = saturate(hc.x / hc.y);
	float3 col = hdr * (yo / max(y, 1e-6));
	const float hi = smoothstep(0.55, 1.0, yo);
	col = lerp(col, yo.xxx, hi * 0.35);
	col = min(col, 1.0);
	const float mid = smoothstep(0.03, 0.25, yo) * (1.0 - hi);
	col = lerp(yo.xxx, col, 1.0 + 0.08 * mid);
	const float sh = 1.0 - smoothstep(0.02, 0.25, yo);
	const float hl = smoothstep(0.30, 0.85, yo);
	col *= lerp(1.0.xxx, float3(1.0 + 0.07 * w, 1.0 + 0.015 * w, 1.0 - 0.07 * w), hl);
	col *= lerp(1.0.xxx, float3(1.0 - 0.03 * w, 1.0 - 0.005 * w, 1.0 + 0.05 * w), sh);
	col *= yo / max(Color::RGBToLuminance(col), 1e-6);
	return lerp(lin, max(col, 0.0), SharedData::bearforjLook.TonemapStrength);
}

float3 ApplyBearforjCrimson(float3 color, bool linearInput)
{
	if (!(SharedData::bearforjLook.Enable && SharedData::bearforjLook.TonemapStrength > 0.0))
		return color;

	float3 lin = linearInput ? max(color, 0.0) : Color::GammaToLinearSafe(color);
	const float peak = max(lin.r, max(lin.g, lin.b));
	float3 graded = BearforjCrimsonGrade(peak > 1.0 ? saturate(lin) : lin);
	if (peak > 1.0) {
		const float y = Color::RGBToLuminance(lin);
		const float ys = max(Color::RGBToLuminance(saturate(lin)), 1e-6);
		graded *= y / ys;
	}
	return linearInput ? graded : Color::LinearToGammaSafe(graded);
}

#endif
