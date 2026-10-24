// Actor snow coverage from Elemental Contact v0.13.2 EICold::Snow and EICold::FeetCoat only.
// WorldSnow, water, SurfaceFlow, and the rest of that shader are not used.
// Albedo, normal, and roughness come from the Snow Cover textures (t38-t40).
#ifndef ACTOR_SNOW_HLSLI
#define ACTOR_SNOW_HLSLI

#if defined(PSHADER) && !defined(TREE_LOD_WIND_VS)

// Pixel-shader b13. Tree LOD Wind owns b13 on the vertex shader only, and that
// permutation defines TREE_LOD_WIND_VS, so this buffer is not declared there.
cbuffer ActorSnowCB : register(b13)
{
	float ActorSnowCover;      // ColdState.y, 0..1
	float ActorSnowAmount;     // EI7.y menu amount
	float ActorCoatSnow;       // ActorGrimeCoat.x
	float ActorCoatMud;        // ActorGrimeCoat.y
	float ActorCoatHeight;     // ActorGrimeActor.y
	float ActorSnowSlide;      // ActorGrimeCoat2.z
	float ActorSnowScale;      // EI_Snow
	float ActorMudScale;       // EI_Mud
	float ActorSnowEnabled;    // ActorGrimeParams.x
	float ActorSnowStrength;   // ActorGrimeParams.w
	float ActorSnowPad0;
	float ActorSnowPad1;
};

#if !defined(SNOW_COVER)
Texture2D<float4> ActorSnowAlbedo : register(t38);
Texture2D<float3> ActorSnowNormal : register(t39);
Texture2D<float4> ActorSnowRmaos : register(t40);
#endif

namespace ActorSnow
{
	float Hash(float3 p)
	{
		p = frac(p * 0.3183099 + 0.1);
		p *= 17.0;
		return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
	}

