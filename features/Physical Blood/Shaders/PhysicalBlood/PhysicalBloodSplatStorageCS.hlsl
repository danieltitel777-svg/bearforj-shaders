#include "PhysicalBlood/PhysicalBloodCommon.hlsli"

// Advances the chronological splat-ring head by binary search and produces two
// indirect draws that preserve chronological order across ring wrap.

StructuredBuffer<PhysicalBloodOwnedSplat> OwnedSplats : register(t0);
RWStructuredBuffer<uint> SplatState : register(u0);
RWByteAddressBuffer SplatDrawArguments : register(u1);

cbuffer PhysicalBloodSharedRender : register(b1)
{
	uint VisibleSegmentBase;
	uint ParticleCapacity;
	uint RenderClass;
	uint OwnedSplatCapacity;

	float CurrentTime;
	float SplatLifetime;
	uint OwnedSplatSegment;
	float PhysicalBloodSharedRenderPadding;
};

// Summary: Writes one D3D11_DRAW_INSTANCED_INDIRECT_ARGS record.
// Args:
//   byteOffset: Raw destination offset.
//   instanceCount: Number of splat records in the draw.
// Returns: Nothing.
void StoreDrawArguments(uint byteOffset, uint instanceCount)
{
	SplatDrawArguments.Store(byteOffset + 0u, 4u);
	SplatDrawArguments.Store(byteOffset + 4u, instanceCount);
	SplatDrawArguments.Store(byteOffset + 8u, 0u);
	// StartInstanceLocation does not offset SV_InstanceID in D3D11. The vertex
	// shader maps each draw segment to its physical ring slots explicitly.
	SplatDrawArguments.Store(byteOffset + 12u, 0u);
}

// Summary: Finds the live chronological suffix and splits it into two ring draws.
// Args:
//   dispatchID: Unused single-thread dispatch identifier.
// Returns: Nothing.
[numthreads(1, 1, 1)]
void BuildDrawArgumentsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint writeSerial = SplatState[0];
	uint oldestSerial = SplatState[1];
	uint capacity = max(OwnedSplatCapacity, 1u);
	uint retained = writeSerial - oldestSerial;
	if (retained > capacity) {
		uint overwritten = retained - capacity;
		oldestSerial += overwritten;
		SplatState[3] += overwritten;
		retained = capacity;
	}

	// Spawn times are chronological even when a frame inserts in parallel. The
	// global lifetime therefore makes expiry monotonic and permits logarithmic
	// retirement instead of a single thread scanning the entire expired prefix.
	uint low = 0u;
	uint high = retained;
	float lifetime = max(SplatLifetime, 0.001f);
	[loop]
	while (low < high) {
		uint middle = low + (high - low) / 2u;
		uint serial = oldestSerial + middle;
		PhysicalBloodOwnedSplat splat = OwnedSplats[serial % capacity];
		bool expired = splat.Serial != serial + 1u ||
			!isfinite(splat.SpawnTime) ||
			CurrentTime >= splat.SpawnTime + lifetime;
		if (expired)
			low = middle + 1u;
		else
			high = middle;
	}
	oldestSerial += low;

	SplatState[1] = oldestSerial;
	uint liveCount = min(writeSerial - oldestSerial, capacity);
	SplatState[2] = liveCount;
	uint firstSlot = oldestSerial % capacity;
	uint firstCount = min(liveCount, capacity - firstSlot);
	StoreDrawArguments(0u, firstCount);
	StoreDrawArguments(16u, liveCount - firstCount);
}
