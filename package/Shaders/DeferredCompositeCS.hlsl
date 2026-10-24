
#include "Common/BRDF.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
Texture2D<float3> SpecularTexture : register(t0);
Texture2D<unorm float3> AlbedoTexture : register(t1);
Texture2D<unorm float3> NormalRoughnessTexture : register(t2);
Texture2D<float3> MasksTexture : register(t3);
Texture2D<unorm float> Masks2Texture : register(t9);

RWTexture2D<float4> MainRW : register(u0);
RWTexture2D<float4> NormalTAAMaskSpecularMaskRW : register(u1);
RWTexture2D<float2> MotionVectorsRW : register(u2);

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT (no unorm),
// OFF -> R24_UNORM_X8_TYPELESS game depth (unorm).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t4);
#else
Texture2D<SCENE_DEPTH_FORMAT> DepthTexture : register(t4);
#endif

#if defined(DYNAMIC_CUBEMAPS)
Texture2D<float3> ReflectanceTexture : register(t5);
TextureCube<float3> EnvTexture : register(t6);
TextureCube<float3> EnvReflectionsTexture : register(t7);

SamplerState LinearSampler : register(s0);
#endif

#if defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t8
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(SSGI) || defined(SSR)
#	include "NRD/NRDReblurSH.hlsli"
#endif

float GetSpecularOcclusionFromAmbientOcclusion(float NdotV, float ao, float roughness)
{
	return saturate(pow(abs(NdotV + ao), exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

#if defined(SSGI)
Texture2D<float4> SsgiTexture : register(t10);
#	if defined(SSGI_SH)
Texture2D<float4> SsgiSH1Texture : register(t11);
#	endif

float SampleSSGIAO(uint2 pixCoord)
{
#	if defined(SSGI_SH)
	NRD_SG sg = REBLUR_BackEnd_UnpackSh(SsgiTexture[pixCoord], SsgiSH1Texture[pixCoord]);
	return saturate(sg.normHitDist);
#	else
	float4 data = SsgiTexture[pixCoord];
	float normHitDist;
	float3 radiance;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(data, radiance, normHitDist);
	return saturate(normHitDist);
#	endif
}
#endif

#if defined(SSR)
Texture2D<float4> SsrTexture : register(t13);

void SampleSSRTracedSpecular(uint2 pixCoord, out float3 specularRadiance, out float normHitDist)
{
	float4 data = SsrTexture[pixCoord];
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(data, specularRadiance, normHitDist);
}
#endif

#if defined(SSGI)
void SampleSSGI(uint2 pixCoord, float3 normalWS, out float ao, out float3 il)
{
	ao = SampleSSGIAO(pixCoord);
	il = 0;
	(void)normalWS;
}
#endif

#if defined(IBL)
#	if !defined(DYNAMIC_CUBEMAPS)
#		undef IBL
#	else
#		define IBL_DEFERRED
#		include "IBL/IBL.hlsli"
#	endif
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res

	float3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);

	float3 diffuseColor = MainRW[dispatchID.xy].xyz;
	float3 specularColor = SpecularTexture[dispatchID.xy];
	float3 albedo = AlbedoTexture[dispatchID.xy];

	float depth = DepthTexture[dispatchID.xy];
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse, positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

#ifdef REVERSE_Z
	if (depth == 0.0)
#else
	if (depth == 1.0)
#endif
		MotionVectorsRW[dispatchID.xy] = MotionBlur::GetSSMotionVector(positionWS, positionWS);  // Apply sky motion vectors

	float glossiness = normalGlossiness.z;

#if defined(DEBUG_VIEW)
	float3 debugV = -normalize(positionWS.xyz);
	float3 debugCubemapIrradiance = 0;
#endif

	float3 linDiffuseColor = ColorManagement::SceneToLinear(diffuseColor);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse, float4(normalVS, 0)).xyz);

#if defined(SSGI)

	float ssgiAo;
	float3 ssgiIl;
	SampleSSGI(dispatchID.xy, normalWS, ssgiAo, ssgiIl);

	// Masks2.x stores 1 - vertexAO (Lighting.hlsl only); cleared to 0 for
	// pixels with no vertex AO contribution, so vertexAO defaults to 1.
	float vertexAO = 1.0 - Masks2Texture[dispatchID.xy].x;
	ssgiAo = saturate(ssgiAo / max(vertexAO, EPSILON_DIVISION));

	float3 linAlbedo = ColorManagement::SceneToLinear(albedo / Color::PBRLightingScale);
	float3 multiBounceSSGIAo = MultiBounceAO(linAlbedo, ssgiAo);

	float3 directionalAmbientColor = 0;

