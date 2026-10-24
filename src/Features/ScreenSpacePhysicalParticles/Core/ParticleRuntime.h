#pragma once

#include "Buffer.h"
#include "Features/ScreenSpacePhysicalParticles/Core/ParticleTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

// ParticleRuntime owns one stable-slot SoA particle pool shared by all SSPP
// features. Features register immutable IDs for configuration and extension
// points; particles reference slots by ID plus generation so stale lifecycle
// entries cannot mutate recycled particles.
//
// QueueBurst is the only gameplay-thread producer API. Registry and persistent-
// emitter APIs run on the render/control thread. RunFrame drains producer work,
// updates emitters, grants capacity by priority and feature budget, initializes
// newborns, simulates active lists, applies lifecycle routes, classifies visible
// particles, draws registered renderers, and queues asynchronous diagnostics.
namespace ScreenSpacePhysicalParticles::Core
{
	enum class BuiltinMaintenancePolicy : std::uint32_t
	{
		kNone,
		kKeepUntilLifetime,
		kDebugWakeCycle
	};

	struct PersistentListDesc
	{
		std::string name;
		std::uint32_t ownerFeature{};
		float maintenanceIntervalSeconds{};
		std::uint32_t capacity{};  // zero uses the global particle capacity
		BuiltinMaintenancePolicy builtinPolicy{ BuiltinMaintenancePolicy::kNone };
		// Query-policy bits required by feature-owned maintenance while this list
		// is nonempty. Static splats use zero; grounded leaves request capsules.
		std::uint32_t requiredCollisionProviders{};
	};

	struct ParticleMaintenanceContext
	{
		ID3D11ShaderResourceView* inputReferences{};  // t0
		std::uint32_t inputBase{};
		std::uint32_t inputCounterOffset{};
		std::uint32_t particleCapacity{};
		ID3D11Buffer* dispatchArguments{};
		std::uint32_t dispatchArgumentOffset{};
		// Mutable canonical SoA contract for custom maintenance. Bind these only
		// as u0..u4 and read through RWStructuredBuffer; never bind matching SRVs.
		ID3D11UnorderedAccessView* positionAge{};       // u0
		ID3D11UnorderedAccessView* velocityLifetime{};  // u1
		ID3D11UnorderedAccessView* appearance{};        // u2
		ID3D11UnorderedAccessView* metadata{};          // u3
		ID3D11UnorderedAccessView* contactData{};       // u4
		ID3D11ShaderResourceView* auxiliaryState{};     // t8
		ID3D11ShaderResourceView* emitterStates{};      // t9
		ID3D11ShaderResourceView* routeDescriptors{};   // common t7
		ID3D11UnorderedAccessView* routeArena{};        // fixed common slot u5
		ID3D11UnorderedAccessView* counters{};          // fixed common slot u7
		ID3D11Buffer* frameConstants{};
		ID3D11Buffer* lifecycleConstants{};
		std::uint32_t keepRoute{};
		std::uint32_t freeRoute{ kFreeReturnRoute };
		std::uint32_t dynamicRouteBase{ kDynamicRouteBase };
		float elapsedSeconds{};
	};

	class IPersistentParticleMaintenance
	{
	public:
		virtual ~IPersistentParticleMaintenance() = default;
		// Summary: Reports whether persistent maintenance can dispatch.
		// Args: None.
		// Returns: True when Dispatch has all required resources.
		[[nodiscard]] virtual bool IsReady() const noexcept = 0;
		// Summary: Updates or routes particles in a registered persistent list.
		// Args:
		//   a_context: Shared buffers, indirect arguments, and route destinations.
		// Returns: None.
		virtual void Dispatch(const ParticleMaintenanceContext& a_context) = 0;
	};

	struct GPUEventConsumerContext
	{
		ID3D11ShaderResourceView* events{};
		ID3D11UnorderedAccessView* counters{};  // fixed common slot u7
		ID3D11Buffer* dispatchArguments{};
		std::uint32_t dispatchArgumentOffset{};
		ID3D11Buffer* lifecycleConstants{};
	};

	class IGPUEventConsumer
	{
	public:
		virtual ~IGPUEventConsumer() = default;
		// Summary: Reports whether the event consumer can dispatch.
		// Args: None.
		// Returns: True when Consume has all required resources.
		[[nodiscard]] virtual bool IsReady() const noexcept = 0;
		// Summary: Consumes events for one registered route using indirect dispatch.
		// Args:
		//   a_context: Route event buffer, counters, constants, and arguments.
		// Returns: None.
		virtual void Consume(const GPUEventConsumerContext& a_context) = 0;
	};

	struct NewbornInitializerDesc
	{
		std::string name;
		std::string profilerName;
	};

	// Binding contract for feature-owned newborn initialization. A callback filters
	// emission chunks by customInitialization and routes each matching newborn once.
	struct NewbornInitializationContext
	{
		ID3D11DeviceContext* context{};
		ID3D11ShaderResourceView* frameRequests{};       // t0
		ID3D11ShaderResourceView* emissionChunks{};      // t1
		ID3D11ShaderResourceView* freeIDs{};             // t2
		ID3D11ShaderResourceView* particleTemplates{};   // t3
		ID3D11ShaderResourceView* sceneDepth{};          // t4
		ID3D11ShaderResourceView* solidHeight{};         // t5
		ID3D11ShaderResourceView* waterHeight{};         // t6
		ID3D11ShaderResourceView* routeDescriptors{};    // t7
		ID3D11ShaderResourceView* simulationProfiles{};  // t8
		ID3D11UnorderedAccessView* positionAge{};        // u0
		ID3D11UnorderedAccessView* velocityLifetime{};   // u1
		ID3D11UnorderedAccessView* appearance{};         // u2
		ID3D11UnorderedAccessView* metadata{};           // u3
		ID3D11UnorderedAccessView* contactData{};        // u4
		ID3D11UnorderedAccessView* routeArena{};         // u5
		ID3D11UnorderedAccessView* auxiliaryState{};     // u6
		ID3D11UnorderedAccessView* counters{};           // u7
		ID3D11Buffer* frameConstants{};                  // b0
		ID3D11Buffer* collisionConstants{};              // b1
		ID3D11Buffer* emitterConstants{};                // b2
		ID3D11Buffer* lifecycleConstants{};              // b3
		ID3D11Buffer* gamePerFrameConstants{};           // b12
		ID3D11Buffer* dispatchArguments{};
		std::uint32_t dispatchArgumentOffset{};
	};

	class INewbornParticleInitializer
	{
	public:
		virtual ~INewbornParticleInitializer() = default;
		virtual void SetupResources() {}
		virtual void Reset() {}
		virtual void ClearShaderCache() {}
		// Summary: Reports whether newborn initialization can dispatch.
		// Args: None.
		// Returns: True when Dispatch has all required resources.
		[[nodiscard]] virtual bool IsReady() const noexcept = 0;
		// Summary: Initializes matching emission chunks and appends every newborn to one route.
		// Args:
		//   a_context: Shared newborn inputs, mutable particle state, and route outputs.
		// Returns: None.
		virtual void Dispatch(const NewbornInitializationContext& a_context) = 0;
	};

	struct EventConsumerDesc
	{
		std::string name;
		std::uint32_t ownerFeature{};
		bool countOnly{};
	};

