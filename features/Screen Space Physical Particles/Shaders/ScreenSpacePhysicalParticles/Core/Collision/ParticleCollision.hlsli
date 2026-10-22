#ifndef SCREEN_SPACE_PHYSICAL_PARTICLES_CORE_COLLISION_INCLUDED
#define SCREEN_SPACE_PHYSICAL_PARTICLES_CORE_COLLISION_INCLUDED

#include "Common/FrameBuffer.hlsli"

// Selects the earliest collision across enabled world, water, and player
// providers, then returns a semantic channel for profile-specific response.
// Positions use Skyrim world space; time of impact is normalized to [0, 1].
StructuredBuffer<SSPPCollisionRule> CollisionRules : register(t2);
Texture2D<float> SceneDepth : register(t3);
Texture2D<float> SolidHeightField : register(t4);
Texture2D<float> WaterHeightField : register(t5);

struct SSPPPlayerCapsule
{
	float3 PointA;
	float Radius;
	float3 PointB;
	float Pad;
};

StructuredBuffer<SSPPPlayerCapsule> PlayerCapsules : register(t6);

// Exact 64-byte mirror of ParticleRuntime::CollisionFrameConstants.
cbuffer SSPPCollisionFrame : register(b1)
{
	uint SceneDepthAvailable;
	uint SolidHeightAvailable;
	uint WaterHeightAvailable;
	uint PlayerCapsuleCount;

	float2 SolidHeightCenter;
	float SolidHeightHalfExtent;
	float SolidHeightTexelSize;

	float2 WaterHeightCenter;
	float WaterHeightHalfExtent;
	float WaterHeightTexelSize;

	float3 PlayerVelocity;
	uint CollisionRuleCount;
};

#include "ScreenSpacePhysicalParticles/ScreenSpaceCollision.hlsli"
#include "ScreenSpacePhysicalParticles/HeightFieldCollision.hlsli"

static const float SSPP_COLLISION_TIE_EPSILON = 1.0e-4f;

struct SSPPCollisionCandidate
{
	uint Valid;
	float TimeOfImpact;
	float3 Position;
	float3 Normal;
	uint Channel;
};

// Summary: Constructs the canonical no-collision candidate.
// Args: None.
// Returns: Invalid candidate at the end of the swept interval.
SSPPCollisionCandidate SSPPInvalidCollisionCandidate()
{
	SSPPCollisionCandidate candidate = {
		0u,
		1.0f,
		float3(0.0f, 0.0f, 0.0f),
		float3(0.0f, 0.0f, 1.0f),
		0u
	};
	return candidate;
}

// Summary: Maps a semantic collision channel to its diagnostics ordinal.
// Args:
//   channel: Single collision-channel bit.
// Returns: Ordinal 0..2, or SSPP_INVALID_ID for an unknown channel.
uint SSPPCollisionChannelOrdinal(uint channel)
{
	if (channel == SSPP_COLLISION_WORLD_SOLID)
		return 0u;
	if (channel == SSPP_COLLISION_WATER)
		return 1u;
	if (channel == SSPP_COLLISION_PLAYER)
		return 2u;
	return SSPP_INVALID_ID;
}

// Summary: Returns the deterministic tie-break priority for a collision channel.
// Args:
//   channel: Single collision-channel bit.
// Returns: Lower value for higher priority: solid, water, then player.
uint SSPPCollisionPriority(uint channel)
{
	uint ordinal = SSPPCollisionChannelOrdinal(channel);
	return ordinal == SSPP_INVALID_ID ? 0xFFFFFFFEu : ordinal;
}

// Summary: Increments one channel-indexed collision diagnostic.
// Args:
//   channel: Queried or hit collision channel.
//   diagnosticBase: Counter offset for query or hit diagnostics.
// Returns: None.
void SSPPRecordCollisionDiagnostic(uint channel, uint diagnosticBase)
{
	uint ordinal = SSPPCollisionChannelOrdinal(channel);
	if (ordinal != SSPP_INVALID_ID)
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + diagnosticBase + ordinal],
			1u);
}

// Summary: Selects the earlier candidate using channel priority for equal times.
// Args:
//   candidate: Candidate to compare.
//   selected: Current winner, replaced when candidate wins.
// Returns: None.
void SSPPSelectEarlierCandidate(
	SSPPCollisionCandidate candidate,
	inout SSPPCollisionCandidate selected)
{
	if (candidate.Valid == 0u)
		return;
	if (selected.Valid == 0u ||
		candidate.TimeOfImpact < selected.TimeOfImpact - SSPP_COLLISION_TIE_EPSILON ||
		(abs(candidate.TimeOfImpact - selected.TimeOfImpact) <= SSPP_COLLISION_TIE_EPSILON &&
			SSPPCollisionPriority(candidate.Channel) < SSPPCollisionPriority(selected.Channel)))
		selected = candidate;
}

