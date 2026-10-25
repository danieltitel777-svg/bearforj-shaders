// Elemental Contact v0.3: live settings from the in-game menu (SKSE Menu Framework).
// The DLL appends 4 float4 (EI0..EI3) to our b13 cbuffer (ActorGrimeCB in Lighting, ShoreWavesWaterfallCB in Water).
// ShoreWavesWaterfallCB is the live water settings block. EI2.x stays in the layout and is unused.
// Layout (must match EISettings::GPUBlock in the DLL):
//   EI0: x shore waves on, y shore strength, z crest spacing (wavelength), w speed
//   EI1: x foam strength, y foam width (depth), z river waves, w rock foam
//   EI2: x waterfall foam, y actor waterline wetness, z wet hair amount, w hair translucency
//   EI3: x snow, y mud, z dust multipliers, w valid (1 = DLL wrote settings)
//   EI4 (v0.4): x foam texture bound (1/0), y foam texture scale (units per tile), z foam opacity, w wave height
//   EI5 (v0.4): x run-up height (units above water), y wave period (s), z actor-draw flag (Lighting), w gear wetness
//   EI6 (v0.5, Lighting only): x blood on, y blood amount, z wound depth, w drying speed
// When the cbuffer is not bound (no DLL, non-actor draw) D3D reads zeros -> valid = 0 -> v0.2 defaults.
#ifndef __EI_SETTINGS_HLSLI__
#define __EI_SETTINGS_HLSLI__
#if defined(EI_HAS_CB)
#	define EI_GET(v, d) ((EI3.w > 0.5) ? (v) : (d))
#else
#	define EI_GET(v, d) (d)
#endif
#define EI_ShoreOn        EI_GET(EI0.x, 1.0)
#define EI_ShoreStrength  EI_GET(EI0.y, 0.22)
#define EI_CrestSpacing   max(EI_GET(EI0.z, 14.0), 1.0)
#define EI_ShoreSpeed     EI_GET(EI0.w, 0.35)
#define EI_FoamStrength   EI_GET(EI1.x, 0.70)
#define EI_FoamDepth      max(EI_GET(EI1.y, 5.0), 0.1)
#define EI_River          EI_GET(EI1.z, 1.0)
#define EI_RockFoam       EI_GET(EI1.w, 1.0)
#define EI_ActorWet       EI_GET(EI2.y, 0.9)
#define EI_WetHair        EI_GET(EI2.z, 1.0)
#define EI_HairSSS        EI_GET(EI2.w, 0.55)
#define EI_Snow           EI_GET(EI3.x, 1.0)
#define EI_Mud            EI_GET(EI3.y, 1.0)
#define EI_Dust           EI_GET(EI3.z, 1.0)
#define EI_FoamTexOn      EI_GET(EI4.x, 0.0)
#define EI_FoamTexScale   max(EI_GET(EI4.y, 420.0), 16.0)
#define EI_FoamOpacity    EI_GET(EI4.z, 0.85)
#define EI_WaveHeight     EI_GET(EI4.w, 1.0)
#define EI_RunUp          max(EI_GET(EI5.x, 40.0), 0.0)
#define EI_WavePeriod     max(EI_GET(EI5.y, 7.0), 1.5)
#define EI_GearWet        EI_GET(EI5.w, 1.0)
// v0.5 blood (EI6 only exists in the Lighting cbuffer; these are only used there)
#define EI_BloodOn        EI_GET(EI6.x, 0.0)
#define EI_BloodAmount    EI_GET(EI6.y, 1.0)
#define EI_WoundDepth     EI_GET(EI6.z, 1.0)

// ---- v0.4 surf: one swash cycle per wave period, shared by Water.hlsl (backwash) and Lighting.hlsl (run-up on sand)
namespace EISurf
{
	float Hash(float2 p)
	{
		p = frac(p * float2(123.34, 456.21));
		p += dot(p, p + 45.32);
		return frac(p.x * p.y);
	}
	float Noise(float2 p)
	{
		float2 i = floor(p), f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
	}
	// per-location phase shift so the shoreline does not move as one rigid line
	float PhaseShift(float2 posAbsXY)
	{
		return Noise(posAbsXY / 900.0) * 0.55 + Noise(posAbsXY / 230.0) * 0.12;
	}
	// swash cycle position 0..1 (0 = crest just broke at the waterline)
	float Cycle(float2 posAbsXY, float time)
	{
		return frac(time / EI_WavePeriod + PhaseShift(posAbsXY));
	}
	// swash excursion 0..1: fast uprush (first 30%), slow backwash
	float Excursion(float c)
	{
		float up = smoothstep(0.0, 0.30, c);
		float down = 1.0 - smoothstep(0.30, 1.0, c);
		return c < 0.30 ? 1.0 - (1.0 - up) * (1.0 - up) : down;
	}
	// swash front elevation relative to the still water surface (units):
	// > 0 = water runs up the beach, < 0 = backwash exposes the shallow bottom
	float Front(float2 posAbsXY, float time)
	{
		float t = time / EI_WavePeriod + PhaseShift(posAbsXY);
		float e = Excursion(frac(t));
		float big = 0.75 + 0.5 * Noise(posAbsXY / 400.0 + floor(t) * 0.37);  // every wave a bit different  // every wave a bit different
		return lerp(-0.45 * EI_RunUp, EI_RunUp * big, e);
	}
}
#endif
