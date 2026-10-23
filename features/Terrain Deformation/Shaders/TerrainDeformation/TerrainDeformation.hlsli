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
	// tess mode in B.y: 0 off, 1 footprints, 2 raised snow/mud/sand (tessellate the whole near field)
	StructuredBuffer<Print> Prints : register(t54);  // Bearforj: t123 clashed with TreeWindSpring
	Texture2D<uint> Heightmap : register(t56);
	Texture2D<uint> Activity : register(t57);
	Texture2D<float2> RaiseMap : register(t58);  // R raise units, G sink fraction, 32^2 over the window

	uint Count() { return min((uint)Prints[0].A.x, 511u); }
	float MaxTessFactor() { return Prints[0].A.y; }
	float TessRange() { return Prints[0].A.z; }
	float DisplacementScale() { return Prints[0].A.w; }
	float NormalStrength() { return Prints[0].B.x; }
	bool TessellationEnabled() { return Prints[0].B.y > 0.5; }
	// Raised ground covers the whole near field.
	bool RaiseField() { return Prints[0].B.y > 1.5; }
	// Raised ground only under the player, about 2.5 m, plus footprints.
	bool FeetBubble() { return Prints[0].B.y > 1.2 && Prints[0].B.y < 1.5; }
	static const float TD_FEET_RADIUS = 180.0;
	float2 Origin() { return Prints[0].B.zw; }

	static const float TD_RAISE_CELL = 128.0;
	static const int TD_RAISE_SIZE = 32;

	float2 LoadRaise(int2 a_texel)
	{
		if (any(a_texel < 0) || any(a_texel >= TD_RAISE_SIZE))
			return 0.0;
		return RaiseMap.Load(int3(a_texel, 0));
	}

	// World-aligned 128-unit cells. Texel 0 is the cell that contains the heightmap origin.
	float2 RaiseAt(float2 a_positionWS)
	{
		float2 origin = floor(Origin() / TD_RAISE_CELL) * TD_RAISE_CELL;
		float2 p = (a_positionWS - origin) / TD_RAISE_CELL - 0.5;
		int2 i = int2(floor(p));
		float2 f = p - i;
		return lerp(lerp(LoadRaise(i), LoadRaise(i + int2(1, 0)), f.x),
			lerp(LoadRaise(i + int2(0, 1)), LoadRaise(i + int2(1, 1)), f.x), f.y);
	}

	// 1 at the camera, 0 at TessRange, so the raised layer meets the flat untessellated ground.
	float RaiseFade(float2 a_positionWS)
	{
		float dist = length(a_positionWS - FrameBuffer::CameraPosAdjust[0].xy);
		float range = max(TessRange(), 1.0);
		return 1.0 - smoothstep(range * 0.65, range, dist);
	}

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

	// a_positionWS: absolute world position. Returns (height offset; darkening weight).
	// With no raised layer this is the old footprint: -dip + rim. With a layer, the foot sinks
	// SinkFraction of it and the whole raise fades to that footprint before TessRange.
	float2 Height(float3 a_positionWS)
	{
		float dip = 0.0;
		float dark = 0.0;
		float rim = 0.0;
		if (Count() > 0) {
			float2 t = (a_positionWS.xy - Origin()) / TD_HM_TEXEL - 0.5;
			int2 i = int2(floor(t));
			float2 f = t - i;
			float3 v = lerp(lerp(Fetch(i), Fetch(i + int2(1, 0)), f.x), lerp(Fetch(i + int2(0, 1)), Fetch(i + int2(1, 1)), f.x), f.y);
			dip = v.x;
			dark = v.y;
			rim = v.z;
		}
		float legacy = -dip + rim * (dip > 1e-3 ? 0.25 : 1.0);
		float height = legacy;
		if (RaiseField() || FeetBubble()) {
			float2 layer = RaiseAt(a_positionWS.xy);
			float mask = RaiseFade(a_positionWS.xy);
			if (FeetBubble()) {
				float dist = length(a_positionWS.xy - FrameBuffer::CameraPosAdjust[0].xy);
				mask = 1.0 - smoothstep(TD_FEET_RADIUS * 0.55, TD_FEET_RADIUS, dist);
			}
			if (layer.x > 0.5 && mask > 1e-3) {
				float foot = saturate(dip * 0.5);
				float sunk = lerp(layer.x, layer.x * (1.0 - saturate(layer.y)), foot);
				float raised = sunk + rim * 0.35;
				height = lerp(legacy, raised, mask);
			}
		}
		return float2(height, dark);
	}

#	if defined(PSHADER)
	// Perturbs a world normal with the print height field (Mikkelsen surface gradient).
	// a_displaced: the geometry was already displaced by the domain shader, so only add a fraction of the
	// analytic gradient for fine detail the tessellated mesh cannot resolve.
	float3 Apply(float3 a_positionRel, float3 a_positionWS, float3 a_normal, out float a_shade)
	{
		a_shade = 1.0;
		if (Count() == 0 && !RaiseField() && !FeetBubble())
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