// Summary: Projects a point onto a swept segment.
// Args:
//   startPosition: Segment start in world space.
//   endPosition: Segment end in world space.
//   samplePosition: World-space point to project.
// Returns: Clamped segment parameter in [0, 1].
float SSPPSegmentParameter(float3 startPosition, float3 endPosition, float3 samplePosition)
{
	float3 segment = endPosition - startPosition;
	return saturate(dot(samplePosition - startPosition, segment) /
		max(dot(segment, segment), 1.0e-8f));
}

// Summary: Queries enabled perspective and orthographic world-solid providers.
// Args:
//   startPosition: Particle-center position before integration.
//   endPosition: Particle-center position after integration.
//   collisionRadius: Profile-scaled particle radius.
//   separationOffset: Rule-specific surface separation.
//   profile: Simulation and provider policy.
// Returns: Earliest valid world-solid collision candidate.
SSPPCollisionCandidate SSPPQueryWorldSolid(
	float3 startPosition,
	float3 endPosition,
	float collisionRadius,
	float separationOffset,
	SSPPSimulationProfile profile)
{
	SSPPCollisionCandidate selected = SSPPInvalidCollisionCandidate();
	bool perspectiveEnabled = SceneDepthAvailable != 0u &&
		(profile.QueryPolicy & SSPP_QUERY_PERSPECTIVE_DEPTH) != 0u;
	bool heightEnabled = SolidHeightAvailable != 0u &&
		(profile.QueryPolicy & SSPP_QUERY_SOLID_HEIGHT) != 0u;
	if (!perspectiveEnabled && !heightEnabled)
		return selected;

	SSPPRecordCollisionDiagnostic(
		SSPP_COLLISION_WORLD_SOLID, SSPP_DIAGNOSTIC_COLLISION_QUERIES);
	float separation = max(collisionRadius + profile.SurfaceOffset + separationOffset, 0.0f);
	if (perspectiveEnabled) {
		float hitTime;
		float3 hitPosition;
		float3 hitNormal;
		if (SweepScene(startPosition, endPosition, separation, hitTime, hitPosition, hitNormal)) {
			SSPPCollisionCandidate candidate = SSPPInvalidCollisionCandidate();
			candidate.Valid = 1u;
			candidate.TimeOfImpact = hitTime;
			candidate.Position = hitPosition;
			candidate.Normal = SSPPSafeNormalize(hitNormal, float3(0.0f, 0.0f, 1.0f));
			candidate.Channel = SSPP_COLLISION_WORLD_SOLID;
			SSPPSelectEarlierCandidate(candidate, selected);
		}
	}

	if (heightEnabled) {
		float3 hitPosition;
		float3 hitNormal;
		if (SSPP_SweepHeightField(
				SolidHeightField,
				SolidHeightAvailable,
				SolidHeightCenter,
				SolidHeightHalfExtent,
				SolidHeightTexelSize,
				0.0f,
				separation,
				startPosition,
				endPosition,
				hitPosition,
				hitNormal)) {
			SSPPCollisionCandidate candidate = SSPPInvalidCollisionCandidate();
			candidate.Valid = 1u;
			candidate.Normal = SSPPSafeNormalize(hitNormal, float3(0.0f, 0.0f, 1.0f));
			candidate.Position = hitPosition;
			// Convert the height-field surface point to particle-center sweep time.
			candidate.TimeOfImpact = SSPPSegmentParameter(
				startPosition,
				endPosition,
				hitPosition + candidate.Normal * separation);
			candidate.Channel = SSPP_COLLISION_WORLD_SOLID;
			SSPPSelectEarlierCandidate(candidate, selected);
		}
	}

	if (selected.Valid != 0u)
		SSPPRecordCollisionDiagnostic(
			SSPP_COLLISION_WORLD_SOLID, SSPP_DIAGNOSTIC_COLLISION_HITS);
	return selected;
}

