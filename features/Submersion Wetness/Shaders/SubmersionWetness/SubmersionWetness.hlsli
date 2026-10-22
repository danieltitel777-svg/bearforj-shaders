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
	};

	bool Active()
	{
		return SubB.w > 0.5f && (SubA.w > 0.0f || SubB.x > 0.0f);
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

	/** @brief Broad coat wetness for this pixel, 0..1. */
	float Coat(float absZ, float3 modelPosition, float materialScale)
	{
		float coat = 0.0f;
		[branch] if (Active())
			coat = saturate(SubA.w * SubmergedMask(absZ, modelPosition) * SubC.x * materialScale);
		return coat;
	}

#	if defined(CHARACTER_RAIN_SURFACE)
	/** @brief Beads/rivulets on the formerly submerged part, above the water. Same output as CharacterRainSpots::Evaluate. */
	float3 Drops(float3 modelPosition, float3 relativeWorldPosition, float3 worldNormal, float absZ, bool heldWeapon)
	{
		float3 water = 0.0f;
		[branch] if (!Active() || SubB.x <= 0.0f)
			return water;
		float region = SubmergedMask(absZ, modelPosition) * AboveWaterMask(absZ);
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
			float density = saturate(SubB.x * SubC.y);
			// Static beads. Rivulets are the upstream rain flow in CharacterRainSpots, and they run
			// only while precipitation is falling. Drying and wading leave droplets in place.
			float flowSlope = 0.0f;
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
