#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/ReverseZ.hlsli"
#include "Common/VR.hlsli"

#if defined(CSHADER)
SamplerState ShadowmapSampler : register(s0);
SamplerState ShadowmapVLSampler : register(s1);
SamplerState InverseRepartitionSampler : register(s2);
SamplerState NoiseSampler : register(s3);

Texture2DArray<float> ShadowmapTex : register(t0);
Texture2DArray<float> ShadowmapVLTex : register(t1);
Texture1D<float> InverseRepartitionTex : register(t2);
Texture3D<float> NoiseTex : register(t3);

RWTexture3D<float> DensityRW : register(u0);

#	define LinearSampler ShadowmapSampler

#	include "Common/Framebuffer.hlsli"
#	include "Common/SharedData.hlsli"

#	include "Common/ShadowSampling.hlsli"
#	include "Common/Game.hlsli"

	Texture2D<float4> VolCloudShadowCookie : register(t4);

	float VolumetricCloudCookie(float3 worldPosAbs, uint eyeIndex)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;
		if (!data.enableVolumetricClouds || data.highestCloudAltitude <= data.lowestCloudAltitude)
			return 1.0;

		float3 posRelative = worldPosAbs;
		posRelative.z -= data.zBottom;
		const float3 direction = SharedData::DirLightDirection.xyz;
		const float lengthSq = dot(direction, direction);
		const float3 shadowLightDir = lengthSq > 1e-6 ? direction * rsqrt(lengthSq) : data.sunDir;
		const float3 reference = abs(shadowLightDir.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
		const float3 right = normalize(cross(reference, shadowLightDir));
		const float3 up = normalize(cross(shadowLightDir, right));
		const float3 camera = FrameBuffer::CameraPosAdjust[eyeIndex].xyz - float3(0, 0, data.zBottom);
		const float shadowWidth = max(data.shadowVolumeRange, 1.0);
		const float2 uv = float2(dot(posRelative - camera, right), dot(posRelative - camera, up)) / shadowWidth + 0.5;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 1.0;

		const float4 cloudShadow = VolCloudShadowCookie.SampleLevel(ShadowmapSampler, uv, 0);
		if (cloudShadow.y == 1.0)
			return 1.0;

		const float3 posPlanet = posRelative + float3(-FrameBuffer::CameraPosAdjust[eyeIndex].xy, data.rPlanet);
		const float3 lightForward = -shadowLightDir;
		const float sphereRadius = data.rPlanet + data.lowestCloudAltitude;
		const float b = dot(posPlanet, lightForward);
		const float c = dot(posPlanet, posPlanet) - sphereRadius * sphereRadius;
		const float discriminant = b * b - c;
		const float zCoord = discriminant >= 0.0 ? -b - sqrt(discriminant) : 0.0;
		const float zCoordKm = zCoord * GAME_UNIT_TO_M * 0.001;
		const float shadowRange = saturate((zCoordKm - cloudShadow.x) / max(cloudShadow.z - cloudShadow.x, 1e-5));
		return lerp(cloudShadow.y, 1.0, shadowRange);
	}

cbuffer PerTechnique : register(b0)
{
#	ifndef VR
	row_major float4x4 CameraViewProj[1] : packoffset(c0);
	row_major float4x4 CameraViewProjInverse[1] : packoffset(c4);
	float4x3 ShadowMapProj[1][3] : packoffset(c8);
	float3 EndSplitDistances : packoffset(c17.x);
	float ShadowMapCount : packoffset(c17.w);
	float EnableShadowCasting : packoffset(c18);
	float3 DirLightDirection : packoffset(c19);
	float3 TextureDimensions : packoffset(c20);
	float3 WindInput[1] : packoffset(c21);
	float InverseDensityScale : packoffset(c21.w);
	float3 PosAdjust[1] : packoffset(c22);
	float IterationIndex : packoffset(c22.w);
	float PhaseContribution : packoffset(c23.x);
	float PhaseScattering : packoffset(c23.y);
	float DensityContribution : packoffset(c23.z);
#	else
	row_major float4x4 CameraViewProj[2] : packoffset(c0);
	row_major float4x4 CameraViewProjInverse[2] : packoffset(c8);
	float4x3 ShadowMapProj[2][3] : packoffset(c16);
	float3 EndSplitDistances : packoffset(c34.x);
	float ShadowMapCount : packoffset(c34.w);
	float EnableShadowCasting : packoffset(c35.x);
	float3 DirLightDirection : packoffset(c36);
	float3 TextureDimensions : packoffset(c37);
	float3 WindInput[2] : packoffset(c38);
	float InverseDensityScale : packoffset(c39.w);
	float3 PosAdjust[2] : packoffset(c40);
	float IterationIndex : packoffset(c41.w);
	float PhaseContribution : packoffset(c42.x);
	float PhaseScattering : packoffset(c42.y);
	float DensityContribution : packoffset(c42.z);
#	endif
}

