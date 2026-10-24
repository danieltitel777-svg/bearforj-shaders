#include "SnowDeformation/Common.hlsli"

#define SD_INTERPOLATE(member) output.member = patch[0].member * bary.x + patch[1].member * bary.y + patch[2].member * bary.z

[domain("tri")]
ControlPoint main(PatchConstants constants, float3 bary : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch)
{
	ControlPoint output;

	precise float4 clipPosition = patch[0].Position * bary.x + patch[1].Position * bary.y + patch[2].Position * bary.z;
	precise float2 windowPosition = ToWindow(ToCameraRelative(clipPosition));
	float groundScale = GroundLayer1.z;
#if !defined(DEPTH_ONLY)
	float4 blendWeights1 = patch[0].LandBlendWeights1 * bary.x + patch[1].LandBlendWeights1 * bary.y + patch[2].LandBlendWeights1 * bary.z;
	float4 blendWeights2 = patch[0].LandBlendWeights2 * bary.x + patch[1].LandBlendWeights2 * bary.y + patch[2].LandBlendWeights2 * bary.z;
	groundScale = dot(blendWeights1, GroundLayer0) + dot(blendWeights2.xy, GroundLayer1.xy);
#endif
	precise float displacement = DisplacedSurface(windowPosition, groundScale);
	precise float4 displacedPosition = DisplaceClipPosition(clipPosition, displacement);
	output.Position = displacedPosition;

#if !defined(DEPTH_ONLY)
	SD_INTERPOLATE(TexCoord0);
	SD_INTERPOLATE(TBN0);
	SD_INTERPOLATE(TBN1);
	SD_INTERPOLATE(TBN2);
	SD_INTERPOLATE(LandBlendWeights1);
	SD_INTERPOLATE(LandBlendWeights2);
	SD_INTERPOLATE(WorldPosition);
	SD_INTERPOLATE(PreviousWorldPosition);
	SD_INTERPOLATE(Color);
	SD_INTERPOLATE(FogParam);
	SD_INTERPOLATE(ModelPosition);

	output.WorldPosition.z += displacement;
	output.PreviousWorldPosition.z += displacement;

	float gradientStep = GradientStep(windowPosition);
	float2 gradient = float2(
						  DisplacedSurface(windowPosition + float2(gradientStep, 0.0), groundScale) - DisplacedSurface(windowPosition - float2(gradientStep, 0.0), groundScale),
						  DisplacedSurface(windowPosition + float2(0.0, gradientStep), groundScale) - DisplacedSurface(windowPosition - float2(0.0, gradientStep), groundScale)) *
	                  (0.5 / gradientStep);

	float3 normal = float3(output.TBN0.z, output.TBN1.z, output.TBN2.z);
	[branch] if (dot(gradient, gradient) > 1e-8 && normal.z > 0.05 * length(normal))
	{
		float3 tangent = float3(output.TBN0.x, output.TBN1.x, output.TBN2.x);
		float3 bitangent = float3(output.TBN0.y, output.TBN1.y, output.TBN2.y);
		normal = normalize(normal);

		float3 displacedNormal = normalize(float3(normal.xy / normal.z - gradient, 1.0));
		float minZ = min(TessParams.w, normal.z);
		[flatten] if (displacedNormal.z < minZ)
			displacedNormal = float3(displacedNormal.xy * (sqrt(1.0 - minZ * minZ) / max(length(displacedNormal.xy), 1e-6)), minZ);
		float handedness = dot(cross(normal, tangent), bitangent) < 0.0 ? -1.0 : 1.0;
		float3 displacedTangent = tangent - displacedNormal * dot(displacedNormal, tangent);
		displacedTangent = dot(displacedTangent, displacedTangent) > 1e-8 ? normalize(displacedTangent) : normalize(cross(float3(0.0, 1.0, 0.0), displacedNormal));
		float3 displacedBitangent = cross(displacedNormal, displacedTangent) * handedness;

		output.TBN0 = float3(displacedTangent.x, displacedBitangent.x, displacedNormal.x);
		output.TBN1 = float3(displacedTangent.y, displacedBitangent.y, displacedNormal.y);
		output.TBN2 = float3(displacedTangent.z, displacedBitangent.z, displacedNormal.z);
	}
#endif

	return output;
}
