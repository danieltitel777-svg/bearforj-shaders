#ifndef PHYSICAL_BLOOD_COMMON_HLSLI
#define PHYSICAL_BLOOD_COMMON_HLSLI

// Physical Blood-owned terminal splat. Chronological serials and spawn times
// support wrapped draws and logarithmic expiry retirement.
struct PhysicalBloodOwnedSplat
{
	float3 Position;
	float SpawnTime;

	float3 ImpactVelocity;
	float ExpiryTime;

	float3 SurfaceNormal;
	float Radius;

	float Opacity;
	float SizeMultiplier;
	float Variation;
	uint Serial;

	float3 AmbientIrradiance;
	float Pad0;

	float3 DirectionalDiffuse;
	float Pad1;
};

#endif
