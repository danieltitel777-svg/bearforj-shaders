#include "ScreenSpacePhysicalParticles/Core/ParticleCommon.hlsli"

// Integrates one compact dynamic active-list class, resolves its earliest enabled
// collision, applies the profile response, and routes each validated reference
// exactly once to a dynamic, persistent, event/free, or free lifecycle path.
StructuredBuffer<SSPPParticleReference> InputReferences : register(t0);
StructuredBuffer<SSPPSimulationProfile> SimulationProfiles : register(t1);

RWStructuredBuffer<float4> PositionAge : register(u0);
RWStructuredBuffer<float4> VelocityLifetime : register(u1);
StructuredBuffer<float4> Appearance : register(t9);
RWStructuredBuffer<uint4> Metadata : register(u3);
RWStructuredBuffer<float4> ContactData : register(u4);

#include "ScreenSpacePhysicalParticles/Core/ParticleRouting.hlsli"

StructuredBuffer<SSPPParticleAuxState> ParticleAuxiliaryState : register(t8);

#include "ScreenSpacePhysicalParticles/Core/Collision/ParticleCollision.hlsli"

// Summary: Increments the invalid-collision-rule diagnostic counter.
// Args: None.
// Returns: None.
void SSPPRecordInvalidRule()
{
	InterlockedAdd(
		Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_COLLISION_RULES],
		1u);
}

// Summary: Applies collision-rule profile, renderer, age, and lifetime overrides.
// Args:
//   rule: Validated collision response rule.
//   forceAgeReset: Whether the resolved response requires age zero.
//   positionAge: Mutable particle position and age.
//   velocityLifetime: Mutable particle velocity and lifetime.
//   metadata: Mutable profile, renderer, flags, and generation metadata.
// Returns: None.
void SSPPApplyTransitionOverrides(
	SSPPCollisionRule rule,
	bool forceAgeReset,
	inout float4 positionAge,
	inout float4 velocityLifetime,
	inout uint4 metadata)
{
	if (rule.NextSimulationProfileID != SSPP_INVALID_ID)
		metadata.x = rule.NextSimulationProfileID;
	if (rule.NextRenderClassID != SSPP_INVALID_ID)
		metadata.y = rule.NextRenderClassID;
	if (forceAgeReset || (rule.Flags & SSPP_COLLISION_RULE_RESET_AGE) != 0u)
		positionAge.w = 0.0f;
	if (rule.NextLifetimeOverride >= 0.0f)
		velocityLifetime.w = max(rule.NextLifetimeOverride, 1.0e-4f);
}

// Summary: Validates profile and renderer IDs referenced by a collision rule.
// Args:
//   rule: Collision rule to validate.
// Returns: True when every optional transition target is registered and usable.
bool SSPPValidateTransitionTargets(SSPPCollisionRule rule)
{
	if (rule.NextSimulationProfileID != SSPP_INVALID_ID) {
		if (rule.NextSimulationProfileID >= SSPPSimulationProfileCount)
			return false;
		SSPPSimulationProfile nextProfile = SimulationProfiles[rule.NextSimulationProfileID];
		if (nextProfile.SimulationClass >= SSPP_SIMULATION_CLASS_COUNT)
			return false;
	}
	if (rule.NextRenderClassID != SSPP_INVALID_ID &&
		rule.NextRenderClassID >= SSPPRenderClassCount)
		return false;
	return true;
}

// Summary: Computes restitution and friction in the moving surface frame.
// Args:
//   impactVelocity: Particle velocity at impact.
//   surfaceVelocity: Collider velocity.
//   normal: Unit outward surface normal.
//   restitution: Normal velocity retention in [0, 1].
//   friction: Tangential velocity removal in [0, 1].
// Returns: Post-collision world-space velocity.
float3 SSPPBounceVelocity(
	float3 impactVelocity,
	float3 surfaceVelocity,
	float3 normal,
	float restitution,
	float friction)
{
	float3 relativeVelocity = impactVelocity - surfaceVelocity;
	float normalSpeed = dot(relativeVelocity, normal);
	float3 tangentVelocity = relativeVelocity - normal * normalSpeed;
	float outwardSpeed = normalSpeed < 0.0f ? -normalSpeed * restitution : normalSpeed;
	return tangentVelocity * (1.0f - friction) + normal * outwardSpeed + surfaceVelocity;
}