	float Noise(float3 p)
	{
		float3 i = floor(p), f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(lerp(Hash(i), Hash(i + float3(1, 0, 0)), f.x), lerp(Hash(i + float3(0, 1, 0)), Hash(i + float3(1, 1, 0)), f.x), f.y),
			lerp(lerp(Hash(i + float3(0, 0, 1)), Hash(i + float3(1, 0, 1)), f.x), lerp(Hash(i + float3(0, 1, 1)), Hash(i + float3(1, 1, 1)), f.x), f.y), f.z);
	}

	float Detail(float3 posModel)
	{
		return Noise(posModel * 0.07) * 0.5 + Noise(posModel * 0.31 + 5.3) * 0.3 + Noise(posModel * 1.3 + 11.0) * 0.2;
	}

	// EICold::Snow. Model-space noise, world up. samp is unused in v0.13.2.
	float Snow(float3 posModel, float3 worldNormal)
	{
		const float s = saturate(ActorSnowCover * ActorSnowAmount);
		if (s <= 0.001)
			return 0.0;
		float up = saturate(normalize(worldNormal).z);
		float n = Detail(posModel);
		float cut = lerp(0.95, 0.30, s);
		float cover = smoothstep(cut - 0.12, cut + 0.12, up + (n - 0.5) * 0.35);
		return saturate(cover * lerp(0.45, 0.72, s) * smoothstep(0.0, 0.25, s));
	}

	// EICold::FeetCoat. Snow in x, mud in y. Skinned meshes only.
	float2 FeetCoat(float3 posModel, float3 worldNormal)
	{
		float2 r = 0;
#if defined(SKINNED)
		if (ActorSnowEnabled < 0.5)
			return r;
		float top = max(ActorCoatHeight, 1.0);
		float n = Detail(posModel * 1.6);
		float h = posModel.z;
		float band = 1.0 - smoothstep(top * (0.55 + 0.3 * n), top * (1.0 + 0.25 * n), h);
		float snow = saturate(ActorCoatSnow * ActorSnowScale);
		float mud = saturate(ActorCoatMud * ActorMudScale);
		float up = saturate(normalize(worldNormal).z * 0.5 + 0.5);
		float slide = ActorSnowSlide * (0.6 + 0.8 * n) * saturate(h / top + 0.3);
		float thS = 1.0 - snow * (1.0 - slide);
		float thM = 1.0 - mud * (1.0 - slide * 0.6);
		r.x = smoothstep(thS - 0.15, thS + 0.15, n * 0.75 + up * 0.25) * band * 0.85;
		r.y = smoothstep(thM - 0.15, thM + 0.15, n * 0.8 + 0.2) * band * 0.9;
#endif
		return r;
	}

	float3 SampleAlbedo(Texture2D<float4> tex, float3 pos, float3 weights)
	{
		return tex.Sample(SampColorSampler, pos.yz * 0.05).rgb * weights.x +
		       tex.Sample(SampColorSampler, pos.xz * 0.05).rgb * weights.y +
		       tex.Sample(SampColorSampler, pos.xy * 0.05).rgb * weights.z;
	}

	float4 SampleRmaos(Texture2D<float4> tex, float3 pos, float3 weights)
	{
		return tex.Sample(SampColorSampler, pos.yz * 0.05) * weights.x +
		       tex.Sample(SampColorSampler, pos.xz * 0.05) * weights.y +
		       tex.Sample(SampColorSampler, pos.xy * 0.05) * weights.z;
	}

	float3 SampleNormal(Texture2D<float3> tex, float3 pos, float3 weights)
	{
		return tex.Sample(SampNormalSampler, pos.yz * 0.05).rgb * weights.x +
		       tex.Sample(SampNormalSampler, pos.xz * 0.05).rgb * weights.y +
		       tex.Sample(SampNormalSampler, pos.xy * 0.05).rgb * weights.z;
	}

	void Apply(inout MaterialProperties material, inout float3 worldNormal, float3 vertexNormal, float3 posModel, float3x3 tbn)
	{
		if (ActorSnowAmount <= 0.0 && ActorSnowEnabled < 0.5)
			return;

		float snow = Snow(posModel, vertexNormal);
		float2 feet = FeetCoat(posModel, vertexNormal);
		float mud = feet.y * ActorSnowStrength;
		if (mud > 0.001) {
			float grit = Detail(posModel * 3.0);
			material.BaseColor = lerp(material.BaseColor, float3(0.16, 0.115, 0.08) * (0.8 + 0.4 * grit), mud);
		}
		// FeetCoat snow (x) is not applied. Mud stays in feet.y. EICold::Snow stays above.
		if (snow <= 0.001)
			return;

		float3 weights = abs(normalize(vertexNormal));
		weights /= max(weights.x + weights.y + weights.z, 1e-3);
#if defined(SNOW_COVER)
		float3 albedo = SampleAlbedo(SnowCover::SnowAlbedo, posModel, weights);
		float4 rmaos = SampleRmaos(SnowCover::SnowRmaos, posModel, weights);
		float3 normalSample = SampleNormal(SnowCover::SnowNormal, posModel, weights);
#else
		float3 albedo = SampleAlbedo(ActorSnowAlbedo, posModel, weights);
		float4 rmaos = SampleRmaos(ActorSnowRmaos, posModel, weights);
		float3 normalSample = SampleNormal(ActorSnowNormal, posModel, weights);
#endif
		if (dot(albedo, 1.0) < 0.02)
			albedo = float3(0.75, 0.78, 0.82);
#if defined(TRUE_PBR)
		albedo = Color::Albedo(Color::LinearSRGBToWorking(albedo));
#else
		albedo = Color::Albedo(ColorManagement::DiffuseToWorking(TransferFunctions::LinearToGamma22(albedo)));
#endif
		albedo *= SharedData::snowCoverSettings.MainTint.rgb;
		material.BaseColor = lerp(material.BaseColor, albedo, snow * SharedData::snowCoverSettings.MainTint.w);
		float rough = rmaos.x > 0.001 ? rmaos.x : 0.75;
		material.Roughness = lerp(material.Roughness, rough, snow);
		float3 snowNormal = normalize(mul(tbn, TransformNormal(normalSample)));
		worldNormal = normalize(lerp(worldNormal, snowNormal, snow * 0.75));
	}
}

#endif  // PSHADER && !TREE_LOD_WIND_VS
#endif
