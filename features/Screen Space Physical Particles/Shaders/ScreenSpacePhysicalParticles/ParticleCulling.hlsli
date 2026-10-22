#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_CULLING_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_CULLING_INCLUDED

// Provides camera-frustum sphere tests for GPU particle visibility. World positions
// are rebased by CameraPosAdjust before testing CameraViewProj clip planes.

// Summary: Tests whether a sphere lies completely behind one clip plane.
// Args:
//   plane: Homogeneous camera clip plane.
//   center: Camera-relative sphere center.
//   radiusSquared: Squared sphere radius in world units.
// Returns: True when the sphere is outside the plane.
bool SSPPSphereOutsidePlane(float4 plane, float3 center, float radiusSquared)
{
	float distance = dot(plane.xyz, center) + plane.w;
	return distance < 0.0f &&
		distance * distance > radiusSquared * dot(plane.xyz, plane.xyz);
}

// Summary: Tests a world-space sphere against all six camera-frustum planes.
// Args:
//   worldPosition: Absolute world-space sphere center.
//   radius: Nonnegative sphere radius in world units.
// Returns: True when the sphere intersects or lies inside the view frustum.
bool IsSphereVisible(float3 worldPosition, float radius)
{
	float3 center = worldPosition - FrameBuffer::CameraPosAdjust[0].xyz;
	float4 row0 = FrameBuffer::CameraViewProj[0][0];
	float4 row1 = FrameBuffer::CameraViewProj[0][1];
	float4 row2 = FrameBuffer::CameraViewProj[0][2];
	float4 row3 = FrameBuffer::CameraViewProj[0][3];
	float radiusSquared = radius * radius;
	return !SSPPSphereOutsidePlane(row3 + row0, center, radiusSquared) &&
		!SSPPSphereOutsidePlane(row3 - row0, center, radiusSquared) &&
		!SSPPSphereOutsidePlane(row3 + row1, center, radiusSquared) &&
		!SSPPSphereOutsidePlane(row3 - row1, center, radiusSquared) &&
		!SSPPSphereOutsidePlane(row2, center, radiusSquared) &&
		!SSPPSphereOutsidePlane(row3 - row2, center, radiusSquared);
}

#endif