// Summary: Loads exposed water height at one world-space horizontal position.
// Args:
//   worldXY: Skyrim world-space horizontal position.
//   dimensions: Water-height texture dimensions.
//   waterHeight: Receives world-space water elevation.
// Returns: True when the position maps to a valid captured water texel.
bool SSPPSampleWaterHeight(float2 worldXY, uint2 dimensions, out float waterHeight)
{
	waterHeight = SSPP_MISSING_HEIGHT;
	if (WaterHeightAvailable == 0u || WaterHeightHalfExtent <= 0.0f ||
		WaterHeightTexelSize <= 0.0f)
		return false;
	if (any(dimensions == 0u))
		return false;
	float2 pixelPosition = float2(
		(worldXY.x - (WaterHeightCenter.x - WaterHeightHalfExtent)) / WaterHeightTexelSize,
		((WaterHeightCenter.y + WaterHeightHalfExtent) - worldXY.y) / WaterHeightTexelSize);
	int2 pixel = int2(floor(pixelPosition));
	if (any(pixel < 0) || pixel.x >= (int)dimensions.x || pixel.y >= (int)dimensions.y)
		return false;
	waterHeight = WaterHeightField.Load(int3(pixel, 0));
	return isfinite(waterHeight) && waterHeight > SSPP_MISSING_HEIGHT;
}

// Summary: Rejects overlap recovery where captured solid geometry covers water.
// Args:
//   position: Particle position used for solid-height lookup.
//   waterHeight: Candidate world-space water elevation.
//   separation: Required particle-center separation.
// Returns: True when no captured solid occludes the water plane.
bool SSPPWaterOverlapIsExposed(float3 position, float waterHeight, float separation)
{
	if (SolidHeightAvailable == 0u)
		return true;
	float solidHeight;
	float2 solidTexelPosition;
	uint2 solidDimensions;
	if (SSPP_SampleHeightFieldValue(
			SolidHeightField,
			SolidHeightAvailable,
			SolidHeightCenter,
			SolidHeightHalfExtent,
			SolidHeightTexelSize,
			position.xy,
			solidHeight,
			solidTexelPosition,
			solidDimensions))
		return solidHeight <= waterHeight + separation;
	return true;
}

// Summary: Sweeps a particle against exposed samples of the water-height field.
// Args:
//   startPosition: Particle-center position before integration.
//   endPosition: Particle-center position after integration.
//   collisionRadius: Profile-scaled particle radius.
//   separationOffset: Rule-specific surface separation.
//   profile: Simulation and provider policy.
// Returns: First downward crossing or exposed underwater-overlap recovery.
SSPPCollisionCandidate SSPPQueryWater(
	float3 startPosition,
	float3 endPosition,
	float collisionRadius,
	float separationOffset,
	SSPPSimulationProfile profile)
{
	SSPPCollisionCandidate candidate = SSPPInvalidCollisionCandidate();
	if (WaterHeightAvailable == 0u ||
		(profile.QueryPolicy & SSPP_QUERY_WATER_HEIGHT) == 0u)
		return candidate;
	SSPPRecordCollisionDiagnostic(
		SSPP_COLLISION_WATER, SSPP_DIAGNOSTIC_COLLISION_QUERIES);
	uint waterWidth, waterHeight;
	WaterHeightField.GetDimensions(waterWidth, waterHeight);
	uint2 waterDimensions = uint2(waterWidth, waterHeight);
	if (any(waterDimensions == 0u))
		return candidate;

	float separation = max(collisionRadius + profile.SurfaceOffset + separationOffset, 0.0f);
	float3 segment = endPosition - startPosition;
	uint steps = clamp(
		(uint)ceil(length(segment.xy) / max(WaterHeightTexelSize, 1.0f)),
		1u,
		16u);
	float previousHeight;
	bool previousValid = SSPPSampleWaterHeight(startPosition.xy, waterDimensions, previousHeight);
	float previousSignedHeight = startPosition.z - previousHeight - separation;
	float3 previousPosition = startPosition;
	float previousT = 0.0f;

	if (previousValid && previousSignedHeight < 0.0f &&
		SSPPWaterOverlapIsExposed(startPosition, previousHeight, separation)) {
		candidate.Valid = 1u;
		candidate.TimeOfImpact = 0.0f;
		candidate.Position = float3(startPosition.xy, previousHeight);
		candidate.Normal = float3(0.0f, 0.0f, 1.0f);
		candidate.Channel = SSPP_COLLISION_WATER;
		SSPPRecordCollisionDiagnostic(candidate.Channel, SSPP_DIAGNOSTIC_COLLISION_HITS);
		return candidate;
	}

	[loop]
	for (uint step = 1u; step <= steps; ++step) {
		float currentT = (float)step / (float)steps;
		float3 currentPosition = lerp(startPosition, endPosition, currentT);
		float currentHeight;
		bool currentValid = SSPPSampleWaterHeight(currentPosition.xy, waterDimensions, currentHeight);
		float currentSignedHeight = currentPosition.z - currentHeight - separation;
		if (previousValid && currentValid && previousSignedHeight >= 0.0f &&
			currentSignedHeight <= 0.0f && segment.z < 0.0f) {
			float crossing = saturate(previousSignedHeight /
				max(previousSignedHeight - currentSignedHeight, 1.0e-6f));
			candidate.Valid = 1u;
			candidate.TimeOfImpact = lerp(previousT, currentT, crossing);
			float3 center = lerp(previousPosition, currentPosition, crossing);
			candidate.Position = float3(center.xy, lerp(previousHeight, currentHeight, crossing));
			candidate.Normal = float3(0.0f, 0.0f, 1.0f);
			candidate.Channel = SSPP_COLLISION_WATER;
			SSPPRecordCollisionDiagnostic(candidate.Channel, SSPP_DIAGNOSTIC_COLLISION_HITS);
			return candidate;
		}
		if (!previousValid && currentValid && currentSignedHeight < 0.0f &&
			SSPPWaterOverlapIsExposed(currentPosition, currentHeight, separation)) {
			candidate.Valid = 1u;
			candidate.TimeOfImpact = currentT;
			candidate.Position = float3(currentPosition.xy, currentHeight);
			candidate.Normal = float3(0.0f, 0.0f, 1.0f);
			candidate.Channel = SSPP_COLLISION_WATER;
			SSPPRecordCollisionDiagnostic(candidate.Channel, SSPP_DIAGNOSTIC_COLLISION_HITS);
			return candidate;
		}
		previousValid = currentValid;
		previousHeight = currentHeight;
		previousSignedHeight = currentSignedHeight;
		previousPosition = currentPosition;
		previousT = currentT;
	}
	return candidate;
}

