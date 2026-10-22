#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_COLLISION_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_COLLISION_INCLUDED

// Reconstructs world surfaces and sweeps particles against SSPP's actor-free camera
// depth. Consumers declare SceneDepth and SceneDepthAvailable before inclusion.
// SceneDepth contains non-linear hardware depth; clear depth is not a valid surface.
static const float DEPTH_CLEAR_THRESHOLD = 0.9999999f;
static const float DEPTH_COLLISION_EPSILON = 0.000001f;

// Summary: Reconstructs an absolute world position from one hardware-depth texel.
// Args:
//   pixel: Pixel in the dynamic-resolution depth texture.
//   depth: Non-linear hardware depth in the range [0, 1).
//   dimensions: Depth texture dimensions.
// Returns: Absolute world-space position represented by the texel.
float3 ReconstructScenePosition(uint2 pixel, float depth, uint2 dimensions)
{
	float2 uvDynamic = (float2(pixel) + 0.5f) / float2(dimensions);
	float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(uvDynamic);
	float4 positionCS = float4(2.0f * float2(uv.x, 1.0f - uv.y) - 1.0f, depth, 1.0f);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse[0], positionCS);
	positionMS.xyz /= max(abs(positionMS.w), 1e-5f);
	return positionMS.xyz + FrameBuffer::CameraPosAdjust[0].xyz;
}

// Summary: Reconstructs a camera-facing world-space normal from neighboring depth.
// Args:
//   pixel: Center pixel in the dynamic-resolution depth texture.
//   centerDepth: Occupied hardware depth at the center pixel.
//   centerPosition: Reconstructed absolute world position at the center pixel.
//   dimensions: Depth texture dimensions.
//   surfaceNormal: Receives a normalized world-space surface normal.
// Returns: True when valid neighbors form two nondegenerate tangents.
bool ReconstructSceneNormal(
	uint2 pixel,
	float centerDepth,
	float3 centerPosition,
	uint2 dimensions,
	out float3 surfaceNormal)
{
	surfaceNormal = float3(0.0f, 0.0f, 1.0f);
	uint width = dimensions.x;
	uint height = dimensions.y;
	if (width < 2u || height < 2u)
		return false;

	bool leftValid = pixel.x > 0u;
	bool rightValid = pixel.x + 1u < width;
	bool upValid = pixel.y > 0u;
	bool downValid = pixel.y + 1u < height;
	uint2 leftPixel = uint2(pixel.x - (leftValid ? 1u : 0u), pixel.y);
	uint2 rightPixel = uint2(pixel.x + (rightValid ? 1u : 0u), pixel.y);
	uint2 upPixel = uint2(pixel.x, pixel.y - (upValid ? 1u : 0u));
	uint2 downPixel = uint2(pixel.x, pixel.y + (downValid ? 1u : 0u));
	float leftDepth = leftValid ? SceneDepth.Load(int3(leftPixel, 0)) : 1.0f;
	float rightDepth = rightValid ? SceneDepth.Load(int3(rightPixel, 0)) : 1.0f;
	float upDepth = upValid ? SceneDepth.Load(int3(upPixel, 0)) : 1.0f;
	float downDepth = downValid ? SceneDepth.Load(int3(downPixel, 0)) : 1.0f;
	leftValid = leftValid && leftDepth < DEPTH_CLEAR_THRESHOLD;
	rightValid = rightValid && rightDepth < DEPTH_CLEAR_THRESHOLD;
	upValid = upValid && upDepth < DEPTH_CLEAR_THRESHOLD;
	downValid = downValid && downDepth < DEPTH_CLEAR_THRESHOLD;
	if ((!leftValid && !rightValid) || (!upValid && !downValid))
		return false;

	// Choose the depth-nearest neighbor on each axis to avoid silhouette backgrounds.
	bool useRight = rightValid && (!leftValid || abs(rightDepth - centerDepth) <= abs(leftDepth - centerDepth));
	bool useDown = downValid && (!upValid || abs(downDepth - centerDepth) <= abs(upDepth - centerDepth));
	float3 tangentX = useRight ?
		ReconstructScenePosition(rightPixel, rightDepth, dimensions) - centerPosition :
		centerPosition - ReconstructScenePosition(leftPixel, leftDepth, dimensions);
	float3 tangentY = useDown ?
		ReconstructScenePosition(downPixel, downDepth, dimensions) - centerPosition :
		centerPosition - ReconstructScenePosition(upPixel, upDepth, dimensions);
	float3 normal = cross(tangentX, tangentY);
	if (dot(normal, normal) <= 1.0e-8f)
		return false;

	surfaceNormal = normalize(normal);
	if (dot(surfaceNormal, FrameBuffer::CameraPosAdjust[0].xyz - centerPosition) < 0.0f)
		surfaceNormal = -surfaceNormal;
	return true;
}

