// Bearforj 0.6.4: clears and stamps the terrain deformation heightmap window (see HeightmapCommon.hlsli).
#include "TerrainDeformation/HeightmapCommon.hlsli"

struct Print
{
	float4 A;  // x, y (world), yaw, radius
	float4 B;  // depth (faded, material-scaled), rim fraction, darkening, shoulder
};

StructuredBuffer<Print> Prints : register(t0);  // [0] = header, A.x = count, B.zw = window origin
RWTexture2D<uint> Heightmap : register(u0);
RWTexture2D<uint> Activity : register(u1);

#if defined(CLEAR)
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TD_HM_SIZE))
		return;
	Heightmap[id.xy] = 0;
	if (all(id.xy < TD_ACT_SIZE))
		Activity[id.xy] = 0;
}
#else
// one 8x8 group per print, each thread loops over the print's bounding box in 8x8 steps
[numthreads(8, 8, 1)] void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
	uint count = (uint)Prints[0].A.x;
	uint index = gid.x + 1;
	if (index > count)
		return;
	Print p = Prints[index];
	float2 origin = Prints[0].B.zw;
	float reach = p.A.w * 1.5;
	int2 lo = max(int2(floor((p.A.xy - reach - origin) / TD_HM_TEXEL)), 0);
	int2 hi = min(int2(ceil((p.A.xy + reach - origin) / TD_HM_TEXEL)), TD_HM_SIZE - 1);
	if (any(lo > hi))
		return;
	[loop] for (int y = lo.y + (int)tid.y; y <= hi.y; y += 8)
	{
		[loop] for (int x = lo.x + (int)tid.x; x <= hi.x; x += 8)
		{
			float2 world = origin + (float2(x, y) + 0.5) * TD_HM_TEXEL;
			float3 v = TD_PrintShape(world - p.A.xy, p.A.z, p.A.w, p.B.x, p.B.y, p.B.z, p.B.w);
			if (all(v < 1e-3))
				continue;
			InterlockedMax(Heightmap[int2(x, y)], TD_Encode(v.x, v.y, v.z));
			uint2 cell = uint2(x, y) / (uint)TD_ACT_CELL;
			if (all(cell < (uint)TD_ACT_SIZE))
				InterlockedMax(Activity[cell], 1u);
		}
	}
}
#endif
