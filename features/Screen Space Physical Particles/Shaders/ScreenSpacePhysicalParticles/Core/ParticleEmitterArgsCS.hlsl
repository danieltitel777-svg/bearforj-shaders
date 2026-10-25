#include "ScreenSpacePhysicalParticles/Core/ParticleEmitterCommon.hlsli"

// Initializes per-frame emitter counters and derives emission dispatch arguments
// from the bounded chunk count produced by the planner.
RWByteAddressBuffer IndirectArguments : register(u0);
RWStructuredBuffer<uint> Counters : register(u7);

[numthreads(1, 1, 1)]
// Summary: Seeds frame requests from CPU bursts and clears emitter outputs.
// Args:
//   dispatchID: Unused single-thread dispatch ID.
// Returns: None.
void ResetEmitterFrameMain(uint3 dispatchID : SV_DispatchThreadID)
{
	Counters[SSPPFrameRequestCounter] = min(SSPPCPURequestCount, SSPPMaxFrameRequests);
	Counters[SSPPEmissionChunkCounter] = 0u;
	Counters[SSPPActiveEmitterCounter] = 0u;
}

[numthreads(1, 1, 1)]
// Summary: Builds the one-dimensional indirect dispatch for emission chunks.
// Args:
//   dispatchID: Unused single-thread dispatch ID.
// Returns: None.
void BuildEmissionArgsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint chunkCount = min(Counters[SSPPEmissionChunkCounter], SSPPMaxEmissionChunks);
	IndirectArguments.Store(0u, chunkCount);
	IndirectArguments.Store(4u, 1u);
	IndirectArguments.Store(8u, 1u);
}
