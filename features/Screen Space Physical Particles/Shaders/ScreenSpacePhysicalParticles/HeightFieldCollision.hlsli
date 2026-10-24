#ifndef SSPP_HEIGHT_FIELD_COLLISION_INCLUDED
#define SSPP_HEIGHT_FIELD_COLLISION_INCLUDED

// Samples SSPP's world-space top-down height texture and performs downward swept
// collision. Missing texels contain a large negative sentinel; texture Y is the
// inverse of world +Y; stored values and returned positions use world-space Z.
static const float SSPP_MISSING_HEIGHT = -90000.0f;
static const float SSPP_HEIGHT_FIELD_CONTACT_EPSILON = 0.05f;
// Reject discontinuities between unrelated layers as non-ground surfaces.
static const float SSPP_HEIGHT_FIELD_MIN_GROUND_NORMAL_Z = 0.25f;
static const float SSPP_HEIGHT_FIELD_MAX_GROUND_SLOPE_SQUARED = 15.0f;

// Summary: Bilinearly samples a top-down height field while ignoring missing texels.
// Args:
//   heightField: R32 texture containing absolute world-space Z.
//   available: Nonzero when the texture contains a completed capture.
//   center: World-space XY at the center of the texture.
//   halfExtent: World-space half-width represented by the texture.
//   texelSize: World units represented by one texel.
//   worldXY: World-space horizontal sample position.
//   surfaceHeight: Receives the interpolated world-space Z.
//   texelPosition: Receives the continuous source-texel coordinate.
//   dimensions: Receives the source texture dimensions.
// Returns: True when at least one valid texel contributes to the sample.
bool SSPP_SampleHeightFieldValue(
	Texture2D<float> heightField,
	uint available,
	float2 center,
	float halfExtent,
	float texelSize,
	float2 worldXY,
	out float surfaceHeight,
	out float2 texelPosition,
	out uint2 dimensions)
{
	surfaceHeight = 0.0f;
	texelPosition = 0.0f;
	dimensions = 0u;
	if (available == 0u || halfExtent <= 0.0f || texelSize <= 0.0f)
		return false;

	uint width, height;
	heightField.GetDimensions(width, height);
	if (width < 3u || height < 3u)
		return false;
	dimensions = uint2(width, height);

	texelPosition = float2(
		(worldXY.x - (center.x - halfExtent)) / texelSize - 0.5f,
		((center.y + halfExtent) - worldXY.y) / texelSize - 0.5f);
	int2 basePixel = int2(floor(texelPosition));
	if (any(basePixel < 0) || basePixel.x + 1 >= (int)width || basePixel.y + 1 >= (int)height)
		return false;

	float h00 = heightField.Load(int3(basePixel, 0));
	float h10 = heightField.Load(int3(basePixel + int2(1, 0), 0));
	float h01 = heightField.Load(int3(basePixel + int2(0, 1), 0));
	float h11 = heightField.Load(int3(basePixel + int2(1, 1), 0));
	float2 fraction = frac(texelPosition);
	float4 heights = float4(h00, h10, h01, h11);
	float4 weights = float4(
		(1.0f - fraction.x) * (1.0f - fraction.y),
		fraction.x * (1.0f - fraction.y),
		(1.0f - fraction.x) * fraction.y,
		fraction.x * fraction.y);
	float4 valid = step(SSPP_MISSING_HEIGHT + 1.0f, heights);
	weights *= valid;
	float weightSum = dot(weights, 1.0f);
	if (weightSum <= 1.0e-6f)
		return false;
	surfaceHeight = dot(heights, weights) / weightSum;
	return true;
}

// Summary: Reconstructs a world-space height-field normal from central differences.
// Args:
//   heightField: R32 texture containing absolute world-space Z.
//   texelSize: World units represented by one texel.
//   minimumNormalZ: Minimum accepted upward component.
//   texelPosition: Continuous source-texel coordinate.
//   dimensions: Source texture dimensions.
//   surfaceNormal: Receives the reconstructed world-space normal.
// Returns: True when the resulting normal meets the upward-facing threshold.
bool SSPP_ReconstructHeightFieldNormal(
	Texture2D<float> heightField,
	float texelSize,
	float minimumNormalZ,
	float2 texelPosition,
	uint2 dimensions,
	out float3 surfaceNormal)
{
	surfaceNormal = float3(0.0f, 0.0f, 1.0f);
	int2 centerPixel = clamp(
		int2(round(texelPosition)),
		int2(1, 1),
		int2(dimensions) - int2(2, 2));
	float left = heightField.Load(int3(centerPixel + int2(-1, 0), 0));
	float right = heightField.Load(int3(centerPixel + int2(1, 0), 0));
	float worldYPositive = heightField.Load(int3(centerPixel + int2(0, -1), 0));
	float worldYNegative = heightField.Load(int3(centerPixel + int2(0, 1), 0));
	if (min(min(left, right), min(worldYPositive, worldYNegative)) > SSPP_MISSING_HEIGHT)
	{
		float3 tangentX = float3(2.0f * texelSize, 0.0f, right - left);
		float3 tangentY = float3(0.0f, 2.0f * texelSize, worldYPositive - worldYNegative);
		surfaceNormal = normalize(cross(tangentX, tangentY));
	}
	return surfaceNormal.z >= minimumNormalZ;
}