// Summary: Finds the closest point on a finite segment.
// Args:
//   inputPosition: Point to project.
//   pointA: Segment start.
//   pointB: Segment end.
// Returns: Closest segment point.
float3 SSPPClosestPointOnSegment(float3 inputPosition, float3 pointA, float3 pointB)
{
	return pointA + (pointB - pointA) *
		SSPPSegmentParameter(pointA, pointB, inputPosition);
}

// Summary: Solves the first intersection of a finite ray segment and a sphere.
// Args:
//   origin: Ray-segment start.
//   ray: Full start-to-end displacement.
//   rayLengthSquared: Precomputed dot(ray, ray).
//   center: Sphere center.
//   radius: Expanded sphere radius.
//   timeOfImpact: Receives normalized intersection time.
// Returns: True when the segment intersects the sphere in [0, 1].
bool SSPPRaySphereTOI(
	float3 origin,
	float3 ray,
	float rayLengthSquared,
	float3 center,
	float radius,
	inout float timeOfImpact)
{
	timeOfImpact = 1.0f;
	if (rayLengthSquared <= 1.0e-8f)
		return false;
	float3 offset = origin - center;
	float b = dot(offset, ray);
	float c = dot(offset, offset) - radius * radius;
	float discriminant = b * b - rayLengthSquared * c;
	if (discriminant < 0.0f)
		return false;
	float candidate = (-b - sqrt(discriminant)) / rayLengthSquared;
	if (candidate < 0.0f || candidate > 1.0f)
		return false;
	timeOfImpact = candidate;
	return true;
}

