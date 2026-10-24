#pragma once

#include "Buffer.h"

#include <cstdint>

// Defines the CPU/GPU ABI for shared particle configuration, emission, state,
// lifecycle routing, rendering, and diagnostics. Aligned records are mirrored by
// ScreenSpacePhysicalParticles shader includes; preserve field order and size.
namespace ScreenSpacePhysicalParticles::Core
{
	using ParticleID = std::uint32_t;
	using FeatureID = std::uint32_t;
	using SimulationProfileID = std::uint32_t;
	using ParticleTemplateID = std::uint32_t;
	using EmissionProfileID = std::uint32_t;
	using NewbornInitializerID = std::uint32_t;
	using RenderClassID = std::uint32_t;
	using PersistentListID = std::uint32_t;
	using EventRouteID = std::uint32_t;

	inline constexpr std::uint32_t kMaxRenderClasses = 16;
	inline constexpr std::uint32_t kMaxNewbornInitializers = 8;
	inline constexpr std::uint32_t kMaxParticleFeatures = 32;

	inline constexpr ParticleID kInvalidParticleID = 0xFFFFFFFFU;
	inline constexpr std::uint32_t kInvalidRoute = 0xFFFFFFFFU;
	inline constexpr std::uint32_t kMaxPersistentLifecycleLists = 8;
	inline constexpr std::uint32_t kMaxEventRoutes = 8;
	inline constexpr std::uint32_t kDynamicRouteBase = 0;
	inline constexpr std::uint32_t kPersistentRouteBase = 4;
	inline constexpr std::uint32_t kFreeReturnRoute =
		kPersistentRouteBase + kMaxPersistentLifecycleLists;
	inline constexpr std::uint32_t kQuarantineRoute = kFreeReturnRoute + 1;
	inline constexpr std::uint32_t kLifecycleRouteCount = kQuarantineRoute + 1;

	enum class SimulationClass : std::uint32_t
	{
		kNoCollision,
		kWorld,
		kWorldWater,
		kWorldPlayer,
		kCount
	};
	static_assert(
		kDynamicRouteBase + static_cast<std::uint32_t>(SimulationClass::kCount) ==
		kPersistentRouteBase);
	static_assert(kPersistentRouteBase + kMaxPersistentLifecycleLists == kFreeReturnRoute);
	static_assert(kFreeReturnRoute + 1U == kQuarantineRoute);
	static_assert(kQuarantineRoute + 1U == kLifecycleRouteCount);

	enum class CollisionChannel : std::uint32_t
	{
		kWorldSolid = 1U << 0U,
		kWater = 1U << 1U,
		kPlayer = 1U << 2U
	};

	enum class CollisionResponse : std::uint32_t
	{
		kIgnore,
		kBounce,
		kSlide,
		kSettle,
		kStick,
		kKill,
		kEmitEventAndKill
	};

	enum class CollisionRuleFlags : std::uint32_t
	{
		kResetAge = 1U << 0U
	};

	// Provider methods selected independently from semantic collision channels.
	enum class CollisionQueryPolicy : std::uint32_t
	{
		kNone = 0,
		kPerspectiveDepth = 1U << 0U,
		kSolidHeightField = 1U << 1U,
		kWaterHeightField = 1U << 2U,
		kPlayerCapsules = 1U << 3U
	};

	inline constexpr std::uint32_t kCollisionRuleSlots = 3;

	enum class CollisionRuleSlot : std::uint32_t
	{
		kWorldSolid,
		kWater,
		kPlayer
	};

	enum class LifecyclePhase : std::uint32_t
	{
		kFree,
		kReserved,
		kDynamic,
		kPersistent,
		kPendingFree,
		kRecycling,
		kQuarantined
	};

	enum class RenderCullFlags : std::uint32_t
	{
		kNone = 0,
		kFrustum = 1U << 0U,
		kDistance = 1U << 1U
	};

	enum class RouteDestinationKind : std::uint32_t
	{
		kDynamic,
		kPersistent,
		kFreeReturn,
		kQuarantine
	};

	enum class DistributionType : std::uint32_t
	{
		kConstant,
		kUniform,
		kNormal,
		kBiasLow,
		kBiasHigh
	};

