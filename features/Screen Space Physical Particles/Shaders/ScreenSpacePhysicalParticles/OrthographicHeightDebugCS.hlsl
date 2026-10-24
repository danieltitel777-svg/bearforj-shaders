// Converts solid or water world-height textures into stable, world-oriented
// grayscale previews. Missing-height sentinel texels are rendered black.
Texture2D<float> SourceHeight : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer Debug : register(b0)
{
	float BottomZ;
	float TopZ;
	float MissingHeight;
	float WaterMode;
};

// Summary: Writes one orthographic height preview pixel.
// Args:
//   dispatchID: Output pixel selected by the compute dispatch.
// Returns: None.
[numthreads(8, 8, 1)]
void Main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint outputWidth, outputHeight;
	Output.GetDimensions(outputWidth, outputHeight);
	if (dispatchID.x >= outputWidth || dispatchID.y >= outputHeight)
		return;

	uint sourceWidth, sourceHeight;
	SourceHeight.GetDimensions(sourceWidth, sourceHeight);
	float2 outputUV = (float2(dispatchID.xy) + 0.5f) / float2(outputWidth, outputHeight);
	// +X maps right and +Y maps up; the UI overlays player orientation.
	float2 sourceUV = outputUV;
	uint2 sourcePixel = min(
		uint2(sourceUV * float2(sourceWidth, sourceHeight)),
		uint2(sourceWidth - 1u, sourceHeight - 1u));
	float height = SourceHeight.Load(int3(sourcePixel, 0));
	if (!isfinite(height) || height <= MissingHeight + 1.0f)
	{
		Output[dispatchID.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}

	if (WaterMode > 0.5f)
	{
		float range = TopZ - BottomZ;
		float value = range > 0.5f ? saturate((height - BottomZ) / range) : 0.5f;
		// Keep a flat water field visible when its height range is zero.
		float gray = range > 0.5f ? lerp(0.2f, 1.0f, value) : 0.65f;
		Output[dispatchID.xy] = float4(gray.xxx, 1.0f);
		return;
	}

	// Logarithmic depth preserves nearby elevation detail across the full range.
	float verticalRange = max(TopZ - BottomZ, 1.0f);
	float depthBelowTop = clamp(TopZ - height, 0.0f, verticalRange);
	float normalizedDepth = log2(1.0f + depthBelowTop) / log2(1.0f + verticalRange);
	float visibleHeight = 1.0f - saturate(normalizedDepth);
	Output[dispatchID.xy] = float4(visibleHeight.xxx, 1.0f);
}