// Summary: Routes a particle to the class selected by its current profile metadata.
// Args:
//   reference: Validated generation-tagged particle reference.
//   metadata: Particle metadata containing the current profile ID.
// Returns: None.
void SSPPRouteCurrentDynamic(SSPPParticleReference reference, uint4 metadata)
{
	if (metadata.x >= SSPPSimulationProfileCount) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		SSPPRouteFree(reference);
		return;
	}
	SSPPSimulationProfile outputProfile = SimulationProfiles[metadata.x];
	if (outputProfile.SimulationClass >= SSPP_SIMULATION_CLASS_COUNT) {
		InterlockedAdd(
			Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES],
			1u);
		SSPPRouteFree(reference);
		return;
	}
	SSPPRouteDynamic(reference, outputProfile.SimulationClass);
}

[numthreads(64, 1, 1)]
// Summary: Integrates and routes one particle in the selected dynamic class.
// Args:
//   dispatchID: Local reference index in its x component.
// Returns: None.
void SimulateMain(uint3 dispatchID : SV_DispatchThreadID)
{
	if (SSPPDispatchClass >= SSPP_SIMULATION_CLASS_COUNT)
		return;
	uint inputCount = min(
		Counters[SSPPInputCounterBase + SSPPDispatchClass],
		SSPPListSegmentCapacity);
	if (dispatchID.x >= inputCount)
		return;

	SSPPParticleReference reference =
		InputReferences[SSPPListAddress(SSPPDispatchClass, dispatchID.x)];
	if (reference.ParticleID >= SSPPCapacity || reference.SlotGeneration == 0u) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES], 1u);
		return;
	}
	uint particleID = reference.ParticleID;
	uint4 metadata = Metadata[particleID];
	uint inputSimulationProfile = metadata.x;
	SSPPParticleAuxState auxiliary = ParticleAuxiliaryState[particleID];
	if (metadata.w != reference.SlotGeneration ||
		auxiliary.LifecyclePhase != SSPP_LIFECYCLE_DYNAMIC) {
		InterlockedAdd(
			Counters[SSPPLifecycleDiagnosticsBase +
				SSPP_LIFECYCLE_DIAGNOSTIC_STALE_REFERENCES], 1u);
		return;
	}
	if (SSPPRetiringOwnerFeature != SSPP_INVALID_ID &&
		auxiliary.OwnerFeature == SSPPRetiringOwnerFeature) {
		SSPPRouteFree(reference);
		return;
	}

	// Paused frames preserve membership without mutating particle state.
	if (SSPPDeltaTime <= 0.0f) {
		SSPPRouteDynamic(reference, SSPPDispatchClass);
		return;
	}
	if (metadata.x >= SSPPSimulationProfileCount) {
		InterlockedAdd(Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES], 1u);
		SSPPRouteFree(reference);
		return;
	}
	SSPPSimulationProfile profile = SimulationProfiles[metadata.x];
	if (profile.SimulationClass != SSPPDispatchClass ||
		profile.SimulationClass >= SSPP_SIMULATION_CLASS_COUNT) {
		InterlockedAdd(Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES], 1u);
		SSPPRouteFree(reference);
		return;
	}

	float4 positionAge = PositionAge[particleID];
	float4 velocityLifetime = VelocityLifetime[particleID];
	float4 appearance = Appearance[particleID];
	if (!all(isfinite(positionAge)) || !all(isfinite(velocityLifetime)) ||
		!all(isfinite(appearance)) || velocityLifetime.w <= 0.0f ||
		positionAge.w >= velocityLifetime.w) {
		SSPPRouteFree(reference);
		return;
	}

	// Simulate the final fractional lifetime segment so terminal contacts remain observable.
	float stepTime = min(max(SSPPDeltaTime, 0.0f),
		max(velocityLifetime.w - positionAge.w, 0.0f));
	float3 previousPosition = positionAge.xyz;
	float3 impactVelocity = velocityLifetime.xyz + profile.Acceleration * stepTime;
	impactVelocity *= exp(-max(profile.Drag, 0.0f) * stepTime);
	float3 proposedPosition = previousPosition + impactVelocity * stepTime;
	float nextAge = positionAge.w + stepTime;
	if (!all(isfinite(impactVelocity)) || !all(isfinite(proposedPosition)) ||
		!isfinite(nextAge)) {
		InterlockedAdd(Counters[SSPPDiagnosticsBase + SSPP_DIAGNOSTIC_INVALID_PROFILES], 1u);
		SSPPRouteFree(reference);
		return;
	}

	SSPPCollisionCandidate hit = SSPPQueryCollision(
		previousPosition, proposedPosition, max(appearance.x, 0.0f), positionAge.w, profile);
	positionAge.w = nextAge;
	velocityLifetime.xyz = impactVelocity;
	if (hit.Valid == 0u) {
		positionAge.xyz = proposedPosition;
		PositionAge[particleID] = positionAge;
		VelocityLifetime[particleID] = velocityLifetime;
		if (positionAge.w >= velocityLifetime.w)
			SSPPRouteFree(reference);
		else
			SSPPRouteDynamic(reference, SSPPDispatchClass);
		return;
	}

	float3 surfaceVelocity = hit.Channel == SSPP_COLLISION_PLAYER ?
		PlayerVelocity : float3(0.0f, 0.0f, 0.0f);
	if (!isfinite(hit.TimeOfImpact) || !all(isfinite(hit.Position)) ||
		!all(isfinite(hit.Normal)) || !all(isfinite(surfaceVelocity))) {
		SSPPRecordInvalidRule();
		SSPPRouteFree(reference);
		return;
	}
	SSPPCollisionRule rule = (SSPPCollisionRule)0;
	if (!SSPPLoadCollisionRule(profile, hit.Channel, rule) ||
		rule.Response > SSPP_RESPONSE_EMIT_EVENT_AND_KILL ||
		!isfinite(rule.MinimumParticleAge) ||
		!isfinite(rule.RestitutionOverride) || !isfinite(rule.FrictionOverride) ||
		!isfinite(rule.CollisionRadiusScale) || !isfinite(rule.SeparationOffset) ||
		!isfinite(rule.MinimumSettleNormalZ) || !isfinite(rule.NextLifetimeOverride) ||
		!SSPPValidateTransitionTargets(rule)) {
		SSPPRecordInvalidRule();
		SSPPRouteFree(reference);
		return;
	}

	float3 normal = SSPPSafeNormalize(hit.Normal, float3(0.0f, 0.0f, 1.0f));
	float collisionRadius = max(appearance.x, 0.0f) * max(rule.CollisionRadiusScale, 0.0f);
	float separation = max(collisionRadius + profile.SurfaceOffset + rule.SeparationOffset, 0.0f);
	float3 contactCenter = hit.Position + normal * separation;
	float restitution = saturate(rule.RestitutionOverride >= 0.0f ?
		rule.RestitutionOverride : profile.Restitution);
	float friction = saturate(rule.FrictionOverride >= 0.0f ?
		rule.FrictionOverride : profile.Friction);
	float3 bouncedVelocity = SSPPBounceVelocity(
		impactVelocity, surfaceVelocity, normal, restitution, friction);
	uint resolvedResponse = rule.Response;
	if (resolvedResponse == SSPP_RESPONSE_SETTLE) {
		float settleSpeed = max(profile.SettleSpeed, 0.0f);
		float3 settledRelativeVelocity = bouncedVelocity - surfaceVelocity;
		if (normal.z < rule.MinimumSettleNormalZ ||
			dot(settledRelativeVelocity, settledRelativeVelocity) > settleSpeed * settleSpeed)
			resolvedResponse = SSPP_RESPONSE_BOUNCE;
	}
	InterlockedAdd(Counters[SSPPDiagnosticsBase +
		SSPP_DIAGNOSTIC_COLLISION_RESPONSES + resolvedResponse], 1u);
	ContactData[particleID] = float4(normal, 0.0f);

	if (resolvedResponse == SSPP_RESPONSE_KILL) {
		SSPPRouteFree(reference);
		return;
	}
	if (resolvedResponse == SSPP_RESPONSE_EMIT_EVENT_AND_KILL) {
		SSPPParticleExitEvent eventRecord;
		eventRecord.Position = contactCenter;
		eventRecord.Radius = max(appearance.x, 0.0f);
		eventRecord.Velocity = impactVelocity;
		eventRecord.Amount = max(appearance.y, 0.0f);
		eventRecord.SurfaceNormal = normal;
		eventRecord.FeatureScalar = appearance.w;
		eventRecord.EventRoute = rule.EventRoute;
		eventRecord.EventPayloadID = rule.EventPayloadID != SSPP_INVALID_ID ?
			rule.EventPayloadID : auxiliary.FeaturePayloadID;
		eventRecord.CollisionChannel = hit.Channel;
		eventRecord.Flags = auxiliary.Flags;
		if (SSPPAppendExitEvent(eventRecord)) {
			SSPPRouteFree(reference);
		} else {
			// Keep ownership and retry the terminal transfer next frame instead of
			// silently losing the particle when the shared event arena is full.
			positionAge.xyz = contactCenter;
			positionAge.w = max(positionAge.w - stepTime, 0.0f);
			PositionAge[particleID] = positionAge;
			VelocityLifetime[particleID] = velocityLifetime;
			SSPPRouteDynamic(reference, SSPPDispatchClass);
		}
		return;
	}

	bool settleFallback = rule.Response == SSPP_RESPONSE_SETTLE &&
		resolvedResponse == SSPP_RESPONSE_BOUNCE;
	if (!settleFallback)
		SSPPApplyTransitionOverrides(rule,
			resolvedResponse == SSPP_RESPONSE_STICK, positionAge, velocityLifetime, metadata);
	positionAge.xyz = contactCenter;
	if (resolvedResponse == SSPP_RESPONSE_SETTLE || resolvedResponse == SSPP_RESPONSE_STICK) {
		// Persistent renderers retain the pre-response impact velocity.
		velocityLifetime.xyz = impactVelocity;
		PositionAge[particleID] = positionAge;
		VelocityLifetime[particleID] = velocityLifetime;
		Metadata[particleID] = metadata;
		if (rule.DestinationListID >= SSPPRegisteredPersistentListCount) {
			SSPPRecordInvalidRule();
			SSPPRouteFree(reference);
		} else {
			SSPPRoutePersistent(reference, rule.DestinationListID);
		}
		return;
	}

	if (resolvedResponse == SSPP_RESPONSE_IGNORE) {
		positionAge.xyz = proposedPosition;
	} else if (resolvedResponse == SSPP_RESPONSE_BOUNCE) {
		velocityLifetime.xyz = bouncedVelocity;
		float remainingTime = max(1.0f - saturate(hit.TimeOfImpact), 0.0f) * stepTime;
		positionAge.xyz = contactCenter + bouncedVelocity * remainingTime;
	} else if (resolvedResponse == SSPP_RESPONSE_SLIDE) {
		float3 relativeVelocity = impactVelocity - surfaceVelocity;
		float inwardSpeed = min(dot(relativeVelocity, normal), 0.0f);
		relativeVelocity = (relativeVelocity - normal * inwardSpeed) * (1.0f - friction);
		velocityLifetime.xyz = relativeVelocity + surfaceVelocity;
		float remainingTime = max(1.0f - saturate(hit.TimeOfImpact), 0.0f) * stepTime;
		positionAge.xyz = contactCenter + velocityLifetime.xyz * remainingTime;
	} else {
		SSPPRecordInvalidRule();
		SSPPRouteFree(reference);
		return;
	}

	if (!all(isfinite(positionAge)) || !all(isfinite(velocityLifetime))) {
		SSPPRecordInvalidRule();
		SSPPRouteFree(reference);
		return;
	}
	PositionAge[particleID] = positionAge;
	VelocityLifetime[particleID] = velocityLifetime;
	Metadata[particleID] = metadata;
	if (positionAge.w >= velocityLifetime.w)
		SSPPRouteFree(reference);
	else if (metadata.x == inputSimulationProfile)
		SSPPRouteDynamic(reference, SSPPDispatchClass);
	else
		SSPPRouteCurrentDynamic(reference, metadata);
}