	// Joint sampling policy for speed and horizontal/vertical trajectory angles.
	enum class EmissionTrajectoryPolicy : std::uint32_t
	{
		kBoundedIndependent,
		kPercentileHeavyTailCoupled
	};

	enum class SpawnPlacement : std::uint32_t
	{
		kDirect,
		kProjectToWorldSolid,
		kProjectToWater,
		kFeatureCustom
	};

	enum class EmitterShape : std::uint32_t
	{
		kPoint,
		kBox
	};

	enum class EmitterCommandType : std::uint32_t
	{
		kCreate,
		kUpdate,
		kDestroy
	};

	enum class EmitterState : std::uint32_t
	{
		kFree,
		kActive
	};

	enum class EmitterFlags : std::uint32_t
	{
		kEnabled = 1U << 0U,
		kMaintainDensity = 1U << 1U
	};

	struct EmitterHandle
	{
		std::uint32_t index{ kInvalidRoute };
		std::uint32_t generation{};

		[[nodiscard]] bool Valid() const noexcept
		{
			return index != kInvalidRoute && generation != 0;
		}

		friend bool operator==(const EmitterHandle&, const EmitterHandle&) = default;
	};

	struct alignas(16) ScalarDistributionGPU
	{
		float2 range{};
		std::uint32_t type{ static_cast<std::uint32_t>(DistributionType::kConstant) };
		float shape{ 1.0F };
	};
	STATIC_ASSERT_ALIGNAS_16(ScalarDistributionGPU);
	static_assert(sizeof(ScalarDistributionGPU) == 16);

	struct alignas(16) SimulationProfileGPU
	{
		float3 acceleration{};
		float drag{};

		float restitution{};
		float friction{};
		float settleSpeed{};
		float surfaceOffset{};

		std::uint32_t collisionMask{};
		std::uint32_t queryPolicy{};
		std::uint32_t simulationClass{};
		std::uint32_t responseTableOffset{};
	};
	STATIC_ASSERT_ALIGNAS_16(SimulationProfileGPU);
	static_assert(sizeof(SimulationProfileGPU) == 48);

	struct alignas(16) CollisionRuleGPU
	{
		std::uint32_t response{ static_cast<std::uint32_t>(CollisionResponse::kIgnore) };
		std::uint32_t destinationListID{ kInvalidRoute };
		std::uint32_t nextSimulationProfileID{ kInvalidRoute };
		std::uint32_t nextRenderClassID{ kInvalidRoute };

		std::uint32_t eventRoute{ kInvalidRoute };
		std::uint32_t eventPayloadID{ kInvalidRoute };
		float restitutionOverride{ -1.0F };
		float frictionOverride{ -1.0F };

		float minimumParticleAge{};
		float collisionRadiusScale{ 1.0F };
		float separationOffset{};
		float minimumSettleNormalZ{ -1.0F };

		float nextLifetimeOverride{ -1.0F };
		std::uint32_t flags{};
		std::uint32_t pad[2]{};
	};
	STATIC_ASSERT_ALIGNAS_16(CollisionRuleGPU);
	static_assert(sizeof(CollisionRuleGPU) == 64);

	struct alignas(16) ParticleTemplateGPU
	{
		std::uint32_t simulationProfile{};
		std::uint32_t renderClass{};
		std::uint32_t pad0{};
		std::uint32_t lifecyclePhase{ static_cast<std::uint32_t>(LifecyclePhase::kDynamic) };

		std::uint32_t persistentListID{ kInvalidRoute };
		std::uint32_t customInitialization{ kInvalidRoute };
		std::uint32_t spawnPlacement{ static_cast<std::uint32_t>(SpawnPlacement::kDirect) };
		std::uint32_t flags{};
	};
	STATIC_ASSERT_ALIGNAS_16(ParticleTemplateGPU);
	static_assert(sizeof(ParticleTemplateGPU) == 32);

	// Process-stable feature allocation policy; zero permits the full global pool.
	struct alignas(16) ParticleFeatureGPU
	{
		std::uint32_t maximumLiveParticles{};
		std::uint32_t pad[3]{};
	};
	STATIC_ASSERT_ALIGNAS_16(ParticleFeatureGPU);
	static_assert(sizeof(ParticleFeatureGPU) == 16);

