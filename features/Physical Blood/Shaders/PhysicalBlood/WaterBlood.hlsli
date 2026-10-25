#ifndef PHYSICAL_BLOOD_WATER_INCLUDED
#define PHYSICAL_BLOOD_WATER_INCLUDED

// Water-shader integration for the advected Physical Blood density field. Water
// vertices are camera-relative, while deposition and advection use absolute world
// XY. The helpers reconstruct world coordinates and apply smooth cubic sampling
// to hide the fixed simulation grid without ringing around dark dye.

Texture2D<float> PhysicalBloodWaterDensity : register(t66);
SamplerState PhysicalBloodLinearClampSampler : register(s12);
cbuffer PhysicalBloodWaterField : register(b10)
{
	float2 PhysicalBloodCenter;
	float2 PhysicalBloodPreviousCenter;
	float PhysicalBloodExtent;
	float PhysicalBloodFlowStep;
	float PhysicalBloodDiffusionBlend;
	float PhysicalBloodDecayMultiplier;
	float PhysicalBloodFieldPadding;
	float PhysicalBloodFlowOffsetX;
	float PhysicalBloodFlowOffsetY;
	float PhysicalBloodFlowWidth;
	float PhysicalBloodFlowHeight;
	uint PhysicalBloodDensityActive;
	float2 PhysicalBloodInverseDimensions;
};

// Summary: Tests whether the CPU-side activity tracker enabled water sampling.
// Args: None.
// Returns: True when the bound density field can contain visible blood.
bool HasPhysicalBloodWaterDensity()
{
	return PhysicalBloodDensityActive != 0u;
}

// Summary: Samples the density field with a separable cubic B-spline.
// Args:
//   uv: Normalized coordinates in the water-density field.
//   inverseDimensions: Reciprocal density texture dimensions.
// Returns: Smooth nonnegative blood density from four bilinear samples.
float SamplePhysicalBloodCubicBSpline(float2 uv, float2 inverseDimensions)
{
	// Four bilinear fetches reconstruct the positive separable cubic kernel.
	float2 size = rcp(inverseDimensions);
	float2 texelPosition = uv * size - 0.5f;
	float2 base = floor(texelPosition);
	float2 fraction = texelPosition - base;
	float2 fraction2 = fraction * fraction;
	float2 fraction3 = fraction2 * fraction;

	float2 weight0 = (1.0f - 3.0f * fraction + 3.0f * fraction2 - fraction3) / 6.0f;
	float2 weight1 = (4.0f - 6.0f * fraction2 + 3.0f * fraction3) / 6.0f;
	float2 weight2 = (1.0f + 3.0f * fraction + 3.0f * fraction2 - 3.0f * fraction3) / 6.0f;
	float2 weight3 = fraction3 / 6.0f;
	float2 group0 = weight0 + weight1;
	float2 group1 = weight2 + weight3;

	float2 uv0 = (base - 1.0f + weight1 / group0 + 0.5f) * inverseDimensions;
	float2 uv1 = (base + 1.0f + weight3 / group1 + 0.5f) * inverseDimensions;
	float2 halfTexel = 0.5f * inverseDimensions;
	uv0 = clamp(uv0, halfTexel, 1.0f - halfTexel);
	uv1 = clamp(uv1, halfTexel, 1.0f - halfTexel);

	float density00 = PhysicalBloodWaterDensity.SampleLevel(
		PhysicalBloodLinearClampSampler, float2(uv0.x, uv0.y), 0);
	float density10 = PhysicalBloodWaterDensity.SampleLevel(
		PhysicalBloodLinearClampSampler, float2(uv1.x, uv0.y), 0);
	float density01 = PhysicalBloodWaterDensity.SampleLevel(
		PhysicalBloodLinearClampSampler, float2(uv0.x, uv1.y), 0);
	float density11 = PhysicalBloodWaterDensity.SampleLevel(
		PhysicalBloodLinearClampSampler, float2(uv1.x, uv1.y), 0);
	return density00 * group0.x * group0.y + density10 * group1.x * group0.y +
		density01 * group0.x * group1.y + density11 * group1.x * group1.y;
}

// Summary: Samples blood density at a rendered water-surface position.
// Args:
//   cameraRelativePosition: Water position relative to Skyrim's render camera.
// Returns: Density at the corresponding absolute world XY, or zero outside the field.
float GetPhysicalBloodWaterDensity(float3 cameraRelativePosition)
{
	float2 worldPosition = cameraRelativePosition.xy + FrameBuffer::CameraPosAdjust.xy;
	float2 uv = (worldPosition - PhysicalBloodCenter) / max(PhysicalBloodExtent, 1.0f) + 0.5f;
	if (any(uv < 0.0f) || any(uv >= 1.0f))
		return 0.0f;
	return SamplePhysicalBloodCubicBSpline(uv, PhysicalBloodInverseDimensions);
}

#endif
