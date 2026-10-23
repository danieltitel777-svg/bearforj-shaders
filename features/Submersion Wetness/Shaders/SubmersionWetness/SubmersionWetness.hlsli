#ifndef __SUBMERSION_WETNESS_HLSLI__
#define __SUBMERSION_WETNESS_HLSLI__

// BearForge Shaders - Submersion Wetness
// Per-actor wetness driven by how deep the actor has been in water.
// Only the part of the body that was under water gets wet; beads (Open's character rain beads)
// appear only on that part and only above the current water surface. When the head comes out of the
// water the beads turn into rivulets that run down and fade.

namespace SubmersionWetness
{
	// PS b9 is only used by the vertex stage (bones) elsewhere in Lighting.hlsl.
	cbuffer SubmersionPerGeometry : register(b9)
	{
		float4 SubA;  // x feet world z, y soak line height above feet, z current water world z (-1e9 none), w soak 0..1
		float4 SubB;  // x bead amount 0..1, y emerge flow 0..1, z edge softness (units), w enabled
		float4 SubC;  // x wetness strength, y bead density, z hair strength, w armor strength
		float4 SubD;  // x rain progress head-to-feet 0..1, y rain darkening 0..1, z actor height, w 1 when the whole body is under water
		float4 SubE;  // x snowfall coat 0..1, y drift surface world z, z leg drift amount, w unused
	};

	bool Active()
	{
		return SubB.w > 0.5f && (SubA.w > 0.0f || SubB.x > 0.0f || SubD.x > 0.0f || SubE.x > 0.0f || SubE.z > 0.0f);
	}