#	if defined(IBL)
	if ((RuntimeFeatures::IsEnabled(RuntimeFeatures::ImageBasedLightingFeature) && RuntimeFeatures::IsEnabled(RuntimeFeatures::DynamicCubemapsFeature) && SharedData::iblSettings.EnableIBL)) {
		float3 vanillaDALC = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));

#		if defined(SKYLIGHTING)
		float3 positionMS = positionWS.xyz;
		sh2 skylightingSH = Skylighting::Sample(positionMS.xyz, normalWS);
		float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, normalWS);
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBLOccluded(vanillaDALC, -normalWS, skylightingDiffuse) * albedo;
#		else
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(vanillaDALC, -normalWS) * albedo;
#		endif

		directionalAmbientColor = Color::RGBToYCoCg(directionalAmbientColor);
		directionalAmbientColor.x = MasksTexture[dispatchID.xy].z;
		directionalAmbientColor = Color::YCoCgToRGB(directionalAmbientColor);
		directionalAmbientColor = max(0, directionalAmbientColor);
	} else
#	endif
	{
		directionalAmbientColor = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));
		directionalAmbientColor *= albedo;

		directionalAmbientColor = Color::RGBToYCoCg(directionalAmbientColor);
		directionalAmbientColor.x = MasksTexture[dispatchID.xy].z;
		directionalAmbientColor = Color::YCoCgToRGB(directionalAmbientColor);
		directionalAmbientColor = max(0, directionalAmbientColor);
	}

	{
		float maxScale = 1.0;
		if (directionalAmbientColor.x > 0.0)
			maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
		if (directionalAmbientColor.y > 0.0)
			maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
		if (directionalAmbientColor.z > 0.0)
			maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
		directionalAmbientColor *= maxScale;

		diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);
		diffuseColor = ColorManagement::SceneColor::ScaleByLinear(diffuseColor, sqrt(multiBounceSSGIAo));

		directionalAmbientColor = ColorManagement::SceneColor::ScaleByLinear(directionalAmbientColor, multiBounceSSGIAo);
		diffuseColor += directionalAmbientColor;
		linDiffuseColor = ColorManagement::SceneToLinear(diffuseColor);
	}

	linDiffuseColor += ssgiIl * linAlbedo;
#endif

	float3 color = linDiffuseColor + specularColor;

#if defined(DYNAMIC_CUBEMAPS)

	float3 reflectance = ReflectanceTexture[dispatchID.xy];

	if (RuntimeFeatures::IsEnabled(RuntimeFeatures::DynamicCubemapsFeature) && any(reflectance > 0.0)) {
		float3 V = -normalize(positionWS.xyz);
		float3 R = reflect(-V, normalWS);

		float roughness = 1.0 - glossiness;
		float level = roughness * 8.0;

		sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normalWS, V, roughness);

		float3 finalIrradiance = 0;

		float directionalAmbientColorSpecular = Color::RGBToLuminance(Color::Ambient(max(0, SharedData::GetAmbient(R)))) * Color::ReflectionNormalisationScale;

#	if defined(SKYLIGHTING)
		float3 positionMS = positionWS.xyz;
		positionMS.z = min(positionMS.z, SharedData::skylightingSettings.PosOffset.z + Skylighting::ARRAY_SIZE.z * 0.5 - Skylighting::CELL_SIZE.z);

		sh2 skylightingSH = Skylighting::Sample(positionMS.xyz, R);
		float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe);
		float skylightingVisibility = Skylighting::EvaluateVisibility(skylightingSH);
#	endif