// Summary: Sweeps a point segment against a radius-expanded player capsule.
// Args:
//   startPosition: Particle-center segment start.
//   endPosition: Particle-center segment end.
//   capsule: World-space capsule endpoints and physical radius.
//   expandedRadius: Capsule radius plus particle separation.
// Returns: Earliest capsule-body or end-cap collision candidate.
SSPPCollisionCandidate SSPPSweepExpandedCapsule(
	float3 startPosition,
	float3 endPosition,
	SSPPPlayerCapsule capsule,
	float expandedRadius)
{
	// Explicit initialization satisfies FXC definite-assignment analysis.
	SSPPCollisionCandidate result = {
		0u,
		1.0f,
		float3(0.0f, 0.0f, 0.0f),
		float3(0.0f, 0.0f, 1.0f),
		0u
	};
	float3 ray = endPosition - startPosition;
	float rayLengthSquared = dot(ray, ray);
	float3 capsuleAxis = capsule.PointB - capsule.PointA;
	float capsuleAxisLengthSquared = dot(capsuleAxis, capsuleAxis);
	float startAxisParameter = 0.0f;
	if (capsuleAxisLengthSquared > 1.0e-8f)
		startAxisParameter = saturate(
			dot(startPosition - capsule.PointA, capsuleAxis) / capsuleAxisLengthSquared);
	float3 startAxisPoint = capsule.PointA + capsuleAxis * startAxisParameter;
	float3 startDelta = startPosition - startAxisPoint;
	bool foundHit = dot(startDelta, startDelta) <= expandedRadius * expandedRadius;
	if (foundHit) {
		result.TimeOfImpact = 0.0f;
	} else if (rayLengthSquared > 1.0e-8f) {
		float3 ba = capsule.PointB - capsule.PointA;
		float3 oa = startPosition - capsule.PointA;
		float baba = dot(ba, ba);
		float bard = dot(ba, ray);
		float baoa = dot(ba, oa);
		float rdoa = dot(ray, oa);
		float oaoa = dot(oa, oa);
		float qa = baba * rayLengthSquared - bard * bard;
		float qb = baba * rdoa - baoa * bard;
		float qc = baba * oaoa - baoa * baoa - expandedRadius * expandedRadius * baba;
		float best = 2.0f;
		if (abs(qa) > 1.0e-8f) {
			float discriminant = qb * qb - qa * qc;
			if (discriminant >= 0.0f) {
				float cylinderT = (-qb - sqrt(discriminant)) / qa;
				float axisT = baoa + cylinderT * bard;
				if (cylinderT >= 0.0f && cylinderT <= 1.0f &&
					axisT >= 0.0f && axisT <= baba)
					best = cylinderT;
			}
		}
		float capT = 1.0f;
		if (SSPPRaySphereTOI(
				startPosition, ray, rayLengthSquared, capsule.PointA, expandedRadius, capT))
			best = min(best, capT);
		if (SSPPRaySphereTOI(
				startPosition, ray, rayLengthSquared, capsule.PointB, expandedRadius, capT))
			best = min(best, capT);
		if (best <= 1.0f) {
			result.TimeOfImpact = best;
			foundHit = true;
		}
	}

	if (foundHit) {
		float3 particleCenter = lerp(startPosition, endPosition, result.TimeOfImpact);
		float3 axisPoint = SSPPClosestPointOnSegment(
			particleCenter, capsule.PointA, capsule.PointB);
		float3 delta = particleCenter - axisPoint;
		if (dot(delta, delta) > 1.0e-8f)
			result.Normal = normalize(delta);
		else if (rayLengthSquared > 1.0e-8f)
			result.Normal = -normalize(ray);
		else
			result.Normal = float3(0.0f, 0.0f, 1.0f);
		result.Position = axisPoint + result.Normal * max(capsule.Radius, 0.0f);
		result.Valid = 1u;
	}
	return result;
}

// Summary: Broad-phase tests and sweeps against the current player capsule.
// Args:
//   startPosition: Particle-center position before integration.
//   endPosition: Particle-center position after integration.
//   collisionRadius: Profile-scaled particle radius.
//   separationOffset: Rule-specific separation from the player.
//   profile: Simulation and provider policy.
// Returns: Earliest player-capsule collision candidate.
SSPPCollisionCandidate SSPPQueryPlayer(
	float3 startPosition,
	float3 endPosition,
	float collisionRadius,
	float separationOffset,
	SSPPSimulationProfile profile)
{
	SSPPCollisionCandidate selected = SSPPInvalidCollisionCandidate();
	if (PlayerCapsuleCount == 0u ||
		(profile.QueryPolicy & SSPP_QUERY_PLAYER_CAPSULES) == 0u)
		return selected;
	SSPPRecordCollisionDiagnostic(
		SSPP_COLLISION_PLAYER, SSPP_DIAGNOSTIC_COLLISION_QUERIES);
	float expandedOffset = max(collisionRadius + profile.SurfaceOffset + separationOffset, 0.0f);
	float3 segmentMinimum = min(startPosition, endPosition);
	float3 segmentMaximum = max(startPosition, endPosition);
	SSPPPlayerCapsule capsule = PlayerCapsules[0];
	float expandedRadius = max(capsule.Radius, 0.0f) + expandedOffset;
	float3 capsuleMinimum = min(capsule.PointA, capsule.PointB) - expandedRadius;
	float3 capsuleMaximum = max(capsule.PointA, capsule.PointB) + expandedRadius;
	if (any(segmentMaximum < capsuleMinimum) || any(segmentMinimum > capsuleMaximum))
		return selected;
	selected = SSPPSweepExpandedCapsule(
		startPosition, endPosition, capsule, expandedRadius);
	selected.Channel = SSPP_COLLISION_PLAYER;
	if (selected.Valid != 0u)
		SSPPRecordCollisionDiagnostic(
			SSPP_COLLISION_PLAYER, SSPP_DIAGNOSTIC_COLLISION_HITS);
	return selected;
}