	struct alignas(16) EmissionProfileGPU
	{
		ScalarDistributionGPU initialSpeed{};
		ScalarDistributionGPU horizontalAngleDegrees{};
		ScalarDistributionGPU verticalAngleDegrees{};
		ScalarDistributionGPU lifetime{};
		ScalarDistributionGPU radius{};
		ScalarDistributionGPU opacity{};

		float generalScalar{ 1.0F };
		std::uint32_t spawnShape{};
		std::uint32_t spawnPlacement{ static_cast<std::uint32_t>(SpawnPlacement::kDirect) };
		std::uint32_t flags{};

		std::uint32_t trajectoryPolicy{
			static_cast<std::uint32_t>(EmissionTrajectoryPolicy::kBoundedIndependent)
		};
		std::uint32_t pad[3]{};
	};
	STATIC_ASSERT_ALIGNAS_16(EmissionProfileGPU);
	static_assert(sizeof(EmissionProfileGPU) == 128);

	// CPU-facing persistent-emitter description copied into an update command.
	struct EmitterDesc
	{
		float3 origin{};
		float strength{ 1.0F };
		float3 forward{ 0.0F, 1.0F, 0.0F };
		std::uint32_t seed{};
		float3 right{ 1.0F, 0.0F, 0.0F };
		ParticleTemplateID particleTemplate{};
		float3 vertical{ 0.0F, 0.0F, 1.0F };
		EmissionProfileID emissionProfile{};
		float3 boundsMin{};
		float particlesPerSecond{};
		float3 boundsMax{};
		std::uint32_t targetAlive{};
		std::uint32_t maximumAlive{};
		std::uint32_t priority{};
		std::uint32_t ownerFeature{};
		std::uint32_t ownerCell{};
		std::uint32_t flags{ static_cast<std::uint32_t>(EmitterFlags::kEnabled) };
	};

	// GPU-owned emitter state; update commands preserve runtime-maintained fields.
	struct alignas(16) PersistentEmitterGPU
	{
		float3 origin{};
		float strength{ 1.0F };

		float3 forward{ 0.0F, 1.0F, 0.0F };
		std::uint32_t seed{};

		float3 right{ 1.0F, 0.0F, 0.0F };
		std::uint32_t particleTemplate{};

		float3 vertical{ 0.0F, 0.0F, 1.0F };
		std::uint32_t emissionProfile{};

		float3 boundsMin{};
		float particlesPerSecond{};

		float3 boundsMax{};
		float spawnAccumulator{};

		std::uint32_t targetAlive{};
		std::uint32_t maximumAlive{};
		std::uint32_t aliveCount{};
		std::uint32_t generation{};

		std::uint32_t priority{};
		std::uint32_t ownerFeature{};
		std::uint32_t ownerCell{};
		std::uint32_t requestOrdinal{};

		std::uint32_t flags{};
		std::uint32_t state{ static_cast<std::uint32_t>(EmitterState::kFree) };
		std::uint32_t pad[2]{};
	};
	STATIC_ASSERT_ALIGNAS_16(PersistentEmitterGPU);
	static_assert(sizeof(PersistentEmitterGPU) == 144);

	struct alignas(16) EmitterCommandGPU
	{
		PersistentEmitterGPU emitter{};
		std::uint32_t emitterIndex{ kInvalidRoute };
		std::uint32_t operation{};
		std::uint32_t pad[2]{};
	};
	STATIC_ASSERT_ALIGNAS_16(EmitterCommandGPU);
	static_assert(sizeof(EmitterCommandGPU) == 160);

	// CPU-to-GPU burst command; shaders derive samples deterministically from seed.
	struct alignas(16) BurstRequestGPU
	{
		float3 origin{};
		float strength{ 1.0F };

		float3 forward{ 0.0F, 1.0F, 0.0F };
		std::uint32_t seed{};

		float3 right{ 1.0F, 0.0F, 0.0F };
		std::uint32_t count{};

		float3 vertical{ 0.0F, 0.0F, 1.0F };
		std::uint32_t particleTemplate{};

		ScalarDistributionGPU initialSpeed{};
		ScalarDistributionGPU horizontalAngleDegrees{};
		ScalarDistributionGPU verticalAngleDegrees{};
		ScalarDistributionGPU lifetime{};
		ScalarDistributionGPU radius{};
		ScalarDistributionGPU opacity{};