	struct RenderClassDesc
	{
		std::string name;
		std::uint32_t drawOrder{};
		bool enabled{ true };
		std::uint32_t vertexCountPerInstance{ 4 };
		std::uint32_t startVertexLocation{};
		std::uint32_t preparationThreads{};
		float cullRadiusScale{ 1.0F };
		float maximumDistance{};
		std::uint32_t cullingFlags{ static_cast<std::uint32_t>(RenderCullFlags::kFrustum) };
		std::string preparationProfilerName;
		std::string drawProfilerName;
	};

	struct ParticleRenderContext
	{
		ID3D11DeviceContext* context{};
		ID3D11ShaderResourceView* positionAge{};         // common t0
		ID3D11ShaderResourceView* velocityLifetime{};    // common t1
		ID3D11ShaderResourceView* appearance{};          // common t2
		ID3D11ShaderResourceView* metadata{};            // common t3
		ID3D11ShaderResourceView* contactData{};         // common t4
		ID3D11ShaderResourceView* visibleParticleIDs{};  // common t5
		ID3D11ShaderResourceView* counters{};            // common t6 (compute only)
		ID3D11Buffer* frameConstants{};                  // common b0 (compute only)
		ID3D11Buffer* gamePerFrameConstants{};           // common b12
		ID3D11Buffer* preparationArguments{};
		ID3D11Buffer* drawArguments{};
		RenderClassID renderClass{};
		std::uint32_t particleCapacity{};
		std::uint32_t visibleSegmentBase{};
		std::uint32_t preparationArgumentOffset{};
		std::uint32_t drawArgumentOffset{};

		// Summary: Dispatches this class's indirect preparation workload.
		// Args: None.
		// Returns: None.
		void DispatchPreparation() const noexcept;
		// Summary: Draws this class's visible segment with its indirect draw arguments.
		// Args: None.
		// Returns: None.
		void DrawInstancedIndirect() const noexcept;
	};

	class IParticleRenderer
	{
	public:
		virtual ~IParticleRenderer() = default;
		virtual void SetupResources() {}
		virtual void Reset() {}
		virtual void ClearShaderCache() {}
		// Summary: Reports whether this renderer can prepare and draw.
		// Args: None.
		// Returns: True when Prepare and Draw have all required resources.
		[[nodiscard]] virtual bool IsReady() const noexcept = 0;
		// Summary: Builds feature-owned per-visible-particle render data when configured.
		// Args:
		//   a_context: Shared state and indirect arguments for this render class.
		// Returns: None.
		virtual void Prepare(const ParticleRenderContext& a_context) = 0;
		// Summary: Draws one registered render class from its shared visible-ID segment.
		// Args:
		//   a_context: Shared particle SRVs and indirect draw arguments.
		// Returns: None.
		virtual void Draw(const ParticleRenderContext& a_context) = 0;
	};

	struct RendererInfo
	{
		RenderClassID id{ kInvalidRoute };
		std::string stableKey;
		std::string name;
		std::uint32_t drawOrder{};
		bool enabled{};
		bool rendererPresent{};
		bool ready{};
		std::uint32_t visibleCount{};
	};

	struct PersistentListInfo
	{
		PersistentListID id{ kInvalidRoute };
		std::string name;
		FeatureID ownerFeature{ kInvalidRoute };
		std::uint32_t activeCount{};
		std::uint32_t capacity{};
		float maintenanceIntervalSeconds{};
	};

	struct EventRouteInfo
	{
		EventRouteID id{ kInvalidRoute };
		std::string name;
		FeatureID ownerFeature{ kInvalidRoute };
		std::uint32_t produced{};
		std::uint32_t consumed{};
	};

	struct FeatureInfo
	{
		FeatureID id{ kInvalidRoute };
		std::string stableKey;
		// Zero in the registered policy means "use the global capacity". This
		// field reports the effective, capacity-clamped value used by the GPU.
		std::uint32_t maximumLiveParticles{};
		std::uint32_t liveParticles{};
		std::uint64_t requestedParticles{};
		std::uint64_t grantedParticles{};
		std::uint64_t droppedParticles{};
		std::uint64_t budgetClampedParticles{};
	};

	// Stable-slot GPU runtime shared by Physical Blood, Physical Leaves, and future
	// SSPP clients. Registered IDs are monotonic and remain valid for process life.
	class ParticleRuntime final
	{
	public:
		// Summary: Returns the process-lifetime runtime instance.
		// Args: None.
		// Returns: Shared particle runtime.
		static ParticleRuntime& GetSingleton() noexcept;
		~ParticleRuntime();

		static constexpr std::uint32_t kMinimumCapacity = 4096;
		static constexpr std::uint32_t kMaximumCapacity = 65536;
		static constexpr std::uint32_t kSimulationClassCount =
			static_cast<std::uint32_t>(SimulationClass::kCount);
		static constexpr std::uint32_t kRenderClassCount = kMaxRenderClasses;
		static constexpr std::uint32_t kPersistentListCount = kMaxPersistentLifecycleLists;
		static constexpr std::uint32_t kEventRouteCount = kMaxEventRoutes;
		static constexpr std::uint32_t kRouteCount = kLifecycleRouteCount;
		static constexpr std::uint32_t kMaxBurstRequests = 128;
		static constexpr std::uint32_t kMaxPersistentEmitters = 1024;
		static constexpr std::uint32_t kMaxEmitterCommands = kMaxPersistentEmitters;
		static constexpr std::uint32_t kMaxFrameRequests =
			kMaxBurstRequests + kMaxPersistentEmitters;
		static constexpr std::uint32_t kEmissionChunkSize = 256;
		// For request grants g_i, sum(ceil(g_i/chunk)) is bounded by
		// ceil(globalCapacity/chunk) + nonemptyRequestCount.
		static constexpr std::uint32_t kMaxEmissionChunks =
			kMaxFrameRequests + (kMaximumCapacity + kEmissionChunkSize - 1U) / kEmissionChunkSize;
		static constexpr std::uint32_t kMaxSimulationProfiles = 64;
		static constexpr std::uint32_t kMaxParticleTemplates = 64;
		static constexpr std::uint32_t kMaxEmissionProfiles = 64;
		static constexpr std::uint32_t kMaxCollisionRules =
			kMaxSimulationProfiles * kCollisionRuleSlots;
		static constexpr std::uint32_t kThreads = 64;

		// Built-in vertical-slice renderer and one test profile/template per
		// simulation class. Feature render classes begin at one.
		static constexpr RenderClassID kDebugRenderClass = 0;
		static constexpr FeatureID kDebugFeature = 0;
		static constexpr SimulationProfileID kDebugNoCollisionProfile = 0;
		static constexpr SimulationProfileID kDebugWorldProfile = 1;
		static constexpr SimulationProfileID kDebugWorldWaterProfile = 2;
		static constexpr SimulationProfileID kDebugWorldPlayerProfile = 3;
		static constexpr ParticleTemplateID kDebugNoCollisionTemplate = 0;
		static constexpr ParticleTemplateID kDebugWorldTemplate = 1;
		static constexpr ParticleTemplateID kDebugWorldWaterTemplate = 2;
		static constexpr ParticleTemplateID kDebugWorldPlayerTemplate = 3;