// Summary: Tests one particle clip position against occupied actor-free depth.
// Args:
//   clipPosition: Particle position in camera clip space.
//   dimensions: Depth texture dimensions.
//   pixel: Receives the sampled depth pixel.
//   sceneDepth: Receives the occupied hardware depth.
//   sampleValid: Receives true only for an in-frame, non-clear depth sample.
// Returns: True when the particle lies behind the sampled scene surface.
bool SampleSceneOcclusion(
	float4 clipPosition,
	uint2 dimensions,
	out uint2 pixel,
	out float sceneDepth,
	out bool sampleValid)
{
	pixel = 0u;
	sceneDepth = 1.0f;
	sampleValid = false;
	if (SceneDepthAvailable == 0u || dimensions.x == 0u || dimensions.y == 0u)
		return false;
	if (clipPosition.w <= 0.01f)
		return false;

	float2 uv = clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f;
	if (FrameBuffer::IsOutsideFrame(uv))
		return false;

	uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
	pixel = min(uint2(uv * float2(dimensions)), dimensions - 1u);
	sceneDepth = SceneDepth.Load(int3(pixel, 0));
	if (sceneDepth >= DEPTH_CLEAR_THRESHOLD)
		return false;
	sampleValid = true;
	float particleDepth = clipPosition.z / clipPosition.w;
	return particleDepth > sceneDepth + DEPTH_COLLISION_EPSILON;
}

// Summary: Finds the first valid visible-to-occluded crossing along a world-space segment.
// Args:
//   startPosition: Absolute world-space segment start.
//   endPosition: Absolute world-space segment end.
//   contactOffset: Nonnegative camera-ray offset applied to depth probes.
//   hitTime: Receives normalized contact time along the segment.
//   hitPosition: Receives the reconstructed world-space scene contact.
//   hitNormal: Receives the reconstructed camera-facing world-space normal.
// Returns: True when an occupied-depth crossing is found and refined.
bool SweepScene(
	float3 startPosition,
	float3 endPosition,
	float contactOffset,
	out float hitTime,
	out float3 hitPosition,
	out float3 hitNormal)
{
	hitTime = 1.0f;
	hitPosition = endPosition;
	hitNormal = float3(0.0f, 0.0f, 1.0f);
	if (SceneDepthAvailable == 0u)
		return false;
	uint width, height;
	SceneDepth.GetDimensions(width, height);
	uint2 dimensions = uint2(width, height);
	if (width == 0u || height == 0u)
		return false;
	float3 cameraPosition = FrameBuffer::CameraPosAdjust[0].xyz;
	float3 startRay = startPosition - cameraPosition;
	float3 endRay = endPosition - cameraPosition;
	contactOffset = max(contactOffset, 0.0f);
	float4 startClip = mul(
		FrameBuffer::CameraViewProj[0],
		float4(startRay + startRay * rsqrt(max(dot(startRay, startRay), 1.0e-6f)) * contactOffset, 1.0f));
	float4 endClip = mul(
		FrameBuffer::CameraViewProj[0],
		float4(endRay + endRay * rsqrt(max(dot(endRay, endRay), 1.0e-6f)) * contactOffset, 1.0f));
	float segmentLength = length(endPosition - startPosition);
	uint steps = clamp((uint)ceil(segmentLength / 2.0f), 4u, 24u);
	uint2 ignoredPixel;
	float ignoredDepth;
	bool previousValid;
	bool previousOccluded = SampleSceneOcclusion(
		startClip, dimensions, ignoredPixel, ignoredDepth, previousValid);
	bool previousVisible = previousValid && !previousOccluded;
	float previousT = 0.0f;
	[loop]
	for (uint step = 1u; step <= steps; ++step) {
		float t = (float)step / (float)steps;
		bool sampleValid;
		bool occluded = SampleSceneOcclusion(
			lerp(startClip, endClip, t), dimensions, ignoredPixel, ignoredDepth, sampleValid);
		if (sampleValid && occluded && previousVisible) {
			float low = previousT;
			float high = t;
			uint2 highPixel = ignoredPixel;
			float highDepth = ignoredDepth;
			[unroll]
			for (uint refine = 0u; refine < 4u; ++refine) {
				float middle = (low + high) * 0.5f;
				uint2 middlePixel;
				float middleDepth;
				bool middleValid;
				bool middleOccluded = SampleSceneOcclusion(
					lerp(startClip, endClip, middle), dimensions, middlePixel, middleDepth, middleValid);
				if (!middleValid)
					return false;
				if (middleOccluded) {
					high = middle;
					highPixel = middlePixel;
					highDepth = middleDepth;
				} else {
					low = middle;
				}
			}
			hitTime = high;
			hitPosition = ReconstructScenePosition(highPixel, highDepth, dimensions);
			ReconstructSceneNormal(
				highPixel, highDepth, hitPosition, dimensions, hitNormal);
			return true;
		}
		previousVisible = sampleValid && !occluded;
		if (previousVisible)
			previousT = t;
	}
	return false;
}

#endif
