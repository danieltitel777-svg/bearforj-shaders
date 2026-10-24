// Elemental Contact v0.13.2 Shaders/ShoreWaves/HairSSS.hlsli, hair only.
// Transmission strength is the EC default EI_HairSSS (0.55). The back-light rim reads EI10,
// which stays off unless the Elemental Contact menu DLL writes it. That DLL is not included.
#ifndef __SHOREWAVES_HAIRSSS_HLSLI__
#define __SHOREWAVES_HAIRSSS_HLSLI__
namespace HairSSS
{
	static const float Strength = 0.55;
	static const float Forward = 6.0;
	static const float RimBoost = 0.65;
	static const float ShadowLeak = 0.45;
	static const float DefaultMenuStrength = 0.55;

	float3 Profile(float d)
	{
		float dd = -d * d;
		return float3(0.233, 0.455, 0.649) * exp(dd / 0.0064) + float3(0.1, 0.336, 0.344) * exp(dd / 0.0484) +
		       float3(0.118, 0.198, 0.0) * exp(dd / 0.187) + float3(0.113, 0.007, 0.007) * exp(dd / 0.567) +
		       float3(0.358, 0.004, 0.0) * exp(dd / 1.99) + float3(0.078, 0.0, 0.0) * exp(dd / 7.41);
	}

	float3 Transmit(float3 N, float3 V, float3 L, float3 lightColor, float detailedShadow, float softShadow, float3 albedo, float thin)
	{
		if (thin <= 0.01)
			return 0.0.xxx;
		float back = saturate(0.3 + dot(L, -N));
		float view = 0.25 + 0.75 * pow(saturate(dot(-L, V)), Forward * 0.5);
		float shadow = lerp(min(detailedShadow, softShadow), 1.0, ShadowLeak * thin);
		float d = lerp(1.6, 0.12, thin);
		float3 prof = Profile(d);
		float3 tint = saturate(albedo) * 0.7 + 0.3 * saturate(albedo) * float3(1.0, 0.8, 0.6);
		return prof * tint * lightColor * (back * view * shadow * thin * 2.0 * DefaultMenuStrength);
	}

	float3 BackRim(float3 N, float3 V, float3 L, float3 lightColor, float detailedShadow, float softShadow, float3 albedo, float thin, float tipThin)
	{
		// EI3.w is 0 without the Elemental Contact DLL, so EC itself returns no rim.
		const float rimAmt = 0.0;
		if (rimAmt <= 0.0)
			return 0.0.xxx;
		const float lobePow = 1.0;
		const float tipBoost = 0.0;
		const float rimWidth = 0.25;
		const float distortion = 0.3;
		float3 bentL = normalize(-L + N * distortion);
		float lobe = pow(saturate(dot(V, bentL)), lobePow);
		float nv = saturate(abs(dot(N, V)));
		float rim = pow(1.0 - nv, 1.0 / rimWidth) * saturate(0.25 + dot(-L, V) * 0.75);
		float thinAll = saturate(max(thin, tipThin));
		float tipW = saturate(thinAll * (1.0 + tipBoost * tipThin));
		float shadowV = lerp(min(detailedShadow, softShadow), 1.0, 0.35 * tipThin);
		float3 tint = saturate(albedo) * 0.75 + 0.25;
		return tint * lightColor * ((lobe * 1.2 + rim * 0.6) * tipW * shadowV * rimAmt);
	}
}
#endif
