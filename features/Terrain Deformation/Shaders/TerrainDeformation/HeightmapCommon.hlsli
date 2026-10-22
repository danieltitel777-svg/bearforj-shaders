// Bearforj 0.6.4 Terrain Deformation heightmap.
// Design adapted from Dynamic Terrain Deformation by NearMidnightNow (NMN), GPL-3.0-only
// (https://github.com/maglarnet/Dynamic_Terrain_Deformation_Skse): prints are stamped by compute into a
// heightmap window around the player, an activity map gates tessellation, surfaces have profiles.
// Own reduced implementation: one 2048^2 R32_UINT window (2 units/texel, 4096 units wide), re-stamped each frame.
//
// Texel encoding (InterlockedMax-friendly): [31:18] dip (14 bit, 0..kMaxDip units)
//                                           [17:12] darkening (6 bit, 0..1)
//                                           [11:0]  rim (12 bit, 0..kMaxRim units)
// Rim only matters where dip == 0, and there darkening is 0 too, so max() over the packed value is a max of the rim.
#ifndef __TD_HEIGHTMAP_COMMON_HLSLI__
#define __TD_HEIGHTMAP_COMMON_HLSLI__

#define TD_HM_SIZE 2048
#define TD_HM_TEXEL 2.0
#define TD_ACT_SIZE 64
#define TD_ACT_CELL 32  // texels per activity cell (64 units)

static const float kMaxDip = 32.0;
static const float kMaxRim = 8.0;

uint TD_Encode(float a_dip, float a_dark, float a_rim)
{
	uint d = (uint)round(saturate(a_dip / kMaxDip) * 16383.0);
	uint k = (uint)round(saturate(a_dark) * 63.0);
	uint r = (uint)round(saturate(a_rim / kMaxRim) * 4095.0);
	return (d << 18) | (k << 12) | r;
}

// returns (dip, darkening, rim)
float3 TD_Decode(uint a_v)
{
	return float3((a_v >> 18) / 16383.0 * kMaxDip, ((a_v >> 12) & 63u) / 63.0, (a_v & 4095u) / 4095.0 * kMaxRim);
}

// Same oriented footprint as the 0.6 analytic loop. a_shoulder is 0 (steep) .. 1 (soft bowl), from the surface profile.
// Returns (dip, darkening, rim) for one print. Dip and rim barely overlap, so packing them for InterlockedMax is safe
// for a single print; where two prints overlap, the deeper dip wins and keeps its own rim/darkening.
float3 TD_PrintShape(float2 a_d, float a_yaw, float a_radius, float a_depth, float a_rim, float a_dark, float a_shoulder)
{
	float s, c;
	sincos(a_yaw, s, c);
	float2 l = float2(a_d.x * c - a_d.y * s, a_d.x * s + a_d.y * c);
	float2 q = l / float2(a_radius * 0.42, a_radius);
	q.x *= l.y < 0.0 ? 1.15 : 1.0;  // heel narrower than the toe
	float e = length(q);
	float dipStart = lerp(0.72, 0.35, saturate(a_shoulder));
	float dip = 1.0 - smoothstep(dipStart, 1.0, e);
	float rimShape = exp(-pow((e - 1.08) / 0.16, 2.0));
	return float3(a_depth * dip, dip * a_dark * saturate(a_depth / 4.0), a_depth * a_rim * rimShape);
}
#endif
