// Elemental Contact v0.13.2 Shaders/ShoreWaves/WetHair.hlsli, hair only.
// The waterline test is the ActorWet depth check this file called (strength 0.9, edges 6 and 3,
// menu default 0.9). Dividing by that strength cancels it, which is the EC result at the default.
// ShoreWaves, ActorSurface and the Elemental Contact menu DLL are not part of this package.
#ifndef __SHOREWAVES_WETHAIR_HLSLI__
#define __SHOREWAVES_WETHAIR_HLSLI__
namespace WetHair
{
	static const float ClumpBlend = 0.30;
	static const float ClumpOffsetUV = 0.004;
	static const float Darken = 0.70;
	static const float Saturate = 1.25;
	static const float TransmissionCut = 0.25;
	static const float WaterlineEdgeAbove = 6.0;
	static const float WaterlineEdgeBelow = 3.0;

	float Hash(float2 p)
	{
		p = frac(p * float2(443.897, 441.423));
		p += dot(p, p.yx + 19.19);
		return frac((p.x + p.y) * p.x);
	}

	float Noise(float2 p)
	{
		float2 i = floor(p), f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
	}

	float Waterline(float waterHeight, float pixelZ)
	{
		if (waterHeight < -1.0e8)
			return 0.0;
		return smoothstep(-WaterlineEdgeAbove, WaterlineEdgeBelow, waterHeight - pixelZ);
	}

	// waterHeight / pixelZ: camera-relative Z (Skyrim is Z-up); rain: hair/skin rain wetness 0..1
	float Wetness(float waterHeight, float pixelZ, float rain)
	{
		return saturate(max(Waterline(waterHeight, pixelZ), rain));
	}

	// strandDir: strand direction in UV space (from the hair flow map, else (0,1))
	float3 ApplyAlbedo(float3 baseColor, Texture2D<float4> tex, SamplerState samp, float2 uv, float2 strandDir, float wet)
	{
		if (wet <= 0.001)
			return baseColor;
		float2 along = normalize(strandDir + float2(1e-4, 0));
		float2 across = float2(-along.y, along.x);
		float2 q = float2(dot(uv, across) * 220.0, dot(uv, along) * 18.0);
		float n = Noise(q) * 2.0 - 1.0;
		float3 clumped = tex.SampleBias(samp, uv + across * n * ClumpOffsetUV * wet, 0).xyz;
		float3 raw = tex.SampleBias(samp, uv, 0).xyz;
		float ratio = dot(clumped, float3(0.3, 0.59, 0.11)) / max(dot(raw, float3(0.3, 0.59, 0.11)), 1e-3);
		float3 c = lerp(baseColor, baseColor * clamp(ratio, 0.5, 1.5), ClumpBlend * wet);
		float l = dot(c, float3(0.3, 0.59, 0.11));
		c = lerp(l.xxx, c, lerp(1.0, Saturate, wet));
		return max(c * lerp(1.0, Darken, wet), 0.0);
	}
}
#endif
