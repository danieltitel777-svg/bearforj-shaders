// BearForge: minimal ColorManagement shim for Physical Sky on Open's colour pipeline.
// Scene colour is linear when Linear Lighting is on, Skyrim gamma otherwise (runtime switch in Open).
#ifndef BF_COLOR_MANAGEMENT_HLSLI
#define BF_COLOR_MANAGEMENT_HLSLI
#include "Common/Color.hlsli"

namespace ColorManagement
{
	float SceneToLinear(float v) { return ENABLE_LL ? v : Color::SkyrimGammaToLinear(v); }
	float3 SceneToLinear(float3 v) { return ENABLE_LL ? v : Color::SkyrimGammaToLinear(v); }
	float LinearToScene(float v) { return ENABLE_LL ? v : Color::LinearToSkyrimGamma(v); }
	float3 LinearToScene(float3 v) { return ENABLE_LL ? v : Color::LinearToSkyrimGamma(v); }

	// Linear-sRGB radiance (LUTs, physical sky) -> scene domain: working gamut with LL, Skyrim gamma without.
	float3 LinearSrgbToScene(float3 v) { return ENABLE_LL ? Color::GamutTransform(v) : Color::LinearToSkyrimGamma(max(v, 0.0)); }

	namespace SceneColor
	{
		float ScaleByLinear(float c, float s) { return ENABLE_LL ? c * s : abs(c) * Color::LinearToSkyrimGamma(s); }
		float3 ScaleByLinear(float3 c, float3 s) { return ENABLE_LL ? c * s : abs(c) * Color::LinearToSkyrimGamma(s); }
		float3 ScaleByLinear(float3 c, float s) { return ScaleByLinear(c, float3(s, s, s)); }
		float3 ScaleAndAddLinear(float3 c, float3 s, float3 o) { return LinearToScene(SceneToLinear(c) * s + o); }
		float3 LerpInLinear(float3 a, float3 b, float w) { return LinearToScene(lerp(SceneToLinear(a), SceneToLinear(b), w)); }
	}
}
#endif
