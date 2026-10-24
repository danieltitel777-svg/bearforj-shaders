#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "PhysicalBlood/PhysicalBloodCommon.hlsli"
#include "ScreenSpacePhysicalParticles/ProceduralQuad.hlsli"

// Renders airborne SSPP particles and Physical Blood-owned terminal splats as
// procedural quads. Particle positions enter in absolute world space and are
// converted to Skyrim's camera-relative render space before projection.

#if defined(PSHADER)
#	include "Common/SharedData.hlsli"
#	include "Common/Color.hlsli"
#	if defined(AIRBORNE_PASS) || defined(SPLAT_PASS)
#		include "Common/PBRMath.hlsli"
#	endif

Texture2D<float4> BloodDiffuseAtlas : register(t1);
Texture2D<float4> BloodNormalAtlas : register(t2);
Texture2D<float4> BloodBlendAtlas : register(t3);
Texture2D<float4> BloodBlendAltAlphaAtlas : register(t4);

SamplerState PhysicalBloodLinearSampler : register(s0);
SamplerState BloodAtlasSampler : register(s1);
#	define LinearSampler PhysicalBloodLinearSampler
#	if defined(AIRBORNE_PASS)
#		include "Common/ShadowSampling.hlsli"
#	elif defined(SPLAT_PASS) && defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	endif
#endif

#if defined(VSHADER)
#if defined(OWNED_SPLAT_STORAGE)
StructuredBuffer<PhysicalBloodOwnedSplat> OwnedSplats : register(t0);
StructuredBuffer<uint> OwnedSplatState : register(t1);
#else
StructuredBuffer<float4> PositionAge : register(t0);
StructuredBuffer<float4> VelocityLifetime : register(t1);
StructuredBuffer<float4> Appearance : register(t2);
StructuredBuffer<uint> VisibleParticleIDs : register(t5);
#endif
#endif

cbuffer PhysicalBloodSharedRender : register(b1)
{
	uint VisibleSegmentBase;
	uint ParticleCapacity;
	uint RenderClass;
	uint OwnedSplatCapacity;

	float CurrentTime;
	float SplatLifetime;
	uint OwnedSplatSegment;
	float PhysicalBloodSharedRenderPadding;
};

// The atlas supplies coverage and local material variation; lighting is applied here.
static const float3 BLOOD_COLOR = float3(0.40f, 0.006f, 0.008f);
static const float BLOOD_ATLAS_GRID_SIZE = 5.0f;
static const uint BLOOD_ATLAS_CELL_COUNT = 25u;
// Half a texel at mip 6, the last mip with one BC3 block per atlas cell.
static const float BLOOD_ATLAS_LOCAL_INSET = 1.0f / 8.0f;

struct VertexOutput
{
	float4 Position : SV_POSITION;
	float2 UV : TEXCOORD0;
	nointerpolation float Life : TEXCOORD1;
	nointerpolation float Variation : TEXCOORD2;
	nointerpolation float Opacity : TEXCOORD3;
	float3 WorldPosition : TEXCOORD4;
	nointerpolation float3 SurfaceNormal : TEXCOORD5;
	nointerpolation float3 Tangent : TEXCOORD6;
	nointerpolation float3 Bitangent : TEXCOORD7;
	nointerpolation uint AtlasIndex : TEXCOORD8;
#if defined(SPLAT_PASS)
	nointerpolation float4 SplatAmbientAndSpecular : TEXCOORD9;
	nointerpolation float3 SplatDirectionalDiffuse : TEXCOORD10;
#endif
};

#if defined(VSHADER)
// Summary: Expands one visible particle into a velocity- or surface-aligned quad.
// Args:
//   vertexID: Procedural triangle-strip corner index.
//   instanceID: Particle instance within the SSPP visible segment.
// Returns: Clip-space geometry and material inputs for the selected render pass.
VertexOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VertexOutput output = (VertexOutput)0;
	float4 positionAge;
	float4 velocityLifetime;
	float4 appearance;
	float3 contactNormal = float3(0.0f, 0.0f, 1.0f);
