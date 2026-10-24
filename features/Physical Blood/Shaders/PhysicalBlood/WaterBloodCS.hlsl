Texture2D<float> PreviousDensity : register(t0);
Texture2D<float4> Flowmap : register(t1);
Texture2D<uint> Sources : register(t2);
RWTexture2D<float> OutputDensity : register(u0);
RWTexture1D<uint> OutputActivity : register(u1);
SamplerState LinearClampSampler : register(s0);

// Evolves the camera-centered water-density field. The pass recenters by world
// coordinates, backtraces authored flow with a midpoint sample, applies isotropic
// diffusion and exponential decay, adds fixed-point impact sources, and emits a
// one-word activity signal. All field positions use absolute world XY.

cbuffer WaterField : register(b0)
{
	float2 Center;
	float2 PreviousCenter;
	float Extent;
	float FlowStep;
	float DiffusionBlend;
	float DecayMultiplier;
	float Padding0;
	float FlowOffsetX;
	float FlowOffsetY;
	float FlowWidth;
	float FlowHeight;
	uint DensityActive;
	float2 InverseDimensions;
};

groupshared uint GroupDensityActive;
groupshared uint2 GroupFlowDimensions;
// Minimum density represented by one fixed-point source unit.
static const float MIN_ACTIVE_DENSITY = 1.0f / 65536.0f;

// Summary: Samples the previous field, treating coordinates outside it as empty.
// Args:
//   uv: Normalized coordinates in the previous camera-centered field.
//   halfTexel: Half the reciprocal field dimensions for clamped sampling.
// Returns: Previous density at the requested coordinate, or zero outside the field.
float LoadPrevious(float2 uv, float2 halfTexel)
{
	if (any(uv < 0.0f) || any(uv >= 1.0f))
		return 0.0f;
	return PreviousDensity.SampleLevel(LinearClampSampler, clamp(uv, halfTexel, 1.0f - halfTexel), 0);
}

// Summary: Samples authored water flow and converts it to world-space displacement.
// Args:
//   worldPosition: Absolute world-space XY sample position.
//   flowDimensions: Dimensions of the currently bound flowmap.
// Returns: Per-update XY displacement, or zero where flow data is unavailable.
float2 GetFlowVelocity(float2 worldPosition, uint2 flowDimensions)
{
	if (FlowStep <= 0.0f || FlowWidth <= 0.0f || FlowHeight <= 0.0f ||
		flowDimensions.x == 0u || flowDimensions.y == 0u)
		return 0.0f;

	float2 flowUV = float2(
		(worldPosition.x / 4096.0f + FlowOffsetX) / FlowWidth,
		1.0f - (worldPosition.y / 4096.0f + FlowOffsetY) / FlowHeight);
	if (any(flowUV < 0.0f) || any(flowUV >= 1.0f))
		return 0.0f;

	float2 flowHalfTexel = 0.5f / float2(flowDimensions);
	float4 flow = Flowmap.SampleLevel(
		LinearClampSampler,
		clamp(flowUV, flowHalfTexel, 1.0f - flowHalfTexel),
		0);
	float2 direction = flow.xy * 2.0f - 1.0f;
	float directionLength = length(direction);
	if (directionLength <= 0.05f)
		return 0.0f;
	// The Y-flipped address mapping places RG in the density field's XY basis.
	return direction / directionLength * FlowStep * flow.w;
}

// Summary: Advances one water-density texel and contributes to the activity flag.
// Args:
//   dispatchID: Global compute-thread coordinates selecting the output texel.
//   groupIndex: Linear thread index used for group-shared initialization and reduction.
// Returns: Nothing; writes OutputDensity and may set OutputActivity.
[numthreads(8, 8, 1)]
void AdvectMain(uint3 dispatchID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
	if (groupIndex == 0u) {
		GroupDensityActive = 0u;
		GroupFlowDimensions = 0u;
		if (FlowWidth > 0.0f && FlowHeight > 0.0f) {
			uint flowWidth, flowHeight;
			Flowmap.GetDimensions(flowWidth, flowHeight);
			GroupFlowDimensions = uint2(flowWidth, flowHeight);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	float2 texel = InverseDimensions;
	float2 halfTexel = 0.5f * texel;
	float2 uv = (float2(dispatchID.xy) + 0.5f) * texel;
	float2 worldPosition = Center + (uv - 0.5f) * Extent;

	float2 displacement = GetFlowVelocity(worldPosition, GroupFlowDimensions);
	float2 midpointDisplacement = displacement;
	[branch]
	if (any(displacement != 0.0f)) {
		float2 midpointWorld = worldPosition - displacement * 0.5f;
		midpointDisplacement = GetFlowVelocity(midpointWorld, GroupFlowDimensions);
	}
	float2 previousWorld = worldPosition - midpointDisplacement;
	float2 previousUV = (previousWorld - PreviousCenter) / Extent + 0.5f;
	float density = LoadPrevious(previousUV, halfTexel);
	[branch]
	if (DiffusionBlend > 0.0f) {
		float cardinalNeighbors = LoadPrevious(previousUV + float2(texel.x, 0), halfTexel) +
			LoadPrevious(previousUV - float2(texel.x, 0), halfTexel) +
			LoadPrevious(previousUV + float2(0, texel.y), halfTexel) +
			LoadPrevious(previousUV - float2(0, texel.y), halfTexel);
		float diagonalNeighbors = LoadPrevious(previousUV + texel, halfTexel) +
			LoadPrevious(previousUV - texel, halfTexel) +
			LoadPrevious(previousUV + float2(texel.x, -texel.y), halfTexel) +
			LoadPrevious(previousUV + float2(-texel.x, texel.y), halfTexel);
		float isotropicNeighborhood = (cardinalNeighbors * 4.0f + diagonalNeighbors) / 20.0f;
		density = lerp(density, isotropicNeighborhood, DiffusionBlend);
	}
	density += (float)Sources.Load(int3(dispatchID.xy, 0)) / 65536.0f;
	density = clamp(density * DecayMultiplier, 0.0f, 16.0f);
	density = density >= MIN_ACTIVE_DENSITY ? density : 0.0f;
	OutputDensity[dispatchID.xy] = density;
	if (density > 0.0f)
		InterlockedOr(GroupDensityActive, 1u);

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex == 0u && GroupDensityActive != 0u)
		InterlockedOr(OutputActivity[0], 1u);
}