		// Summary: Allocates or reuses runtime resources for the requested capacity.
		// Args:
		//   a_capacity: Requested slots, clamped to kMinimumCapacity..kMaximumCapacity.
		// Returns: None.
		void SetupResources(std::uint32_t a_capacity);
		// Summary: Schedules a render-thread reset of particles and producer state.
		// Args: None.
		// Returns: None.
		void Reset();
		// Summary: Releases runtime and registered-extension shaders.
		// Args: None.
		// Returns: None.
		void ClearShaderCache();

		// Summary: Publishes one transient burst through the bounded producer queue.
		// Args:
		//   a_request: Fully specified burst command.
		// Returns: True when a queue slot was reserved and published.
		[[nodiscard]] bool QueueBurst(const BurstRequestGPU& a_request) noexcept;
		// Summary: Publishes the four built-in bursts used by the stress test.
		// Args:
		//   a_origin: Shared world-space burst origin.
		//   a_forward: World-space launch direction; normalized internally.
		// Returns: True when all four commands were published atomically.
		[[nodiscard]] bool QueueDebugBurst(const float3& a_origin, const float3& a_forward) noexcept;

		// Summary: Registers or finds a particle-feature allocation domain.
		// Args:
		//   a_stableKey: Nonempty process-stable feature key.
		// Returns: Monotonic feature ID, or kInvalidRoute when the registry is full.
		[[nodiscard]] FeatureID RegisterFeature(std::string_view a_stableKey);
		// Summary: Updates a feature's soft live-particle budget.
		// Args:
		//   a_feature: Registered feature ID.
		//   a_maximumLiveParticles: Maximum live slots; zero uses global capacity.
		// Returns: False when a_feature is not registered.
		[[nodiscard]] bool SetFeatureLiveParticleBudget(
			FeatureID a_feature,
			std::uint32_t a_maximumLiveParticles) noexcept;
		// Summary: Builds registry and diagnostic information for particle features.
		// Args: None.
		// Returns: Registered features with effective budgets and latest diagnostics.
		[[nodiscard]] std::vector<FeatureInfo> FeatureInfos() const;

		// Summary: Creates a persistent emitter and queues its GPU command.
		// Args:
		//   a_desc: Emitter configuration and ownership.
		// Returns: Generation-tagged handle, or an invalid handle on rejection.
		[[nodiscard]] EmitterHandle CreateEmitter(const EmitterDesc& a_desc) noexcept;
		// Summary: Replaces a persistent emitter's desired configuration.
		// Args:
		//   a_handle: Live generation-tagged emitter handle.
		//   a_desc: Replacement configuration and ownership.
		// Returns: True when the update was accepted or coalesced.
		[[nodiscard]] bool UpdateEmitter(EmitterHandle a_handle, const EmitterDesc& a_desc) noexcept;
		// Summary: Queues destruction of a persistent emitter generation.
		// Args:
		//   a_handle: Live generation-tagged emitter handle.
		// Returns: True when destruction was accepted or a pending create was canceled.
		[[nodiscard]] bool DestroyEmitter(EmitterHandle a_handle) noexcept;

		// Summary: Registers or updates a persistent lifecycle list.
		// Args:
		//   a_desc: List ownership, capacity, cadence, and built-in policy.
		//   a_maintenance: Optional non-owning feature callback.
		// Returns: Generation-tagged list handle, or an invalid handle on rejection.
		[[nodiscard]] PersistentListHandle RegisterPersistentList(
			const PersistentListDesc& a_desc,
			IPersistentParticleMaintenance* a_maintenance = nullptr);
		// Summary: Registers or updates a GPU exit-event route.
		// Args:
		//   a_desc: Route name, owner, and count-only behavior.
		//   a_consumer: Optional non-owning feature callback.
		// Returns: Generation-tagged route handle, or an invalid handle on rejection.
		[[nodiscard]] EventRouteHandle RegisterGPUEventConsumer(
			const EventConsumerDesc& a_desc,
			IGPUEventConsumer* a_consumer = nullptr);
		// Summary: Registers or updates a feature-owned newborn initializer.
		// Args:
		//   a_stableKey: Process-stable initializer key.
		//   a_desc: Display and profiler metadata.
		//   a_initializer: Non-owning callback valid until runtime teardown.
		// Returns: Monotonic initializer ID, or kInvalidRoute on rejection.
		[[nodiscard]] NewbornInitializerID RegisterNewbornInitializer(
			std::string_view a_stableKey,
			const NewbornInitializerDesc& a_desc,
			INewbornParticleInitializer* a_initializer);
		// Summary: Registers or updates a renderer under a monotonic class ID.
		// Args:
		//   a_stableKey: Process-stable render-class key.
		//   a_desc: Draw, culling, preparation, and profiler configuration.
		//   a_renderer: Non-owning callback valid until runtime teardown.
		// Returns: Render-class ID, or kInvalidRoute when the registry is full.
		[[nodiscard]] RenderClassID RegisterRenderer(
			std::string_view a_stableKey,
			const RenderClassDesc& a_desc,
			IParticleRenderer* a_renderer);
		// Summary: Enables or disables classification and drawing for a render class.
		// Args:
		//   a_id: Registered render-class ID.
		//   a_enabled: Desired state.
		// Returns: False when a_id is not registered.
		[[nodiscard]] bool SetRendererEnabled(RenderClassID a_id, bool a_enabled) noexcept;
		// Summary: Builds registry and diagnostic information for renderers.
		// Args: None.
		// Returns: Registered renderers with readiness and latest visible counts.
		[[nodiscard]] std::vector<RendererInfo> RendererInfos() const;
		// Summary: Builds registry and diagnostic information for persistent lists.
		// Args: None.
		// Returns: Registered persistent lists with effective capacities and counts.
		[[nodiscard]] std::vector<PersistentListInfo> PersistentListInfos() const;
		// Summary: Builds registry and diagnostic information for event routes.
		// Args: None.
		// Returns: Registered event routes with production and consumption counts.
		[[nodiscard]] std::vector<EventRouteInfo> EventRouteInfos() const;
		// Summary: Schedules a feature's dynamic and persistent particles for recycling.
		// Args:
		//   a_ownerFeature: Registered owner feature ID.
		// Returns: Number of newly queued drain requests.
		[[nodiscard]] std::uint32_t RequestFeatureDrain(std::uint32_t a_ownerFeature) noexcept;

		// Summary: Reads collision providers required by live or queued work.
		// Args: None.
		// Returns: Conservative collision-provider mask.
		[[nodiscard]] std::uint32_t RequiredCollisionProviders() const noexcept
		{
			return static_cast<std::uint32_t>(
				collisionProviderActivityState.load(std::memory_order_acquire) >> 32U);
		}

		// Summary: Executes one complete shared GPU-runtime frame.
		// Args:
		//   a_deltaTime: Unpaused simulation step in seconds.
		// Returns: None.
		void RunFrame(float a_deltaTime);