// Forward declaration; the contract accompanies the definition below.
bool SSPPLoadCollisionRule(
	SSPPSimulationProfile profile,
	uint collisionChannel,
	inout SSPPCollisionRule rule);

// Summary: Queries collision channels enabled for the active simulation class.
// Args:
//   startPosition: Particle-center position before integration.
//   endPosition: Particle-center position after integration.
//   particleRadius: Unscaled particle radius.
//   particleAge: Current particle age in seconds.
//   profile: Simulation, provider, and response-table configuration.
// Returns: Earliest eligible collision candidate across enabled channels.
SSPPCollisionCandidate SSPPQueryCollision(
	float3 startPosition,
	float3 endPosition,
	float particleRadius,
	float particleAge,
	SSPPSimulationProfile profile)
{
	SSPPCollisionCandidate selected = SSPPInvalidCollisionCandidate();
	if (SSPPDispatchClass == SSPP_SIMULATION_NO_COLLISION)
		return selected;

	if ((profile.CollisionMask & SSPP_COLLISION_WORLD_SOLID) != 0u) {
		SSPPCollisionRule rule = (SSPPCollisionRule)0;
		if (SSPPLoadCollisionRule(profile, SSPP_COLLISION_WORLD_SOLID, rule) &&
			particleAge >= max(rule.MinimumParticleAge, 0.0f)) {
			SSPPCollisionCandidate solid = SSPPQueryWorldSolid(
				startPosition,
				endPosition,
				particleRadius * max(rule.CollisionRadiusScale, 0.0f),
				rule.SeparationOffset,
				profile);
			SSPPSelectEarlierCandidate(solid, selected);
		}
	}
	if (SSPPDispatchClass == SSPP_SIMULATION_WORLD_WATER &&
		(profile.CollisionMask & SSPP_COLLISION_WATER) != 0u) {
		SSPPCollisionRule rule = (SSPPCollisionRule)0;
		if (SSPPLoadCollisionRule(profile, SSPP_COLLISION_WATER, rule) &&
			particleAge >= max(rule.MinimumParticleAge, 0.0f)) {
			SSPPCollisionCandidate water = SSPPQueryWater(
				startPosition,
				endPosition,
				particleRadius * max(rule.CollisionRadiusScale, 0.0f),
				rule.SeparationOffset,
				profile);
			SSPPSelectEarlierCandidate(water, selected);
		}
	}
	if (SSPPDispatchClass == SSPP_SIMULATION_WORLD_PLAYER &&
		(profile.CollisionMask & SSPP_COLLISION_PLAYER) != 0u) {
		SSPPCollisionRule rule = (SSPPCollisionRule)0;
		if (SSPPLoadCollisionRule(profile, SSPP_COLLISION_PLAYER, rule) &&
			particleAge >= max(rule.MinimumParticleAge, 0.0f)) {
			SSPPCollisionCandidate player = SSPPQueryPlayer(
				startPosition,
				endPosition,
				particleRadius * max(rule.CollisionRadiusScale, 0.0f),
				rule.SeparationOffset,
				profile);
			SSPPSelectEarlierCandidate(player, selected);
		}
	}
	return selected;
}

// Summary: Loads one bounds-checked collision rule from a profile response table.
// Args:
//   profile: Profile containing the table base offset.
//   collisionChannel: Semantic channel selecting the table slot.
//   rule: Receives the loaded rule or zero initialization.
// Returns: True when the channel and resolved rule index are valid.
bool SSPPLoadCollisionRule(
	SSPPSimulationProfile profile,
	uint collisionChannel,
	inout SSPPCollisionRule rule)
{
	rule = (SSPPCollisionRule)0;
	uint slot = SSPPCollisionChannelOrdinal(collisionChannel);
	if (slot == SSPP_INVALID_ID)
		return false;
	uint ruleIndex = profile.ResponseTableOffset + slot;
	if (ruleIndex >= CollisionRuleCount)
		return false;
	rule = CollisionRules[ruleIndex];
	return true;
}

#endif