#	if defined(IBL)
		if ((RuntimeFeatures::IsEnabled(RuntimeFeatures::ImageBasedLightingFeature) && RuntimeFeatures::IsEnabled(RuntimeFeatures::DynamicCubemapsFeature) && SharedData::iblSettings.EnableIBL)) {
			float3 envSample = EnvTexture.SampleLevel(LinearSampler, R, level);
			float3 fullSample = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
			float3 envSpecular, skySpecular;

			if (SharedData::iblSettings.DALCMode >= 2) {
				// Mode 2/3: DALC-normalized env scaled by DALCAmount + sky overlay
				float envLum = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
				envSpecular = ColorManagement::SceneToLinear((envSample / max(envLum, 0.001)) * directionalAmbientColorSpecular) * SharedData::iblSettings.DALCAmount;
				skySpecular = ColorManagement::SceneToLinear(max(0, fullSample - envSample)) * SharedData::iblSettings.SkyIBLScale;
#		if defined(SKYLIGHTING)
				envSpecular *= (SharedData::iblSettings.DALCMode == 3) ? skylightingVisibility : 1.0;
				skySpecular *= skylightingSpecular;
#		endif
			} else {
				// Mode 0/1: IBL ratio-based
				float3 ratio = ImageBasedLighting::GetIBLRatio();
				envSpecular = ColorManagement::SceneToLinear(envSample * ratio) * SharedData::iblSettings.EnvIBLScale;
				skySpecular = ColorManagement::SceneToLinear(max(0, fullSample - envSample)) * SharedData::iblSettings.SkyIBLScale;
#		if defined(SKYLIGHTING)
				skySpecular *= skylightingSpecular;
#		endif
			}
			if (SharedData::InInterior) {
				skySpecular = 0;
			}

			finalIrradiance = envSpecular + skySpecular;
		} else
#	endif
		{
			// Fallback without IBL: normalize-by-luminance with DALC
#	if defined(INTERIOR)
			float3 specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);
			float specularIrradianceLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;
			finalIrradiance = ColorManagement::SceneToLinear(specularIrradiance);
#	elif defined(SKYLIGHTING)
			float3 specularIrradianceReflections = 0.0;
			if (skylightingSpecular > 0.0) {
				specularIrradianceReflections = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
				float lum = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));
				specularIrradianceReflections = (specularIrradianceReflections / max(lum, 0.001)) * directionalAmbientColorSpecular;
				specularIrradianceReflections = ColorManagement::SceneToLinear(specularIrradianceReflections);
			}
			float3 specularIrradiance = 0.0;
			if (skylightingSpecular < 1.0) {
				specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);
				float lum = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
				float dalcScaled = ColorManagement::SceneColor::ScaleByLinear(directionalAmbientColorSpecular, skylightingSpecular);
				specularIrradiance = (specularIrradiance / max(lum, 0.001)) * dalcScaled;
				specularIrradiance = ColorManagement::SceneToLinear(specularIrradiance);
			}
			finalIrradiance = lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular);
#	else
			float3 specularIrradiance = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
			float specularIrradianceLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));
			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;
			finalIrradiance = ColorManagement::SceneToLinear(specularIrradiance);
#	endif
		}

#	if defined(SSR)
		if (SharedData::ssrSettings.Enabled != 0) {
			float3 tracedSpecular;
			float specNormHitDist;
			SampleSSRTracedSpecular(dispatchID.xy, tracedSpecular, specNormHitDist);
			float tracedAvailability = saturate(specNormHitDist / NRD_EPS);
			float3 fallbackIrradiance = finalIrradiance * SharedData::ssrSettings.SpecCubemapMult;
			finalIrradiance = lerp(fallbackIrradiance, tracedSpecular, tracedAvailability);
		}
#	endif

#	if defined(SSGI)
		if (SharedData::ssgiSettings.Enabled != 0)
			finalIrradiance *= GetSpecularOcclusionFromAmbientOcclusion(saturate(dot(normalWS, V)), ssgiAo, roughness);
#	endif

		color += reflectance * finalIrradiance;
	}

#endif

	color = ColorManagement::LinearToScene(color);

#if defined(DEBUG)

	if (uv.x < 0.5 && uv.y < 0.5) {
		color = color;
	} else if (uv.x < 0.5) {
		color = albedo;
	} else if (uv.y < 0.5) {
		color = normalVS;
	} else {
		color = glossiness;
	}

#endif

	MainRW[dispatchID.xy] = float4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[dispatchID.xy] = float4(GBuffer::EncodeNormalVanilla(normalVS), 0.0, 0.0);
}