		float generalScalar{ 1.0F };
		std::uint32_t priority{};
		std::uint32_t payloadID{};
		std::uint32_t ownerEmitter{ kInvalidRoute };

		std::uint32_t ownerEmitterGeneration{};
		std::uint32_t ownerFeature{};
		std::uint32_t flags{};
		std::uint32_t trajectoryPolicy{
			static_cast<std::uint32_t>(EmissionTrajectoryPolicy::kBoundedIndependent)
		};

		float3 spawnBoundsMin{};
		std::uint32_t spawnShape{ static_cast<std::uint32_t>(EmitterShape::kPoint) };

		float3 spawnBoundsMax{};
		std::uint32_t spawnPlacement{ static_cast<std::uint32_t>(SpawnPlacement::kDirect) };
	};
	STATIC_ASSERT_ALIGNAS_16(BurstRequestGPU);
	static_assert(sizeof(BurstRequestGPU) == 224);

	struct alignas(16) EmissionChunkGPU
	{
		std::uint32_t requestIndex{};
		std::uint32_t freeBase{};
		std::uint32_t firstOrdinal{};
		std::uint32_t count{};

		std::uint32_t simulationClass{};
		std::uint32_t persistentListID{ kInvalidRoute };
		std::uint32_t customInitialization{ kInvalidRoute };
		std::uint32_t flags{};
	};
	STATIC_ASSERT_ALIGNAS_16(EmissionChunkGPU);
	static_assert(sizeof(EmissionChunkGPU) == 32);

	// Generation-tagged reference used by every lifecycle arena.
	struct ParticleReferenceGPU
	{
		ParticleID particleID{ kInvalidParticleID };
		std::uint32_t slotGeneration{};
	};
	static_assert(sizeof(ParticleReferenceGPU) == 8);

	// Ownership and lifecycle state for one stable slot. State changes pass through
	// generation-tagged route arenas before transition or free-return merge.
	struct alignas(16) ParticleAuxStateGPU
	{
		std::uint32_t emitterIndex{ kInvalidRoute };
		std::uint32_t emitterGeneration{};
		std::uint32_t ownerFeature{};
		std::uint32_t featurePayloadID{};

		std::uint32_t lifecyclePhase{ static_cast<std::uint32_t>(LifecyclePhase::kFree) };
		PersistentListID persistentListID{ kInvalidRoute };
		std::uint32_t lastRouteEpoch{};
		std::uint32_t flags{};
	};
	STATIC_ASSERT_ALIGNAS_16(ParticleAuxStateGPU);
	static_assert(sizeof(ParticleAuxStateGPU) == 32);

	struct alignas(16) RouteDescriptorGPU
	{
		std::uint32_t baseOffset{};
		std::uint32_t capacity{};
		std::uint32_t counterOffset{};
		std::uint32_t destinationKind{};

		std::uint32_t destinationIndex{};
		std::uint32_t ownerFeature{};
		std::uint32_t flags{};
		std::uint32_t pad{};
	};
	STATIC_ASSERT_ALIGNAS_16(RouteDescriptorGPU);
	static_assert(sizeof(RouteDescriptorGPU) == 32);

	struct alignas(16) ParticleExitEventGPU
	{
		float3 position{};
		float radius{};

		float3 velocity{};
		float amount{};

		float3 surfaceNormal{};
		float featureScalar{};

		EventRouteID eventRoute{ kInvalidRoute };
		std::uint32_t eventPayloadID{};
		std::uint32_t collisionChannel{};
		std::uint32_t flags{};
	};
	STATIC_ASSERT_ALIGNAS_16(ParticleExitEventGPU);
	static_assert(sizeof(ParticleExitEventGPU) == 64);

	// Fixed GPU descriptor for one registered render class.
	struct alignas(16) RenderClassGPU
	{
		std::uint32_t enabled{};
		std::uint32_t vertexCountPerInstance{ 4 };
		std::uint32_t startVertexLocation{};
		std::uint32_t preparationThreads{};

		float cullRadiusScale{ 1.0F };
		float maximumDistance{};  // zero disables distance culling
		std::uint32_t cullingFlags{ static_cast<std::uint32_t>(RenderCullFlags::kFrustum) };
		std::uint32_t pad{};
	};
	STATIC_ASSERT_ALIGNAS_16(RenderClassGPU);
	static_assert(sizeof(RenderClassGPU) == 32);

