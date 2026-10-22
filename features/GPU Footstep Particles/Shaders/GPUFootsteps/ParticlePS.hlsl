// Bearforj: footstep particle shading (adapted from Bottled Shaders' FootstepParticlePS).
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "GPUFootsteps/Common.hlsli"

SamplerState LinearSampler : register(s0);
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);

#include "Common/ShadowSampling.hlsli"

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	nointerpolation float4 Color : COLOR0;
	float3 WorldPosition : POSITION1;
	float4 FogParam : COLOR1;
	nointerpolation uint Type : TEXCOORD1;
	nointerpolation float Seed : TEXCOORD2;
	nointerpolation float Softness : TEXCOORD3;
	float ViewDepth : TEXCOORD4;
	float2 SphereCoord : TEXCOORD5;
};

float4 main(PS_INPUT input) : SV_Target
{
	uint type = input.Type & 0xFFu;
	bool snowy = (input.Type & 0x100u) != 0;
	float2 uv = input.TexCoord;
	float radiusSquared = dot(uv, uv);
	if (radiusSquared >= 1.0)
		discard;

	float shape;
	if (type == GPUFootsteps::TypeClod) {
		float lumpScale = snowy ? 2.3 : 1.7;
		float2 rotatedUV = float2(uv.x * 0.8 - uv.y * 0.6, uv.x * 0.6 + uv.y * 0.8);
		float lumps = Random::perlinNoise(float3(uv * lumpScale, input.Seed * 23.0)) * 0.7 + Random::perlinNoise(float3(rotatedUV * lumpScale * 2.1, input.Seed * 23.0 + 7.0)) * 0.3;
		float edge = snowy ? 0.66 + 0.3 * lumps : 0.76 + 0.22 * lumps;
		shape = saturate((edge - sqrt(radiusSquared)) * (snowy ? 3.5 : 6.0));
	} else if (type == GPUFootsteps::TypeDroplet) {
		shape = saturate((1.0 - radiusSquared) * 2.5);
	} else {
		float noise = Random::perlinNoise(float3(uv * 1.7, input.Seed * 17.0)) * 0.5 + 0.5;
		float falloff = 1.0 - radiusSquared;
		shape = falloff * falloff * lerp(0.55, 1.25, noise);
	}

	// Soft particles; this also hides particles behind scene geometry (no depth test is bound).
	float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	float softness = saturate((sceneDepth - input.ViewDepth) / max(input.Softness, 0.5));
	float coverage = saturate(shape * softness);
	float alpha = saturate(coverage * input.Color.a);
	if (alpha <= 0.002)
		discard;

	float3 positionWS = input.WorldPosition;
	float3 viewDirection = normalize(positionWS);
	float3 lightDirection = SharedData::DirLightDirection.xyz;

	float3 ambient = ShadowSampling::GetAmbientLighting();
	float3 directional = ShadowSampling::GetDirectionalLighting();
	float shadow = SharedData::InInterior ? 1.0 : ShadowSampling::GetWorldShadow(positionWS, GPUFootsteps::CameraPosAdjust.xyz, 0);

	float3 right = normalize(mul(FrameBuffer::CameraViewInverse[0], float4(1.0, 0.0, 0.0, 0.0)).xyz);
	float3 up = normalize(mul(FrameBuffer::CameraViewInverse[0], float4(0.0, 1.0, 0.0, 0.0)).xyz);
	float3 sphereNormal = normalize(right * input.SphereCoord.x + up * input.SphereCoord.y - viewDirection * sqrt(saturate(1.0 - radiusSquared)));

	float3 color;
	float3 highlight = 0.0;

	if (type == GPUFootsteps::TypeClod || type == GPUFootsteps::TypeDroplet) {
		float wrap = saturate(dot(sphereNormal, lightDirection) * 0.6 + 0.4);
		color = input.Color.rgb * (ambient + directional * shadow * wrap);
		if (type == GPUFootsteps::TypeDroplet || snowy) {
			float3 reflected = reflect(viewDirection, sphereNormal);
			float specularPower = type == GPUFootsteps::TypeDroplet ? 48.0 : 96.0;
			float specular = pow(saturate(dot(reflected, lightDirection)), specularPower);
			float fresnel = pow(1.0 - saturate(dot(-viewDirection, sphereNormal)), 3.0);
			highlight = directional * shadow * specular * (type == GPUFootsteps::TypeDroplet ? 1.5 : 0.6) + ambient * fresnel * (type == GPUFootsteps::TypeDroplet ? 0.35 : 0.1);
		}
	} else {
		const float g = 0.3;
		float cosTheta = dot(viewDirection, lightDirection);
		float denominator = 1.0 + g * g - 2.0 * g * cosTheta;
		float phase = lerp(1.0, (1.0 - g * g) / (denominator * sqrt(denominator)), 0.5);
		color = input.Color.rgb * (ambient + directional * shadow * phase);
		if (snowy) {
			float2 cellUV = uv * 6.0 + input.Seed * 37.0;
			float2 cell = floor(cellUV);
			float sparkle = step(0.93, GPUFootsteps::Hash(asuint(cell.x) * 73856093u ^ asuint(cell.y) * 19349663u, asuint(input.Seed)));
			sparkle *= 1.0 - smoothstep(0.08, 0.25, length(frac(cellUV) - 0.5));
			highlight = directional * shadow * sparkle * phase * 0.5;
		}
	}

	float fogFactor = 0.0;
	float3 fogColor = 0.0;
	if (GPUFootsteps::FrameFlags & GPUFootsteps::FrameFlagFog) {
		fogFactor = Color::FogAlpha(input.FogParam.w);
		fogColor = Color::Fog(input.FogParam.xyz);
	}

	color = lerp(color, fogColor, fogFactor);
	highlight *= 1.0 - fogFactor;

	// premultiplied alpha
	return float4(max(color * alpha + highlight * coverage * saturate(input.Color.a * 2.0), 0.0), alpha);
}
