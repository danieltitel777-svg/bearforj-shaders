Texture2D<float> Density : register(t0);
Texture2D<uint> Sources : register(t1);
Texture2D<uint> MaskIn : register(t2);

RWTexture2D<uint> MaskOut : register(u0);
RWStructuredBuffer<uint> TileIDs : register(u1);
RWBuffer<uint> DispatchArgs : register(u2);

// 1024 density texels / 8 threads = 128 tiles. One tile of halo covers flow and
// diffusion, which move about one texel per update.

static const uint TILE_COUNT = 128u;
static const uint TILE_TEXELS = 8u;

// Summary: Marks every 8x8 tile that currently holds density or a new source.
// Args:
//   id: Tile coordinate in the 128x128 tile grid.
// Returns: Nothing. Writes 1 into MaskOut for occupied tiles.
[numthreads(8, 8, 1)]
void MarkMain(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TILE_COUNT))
		return;

	uint2 origin = id.xy * TILE_TEXELS;
	uint active = 0u;
	[unroll]
	for (uint y = 0; y < TILE_TEXELS; ++y) {
		[unroll]
		for (uint x = 0; x < TILE_TEXELS; ++x) {
			uint2 texel = origin + uint2(x, y);
			if (Density.Load(int3(texel, 0)) > 0.0f || Sources.Load(int3(texel, 0)) != 0u)
				active = 1u;
		}
	}
	MaskOut[id.xy] = active;
}

// Summary: Dilates the occupied-tile mask by one tile.
// Args:
//   id: Tile coordinate in the 128x128 tile grid.
// Returns: Nothing. Writes the dilated mask.
[numthreads(8, 8, 1)]
void HaloMain(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TILE_COUNT))
		return;

	uint active = 0u;
	[unroll]
	for (int y = -1; y <= 1; ++y) {
		[unroll]
		for (int x = -1; x <= 1; ++x) {
			int2 tile = int2(id.xy) + int2(x, y);
			if (all(tile >= 0) && all(tile < int2(TILE_COUNT, TILE_COUNT)) &&
				MaskIn.Load(int3(tile, 0)) != 0u)
				active = 1u;
		}
	}
	MaskOut[id.xy] = active;
}

// Summary: Packs occupied tile ids and the indirect group count.
// Args:
//   id: Tile coordinate in the 128x128 tile grid.
// Returns: Nothing. DispatchArgs.x becomes the live tile count.
[numthreads(8, 8, 1)]
void CompactMain(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TILE_COUNT))
		return;
	if (MaskIn.Load(int3(id.xy, 0)) == 0u)
		return;

	uint slot;
	InterlockedAdd(DispatchArgs[0], 1u, slot);
	TileIDs[slot] = id.x | (id.y << 7u);
}