		// Summary: Updates a profile and schedules active-list reclassification if needed.
		// Args:
		//   a_id: Profile ID to replace.
		//   a_profile: Valid finite profile data.
		// Returns: None.
		void SetSimulationProfile(SimulationProfileID a_id, const SimulationProfileGPU& a_profile);
		// Summary: Registers or updates a simulation profile by stable key.
		// Args:
		//   a_stableKey: Nonempty process-stable profile key.
		//   a_profile: Profile data to upload.
		// Returns: Monotonic profile ID, or kInvalidRoute when full.
		[[nodiscard]] SimulationProfileID RegisterSimulationProfile(
			std::string_view a_stableKey,
			const SimulationProfileGPU& a_profile);
		// Summary: Sets one collision-channel response for a simulation profile.
		// Args:
		//   a_profileID: Registered simulation profile.
		//   a_channel: Exactly one semantic collision channel.
		//   a_rule: Response and lifecycle transition data.
		// Returns: None.
		void SetCollisionRule(
			SimulationProfileID a_profileID,
			CollisionChannel a_channel,
			const CollisionRuleGPU& a_rule);
		// Summary: Replaces a particle template at an explicit ID.
		// Args:
		//   a_id: Template ID within GPU capacity.
		//   a_template: Simulation, rendering, lifecycle, and spawn configuration.
		// Returns: None.
		void SetParticleTemplate(ParticleTemplateID a_id, const ParticleTemplateGPU& a_template);
		// Summary: Registers or updates a particle template by stable key.
		// Args:
		//   a_stableKey: Nonempty process-stable template key.
		//   a_template: Template data to upload.
		// Returns: Monotonic template ID, or kInvalidRoute when full.
		[[nodiscard]] ParticleTemplateID RegisterParticleTemplate(
			std::string_view a_stableKey,
			const ParticleTemplateGPU& a_template);
		// Summary: Replaces an emission profile at an explicit ID.
		// Args:
		//   a_id: Emission-profile ID within GPU capacity.
		//   a_profile: Spawn and distribution configuration.
		// Returns: None.
		void SetEmissionProfile(EmissionProfileID a_id, const EmissionProfileGPU& a_profile);
		// Summary: Registers or updates an emission profile by stable key.
		// Args:
		//   a_stableKey: Nonempty process-stable profile key.
		//   a_profile: Profile data to upload.
		// Returns: Monotonic emission-profile ID, or kInvalidRoute when full.
		[[nodiscard]] EmissionProfileID RegisterEmissionProfile(
			std::string_view a_stableKey,
			const EmissionProfileGPU& a_profile);