// Summary: Reconstructs a height-field surface position and upward-facing normal.
// Args:
//   heightField: R32 texture containing absolute world-space Z.
//   available: Nonzero when the texture contains a completed capture.
//   center: World-space XY at the center of the texture.
//   halfExtent: World-space half-width represented by the texture.
//   texelSize: World units represented by one texel.
//   minimumNormalZ: Minimum accepted upward normal component.
//   worldXY: World-space horizontal sample position.
//   surfacePosition: Receives the world-space surface position.
//   surfaceNormal: Receives the world-space surface normal.
// Returns: True when both height and normal reconstruction succeed.
bool SSPP_SampleHeightField(
	Texture2D<float> heightField,
	uint available,
	float2 center,
	float halfExtent,
	float texelSize,
	float minimumNormalZ,
	float2 worldXY,
	out float3 surfacePosition,
	out float3 surfaceNormal)
{
	surfacePosition = float3(worldXY, 0.0f);
	surfaceNormal = float3(0.0f, 0.0f, 1.0f);
	float surfaceHeight;
	float2 texelPosition;
	uint2 dimensions;
	if (!SSPP_SampleHeightFieldValue(
			heightField,
			available,
			center,
			halfExtent,
			texelSize,
			worldXY,
			surfaceHeight,
			texelPosition,
			dimensions))
		return false;
	surfacePosition.z = surfaceHeight;
	return SSPP_ReconstructHeightFieldNormal(
		heightField, texelSize, minimumNormalZ, texelPosition, dimensions, surfaceNormal);
}

// Summary: Finds a downward particle crossing of a continuous ground-like height sheet.
// Args:
//   heightField: R32 texture containing absolute world-space Z.
//   available: Nonzero when the texture contains a completed capture.
//   center: World-space XY at the center of the texture.
//   halfExtent: World-space half-width represented by the texture.
//   texelSize: World units represented by one texel.
//   minimumNormalZ: Consumer-specific minimum upward normal component.
//   collisionBias: Distance from particle center to its collision bottom.
//   startPosition: World-space segment start.
//   endPosition: World-space segment end.
//   hitPosition: Receives the reconstructed world-space contact point.
//   hitNormal: Receives the reconstructed world-space surface normal.
// Returns: True for a valid downward visible-to-surface crossing or shallow recovery.
bool SSPP_SweepHeightField(
	Texture2D<float> heightField,
	uint available,
	float2 center,
	float halfExtent,
	float texelSize,
	float minimumNormalZ,
	float collisionBias,
	float3 startPosition,
	float3 endPosition,
	out float3 hitPosition,
	out float3 hitNormal)
{
	hitPosition = endPosition;
	hitNormal = float3(0.0f, 0.0f, 1.0f);
	if (endPosition.z >= startPosition.z)
		return false;

	float endHeight;
	float2 endTexelPosition;
	uint2 endDimensions;
	if (!SSPP_SampleHeightFieldValue(
			heightField,
			available,
			center,
			halfExtent,
			texelSize,
			endPosition.xy,
			endHeight,
			endTexelPosition,
			endDimensions))
		return false;
	float endBottom = endPosition.z - collisionBias;
	float endDistance = endBottom - endHeight;
	if (endDistance > SSPP_HEIGHT_FIELD_CONTACT_EPSILON)
		return false;
	float startBottom = startPosition.z - collisionBias;
	if (endHeight > startBottom + SSPP_HEIGHT_FIELD_CONTACT_EPSILON)
		return false;

	float3 endNormal;
	if (!SSPP_ReconstructHeightFieldNormal(
			heightField,
			texelSize,
			max(minimumNormalZ, SSPP_HEIGHT_FIELD_MIN_GROUND_NORMAL_Z),
			endTexelPosition,
			endDimensions,
			endNormal))
		return false;

	float startHeight;
	float2 startTexelPosition;
	uint2 startDimensions;
	bool startValid = SSPP_SampleHeightFieldValue(
			heightField,
			available,
			center,
			halfExtent,
			texelSize,
			startPosition.xy,
			startHeight,
			startTexelPosition,
			startDimensions);
	if (!startValid) {
		float hitT = saturate((startBottom - endHeight) /
			max(startBottom - endBottom, 1.0e-5f));
		hitPosition = float3(lerp(startPosition.xy, endPosition.xy, hitT), endHeight);
		hitNormal = endNormal;
		return true;
	}

	float startDistance = startBottom - startHeight;
	if (startDistance < -max(collisionBias, 2.0f * SSPP_HEIGHT_FIELD_CONTACT_EPSILON) ||
		startDistance - endDistance <= 1.0e-5f)
		return false;
	if (startDistance < 0.0f) {
		hitPosition = float3(startPosition.xy, startHeight);
		hitNormal = endNormal;
		return true;
	}

	float2 xyDelta = endPosition.xy - startPosition.xy;
	float heightDelta = max(
		abs(endHeight - startHeight) -
			2.0f * SSPP_HEIGHT_FIELD_CONTACT_EPSILON,
		0.0f);
	if (heightDelta * heightDelta >
		SSPP_HEIGHT_FIELD_MAX_GROUND_SLOPE_SQUARED * dot(xyDelta, xyDelta))
		return false;
	float hitT = saturate(startDistance / (startDistance - endDistance));
	hitPosition = lerp(startPosition, endPosition, hitT);
	hitPosition.z = lerp(startHeight, endHeight, hitT);
	hitNormal = endNormal;
	return true;
}

#endif
