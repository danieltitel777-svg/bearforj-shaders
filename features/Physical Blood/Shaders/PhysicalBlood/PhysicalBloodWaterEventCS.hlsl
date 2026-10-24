#include "Common/SharedData.hlsli"
#include "Common/Color.hlsli"
#include "PhysicalBlood/PhysicalBloodCommon.hlsli"
#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Consumes Physical Blood's single SSPP terminal-event route. Solid contacts
// become feature-owned splat records; water contacts deposit a compact radial
// kernel. SSPP can then recycle both source particles in the same frame.

SamplerState PhysicalBloodLinearSampler : register(s0);
#define LinearSampler PhysicalBloodLinearSampler
#include "Common/ShadowSampling.hlsli"

StructuredBuffer<SSPPParticleExitEvent> ExitEvents : register(t0);
RWTexture2D<uint> WaterSources : register(u0);
RWTexture1D<uint> WaterActivity : register(u1);
RWStructuredBuffer<PhysicalBloodOwnedSplat> OwnedSplats : register(u2);
RWStructuredBuffer<uint> SplatState : register(u3);
RWStructuredBuffer<uint> Counters : register(u7);

cbuffer PhysicalBloodTerminalEvent : register(b7)
{
	float2 WaterFieldCenter;
	float WaterFieldExtent;
	float WaterImpactSizeMultiplier;

	float WaterImpactStrength;
	float CurrentTime;
	float SplatLifetime;
	uint SplatCapacity;
};

static const uint WATER_SPLAT_MAX_RADIUS = 10u;
static const float WATER_SPLAT_SOURCE_MASS = 16384.0f;
static const float WATER_SPLAT_NORMALIZATION = 3.0f * Math::INV_PI;