		[[nodiscard]] bool Ready() const noexcept { return ready; }
		[[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity; }
		[[nodiscard]] std::uint32_t RegisteredFeatureCount() const noexcept
		{
			return registeredFeatureCount.load(std::memory_order_acquire);
		}
		[[nodiscard]] std::uint64_t FrameCounter() const noexcept { return frameCounter; }
		[[nodiscard]] const DiagnosticsSnapshot& Diagnostics() const noexcept { return diagnostics; }

	private:
		class DebugParticleRenderer;
		// Summary: Registers built-in debug profiles, templates, and renderer class.
		// Args: None.
		// Returns: Constructed runtime.
		ParticleRuntime();

		enum Counter : std::uint32_t
		{
			kFreeTop,
			kDynamicA = 1,
			kDynamicB = kDynamicA + kSimulationClassCount,
			kRender = kDynamicB + kSimulationClassCount,
			kFrameCulled = kRender + kRenderClassCount,
			kAllocated,
			kHighWater,
			kBurstRequests,
			kRequestedParticles,
			kGrantedParticles,
			kDroppedParticles,
			kRecycledParticles,
			kInvalidProfiles,
			kListOverflows,
			kEventOverflows,
			kCollisionQueries,
			kCollisionHits = kCollisionQueries + 3,
			kCollisionResponses = kCollisionHits + 3,
			kInvalidCollisionRules = kCollisionResponses + 7,
			kPersistentA,
			kPersistentB = kPersistentA + kPersistentListCount,
			kFrameRequestCount = kPersistentB + kPersistentListCount,
			kEmissionChunkCount,
			kFrameActiveEmitterCount,
			kFrameOwnedParticleCount,
			kEmitterCreatesApplied,
			kEmitterUpdatesApplied,
			kEmitterDestroysApplied,
			kPersistentRequests,
			kPersistentParticlesRequested,
			kPersistentParticlesGranted,
			kPersistentParticlesDropped,
			kInvalidEmitterCommands,
			kRequestArenaOverflows,
			kEmissionChunkOverflows,
			kEmitterBudgetClamps,
			kOwnerGenerationMismatches,
			kRoute = kOwnerGenerationMismatches + 1,
			kEventCount = kRoute + kRouteCount,
			kEventsProducedByRoute,
			kEventsConsumedByRoute = kEventsProducedByRoute + kEventRouteCount,
			kStaleReferences = kEventsConsumedByRoute + kEventRouteCount,
			kDuplicateRoutes,
			kInvalidDestinations,
			kQuarantinedParticles,
			kFreeReturns,
			kMaintenanceRuns,
			kDynamicTransitions,
			kPersistentTransitions = kDynamicTransitions + kSimulationClassCount,
			kRenderCandidates = kPersistentTransitions + kPersistentListCount,
			kRenderFrustumCulled,
			kRenderDistanceCulled,
			kRenderDisabledOrMissing,
			kRenderInvalidClass,
			kRenderStaleReferences,
			kRenderBinOverflows,
			kFeatureLive = kRenderBinOverflows + 1,
			kFeatureRequested = kFeatureLive + kMaxParticleFeatures,
			kFeatureGranted = kFeatureRequested + kMaxParticleFeatures,
			kFeatureDropped = kFeatureGranted + kMaxParticleFeatures,
			kFeatureBudgetClamped = kFeatureDropped + kMaxParticleFeatures,
			kCounterCount = kFeatureBudgetClamped + kMaxParticleFeatures
		};

		struct alignas(16) FrameConstants
		{
			std::uint32_t capacity{};
			std::uint32_t listSegmentCapacity{};
			std::uint32_t inputCounterBase{};
			std::uint32_t outputCounterBase{};

			std::uint32_t dispatchClass{};
			std::uint32_t pad0{};
			std::uint32_t simulationProfileCount{};
			std::uint32_t particleTemplateCount{};

			float deltaTime{};
			std::uint32_t renderClassCount{ kRenderClassCount };
			std::uint32_t renderCounterBase{ kRender };
			std::uint32_t frameCulledCounter{ kFrameCulled };

			std::uint32_t freeTopCounter{ kFreeTop };
			std::uint32_t allocatedCounter{ kAllocated };
			std::uint32_t highWaterCounter{ kHighWater };
			std::uint32_t diagnosticsBase{ kBurstRequests };

			std::uint32_t registeredFeatureCount{};
			std::uint32_t featureLiveCounterBase{ kFeatureLive };
			std::uint32_t featureRequestedCounterBase{ kFeatureRequested };
			std::uint32_t featureGrantedCounterBase{ kFeatureGranted };

			std::uint32_t featureDroppedCounterBase{ kFeatureDropped };
			std::uint32_t featureBudgetClampedCounterBase{ kFeatureBudgetClamped };
			std::uint32_t pad[2]{};
		};
		STATIC_ASSERT_ALIGNAS_16(FrameConstants);
		static_assert(sizeof(FrameConstants) == 96);

		// Exact 64-byte mirror of the collision-provider constant buffer at b1.
		struct alignas(16) CollisionFrameConstants
		{
			std::uint32_t sceneDepthAvailable{};
			std::uint32_t solidHeightAvailable{};
			std::uint32_t waterHeightAvailable{};
			std::uint32_t playerCapsuleCount{};

			float2 solidCenter{};
			float solidHalfExtent{};
			float solidTexelSize{};

			float2 waterCenter{};
			float waterHalfExtent{};
			float waterTexelSize{};

			float3 playerVelocity{};
			std::uint32_t collisionRuleCount{};
		};
		STATIC_ASSERT_ALIGNAS_16(CollisionFrameConstants);
		static_assert(sizeof(CollisionFrameConstants) == 64);

		struct alignas(16) EmitterFrameConstants
		{
			std::uint32_t maxEmitters{ kMaxPersistentEmitters };
			std::uint32_t activeEmitterCount{};
			std::uint32_t cpuRequestCount{};
			std::uint32_t emitterCommandCount{};

			std::uint32_t maxFrameRequests{ kMaxFrameRequests };
			std::uint32_t maxEmissionChunks{ kMaxEmissionChunks };
			std::uint32_t frameRequestCounter{ kFrameRequestCount };
			std::uint32_t emissionChunkCounter{ kEmissionChunkCount };

			std::uint32_t activeEmitterCounter{ kFrameActiveEmitterCount };
			std::uint32_t ownedParticleCounter{ kFrameOwnedParticleCount };
			std::uint32_t emitterDiagnosticsBase{ kEmitterCreatesApplied };
			std::uint32_t planningPriority{};

			std::uint32_t emissionChunkSize{ kEmissionChunkSize };
			std::uint32_t emissionProfileCount{};
			float deltaTime{};
			std::uint32_t readyInitializerMask{};

			std::uint32_t planningFeature{ kInvalidRoute };
			std::uint32_t pad[3]{};
		};
		STATIC_ASSERT_ALIGNAS_16(EmitterFrameConstants);
		static_assert(sizeof(EmitterFrameConstants) == 80);

		struct alignas(16) LifecycleFrameConstants
		{
			std::uint32_t routeCounterBase{ kRoute };
			std::uint32_t eventCountCounter{ kEventCount };
			std::uint32_t eventCapacity{};
			std::uint32_t routeEpoch{ 1 };

			std::uint32_t persistentCounterBaseA{ kPersistentA };
			std::uint32_t persistentCounterBaseB{ kPersistentB };
			std::uint32_t persistentDestinationParityMask{};
			std::uint32_t persistentListCount{};

			std::uint32_t eventsProducedCounterBase{ kEventsProducedByRoute };
			std::uint32_t eventsConsumedCounterBase{ kEventsConsumedByRoute };
			std::uint32_t lifecycleDiagnosticsBase{ kStaleReferences };
			std::uint32_t dispatchRoute{};

			std::uint32_t maintenanceList{};
			std::uint32_t maintenancePolicy{};
			float maintenanceElapsed{};
			std::uint32_t retiringOwnerFeature{ kInvalidRoute };
		};
		STATIC_ASSERT_ALIGNAS_16(LifecycleFrameConstants);
		static_assert(sizeof(LifecycleFrameConstants) == 64);

		struct alignas(16) RenderFrameConstants
		{
			std::uint32_t diagnosticsBase{ kRenderCandidates };
			std::uint32_t registeredClassCount{};
			std::uint32_t classCapacity{ kRenderClassCount };
			std::uint32_t pad{};
		};
		STATIC_ASSERT_ALIGNAS_16(RenderFrameConstants);
		static_assert(sizeof(RenderFrameConstants) == 16);

		enum class CPUSlotState : std::uint8_t
		{
			kFree,
			kLive,
			kPendingDestroy
		};

		enum class BurstSlotState : std::uint8_t
		{
			kEmpty,
			kWriting,
			kReady,
			kReading
		};

		struct BurstQueueSlot
		{
			std::atomic<BurstSlotState> state{ BurstSlotState::kEmpty };
			BurstRequestGPU request{};
			std::uint32_t generation{};
		};

		enum class PendingEmitterOperation : std::uint8_t
		{
			kNone,
			kCreate,
			kUpdate,
			kDestroy,
			kCancelCreate
		};

		struct CPUEmitterSlot
		{
			EmitterDesc desired{};
			std::uint32_t generation{};
			CPUSlotState state{ CPUSlotState::kFree };
			PendingEmitterOperation pending{ PendingEmitterOperation::kNone };
			bool queued{};
		};

		struct EmitterUploadCounts
		{
			std::uint32_t commands{};
			std::uint32_t activeEmitters{};
		};

		struct PersistentListSlot
		{
			PersistentListDesc desc{};
			std::string profilerName;
			IPersistentParticleMaintenance* maintenance{};
			std::uint32_t generation{};
			std::uint32_t currentArena{};
			float elapsedSeconds{};
			bool registered{};
			bool drainRequested{};
			bool drainThisFrame{};
			bool rebuildThisFrame{};
		};

		struct EventConsumerSlot
		{
			EventConsumerDesc desc{};
			IGPUEventConsumer* consumer{};
			std::uint32_t generation{};
			bool registered{};
		};

		struct NewbornInitializerSlot
		{
			std::string stableKey;
			NewbornInitializerDesc desc{};
			INewbornParticleInitializer* initializer{};
			bool registered{};
			bool setupPending{ true };
		};

		struct InitializerExecutionSlot
		{
			NewbornInitializerID id{};
			INewbornParticleInitializer* initializer{};
			NewbornInitializerDesc desc{};
		};

		struct RendererSlot
		{
			std::string stableKey;
			RenderClassDesc desc{};
			IParticleRenderer* renderer{};
			bool registered{};
			bool setupPending{ true };
		};

		// Immutable for the duration of a render frame. The render/control thread
		// publishes this plan before invoking any draw callbacks, so re-entrant
		// configuration changes become visible on the following frame.
		struct RendererExecutionSlot
		{
			RenderClassID renderClass{};
			IParticleRenderer* renderer{};
			RenderClassDesc desc{};
			bool enabled{};
		};

		// Summary: Allocates structured buffers that hold particle state and registries.
		// Args: None.
		// Returns: None.
		void CreateCoreBuffers();
		// Summary: Allocates the asynchronous diagnostics staging ring and queries.
		// Args: None.
		// Returns: None.
		void CreateReadbackResources();
		// Summary: Allocates indirect dispatch/draw argument buffers and UAVs.
		// Args: None.
		// Returns: None.
		void CreateIndirectBuffers();
		// Summary: Creates shared blend, rasterizer, depth, and sampler states.
		// Args: None.
		// Returns: None.
		void CreateRenderStates();
		// Summary: Compiles every shared compute and built-in debug shader on demand.
		// Args: None.
		// Returns: None.
		void EnsureShaders();
		// Summary: Initializes all slots as free and clears counters and arenas.
		// Args: None.
		// Returns: None.
		void InitializePool();
		// Summary: Invalidates all producer queue slots and advances their generation.
		// Args: None.
		// Returns: None.
		void ResetBurstQueue() noexcept;
		// Summary: Atomically reserves all requested producer queue slots.
		// Args:
		//   a_slots: Output span receiving reserved slot indices.
		// Returns: True only when the complete span was reserved.
		[[nodiscard]] bool ReserveBurstSlots(std::span<std::uint32_t> a_slots) noexcept;
		// Summary: Copies and release-publishes one reserved burst slot.
		// Args:
		//   a_slot: Reserved queue index.
		//   a_request: Burst command to publish.
		//   a_generation: Producer generation assigned to the slot.
		// Returns: None.
		void PublishBurstSlot(
			std::uint32_t a_slot,
			const BurstRequestGPU& a_request,
			std::uint32_t a_generation) noexcept;
		// Summary: Clears the CPU emitter registry and repopulates its free stack.
		// Args: None.
		// Returns: None.
		void ResetEmitterRegistry();
		// Summary: Uploads changed profile, template, rule, feature, and class tables.
		// Args: None.
		// Returns: None.
		void UploadDirtyConfiguration();
		// Summary: Rebuilds and uploads lifecycle route descriptors when registrations change.
		// Args: None.
		// Returns: None.
		void UploadRouteDescriptors();
		// Summary: Runs deferred resource setup for newly registered renderers.
		// Args: None.
		// Returns: None.
		void SetupRegisteredRenderers();
		// Summary: Runs deferred resource setup for newly registered newborn initializers.
		// Args: None.
		// Returns: None.
		void SetupRegisteredInitializers();
		// Summary: Publishes a frame-stable list of ready newborn initializer callbacks.
		// Args: None.
		// Returns: None.
		void RefreshInitializerExecutionPlan();
		// Summary: Publishes sorted renderer callbacks and uploads changed class descriptors.
		// Args: None.
		// Returns: None.
		void RefreshRenderClassConfiguration();
		// Summary: Builds availability, transform, and capsule-count constants for collision.
		// Args: None.
		// Returns: Collision-provider constants for the current frame.
		[[nodiscard]] CollisionFrameConstants BuildCollisionFrameConstants() const noexcept;
		// Summary: Drains ready producer slots into the contiguous burst upload buffer.
		// Args: None.
		// Returns: Number of uploaded burst requests.
		std::uint32_t UploadBurstRequests();
		// Summary: Coalesces dirty emitter slots into commands and an active-emitter list.
		// Args: None.
		// Returns: Uploaded command and active-emitter counts.
		EmitterUploadCounts UploadEmitterCommands();
		// Summary: Copies CPU burst commands into the unified GPU request arena.
		// Args:
		//   a_requestCount: Number of uploaded commands to copy.
		// Returns: None.
		void CopyBurstRequestsToUnifiedArena(std::uint32_t a_requestCount);
		// Summary: Clears per-frame counters and initializes indirect argument state.
		// Args:
		//   a_constants: Shared frame counter offsets and capacity.
		//   a_emitterConstants: Emitter request and chunk counter offsets.
		//   a_lifecycleConstants: Route, event, and persistent-list counter offsets.
		// Returns: None.
		void ResetTransientCounters(
			const FrameConstants& a_constants,
			const EmitterFrameConstants& a_emitterConstants,
			const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Rebuilds dynamic active lists after profile simulation classes change.
		// Args:
		//   a_constants: Mutable frame constants used to dispatch each source class.
		// Returns: None.
		void ReclassifyDynamicParticles(FrameConstants& a_constants);
		// Summary: Clears the next dynamic active-list counters before simulation.
		// Args:
		//   a_constants: Shared frame counter offsets.
		// Returns: None.
		void ResetDynamicOutputCounters(const FrameConstants& a_constants);
		// Summary: Applies queued persistent-emitter create, update, and destroy commands.
		// Args:
		//   a_constants: Emitter command counts and buffer limits.
		// Returns: None.
		void ApplyEmitterCommands(const EmitterFrameConstants& a_constants);
		// Summary: Builds indirect dispatch arguments for each active simulation class.
		// Args:
		//   a_constants: Current active-list counter bases.
		// Returns: None.
		void BuildSimulationArguments(const FrameConstants& a_constants);
		// Summary: Runs due built-in or feature-owned persistent-list maintenance passes.
		// Args:
		//   a_constants: Shared particle and active-list state.
		//   a_lifecycleConstants: Persistent-list routing state.
		// Returns: None.
		void MaintainPersistentLists(
			const FrameConstants& a_constants,
			const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Simulates every nonempty dynamic active-list class indirectly.
		// Args:
		//   a_constants: Shared per-frame simulation constants.
		// Returns: None.
		void Simulate(const FrameConstants& a_constants);
		// Summary: Updates persistent emitters and appends their frame requests.
		// Args:
		//   a_constants: Shared particle counters and capacity.
		//   a_emitterConstants: Emitter counts, limits, and time step.
		// Returns: None.
		void UpdatePersistentEmitters(
			const FrameConstants& a_constants,
			const EmitterFrameConstants& a_emitterConstants);
		// Summary: Grants queued requests, allocates free slots, and initializes common newborns.
		// Args:
		//   a_constants: Shared particle counters and configuration counts.
		//   a_emitterConstants: Request, priority, and emission-chunk limits.
		// Returns: None.
		void PlanAndInitialize(
			const FrameConstants& a_constants,
			const EmitterFrameConstants& a_emitterConstants);
		// Summary: Dispatches ready feature-owned initialization over matching newborn chunks.
		// Args:
		//   a_constants: Shared particle and configuration state.
		//   a_emitterConstants: Emission chunk count and initializer mask.
		// Returns: None.
		void InitializeFeatureNewborns(
			const FrameConstants& a_constants,
			const EmitterFrameConstants& a_emitterConstants);
		// Summary: Builds indirect arguments for populated lifecycle and event routes.
		// Args:
		//   a_constants: Shared capacity and counter offsets.
		//   a_lifecycleConstants: Route layout and event capacity.
		// Returns: None.
		void BuildRouteArguments(
			const FrameConstants& a_constants,
			const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Builds the free-return indirect argument after persistent maintenance.
		// Args:
		//   a_constants: Shared capacity and counter offsets.
		//   a_lifecycleConstants: Free-return route layout.
		// Returns: None.
		void BuildFreeReturnArgument(
			const FrameConstants& a_constants,
			const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Applies generation-validated dynamic and persistent lifecycle routes.
		// Args:
		//   a_constants: Shared particle state and active-list destinations.
		//   a_lifecycleConstants: Route descriptors, epoch, and parity state.
		// Returns: None.
		void TransitionRoutes(
			const FrameConstants& a_constants,
			const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Invokes registered consumers for populated GPU event routes.
		// Args:
		//   a_lifecycleConstants: Event route offsets and indirect arguments.
		// Returns: None.
		void ConsumeEvents(const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Claims pending-free particles and appends them to the free stack.
		// Args:
		//   a_constants: Shared particle state and free-stack counters.
		//   a_lifecycleConstants: Free-return route and epoch state.
		// Returns: None.
		void MergeFreeReturns(
			const FrameConstants& a_constants,
			const LifecycleFrameConstants& a_lifecycleConstants);
		// Summary: Classifies dynamic and persistent particles into visible render segments.
		// Args:
		//   a_constants: Shared active-list, render-class, and culling state.
		// Returns: None.
		void Classify(const FrameConstants& a_constants);
		// Summary: Builds indirect preparation and draw arguments from visible counts.
		// Args:
		//   a_constants: Render counter layout and registered class count.
		// Returns: None.
		void BuildRenderArguments(const FrameConstants& a_constants);
		// Summary: Executes the frame-stable renderer plan in configured draw order.
		// Args: None.
		// Returns: None.
		void PrepareAndDrawRenderers();
		// Summary: Draws the built-in stress-test particles as camera-facing quads.
		// Args:
		//   a_context: Shared particle SRVs and debug-class indirect arguments.
		// Returns: None.
		void DrawBuiltinDebug(const ParticleRenderContext& a_context);
		// Summary: Copies counters into an available asynchronous diagnostics slot.
		// Args: None.
		// Returns: None.
		void QueueDiagnosticsReadback();
		// Summary: Maps one completed diagnostics slot and publishes its decoded snapshot.
		// Args:
		//   a_slot: Readback-ring slot to consume.
		// Returns: True when the slot's query completed and data was published.
		[[nodiscard]] bool ConsumeDiagnosticsReadback(std::uint32_t a_slot);
		// Summary: Clears compute SRV, UAV, and constant-buffer bindings used by SSPP.
		// Args: None.
		// Returns: None.
		void UnbindComputeResources() const;

		// Summary: Selects the dynamic active-list counter block read this frame.
		// Args: None.
		// Returns: Current counter-block base index.
		[[nodiscard]] std::uint32_t CurrentCounterBase() const noexcept;
		// Summary: Selects the dynamic active-list counter block written this frame.
		// Args: None.
		// Returns: Output counter-block base index.
		[[nodiscard]] std::uint32_t OutputCounterBase() const noexcept;
		// Summary: Converts a CPU emitter description into validated GPU state.
		// Args:
		//   a_desc: Desired emitter configuration.
		//   a_generation: Generation assigned to the GPU emitter.
		// Returns: Initialized active GPU emitter record.
		[[nodiscard]] static PersistentEmitterGPU BuildEmitterGPU(
			const EmitterDesc& a_desc,
			std::uint32_t a_generation) noexcept;
		// Summary: Recomputes collision-provider requirements from registered profiles.
		// Args: None.
		// Returns: None.
		void RefreshCollisionProviderConfiguration() noexcept;
		// Summary: Resolves provider requirements for one particle template.
		// Args:
		//   a_template: Particle-template ID.
		// Returns: Conservative provider mask, or zero for an invalid template.
		[[nodiscard]] std::uint32_t CollisionProvidersForTemplate(
			ParticleTemplateID a_template) const noexcept;
		// Summary: Atomically publishes provider requirements and advances activity epoch.
		// Args:
		//   a_mask: Provider bits required by accepted producer-side work.
		// Returns: None.
		void RecordCollisionProviderActivity(std::uint32_t a_mask) noexcept;
		// Summary: Reads the producer activity epoch paired with the provider mask.
		// Args: None.
		// Returns: Current low-word activity epoch.
		[[nodiscard]] std::uint32_t CollisionProviderActivityEpoch() const noexcept;
		// Summary: Narrows the conservative provider mask after matching GPU diagnostics.
		// Args:
		//   a_sampledEpoch: Producer epoch represented by the diagnostics sample.
		//   a_requiredMask: Providers required by live GPU particles and emitters.
		// Returns: None.
		void ReconcileCollisionProviders(
			std::uint32_t a_sampledEpoch,
			std::uint32_t a_requiredMask) noexcept;

		std::uint32_t capacity{};
		std::uint32_t currentArena{};
		std::uint32_t persistentParityMask{};
		std::uint32_t persistentDestinationParityMask{};
		std::uint32_t routeEpoch{ 1 };
		std::uint64_t frameCounter{};
		bool ready{};
		bool configurationDirty{ true };
		std::atomic_bool routeDescriptorConfigurationDirty{ true };
		std::atomic_bool resetPending{};
		std::atomic_bool simulationProfileReclassificationPending{};

		std::array<BurstQueueSlot, kMaxBurstRequests> burstQueue{};
		std::atomic<std::uint32_t> burstReservationCursor{};
		std::atomic<std::uint32_t> burstQueueGeneration{ 1U };
		std::atomic<std::uint64_t> rejectedCPUCommands{};
		std::atomic<std::uint64_t> rejectedCPUParticles{};
		std::atomic<std::uint64_t> rejectedEmitterCommands{};
		std::atomic<std::uint64_t> rejectedEmitterHandles{};
		// [63:32] conservative provider mask, [31:0] producer activity epoch.
		// Updating both words in one CAS prevents a diagnostics clear from erasing
		// requirements concurrently published by gameplay producers.
		std::atomic<std::uint64_t> collisionProviderActivityState{};
		std::array<std::atomic<std::uint32_t>, kSimulationClassCount>
			simulationClassCollisionProviderMasks{};
		std::array<std::atomic<std::uint32_t>, kMaxParticleTemplates>
			particleTemplateCollisionProviderMasks{};
		std::array<std::atomic<std::uint32_t>, kPersistentListCount>
			persistentListCollisionProviderMasks{};
		std::uint32_t submittedActivityEpoch{};
		std::atomic<std::uint32_t> registeredFeatureCount{};
		std::array<std::atomic<std::uint64_t>, kMaxParticleFeatures>
			rejectedCPURequestedByFeature{};
		std::array<std::atomic<std::uint64_t>, kMaxParticleFeatures>
			rejectedCPUDroppedByFeature{};

		std::array<CPUEmitterSlot, kMaxPersistentEmitters> emitterSlots{};
		std::array<BurstRequestGPU, kMaxBurstRequests> burstUploadScratch{};
		std::array<EmitterCommandGPU, kMaxEmitterCommands> emitterCommandScratch{};
		std::array<EmitterHandle, kMaxPersistentEmitters> activeEmitterScratch{};
		std::array<std::atomic<std::uint32_t>, kMaxPersistentEmitters>
			liveEmitterCollisionProviderMasks{};
		std::array<std::uint32_t, kMaxEmitterCommands> dirtyEmitterSlots{};
		std::uint32_t dirtyEmitterCount{};
		std::vector<std::uint32_t> freeEmitterSlots;

		std::array<PersistentListSlot, kPersistentListCount> persistentLists{};
		std::array<EventConsumerSlot, kEventRouteCount> eventConsumers{};
		std::uint32_t registeredPersistentListCount{};
		std::uint32_t registeredEventRouteCount{};
		// Render-thread count committed with the matching GPU route descriptors.
		std::uint32_t publishedPersistentListCount{};
		std::array<std::uint32_t, kPersistentListCount> pendingFeatureDrains{};
		std::uint32_t pendingFeatureDrainCount{};

		std::array<NewbornInitializerSlot, kMaxNewbornInitializers> newbornInitializers{};
		std::array<InitializerExecutionSlot, kMaxNewbornInitializers> initializerExecutionPlan{};
		std::uint32_t registeredNewbornInitializerCount{};
		std::uint32_t initializerExecutionCount{};
		std::atomic<std::uint64_t> initializerConfigurationGeneration{ 1 };
		std::uint64_t publishedInitializerConfigurationGeneration{};
		std::atomic_bool initializerSetupPending{};

		std::array<RendererSlot, kRenderClassCount> renderers{};
		std::array<RendererExecutionSlot, kRenderClassCount> rendererExecutionPlan{};
		// Keep the CPU comparison cache byte-addressed. RenderClassGPU is explicitly
		// 16-byte aligned for upload, and embedding it here would force padding on
		// ParticleRuntime under MSVC /WX.
		std::array<std::byte, sizeof(RenderClassGPU) * kRenderClassCount>
			renderClassGPUDescriptorBytes{};
		std::uint32_t registeredRenderClassCount{};
		std::uint32_t rendererExecutionCount{};
		std::atomic<std::uint64_t> rendererConfigurationGeneration{ 1 };
		std::uint64_t publishedRendererConfigurationGeneration{};
		std::atomic_bool rendererSetupPending{};
		std::unique_ptr<DebugParticleRenderer> debugRenderer;

		std::vector<SimulationProfileGPU> simulationProfiles;
		std::vector<CollisionRuleGPU> collisionRules;
		std::vector<ParticleTemplateGPU> particleTemplates;
		std::vector<EmissionProfileGPU> emissionProfiles;
		std::vector<std::string> simulationProfileKeys;
		std::vector<std::string> particleTemplateKeys;
		std::vector<std::string> emissionProfileKeys;
		std::vector<std::string> featureKeys;
		std::vector<ParticleFeatureGPU> features;
		std::atomic<std::uint64_t> featureConfigurationGeneration{ 1 };
		std::uint64_t uploadedFeatureConfigurationGeneration{};

		std::unique_ptr<Buffer> positionAge;
		std::unique_ptr<Buffer> velocityLifetime;
		std::unique_ptr<Buffer> appearance;
		std::unique_ptr<Buffer> metadata;
		std::unique_ptr<Buffer> contactData;
		std::unique_ptr<Buffer> particleAuxiliaryState;
		std::unique_ptr<Buffer> freeIDs;
		std::unique_ptr<Buffer> counters;
		std::array<std::unique_ptr<Buffer>, 2> dynamicArenas;
		std::array<std::unique_ptr<Buffer>, 2> persistentArenas;
		std::unique_ptr<Buffer> routeArena;
		std::unique_ptr<Buffer> routeDescriptors;
		std::unique_ptr<Buffer> eventArena;
		std::unique_ptr<Buffer> renderIndexArena;
		std::unique_ptr<Buffer> renderClassBuffer;
		std::unique_ptr<Buffer> burstUpload;
		std::unique_ptr<Buffer> frameRequests;
		std::unique_ptr<Buffer> emissionChunks;
		std::unique_ptr<Buffer> emitterStates;
		std::unique_ptr<Buffer> emitterCommandUpload;
		std::unique_ptr<Buffer> activeEmitterUpload;
		std::unique_ptr<Buffer> simulationProfileBuffer;
		std::unique_ptr<Buffer> collisionRuleBuffer;
		std::unique_ptr<Buffer> particleTemplateBuffer;
		std::unique_ptr<Buffer> emissionProfileBuffer;
		std::unique_ptr<Buffer> featureBuffer;
		std::unique_ptr<ConstantBuffer> frameCB;
		std::unique_ptr<ConstantBuffer> collisionFrameCB;
		std::unique_ptr<ConstantBuffer> emitterFrameCB;
		std::unique_ptr<ConstantBuffer> lifecycleFrameCB;
		std::unique_ptr<ConstantBuffer> renderFrameCB;

		winrt::com_ptr<ID3D11Buffer> simulationDispatchArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> simulationDispatchArgsUAV;
		winrt::com_ptr<ID3D11Buffer> classifyDispatchArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> classifyDispatchArgsUAV;
		winrt::com_ptr<ID3D11Buffer> emissionDispatchArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> emissionDispatchArgsUAV;
		winrt::com_ptr<ID3D11Buffer> renderDrawArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> renderDrawArgsUAV;
		winrt::com_ptr<ID3D11Buffer> renderPreparationArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> renderPreparationArgsUAV;
		winrt::com_ptr<ID3D11Buffer> routeDispatchArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> routeDispatchArgsUAV;
		winrt::com_ptr<ID3D11Buffer> eventDispatchArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> eventDispatchArgsUAV;
		winrt::com_ptr<ID3D11Buffer> persistentDispatchArgs;
		winrt::com_ptr<ID3D11UnorderedAccessView> persistentDispatchArgsUAV;

		static constexpr std::uint32_t kReadbackSlots = 3;
		static constexpr std::uint32_t kReadbackCadence = 15;
		std::array<winrt::com_ptr<ID3D11Buffer>, kReadbackSlots> counterReadbacks;
		std::array<winrt::com_ptr<ID3D11Query>, kReadbackSlots> readbackQueries;
		std::array<bool, kReadbackSlots> readbackPending{};
		std::array<std::uint32_t, kReadbackSlots> readbackArena{};
		std::array<std::uint32_t, kReadbackSlots> readbackPersistentParity{};
		std::array<std::uint32_t, kReadbackSlots> readbackRenderClassCount{};
		std::array<std::uint32_t, kReadbackSlots> readbackFeatureCount{};
		std::array<std::uint64_t, kReadbackSlots> readbackFrame{};
		std::array<std::uint64_t, kReadbackSlots> readbackRejectedCPUCommands{};
		std::array<std::uint64_t, kReadbackSlots> readbackRejectedCPUParticles{};
		std::array<std::uint64_t, kReadbackSlots> readbackRejectedEmitterCommands{};
		std::array<std::uint64_t, kReadbackSlots> readbackRejectedEmitterHandles{};
		std::array<std::uint64_t, kReadbackSlots> readbackActivityEpoch{};
		std::array<std::array<std::uint64_t, kMaxParticleFeatures>, kReadbackSlots>
			readbackRejectedCPURequestedByFeature{};
		std::array<std::array<std::uint64_t, kMaxParticleFeatures>, kReadbackSlots>
			readbackRejectedCPUDroppedByFeature{};
		std::uint32_t readbackCursor{};
		DiagnosticsSnapshot diagnostics{};

		winrt::com_ptr<ID3D11ComputeShader> resetCountersCS;
		winrt::com_ptr<ID3D11ComputeShader> resetDynamicCountersCS;
		winrt::com_ptr<ID3D11ComputeShader> buildSimulationArgsCS;
		winrt::com_ptr<ID3D11ComputeShader> emissionPlanCS;
		winrt::com_ptr<ID3D11ComputeShader> emissionCS;
		winrt::com_ptr<ID3D11ComputeShader> resetEmitterFrameCS;
		winrt::com_ptr<ID3D11ComputeShader> applyEmitterCommandsCS;
		winrt::com_ptr<ID3D11ComputeShader> updateEmittersCS;
		winrt::com_ptr<ID3D11ComputeShader> buildEmissionArgsCS;
		winrt::com_ptr<ID3D11ComputeShader> simulationCS;
		winrt::com_ptr<ID3D11ComputeShader> reclassifySimulationProfilesCS;
		winrt::com_ptr<ID3D11ComputeShader> classifyCS;
		winrt::com_ptr<ID3D11ComputeShader> classifyPersistentCS;
		winrt::com_ptr<ID3D11ComputeShader> buildRenderArgsCS;
		winrt::com_ptr<ID3D11ComputeShader> buildRouteArgsCS;
		winrt::com_ptr<ID3D11ComputeShader> buildFreeRouteArgsCS;
		winrt::com_ptr<ID3D11ComputeShader> transitionCS;
		winrt::com_ptr<ID3D11ComputeShader> freeClaimCS;
		winrt::com_ptr<ID3D11ComputeShader> freeMergeCS;
		winrt::com_ptr<ID3D11ComputeShader> builtinMaintenanceCS;
		winrt::com_ptr<ID3D11ComputeShader> resetPersistentDestinationCS;
		winrt::com_ptr<ID3D11ComputeShader> countEventConsumerCS;
		winrt::com_ptr<ID3D11VertexShader> debugVS;
		winrt::com_ptr<ID3D11PixelShader> debugPS;
		winrt::com_ptr<ID3D11BlendState> debugBlendState;
		winrt::com_ptr<ID3D11DepthStencilState> debugDepthState;
		winrt::com_ptr<ID3D11RasterizerState> debugRasterizerState;
	};
}