#if defined(OWNED_SPLAT_STORAGE)
	uint oldestSerial = OwnedSplatState[1];
	uint firstSlot = oldestSerial % max(OwnedSplatCapacity, 1u);
	uint segmentOffset = OwnedSplatSegment == 0u ? 0u : OwnedSplatCapacity - firstSlot;
	uint physicalSlot = OwnedSplatSegment == 0u ? firstSlot + instanceID : instanceID;
	uint expectedSerial = oldestSerial + segmentOffset + instanceID;
	if (physicalSlot >= OwnedSplatCapacity) {
		output.Position = float4(2.0f, 2.0f, 2.0f, 1.0f);
		return output;
	}
	PhysicalBloodOwnedSplat splat = OwnedSplats[physicalSlot];
	float lifetime = max(SplatLifetime, 0.001f);
	if (splat.Serial != expectedSerial + 1u ||
		CurrentTime >= splat.SpawnTime + lifetime) {
		output.Position = float4(2.0f, 2.0f, 2.0f, 1.0f);
		return output;
	}
	positionAge = float4(splat.Position, max(CurrentTime - splat.SpawnTime, 0.0f));
	velocityLifetime = float4(splat.ImpactVelocity, lifetime);
	appearance = float4(
		splat.Radius, splat.Opacity, splat.Variation, splat.SizeMultiplier);
	contactNormal = splat.SurfaceNormal;
	output.SplatAmbientAndSpecular = float4(splat.AmbientIrradiance, 0.0f);
	output.SplatDirectionalDiffuse = splat.DirectionalDiffuse;
#else
	uint particleIndex = VisibleParticleIDs[VisibleSegmentBase + instanceID];
	positionAge = PositionAge[particleIndex];
	velocityLifetime = VelocityLifetime[particleIndex];
	appearance = Appearance[particleIndex];
#endif
	float2 corner = GetProceduralQuadCorner(vertexID);

	float3 cameraRelative = positionAge.xyz - FrameBuffer::CameraPosAdjust.xyz;
	float3 worldPosition = cameraRelative;
#if defined(SPLAT_PASS)
	{
		float3 normal = normalize(contactNormal);
		float impactSpeed = length(velocityLifetime.xyz);
		float3 impactDirection = impactSpeed > 0.01f ?
			velocityLifetime.xyz / impactSpeed : float3(0.0f, 0.0f, -1.0f);
		float3 surfaceTravel = impactDirection - normal * dot(impactDirection, normal);
		float surfaceTravelLength = length(surfaceTravel);
		float3 tangent;
		float3 bitangent;
		if (surfaceTravelLength > 0.05f) {
			// Atlas V=0 is the impact head, so increasing V trails the flight path.
			bitangent = -surfaceTravel / surfaceTravelLength;
			tangent = normalize(cross(bitangent, normal));
		} else {
			float3 reference = abs(normal.z) < 0.95f ?
				float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
			tangent = normalize(cross(reference, normal));
			bitangent = cross(normal, tangent);
			float angle = appearance.z * Math::TAU;
			float sine;
			float cosine;
			sincos(angle, sine, cosine);
			float3 rotatedTangent = tangent * cosine + bitangent * sine;
			bitangent = -tangent * sine + bitangent * cosine;
			tangent = rotatedTangent;
		}
		float splatSize = appearance.x * max(appearance.w, 0.0f);
		float grazing = saturate(surfaceTravelLength);
		float impactStretch = 1.0f + 1.75f * grazing *
			saturate(impactSpeed / 1000.0f);
		float longitudinalExtent = corner.y > 0.0f ? impactStretch : 0.80f;
		worldPosition += tangent * corner.x * splatSize;
		worldPosition += bitangent * corner.y * splatSize * longitudinalExtent;
		output.SurfaceNormal = normal;
		output.Tangent = tangent;
		output.Bitangent = bitangent;
	}