// Summary: Produces a stable pseudorandom scalar from an insertion serial.
// Args:
//   value: Chronological splat serial.
// Returns: Scalar in the [0, 1) range.
float SplatVariation(uint value)
{
	value ^= value >> 16u;
	value *= 0x7FEB352Du;
	value ^= value >> 15u;
	value *= 0x846CA68Bu;
	value ^= value >> 16u;
	return (float)(value & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

// Summary: Evaluates the compact squared radial deposition kernel.
// Args:
//   offset: Offset from the continuous impact center in texels.
//   inverseRadiusSquared: Reciprocal squared support radius.
// Returns: Unitless source weight in the [0, 1] range.
float WaterSplatWeight(float2 offset, float inverseRadiusSquared)
{
	float falloff = saturate(1.0f - dot(offset, offset) * inverseRadiusSquared);
	return falloff * falloff;
}

// Summary: Atomically deposits one water impact into the fixed-point source field.
// Args:
//   worldPosition: Absolute world-space XY impact position.
//   particleSize: Incoming particle radius in world units.
//   opacity: Incoming particle opacity used as deposited mass.
// Returns: True when the impact lies inside a valid source field.
bool DepositWaterSource(float2 worldPosition, float particleSize, float opacity)
{
	float2 waterUV =
		(worldPosition - WaterFieldCenter) / WaterFieldExtent + 0.5f;
	if (any(waterUV < 0.0f) || any(waterUV >= 1.0f))
		return false;

	uint width;
	uint height;
	WaterSources.GetDimensions(width, height);
	if (width == 0u || height == 0u)
		return false;
	int2 dimensions = int2(width, height);
	float2 texelPosition = waterUV * float2(width, height) - 0.5f;
	int2 centerPixel = int2(floor(texelPosition + 0.5f));
	float texelWorldSize = WaterFieldExtent / max((float)width, 1.0f);
	float physicalRadius = clamp(
		particleSize * WaterImpactSizeMultiplier / max(texelWorldSize, 1.0e-4f),
		0.0f,
		(float)WATER_SPLAT_MAX_RADIUS - 0.25f);
	if (physicalRadius <= 1.0e-4f || opacity <= 0.0f)
		return false;
	// A sub-texel impact still needs a raster footprint, but its covered area
	// must retain the configured particle radius instead of collapsing every
	// 0-5 world-unit droplet to the same minimum kernel.
	float supportRadius = max(physicalRadius, 0.75f);
	int kernelRadius = min((int)ceil(supportRadius), (int)WATER_SPLAT_MAX_RADIUS);
	float inverseRadiusSquared = rcp(supportRadius * supportRadius);
	float subpixelCoverage = min(
		physicalRadius * physicalRadius * inverseRadiusSquared,
		1.0f);
	float normalizedMass = WATER_SPLAT_SOURCE_MASS * WaterImpactStrength *
		saturate(opacity) * WATER_SPLAT_NORMALIZATION * inverseRadiusSquared *
		subpixelCoverage;
	bool deposited = false;

	[loop]
	for (int y = -kernelRadius; y <= kernelRadius; ++y) {
		[loop]
		for (int x = -kernelRadius; x <= kernelRadius; ++x) {
			int2 pixel = centerPixel + int2(x, y);
			if (all(pixel >= 0) && all(pixel < dimensions)) {
				float weight = WaterSplatWeight(
					float2(pixel) - texelPosition, inverseRadiusSquared);
				if (weight > 0.0f) {
					uint amount = (uint)round(weight * normalizedMass);
					if (amount > 0u) {
						InterlockedAdd(WaterSources[pixel], amount);
						deposited = true;
					}
				}
			}
		}
	}
	return deposited;
}

// Summary: Appends one solid collision to Physical Blood's chronological ring.
// Args:
//   eventRecord: Self-contained SSPP terminal event.
// Returns: Nothing.
void StoreOwnedSplat(SSPPParticleExitEvent eventRecord)
{
	if (SplatCapacity == 0u)
		return;
	uint frameOrdinal;
	InterlockedAdd(SplatState[4], 1u, frameOrdinal);
	if (frameOrdinal >= SplatCapacity) {
		InterlockedAdd(SplatState[3], 1u);
		return;
	}
	uint serial;
	InterlockedAdd(SplatState[0], 1u, serial);
	uint slot = serial % SplatCapacity;
	float3 normal = SSPPSafeNormalize(
		eventRecord.SurfaceNormal, float3(0.0f, 0.0f, 1.0f));
	float3 cameraRelative = eventRecord.Position - FrameBuffer::CameraPosAdjust.xyz;
	float3 toCamera = -cameraRelative;
	float3 viewDirection = toCamera * rsqrt(max(dot(toCamera, toCamera), 1.0e-6f));
	float3 lightingNormal = dot(normal, viewDirection) < 0.0f ? -normal : normal;
	float3 lightDirection = SharedData::DirLightDirection.xyz;
	float NdotL = saturate(dot(lightingNormal, lightDirection));

	PhysicalBloodOwnedSplat splat = (PhysicalBloodOwnedSplat)0;
	splat.Position = eventRecord.Position;
	splat.SpawnTime = CurrentTime;
	splat.ImpactVelocity = eventRecord.Velocity;
	splat.ExpiryTime = CurrentTime + max(SplatLifetime, 0.001f);
	splat.SurfaceNormal = normal;
	splat.Radius = max(eventRecord.Radius, 0.0f);
	splat.Opacity = saturate(eventRecord.Amount);
	splat.SizeMultiplier = max(eventRecord.FeatureScalar, 0.0f);
	splat.Variation = SplatVariation(serial ^ eventRecord.EventPayloadID);
	splat.Serial = serial + 1u;
	splat.AmbientIrradiance =
		Color::Ambient(max(0.0f, SharedData::GetAmbient(lightingNormal)));
	[branch]
	if (NdotL > 0.0f) {
		float detailedShadow = 1.0f;
		float directionalShadow = ShadowSampling::GetLightingShadow(
			cameraRelative, detailedShadow);
		directionalShadow = min(directionalShadow, detailedShadow);
		directionalShadow *= ShadowSampling::GetWorldShadow(
			cameraRelative, FrameBuffer::CameraPosAdjust.xyz);
		splat.DirectionalDiffuse = ShadowSampling::GetDirectionalLighting() *
			directionalShadow * NdotL * Color::VanillaNormalization();
	}
	OwnedSplats[slot] = splat;
}

// Summary: Transfers matching solid and water events into Physical Blood state.
// Args:
//   dispatchID: Global compute-thread index; X selects an exit event.
// Returns: Nothing.
[numthreads(64, 1, 1)]
void ConsumeTerminalEventsMain(uint3 dispatchID : SV_DispatchThreadID)
{
	uint eventCount = min(Counters[SSPPEventCountCounter], SSPPEventCapacity);
	uint eventIndex = dispatchID.x;
	if (eventIndex >= eventCount || SSPPDispatchRoute >= SSPP_MAX_EVENT_ROUTES)
		return;
	SSPPParticleExitEvent eventRecord = ExitEvents[eventIndex];
	if (eventRecord.EventRoute != SSPPDispatchRoute)
		return;
	if (all(isfinite(eventRecord.Position)) && isfinite(eventRecord.Radius) &&
		all(isfinite(eventRecord.Velocity)) && isfinite(eventRecord.Amount) &&
		all(isfinite(eventRecord.SurfaceNormal)) &&
		isfinite(eventRecord.FeatureScalar)) {
		if (eventRecord.CollisionChannel == SSPP_COLLISION_WORLD_SOLID) {
			StoreOwnedSplat(eventRecord);
		} else if (eventRecord.CollisionChannel == SSPP_COLLISION_WATER &&
			DepositWaterSource(
				eventRecord.Position.xy,
				max(eventRecord.Radius, 0.0f),
				max(eventRecord.Amount, 0.0f))) {
			uint ignored;
			InterlockedOr(WaterActivity[0], 1u, ignored);
		}
	}
	InterlockedAdd(
		Counters[SSPPEventsConsumedCounterBase + SSPPDispatchRoute], 1u);
}
