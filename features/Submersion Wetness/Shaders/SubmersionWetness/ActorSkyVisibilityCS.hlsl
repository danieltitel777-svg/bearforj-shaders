// Bearforj Submersion Wetness: sky (precipitation) visibility above each tracked actor's head,
// from the Skylighting probe array - the same occlusion Wetness Effects uses for surfaces.
#define SKYLIGHTING_PROBE_REGISTER t1
#include "Skylighting/Skylighting.hlsli"

StructuredBuffer<float4> ActorPositions : register(t0);  // xyz camera-relative head position, w valid
RWStructuredBuffer<float> Visibility : register(u0);

cbuffer ActorCB : register(b0)
{
	uint ActorCount;
	uint3 ActorPad;
};

[numthreads(64, 1, 1)] void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= ActorCount)
		return;
	float4 p = ActorPositions[tid.x];
	float vis = 1.0;
	if (p.w > 0.5) {
		// sample a little above the head and around the shoulders so a porch roof edge counts
		vis = 0.0;
		[unroll] for (int i = 0; i < 3; i++) {
			float3 offset = float3(i == 1 ? 24.0 : (i == 2 ? -24.0 : 0.0), 0.0, 24.0);
			sh2 sh = Skylighting::SampleNoBias(p.xyz + offset);
			vis += saturate(SphericalHarmonics::Unproject(sh, float3(0, 0, 1)));
		}
		vis /= 3.0;
	}
	Visibility[tid.x] = vis;
}