#else
	{
		float3 viewRight = mul(
			FrameBuffer::CameraViewInverse,
			float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz;
		float3 viewUp = mul(
			FrameBuffer::CameraViewInverse,
			float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz;
		float speed = length(velocityLifetime.xyz);
		float3 viewDirection = cameraRelative *
			rsqrt(max(dot(cameraRelative, cameraRelative), 1.0e-6f));
		float3 velocityDirection = speed > 0.01f ?
			velocityLifetime.xyz / speed : viewUp;
		float3 projectedVelocity = velocityDirection -
			viewDirection * dot(velocityDirection, viewDirection);
		float projectedLength = length(projectedVelocity);
		// Increasing atlas V points down the texture and therefore trails velocity.
		float3 stretchDirection = projectedLength > 0.01f ?
			-projectedVelocity / projectedLength : -viewUp;
		float3 sideDirection = cross(viewDirection, stretchDirection);
		float sideLength = length(sideDirection);
		if (sideLength < 0.01f)
			sideDirection = viewRight;
		else
			sideDirection /= sideLength;
		float speedStretch = saturate(speed / 1200.0f);
		float longitudinalExtent = corner.y > 0.0f ?
			1.0f + 2.75f * speedStretch : 0.85f;
		float3 facingNormal = normalize(cross(sideDirection, stretchDirection));
		if (dot(facingNormal, -viewDirection) < 0.0f) {
			sideDirection = -sideDirection;
			facingNormal = -facingNormal;
		}
		worldPosition += sideDirection * corner.x * appearance.x;
		worldPosition += stretchDirection * corner.y *
			appearance.x * longitudinalExtent;
		output.SurfaceNormal = facingNormal;
		output.Tangent = sideDirection;
		output.Bitangent = stretchDirection;
	}
#endif

	output.Position = mul(FrameBuffer::CameraViewProj, float4(worldPosition, 1.0f));
	output.WorldPosition = worldPosition;
	output.UV = corner * 0.5f + 0.5f;
	output.Life = saturate(1.0f - positionAge.w / max(velocityLifetime.w, 0.001f));
	output.Variation = appearance.z;
	output.Opacity = appearance.y;
	output.AtlasIndex = min(
		(uint)(appearance.z * (float)BLOOD_ATLAS_CELL_COUNT),
		BLOOD_ATLAS_CELL_COUNT - 1u);
	return output;
}
#endif

#if defined(PSHADER) && defined(AIRBORNE_PASS)
// Summary: Reconstructs a rounded droplet normal from quad-local coordinates.
// Args:
//   input: Interpolated airborne vertex data in camera-relative world space.
//   centered: Quad UV remapped to the [-1, 1] range.
//   atlasNormal: Sampled tangent-space normal and gloss mask.
// Returns: A camera-facing unit surface normal.
float3 GetPhysicalBloodNormal(
	VertexOutput input,
	float2 centered,
	float4 atlasNormal)
{
	float3 toCamera = -input.WorldPosition;
	float3 viewDirection = toCamera * rsqrt(max(dot(toCamera, toCamera), 1.0e-6f));
	float hemisphereHeight = sqrt(saturate(1.0f - dot(centered, centered)));
	float3 baseNormal = normalize(float3(centered, hemisphereHeight));
	float3 detailNormal = atlasNormal.rgb * 2.0f - 1.0f;
	detailNormal.xy *= lerp(0.30f, 0.65f, saturate(atlasNormal.a));
	detailNormal.z = max(detailNormal.z, 0.20f);
	detailNormal = normalize(detailNormal);
	float3 reorientedBase = baseNormal + float3(0.0f, 0.0f, 1.0f);
	float3 reorientedDetail = detailNormal * float3(-1.0f, -1.0f, 1.0f);
	float3 tangentNormal = normalize(
		reorientedBase *
			dot(reorientedBase, reorientedDetail) / reorientedBase.z -
		reorientedDetail);
	float3 normal = normalize(
		input.Tangent * tangentNormal.x +
		input.Bitangent * tangentNormal.y +
		input.SurfaceNormal * tangentNormal.z);
	if (dot(normal, viewDirection) < 0.0f)
		normal = -normal;
	return normal;
}

// Summary: Evaluates the direct-light GGX reflection of a wet blood surface.
// Args:
//   normal: Unit surface normal.
//   viewDirection: Unit direction from the surface to the camera.
//   lightDirection: Unit direction from the surface to the directional light.
//   lightColor: Shadowed directional-light radiance.
//   roughness: GGX material roughness.
// Returns: Direct specular radiance in Skyrim's PBR lighting scale.
float3 EvaluateWetSpecular(
	float3 normal,
	float3 viewDirection,
	float3 lightDirection,
	float3 lightColor,
	float roughness)
{
	float NdotL = saturate(dot(normal, lightDirection));
	float NdotV = saturate(dot(normal, viewDirection));
	if (NdotL <= 0.0f || NdotV <= 0.0f)
		return 0.0f;

	float3 halfVector = normalize(lightDirection + viewDirection);
	float NdotH = saturate(dot(normal, halfVector));
	float VdotH = saturate(dot(viewDirection, halfVector));
	float3 fresnel;
	float3 microfacet = PBR::SpecularMicrofacet(
		roughness, float3(0.025f, 0.025f, 0.025f),
		NdotL, NdotV, NdotH, VdotH, fresnel);
	return microfacet * NdotL * lightColor *
		Color::PBRLightingCompensation * Color::PBRLightingScale;
}
#endif

#if defined(PSHADER) && defined(SPLAT_PASS)
// Summary: Evaluates wet GGX specular intensity relative to diffuse lighting.
// Args:
//   normal: Unit surface normal.
//   viewDirection: Unit direction from the surface to the camera.
//   lightDirection: Unit direction toward the directional light.
//   roughness: GGX material roughness.
// Returns: Scalar specular-to-diffuse ratio.
float EvaluateWetSpecularToDiffuse(
	float3 normal,
	float3 viewDirection,
	float3 lightDirection,
	float roughness)
{
	float NdotL = saturate(dot(normal, lightDirection));
	float NdotV = saturate(dot(normal, viewDirection));
	if (NdotL <= 0.0f || NdotV <= 0.0f)
		return 0.0f;
	float3 halfVector = normalize(lightDirection + viewDirection);
	float NdotH = saturate(dot(normal, halfVector));
	float VdotH = saturate(dot(viewDirection, halfVector));
	float3 fresnel;
	float microfacet = PBR::SpecularMicrofacet(
		roughness,
		float3(0.025f, 0.025f, 0.025f),
		NdotL,
		NdotV,
		NdotH,
		VdotH,
		fresnel).r;
	return microfacet * Color::PBRLightingCompensation *
		Color::PBRLightingScale / Color::VanillaNormalization();
}
#endif

#if defined(PSHADER)
// Summary: Shades an atlas-masked airborne droplet or persistent surface splat.
// Args:
//   input: Rasterized quad data for the active render permutation.
// Returns: Blood color and alpha coverage.
float4 PSMain(VertexOutput input) : SV_TARGET
{
	float2 centered = input.UV * 2.0f - 1.0f;
	float radial = length(centered);
	float2 atlasCell = float2(
		input.AtlasIndex % 5u,
		input.AtlasIndex / 5u);
	float2 localUV = clamp(
		saturate(input.UV),
		BLOOD_ATLAS_LOCAL_INSET,
		1.0f - BLOOD_ATLAS_LOCAL_INSET);
	float2 atlasUV = (atlasCell + localUV) / BLOOD_ATLAS_GRID_SIZE;
	float2 atlasDx = ddx(input.UV) / BLOOD_ATLAS_GRID_SIZE;
	float2 atlasDy = ddy(input.UV) / BLOOD_ATLAS_GRID_SIZE;
	float4 diffuseSample = BloodDiffuseAtlas.SampleGrad(
		BloodAtlasSampler, atlasUV, atlasDx, atlasDy);
	float mask = saturate(diffuseSample.a);
#if defined(AIRBORNE_PASS)
	float lifeFade = smoothstep(0.0f, 0.18f, input.Life);
#else
	float lifeFade = smoothstep(0.0f, 0.08f, input.Life);
#endif
	float alpha = mask * lifeFade * input.Opacity;
	clip(alpha - (1.0f / 255.0f));
	float4 normalSample = BloodNormalAtlas.SampleGrad(
		BloodAtlasSampler, atlasUV, atlasDx, atlasDy);
	float4 blendSample = BloodBlendAtlas.SampleGrad(
		BloodAtlasSampler, atlasUV, atlasDx, atlasDy);
	float4 blendAltSample = BloodBlendAltAlphaAtlas.SampleGrad(
		BloodAtlasSampler, atlasUV, atlasDx, atlasDy);
	float colorDetail = saturate(dot(diffuseSample.rgb, float3(0.80f, 0.15f, 0.05f)) * 1.35f);
	float blendDetail = saturate(max(blendSample.r, blendAltSample.r));
	float wetCore = saturate(blendAltSample.a);
	float gloss = saturate(normalSample.a);
	float3 textureAlbedo = BLOOD_COLOR *
		lerp(0.72f, 1.08f, colorDetail) *
		lerp(1.0f, 0.82f, blendDetail);

#if defined(AIRBORNE_PASS)
	float3 normal = GetPhysicalBloodNormal(input, centered, normalSample);
	float3 toCamera = -input.WorldPosition;
	float3 viewDirection = toCamera * rsqrt(max(dot(toCamera, toCamera), 1.0e-6f));
	float3 lightDirection = SharedData::DirLightDirection.xyz;
	float NdotL = saturate(dot(normal, lightDirection));
	float3 ambientLighting = Color::Ambient(max(0.0f, SharedData::GetAmbient(normal)));
	float3 directionalLighting = 0.0f;
	float3 wetSpecular = 0.0f;
	[branch]
	if (NdotL > 0.0f) {
		float detailedShadow = 1.0f;
		float directionalShadow = ShadowSampling::GetLightingShadow(
			input.WorldPosition, detailedShadow);
		directionalShadow = min(directionalShadow, detailedShadow);
		directionalShadow *= ShadowSampling::GetWorldShadow(
			input.WorldPosition, FrameBuffer::CameraPosAdjust.xyz);
		directionalLighting =
			ShadowSampling::GetDirectionalLighting() * directionalShadow;
		wetSpecular = EvaluateWetSpecular(
			normal,
			viewDirection,
			lightDirection,
			directionalLighting,
			lerp(0.40f, 0.16f, gloss * lerp(0.55f, 1.0f, wetCore)));
	}
	float absorption = lerp(1.0f, 0.76f, saturate(1.0f - radial));
	float3 albedo = Color::Diffuse(
		textureAlbedo * (0.78f + input.Variation * 0.18f) * absorption);
	float3 diffuseIrradiance = ambientLighting +
		directionalLighting * NdotL * Color::VanillaNormalization();
	diffuseIrradiance /= 1.0f +
		max(diffuseIrradiance - 1.0f, 0.0f) * 0.45f;
	float3 color = albedo * diffuseIrradiance + wetSpecular;
#else
	float absorption = lerp(1.0f, 0.86f, saturate(1.0f - radial));
	float3 albedo = Color::Diffuse(
		textureAlbedo * (0.78f + input.Variation * 0.18f) * absorption);
	float contactShadow = 1.0f;
#	if defined(SCREEN_SPACE_SHADOWS)
	[branch]
	if (!SharedData::InInterior && any(input.SplatDirectionalDiffuse > 0.0f)) {
		float2 screenUV = input.Position.xy * SharedData::BufferDim.zw;
		contactShadow = lerp(
			1.0f,
			ScreenSpaceShadows::GetScreenSpaceShadow(
				input.Position.xyz, screenUV, input.Variation),
			0.8f);
	}
#	endif
	float3 directionalDiffuse =
		input.SplatDirectionalDiffuse * contactShadow;
	float3 diffuseIrradiance =
		input.SplatAmbientAndSpecular.xyz + directionalDiffuse;
	diffuseIrradiance /= 1.0f +
		max(diffuseIrradiance - 1.0f, 0.0f) * 0.45f;
	float specularMask = lerp(
		0.25f, 1.20f, gloss * lerp(0.55f, 1.0f, wetCore));
	float3 normal = normalize(input.SurfaceNormal);
	float3 viewDirection = -input.WorldPosition *
		rsqrt(max(dot(input.WorldPosition, input.WorldPosition), 1.0e-6f));
	if (dot(normal, viewDirection) < 0.0f)
		normal = -normal;
	float specularToDiffuse = EvaluateWetSpecularToDiffuse(
		normal,
		viewDirection,
		SharedData::DirLightDirection.xyz,
		lerp(0.22f, 0.34f, input.Variation));
	float3 color = albedo * diffuseIrradiance +
		directionalDiffuse * specularToDiffuse * specularMask;
#endif
	return float4(color, alpha);
}
#endif