	struct PersistentListHandle
	{
		PersistentListID index{ kInvalidRoute };
		std::uint32_t generation{};

		[[nodiscard]] bool Valid() const noexcept
		{
			return index != kInvalidRoute && generation != 0;
		}
	};

	struct EventRouteHandle
	{
		EventRouteID index{ kInvalidRoute };
		std::uint32_t generation{};

		[[nodiscard]] bool Valid() const noexcept
		{
			return index != kInvalidRoute && generation != 0;
		}
	};

	struct DiagnosticsSnapshot
	{
		std::uint64_t sampledFrame{};
		std::uint32_t capacity{};
		std::uint32_t freeCount{};
		std::uint32_t allocatedCount{};
		std::uint32_t dynamicCount{};
		std::uint32_t persistentCount{};
		std::uint32_t highWaterMark{};
		std::uint32_t visibleCount{};
		std::uint32_t culledCount{};
		std::uint64_t burstRequests{};
		std::uint64_t requestedParticles{};
		std::uint64_t grantedParticles{};
		std::uint64_t droppedParticles{};
		std::uint64_t recycledParticles{};
		std::uint64_t invalidProfiles{};
		std::uint64_t listOverflows{};
		std::uint64_t eventOverflows{};
		std::uint64_t collisionQueries[3]{};
		std::uint64_t collisionHits[3]{};
		std::uint64_t collisionResponses[7]{};
		std::uint64_t invalidCollisionRules{};
		std::uint32_t activeEmitters{};
		std::uint32_t ownedParticles{};
		std::uint64_t emitterCreatesApplied{};
		std::uint64_t emitterUpdatesApplied{};
		std::uint64_t emitterDestroysApplied{};
		std::uint64_t persistentRequests{};
		std::uint64_t persistentParticlesRequested{};
		std::uint64_t persistentParticlesGranted{};
		std::uint64_t persistentParticlesDropped{};
		std::uint64_t invalidEmitterCommands{};
		std::uint64_t requestArenaOverflows{};
		std::uint64_t emissionChunkOverflows{};
		std::uint64_t emitterBudgetClamps{};
		std::uint64_t ownerGenerationMismatches{};
		std::uint64_t staleReferences{};
		std::uint64_t duplicateRoutes{};
		std::uint64_t invalidDestinations{};
		std::uint64_t quarantinedParticles{};
		std::uint64_t freeReturns{};
		std::uint64_t eventsProduced{};
		std::uint64_t eventsConsumed{};
		std::uint64_t maintenanceRuns{};
		std::uint64_t rejectedEmitterCommands{};
		std::uint64_t rejectedEmitterHandles{};
		std::uint32_t activeBySimulationClass[static_cast<std::uint32_t>(SimulationClass::kCount)]{};
		std::uint32_t activeByPersistentList[kMaxPersistentLifecycleLists]{};
		std::uint32_t eventsProducedByRoute[kMaxEventRoutes]{};
		std::uint32_t eventsConsumedByRoute[kMaxEventRoutes]{};
		std::uint32_t dynamicTransitions[static_cast<std::uint32_t>(SimulationClass::kCount)]{};
		std::uint32_t persistentTransitions[kMaxPersistentLifecycleLists]{};
		std::uint32_t registeredRenderClasses{};
		std::uint32_t renderCandidates{};
		std::uint32_t frustumCulled{};
		std::uint32_t distanceCulled{};
		std::uint32_t disabledOrMissingRenderers{};
		std::uint32_t invalidRenderClasses{};
		std::uint32_t renderStaleReferences{};
		std::uint32_t renderBinOverflows{};
		std::uint32_t visibleByRenderClass[kMaxRenderClasses]{};
		std::uint32_t registeredFeatures{};
		std::uint32_t liveByFeature[kMaxParticleFeatures]{};
		std::uint64_t requestedByFeature[kMaxParticleFeatures]{};
		std::uint64_t grantedByFeature[kMaxParticleFeatures]{};
		std::uint64_t droppedByFeature[kMaxParticleFeatures]{};
		std::uint64_t budgetClampedByFeature[kMaxParticleFeatures]{};
		bool valid{};
	};
}