[numthreads(32, 32, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const float3 StepCoefficients[] = {
		{ 0, 0, 0 },
		{ 0, 0, 0.001 },
		{ 0, 0.001, 0 },
		{ 0, 0.001, 0.001 },
		{ 0.001, 0, 0 },
		{ 0.001, 0, 0.001 },
		{ 0.001, 0.001, 0 },
		{ 0.001, 0.001, 0.001 }
	};

	float3 normalizedCoordinates = float3(dispatchID.xy + 0.5, dispatchID.z - 1.0) * rcp(TextureDimensions.xyz);
	float2 uv = normalizedCoordinates.xy;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	float3 depthUv = Stereo::ConvertFromStereoUV(normalizedCoordinates, eyeIndex) + StepCoefficients[IterationIndex];
	float depth = FrameBuffer::ToNativeDepth(InverseRepartitionTex.SampleLevel(InverseRepartitionSampler, depthUv.z, 0));
	float4 positionCS = float4(2 * depthUv.x - 1, 1 - 2 * depthUv.y, depth, 1);

	float4 positionWS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionWS *= rcp(positionWS.w);

	float4 positionCSShifted = mul(CameraViewProj[eyeIndex], positionWS);
	positionCSShifted *= rcp(positionCSShifted.w);

	float shadowMapDepth = FrameBuffer::ToStandardDepth(positionCSShifted.z);

	bool noShadow = !SharedData::InInterior;
	if (EndSplitDistances.z >= shadowMapDepth) {
		uint cascadeIndex = ShadowMapCount >= 3.0f && shadowMapDepth > EndSplitDistances.y ? 2 : shadowMapDepth > EndSplitDistances.x ? 1 :
		                                                                                                                                0;
		float shadowMapThreshold = cascadeIndex == 0 ? 0.01f : 0.0f;
		float4x3 lightProjectionMatrix = ShadowMapProj[eyeIndex][cascadeIndex];

		float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionWS.xyz, 1)).xyz;
		float shadowMapValue = ShadowmapTex.SampleLevel(ShadowmapSampler, float3(positionLS.xy, cascadeIndex), 0);
		noShadow = shadowMapValue >= positionLS.z - shadowMapThreshold;

		if (EnableShadowCasting < 0.5) {
			float shadowMapVLValue = ShadowmapVLTex.SampleLevel(ShadowmapVLSampler, float3(positionLS.xy, cascadeIndex), 0);
			noShadow = noShadow & shadowMapVLValue >= positionLS.z - shadowMapThreshold;
		}
	}

	float3 noiseUv = 0.0125 * (InverseDensityScale * (positionWS.xyz + WindInput[eyeIndex]));
	float noise = NoiseTex.SampleLevel(NoiseSampler, noiseUv, 0);
	float densityFactor = noise * (1 - 0.75 * smoothstep(0, 1, saturate(2 * positionWS.z / 300)));
	float densityContribution = lerp(1, densityFactor, DensityContribution);

#	if defined(VR)
	float3 viewDirection = normalize(FrameBuffer::ViewToWorld(float3(0.0, 0.0, 0.0), true, eyeIndex) - positionWS.xyz);
#	else
	float3 viewDirection = normalize(-positionWS.xyz);
#	endif
	float LdotN = dot(viewDirection, DirLightDirection);
	float phaseFactor = (1 - PhaseScattering * PhaseScattering) * rcp(4 * Math::PI * (1 - LdotN * PhaseScattering));
	float phaseContribution = lerp(1, phaseFactor, PhaseContribution);

	float shadowContribution = noShadow;

#	if defined(TERRAIN_SHADOWS) || defined(CLOUD_SHADOWS)
	shadowContribution *= sqrt(ShadowSampling::GetWorldShadow(positionWS.xyz, PosAdjust[eyeIndex], eyeIndex));
#	endif
	shadowContribution *= VolumetricCloudCookie(positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	float vl = shadowContribution * densityContribution * phaseContribution;

	DensityRW[dispatchID.xyz] = vl;
}
#endif
