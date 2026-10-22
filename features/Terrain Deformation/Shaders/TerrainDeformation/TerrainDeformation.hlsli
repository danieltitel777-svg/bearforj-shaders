#ifndef __TERRAIN_DEFORMATION_HLSLI__
#define __TERRAIN_DEFORMATION_HLSLI__
// Bearforj Terrain Deformation (0.6): footprints and actor trails in snow, mud, sand and dirt.
// 0.6.4: prints are stamped by compute into a heightmap window (HeightmapCommon.hlsli), replacing the 511-print loop.
// The same buffer is read by the landscape domain shader (real displacement through tessellation) and by the
// landscape pixel shader (normal perturbation + darkening, also the fallback when tessellation is off).
//
// Prints[0] is a header:
//   A = (count, maxTessFactor, tessRange, displacementScale)
//   B = (normalStrength, tessellationEnabled, window origin x, window origin y)

#include "TerrainDeformation/HeightmapCommon.hlsli"

namespace TerrainDeformation
{
	struct Print
	{
		float4 A;
		float4 B;
	};

	// 0.6.4: only Prints[0] (header) is read here; prints are stamped into Heightmap by StampCS.
	//   A = (count, maxTessFactor, tessRange, displacementScale)
	//   B = (normalStrength, tessellationEnabled, window origin x, window origin y)
	StructuredBuffer<Print> Prints : register(t54);  // Bearforj: t123 clashed with TreeWindSpring
	Texture2D<uint> Heightmap : register(t56);
	Texture2D<uint> Activity : register(t57);

	uint Count() { return min((uint)Prints[0].A.x, 511u); }
	float MaxTessFactor() { return Prints[0].A.y; }
	float TessRange() { return Prints[0].A.z; }
	float DisplacementScale() { return Prints[0].A.w; }
	float NormalStrength() { return Prints[0].B.x; }
	bool TessellationEnabled() { return Prints[0].B.y > 0.5; }
	float2 Origin() { return Prints[0].B.zw; }

	float3 Fetch(int2 a_texel)
	{
		if (any(a_texel < 0) || any(a_texel >= TD_HM_SIZE))
			return 0.0;
		return TD_Decode(Heightmap.Load(int3(a_texel, 0)));
	}

	// true when the 64-unit cell under a_positionWS (absolute world) holds a print (DTD-style activity gate)
	bool Active(float2 a_positionWS)
	{
		int2 cell = int2(floor((a_positionWS - Origin()) / (TD_HM_TEXEL * TD_ACT_CELL)));
		if (any(cell < 0) || any(cell >= TD_ACT_SIZE))
			return false;
		return Activity.Load(int3(cell, 0)) != 0;
	}

	// a_positionWS: absolute world position. Returns (height offset, <0 inside a print; darkening weight).
	float2 Height(float3 a_positionWS)
	{
		if (Count() == 0)
			return 0.0;
		float2 t = (a_positionWS.xy - Origin()) / TD_HM_TEXEL - 0.5;
		int2 i = int2(floor(t));
		float2 f = t - i;
		float3 v = lerp(lerp(Fetch(i), Fetch(i + int2(1, 0)), f.x), lerp(Fetch(i + int2(0, 1)), Fetch(i + int2(1, 1)), f.x), f.y);
		float height = -v.x + v.z * (v.x > 1e-3 ? 0.25 : 1.0);
		return float2(height, v.y);
	}

#	if defined(PSHADER)
	// Perturbs a world normal with the print height field (Mikkelsen surface gradient).
	// a_displaced: the geometry was already displaced by the domain shader, so only add a fraction of the
	// analytic gradient for fine detail the tessellated mesh cannot resolve.
	float3 Apply(float3 a_positionRel, float3 a_positionWS, float3 a_normal, out float a_shade)
	{
		a_shade = 1.0;
		if (Count() == 0)
			return a_normal;
		float2 hd = Height(a_positionWS);
		a_shade = 1.0 - hd.y;
		// Linear Lighting: baseColor is linear albedo here, so match the gamma-space darkening of LL-off.
#if defined(ENABLE_LL)
		if (ENABLE_LL)
			a_shade = pow(saturate(a_shade), 2.2);
#endif
		if (hd.x == 0.0 && hd.y == 0.0)
			return a_normal;
		float3 dpdx = ddx(a_positionRel);
		float3 dpdy = ddy(a_positionRel);
		float dhx = ddx(hd.x) * NormalStrength();
		float dhy = ddy(hd.x) * NormalStrength();
		float3 r1 = cross(dpdy, a_normal);
		float3 r2 = cross(a_normal, dpdx);
		float det = dot(dpdx, r1);
		float3 surfGrad = sign(det) * (dhx * r1 + dhy * r2);
		return normalize(abs(det) * a_normal - surfGrad);
	}
#	endif
}
#endif