	float SnowHash(float3 p)
	{
		return frac(sin(dot(floor(p), float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
	}

	/** @brief Light directional snow while it is snowing, plus the part of the leg buried in a raised drift. */
	float Snow(float3 modelPosition, float3 worldNormal, float absZ)
	{
		float coat = 0.0f;
		float s = saturate(SubE.x);
		if (s > 0.001f) {
			float n = SnowHash(modelPosition * 0.17f);
			float up = saturate(normalize(worldNormal).z);
			float cut = lerp(0.94f, 0.58f, s);
			coat = saturate(smoothstep(cut - 0.1f, cut + 0.1f, up + (n - 0.5f) * 0.28f) * lerp(0.12f, 0.45f, s));
		}
		float drift = 0.0f;
		if (SubE.z > 0.001f) {
			float below = 1.0f - smoothstep(SubE.y - 8.0f, SubE.y + 1.0f, absZ);
			float aboveFeet = smoothstep(SubA.x - 4.0f, SubA.x + 6.0f, absZ);
			float facing = saturate(normalize(worldNormal).z * 0.65f + 0.35f);
			drift = below * aboveFeet * saturate(SubE.z) * facing;
		}
		return saturate(max(coat, drift));
	}

	float EdgeNoise(float3 p)
	{
		return frac(sin(dot(floor(p * 0.25f), float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
	}

	/** @brief 1 below the soak line (the part that was under water), with a soft uneven edge. */
	float SubmergedMask(float absZ, float3 modelPosition)
	{
		float soft = max(SubB.z, 0.5f);
		float soakZ = SubA.x + SubA.y + (EdgeNoise(modelPosition) - 0.5f) * soft;
		return 1.0f - smoothstep(soakZ - soft, soakZ + soft, absZ);
	}

	/** @brief 1 above the current water surface. */
	float AboveWaterMask(float absZ)
	{
		return SubA.z < -1.0e8f ? 1.0f : smoothstep(SubA.z + 0.5f, SubA.z + 2.0f, absZ);
	}

	/** @brief 1 on the part rain has reached, starting at the head and moving down. */
	float RainMask(float absZ)
	{
		float height = max(SubD.z, 1.0f);
		float along = saturate((absZ - SubA.x) / height);
		float front = 1.0f - SubD.x;
		return smoothstep(front - 0.05f, front + 0.02f, along) * smoothstep(0.02f, 0.1f, SubD.x);
	}

	/** @brief Broad coat wetness for this pixel, 0..1. Water darkens up to the soak line. Rain darkening follows the droplets. */
	float Coat(float absZ, float3 modelPosition, float materialScale)
	{
		float coat = 0.0f;
		[branch] if (Active())
		{
			float water = SubD.w > 0.5f ? SubA.w : SubA.w * SubmergedMask(absZ, modelPosition);
			float rain = SubD.y * RainMask(absZ);
			coat = saturate(max(water, rain) * SubC.x * materialScale);
		}
		return coat;
	}

#	if defined(CHARACTER_RAIN_SURFACE)
	/** @brief Beads/rivulets on the formerly submerged part, above the water. Same output as CharacterRainSpots::Evaluate. */
	float3 Drops(float3 modelPosition, float3 relativeWorldPosition, float3 worldNormal, float absZ, bool heldWeapon)
	{
		float3 water = 0.0f;
		[branch] if (!Active() || SubB.x <= 0.0f)
			return water;
		float region = max(RainMask(absZ), SubmergedMask(absZ, modelPosition)) * AboveWaterMask(absZ);
		float3 modelDx = ddx(modelPosition);
		float3 modelDy = ddy(modelPosition);
		float3 worldDx = ddx(relativeWorldPosition);
		float3 worldDy = ddy(relativeWorldPosition);
		float footprint = max(length(worldDx), length(worldDy));
		float3 projectionWeights = abs(cross(modelDx, modelDy));
		projectionWeights /= max(max(projectionWeights.x, max(projectionWeights.y, projectionWeights.z)), 1e-5f);
		projectionWeights *= projectionWeights;
		projectionWeights *= projectionWeights;
		projectionWeights /= max(dot(projectionWeights, 1.0f.xxx), 1e-5f);
		[branch] if (region > 0.001f)
		{
			float density = saturate(max(SubD.x, SubB.x) * SubC.y);
			// After the body leaves the water, the same droplets run down and thin out with the coat.
			float flowSlope = SubB.y * smoothstep(0.05f, 0.45f, length(worldNormal.xy));
			[unroll] for (uint projectionIndex = 0u; projectionIndex < 3u; ++projectionIndex)
			{
				[branch] if (projectionWeights[projectionIndex] > 0.01f)
				{
					float2 surfacePosition = projectionIndex == 0u ? modelPosition.yz : (projectionIndex == 1u ? modelPosition.xz : modelPosition.xy);
					float2 surfaceDx = projectionIndex == 0u ? modelDx.yz : (projectionIndex == 1u ? modelDx.xz : modelDx.xy);
					float2 surfaceDy = projectionIndex == 0u ? modelDy.yz : (projectionIndex == 1u ? modelDy.xz : modelDy.xy);
					float determinant = surfaceDx.x * surfaceDy.y - surfaceDx.y * surfaceDy.x;
					float inverseDeterminant = abs(determinant) > 1e-8f ? rcp(determinant) : 0.0f;
					float3 surfaceU = (worldDx * surfaceDy.y - worldDy * surfaceDx.y) * inverseDeterminant;
					float3 surfaceV = (worldDy * surfaceDx.x - worldDx * surfaceDy.x) * inverseDeterminant;
					float3 surfaceMetric = abs(determinant) > 1e-8f ? float3(dot(surfaceU, surfaceU), dot(surfaceU, surfaceV), dot(surfaceV, surfaceV)) : float3(1, 0, 1);
					water += CharacterRainSpots::EvaluateProjection(surfacePosition, surfaceMetric, footprint, projectionIndex,
								 density, density, flowSlope, heldWeapon) *
					         projectionWeights[projectionIndex];
				}
			}
			water.xz *= region;
		}
		return saturate(water);
	}
#	endif
}

#endif
