// Projects geometry and bounded water planes into SSPP's world-XY height fields.
// Color stores absolute world Z. Solid capture uses MAX blending; water capture
// uses top-down LAND depth to retain only exposed water.

cbuffer Capture : register(b0)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;
	float2 CaptureCenter;
	float CaptureHalfExtent;
	float CaptureTopZ;
	float CaptureBottomZ;
	float3 CapturePadding;
};

struct VS_INPUT
{
	float4 Position : POSITION0;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

struct WaterSurface
{
	float2 Minimum;
	float2 Maximum;
	float Height;
	float3 Padding;
};

// Summary: Converts world-space Z into top-down hardware depth.
// Args:
//   worldZ: Absolute world-space height.
// Returns: Depth with CaptureTopZ at zero and CaptureBottomZ at one.
float WorldHeightToDepth(float worldZ)
{
	return saturate((CaptureTopZ - worldZ) / max(CaptureTopZ - CaptureBottomZ, 1.0f));
}

#if defined(VSHADER) || defined(WATER_TERRAIN_VSHADER)
// Summary: Projects one Skyrim geometry vertex into the top-down capture.
// Args:
//   input: Object-space position supplied by the source geometry.
// Returns: Clip position and absolute world-space Z.
VS_OUTPUT main(VS_INPUT input)
{
	float3 worldPosition = float3(
		dot(WorldRow0.xyz, input.Position.xyz) + WorldRow0.w,
		dot(WorldRow1.xyz, input.Position.xyz) + WorldRow1.w,
		dot(WorldRow2.xyz, input.Position.xyz) + WorldRow2.w);
	float2 ndc = (worldPosition.xy - CaptureCenter) / CaptureHalfExtent;

	VS_OUTPUT output;
#if defined(WATER_TERRAIN_VSHADER)
	output.Position = float4(ndc, WorldHeightToDepth(worldPosition.z), 1.0f);
#else
	output.Position = float4(ndc, 0.5f, 1.0f);
#endif
	output.WorldZ = worldPosition.z;
	return output;
}
#endif

#if defined(WATER_VSHADER)
StructuredBuffer<WaterSurface> WaterSurfaces : register(t0);

// Summary: Expands one bounded water surface into a top-down quad.
// Args:
//   vertexID: Procedural triangle-list vertex index in the range [0, 5].
//   instanceID: Index of the water surface to expand.
// Returns: Clip position and absolute water height.
VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	static const float2 corners[6] = {
		float2(0.0f, 0.0f),
		float2(1.0f, 0.0f),
		float2(0.0f, 1.0f),
		float2(0.0f, 1.0f),
		float2(1.0f, 0.0f),
		float2(1.0f, 1.0f)
	};
	WaterSurface surface = WaterSurfaces[instanceID];
	float2 worldXY = lerp(surface.Minimum, surface.Maximum, corners[vertexID]);
	float2 ndc = (worldXY - CaptureCenter) / CaptureHalfExtent;

	VS_OUTPUT output;
	output.Position = float4(ndc, WorldHeightToDepth(surface.Height), 1.0f);
	output.WorldZ = surface.Height;
	return output;
}
#endif

#if defined(PSHADER) || defined(WATER_PSHADER)
// Summary: Stores absolute world height for fragments inside the vertical capture range.
// Args:
//   input: Interpolated top-down position and world height.
// Returns: Absolute world-space Z for the height target.
float main(VS_OUTPUT input) : SV_Target0
{
	if (input.WorldZ > CaptureTopZ || input.WorldZ < CaptureBottomZ)
		discard;
	return input.WorldZ;
}
#endif
