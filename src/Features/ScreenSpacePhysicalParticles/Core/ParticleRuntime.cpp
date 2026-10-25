#include "ParticleRuntime.h"

#include "Deferred.h"
#include "Features/ScreenSpacePhysicalParticles/Collision/PlayerCollisionProvider.h"
#include "Features/ScreenSpacePhysicalParticles/OrthographicHeightField.h"
#include "Features/ScreenSpacePhysicalParticles/ScreenSpaceCollisionDepth.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// Implements the shared GPU frame graph. CPU work is limited to registration,
// bounded command uploads, dispatch orchestration, and asynchronous readback;
// allocation, simulation, lifecycle routing, and visibility stay on the GPU.
namespace ScreenSpacePhysicalParticles::Core
{
	namespace
	{
		// Summary: Creates a typed structured buffer with matching SRV and optional UAV.
		// Args:
		//   a_count: Element count.
		//   a_name: D3D debug name.
		//   a_cpuWritable: Whether the CPU maps the buffer for writes.
		//   a_initialData: Optional contiguous initial contents.
		// Returns: Owned buffer wrapper with views matching its usage.
		template <class T>
		std::unique_ptr<Buffer> CreateStructuredBuffer(
			std::uint32_t a_count,
			const char* a_name,
			bool a_cpuWritable = false,
			const void* a_initialData = nullptr)
		{
			if (a_count == 0 ||
				a_count > std::numeric_limits<std::uint32_t>::max() / sizeof(T)) {
				throw std::invalid_argument("Invalid SSPP structured-buffer dimensions");
			}

			auto desc = StructuredBufferDesc<T>(
				static_cast<std::uint64_t>(a_count), !a_cpuWritable, a_cpuWritable);
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = a_initialData;
			auto buffer = std::make_unique<Buffer>(desc, a_initialData ? &initial : nullptr, a_name);

			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = a_count;
			buffer->CreateSRV(srv);
			if (!a_cpuWritable) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.Format = DXGI_FORMAT_UNKNOWN;
				uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				uav.Buffer.NumElements = a_count;
				buffer->CreateUAV(uav);
			}
			return buffer;
		}

		// Summary: Creates a raw UAV buffer usable as indirect arguments.
		// Args:
		//   a_bytes: Buffer size in bytes.
		//   a_name: D3D debug name.
		//   a_buffer: Destination buffer pointer.
		//   a_uav: Destination raw UAV pointer.
		// Returns: None.
		void CreateIndirectBuffer(
			std::uint32_t a_bytes,
			const char* a_name,
			winrt::com_ptr<ID3D11Buffer>& a_buffer,
			winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav)
		{
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.ByteWidth = a_bytes;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS |
			                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, a_buffer.put()));
			Util::SetResourceName(a_buffer.get(), "%s", a_name);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32_TYPELESS;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.NumElements = a_bytes / sizeof(std::uint32_t);
			uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			DX::ThrowIfFailed(globals::d3d::device->CreateUnorderedAccessView(
				a_buffer.get(), &uav, a_uav.put()));
			Util::SetResourceName(a_uav.get(), "%s UAV", a_name);
		}

		float3 Normalize(const float3& a_value) noexcept
		{
			if (a_value.LengthSquared() <= 1.0e-6F)
				return { 0.0F, 1.0F, 0.0F };
			auto normalized = a_value;
			normalized.Normalize();
			return normalized;
		}

		// Summary: Maps one semantic collision-channel bit to its rule-table slot.
		// Args:
		//   a_channel: Single collision channel.
		// Returns: Rule slot, or kInvalidRoute for combined or unknown values.
		std::uint32_t CollisionRuleIndex(CollisionChannel a_channel) noexcept
		{
			switch (a_channel) {
			case CollisionChannel::kWorldSolid:
				return static_cast<std::uint32_t>(CollisionRuleSlot::kWorldSolid);
			case CollisionChannel::kWater:
				return static_cast<std::uint32_t>(CollisionRuleSlot::kWater);
			case CollisionChannel::kPlayer:
				return static_cast<std::uint32_t>(CollisionRuleSlot::kPlayer);
			default:
				return kInvalidRoute;
			}
		}

		constexpr std::uint32_t kAllCollisionProviders =
			static_cast<std::uint32_t>(CollisionQueryPolicy::kPerspectiveDepth) |
			static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField) |
			static_cast<std::uint32_t>(CollisionQueryPolicy::kWaterHeightField) |
			static_cast<std::uint32_t>(CollisionQueryPolicy::kPlayerCapsules);
	}

	void ParticleRenderContext::DispatchPreparation() const noexcept
	{
		if (context && preparationArguments)
			context->DispatchIndirect(preparationArguments, preparationArgumentOffset);
	}

	void ParticleRenderContext::DrawInstancedIndirect() const noexcept
	{
		if (context && drawArguments)
			context->DrawInstancedIndirect(drawArguments, drawArgumentOffset);
	}

	class ParticleRuntime::DebugParticleRenderer final : public IParticleRenderer
	{
	public:
		explicit DebugParticleRenderer(ParticleRuntime& a_runtime) noexcept :
			runtime(a_runtime) {}

		[[nodiscard]] bool IsReady() const noexcept override
		{
			return runtime.debugVS && runtime.debugPS && runtime.debugBlendState &&
			       runtime.debugDepthState && runtime.debugRasterizerState;
		}

		void Prepare(const ParticleRenderContext&) override {}

		void Draw(const ParticleRenderContext& a_context) override
		{
			runtime.DrawBuiltinDebug(a_context);
		}

	private:
		ParticleRuntime& runtime;
	};

	ParticleRuntime& ParticleRuntime::GetSingleton() noexcept
	{
		static ParticleRuntime singleton;
		return singleton;
	}

	ParticleRuntime::ParticleRuntime()
	{
		simulationProfiles.resize(4);
		collisionRules.resize(4U * kCollisionRuleSlots);
		particleTemplates.resize(4);
		emissionProfiles.resize(1);
		simulationProfileKeys = {
			"SSPP.Debug.NoCollision",
			"SSPP.Debug.World",
			"SSPP.Debug.WorldWater",
			"SSPP.Debug.WorldPlayer"
		};
		particleTemplateKeys = {
			"SSPP.Debug.NoCollision",
			"SSPP.Debug.World",
			"SSPP.Debug.WorldWater",
			"SSPP.Debug.WorldPlayer"
		};
		emissionProfileKeys = { "SSPP.Debug.Default" };
		const auto debugFeature = RegisterFeature("SSPP.Debug");
		if (debugFeature != kDebugFeature)
			throw std::runtime_error("SSPP built-in debug feature did not receive feature zero");

		auto& debugEmission = emissionProfiles.front();
		debugEmission.initialSpeed = {
			{ 20.0F, 80.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F
		};
		debugEmission.horizontalAngleDegrees = {
			{ -30.0F, 30.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F
		};
		debugEmission.verticalAngleDegrees = {
			{ -10.0F, 35.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F
		};
		debugEmission.lifetime = {
			{ 2.0F, 5.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F
		};
		debugEmission.radius = {
			{ 3.0F, 7.0F }, static_cast<std::uint32_t>(DistributionType::kUniform), 1.0F
		};
		debugEmission.opacity = {
			{ 0.7F, 1.0F }, static_cast<std::uint32_t>(DistributionType::kUniform), 1.0F
		};
		debugEmission.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);

		const auto worldSolid = static_cast<std::uint32_t>(CollisionChannel::kWorldSolid);
		const auto water = static_cast<std::uint32_t>(CollisionChannel::kWater);
		const auto player = static_cast<std::uint32_t>(CollisionChannel::kPlayer);
		const auto perspective = static_cast<std::uint32_t>(CollisionQueryPolicy::kPerspectiveDepth);
		const auto solidHeight = static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField);
		const auto waterHeight = static_cast<std::uint32_t>(CollisionQueryPolicy::kWaterHeightField);
		const auto playerCapsules = static_cast<std::uint32_t>(CollisionQueryPolicy::kPlayerCapsules);

		for (std::uint32_t profileID = 0; profileID < simulationProfiles.size(); ++profileID) {
			auto& profile = simulationProfiles[profileID];
			profile.acceleration = { 0.0F, 0.0F, -180.0F };
			profile.drag = 0.12F;
			profile.restitution = 0.42F;
			profile.friction = 0.18F;
			profile.settleSpeed = 20.0F;
			profile.surfaceOffset = 0.15F;
			profile.responseTableOffset = profileID * kCollisionRuleSlots;

			auto& particleTemplate = particleTemplates[profileID];
			particleTemplate.simulationProfile = profileID;
			particleTemplate.renderClass = kDebugRenderClass;
			particleTemplate.lifecyclePhase = static_cast<std::uint32_t>(LifecyclePhase::kDynamic);
		}

		simulationProfiles[kDebugNoCollisionProfile].simulationClass =
			static_cast<std::uint32_t>(SimulationClass::kNoCollision);

		auto& worldProfile = simulationProfiles[kDebugWorldProfile];
		worldProfile.simulationClass = static_cast<std::uint32_t>(SimulationClass::kWorld);
		worldProfile.collisionMask = worldSolid;
		worldProfile.queryPolicy = perspective | solidHeight;

		auto& worldWaterProfile = simulationProfiles[kDebugWorldWaterProfile];
		worldWaterProfile.simulationClass = static_cast<std::uint32_t>(SimulationClass::kWorldWater);
		worldWaterProfile.collisionMask = worldSolid | water;
		worldWaterProfile.queryPolicy = perspective | solidHeight | waterHeight;

		auto& worldPlayerProfile = simulationProfiles[kDebugWorldPlayerProfile];
		worldPlayerProfile.simulationClass = static_cast<std::uint32_t>(SimulationClass::kWorldPlayer);
		worldPlayerProfile.collisionMask = worldSolid | player;
		worldPlayerProfile.queryPolicy = perspective | solidHeight | playerCapsules;

		CollisionRuleGPU bounce{};
		bounce.response = static_cast<std::uint32_t>(CollisionResponse::kBounce);
		const auto debugPersistent = RegisterPersistentList(
			{ "SSPP.DebugSettled", debugFeature, 0.25F, 0U,
				BuiltinMaintenancePolicy::kDebugWakeCycle });
		const auto debugEvents = RegisterGPUEventConsumer(
			{ "SSPP.DebugWaterEvents", debugFeature, true });
		CollisionRuleGPU settle = bounce;
		settle.response = static_cast<std::uint32_t>(CollisionResponse::kSettle);
		settle.destinationListID = debugPersistent.index;
		settle.minimumSettleNormalZ = 0.35F;
		settle.nextLifetimeOverride = 3.0F;
		settle.flags = static_cast<std::uint32_t>(CollisionRuleFlags::kResetAge);
		SetCollisionRule(kDebugWorldProfile, CollisionChannel::kWorldSolid, settle);
		SetCollisionRule(kDebugWorldWaterProfile, CollisionChannel::kWorldSolid, settle);
		CollisionRuleGPU waterEvent = bounce;
		waterEvent.response = static_cast<std::uint32_t>(CollisionResponse::kEmitEventAndKill);
		waterEvent.eventRoute = debugEvents.index;
		SetCollisionRule(kDebugWorldWaterProfile, CollisionChannel::kWater, waterEvent);
		SetCollisionRule(kDebugWorldPlayerProfile, CollisionChannel::kWorldSolid, settle);
		CollisionRuleGPU playerBounce = bounce;
		playerBounce.restitutionOverride = 0.65F;
		playerBounce.frictionOverride = 0.08F;
		SetCollisionRule(kDebugWorldPlayerProfile, CollisionChannel::kPlayer, playerBounce);
		ResetEmitterRegistry();

		debugRenderer = std::make_unique<DebugParticleRenderer>(*this);
		RenderClassDesc debugDesc{};
		debugDesc.name = "Built-in debug particles";
		debugDesc.drawOrder = 0;
		debugDesc.vertexCountPerInstance = 4;
		debugDesc.cullRadiusScale = 1.0F;
		debugDesc.cullingFlags = static_cast<std::uint32_t>(RenderCullFlags::kFrustum);
		debugDesc.preparationProfilerName = "SSPP::DebugParticlePrepare";
		debugDesc.drawProfilerName = "SSPP::DebugParticleRender";
		const auto debugClass = RegisterRenderer("SSPP.Debug", debugDesc, debugRenderer.get());
		if (debugClass != kDebugRenderClass)
			throw std::runtime_error("SSPP built-in debug renderer did not receive class zero");
	}

	ParticleRuntime::~ParticleRuntime() = default;

	FeatureID ParticleRuntime::RegisterFeature(std::string_view a_stableKey)
	{
		if (a_stableKey.empty())
			return kInvalidRoute;
		for (FeatureID id = 0; id < featureKeys.size(); ++id) {
			if (featureKeys[id] == a_stableKey)
				return id;
		}
		if (featureKeys.size() >= kMaxParticleFeatures) {
			logger::error(
				"[SSPP 2.0] Feature '{}' rejected: the {}-feature registry is full",
				a_stableKey,
				kMaxParticleFeatures);
			return kInvalidRoute;
		}
		featureKeys.emplace_back(a_stableKey);
		features.emplace_back();
		featureConfigurationGeneration.fetch_add(1, std::memory_order_release);
		const auto id = static_cast<FeatureID>(featureKeys.size() - 1U);
		registeredFeatureCount.store(static_cast<std::uint32_t>(featureKeys.size()), std::memory_order_release);
		return id;
	}

	bool ParticleRuntime::SetFeatureLiveParticleBudget(
		FeatureID a_feature,
		std::uint32_t a_maximumLiveParticles) noexcept
	{
		if (a_feature >= features.size())
			return false;
		const auto maximum = std::min(a_maximumLiveParticles, kMaximumCapacity);
		if (features[a_feature].maximumLiveParticles == maximum)
			return true;
		features[a_feature].maximumLiveParticles = maximum;
		featureConfigurationGeneration.fetch_add(1, std::memory_order_release);
		return true;
	}

	std::vector<FeatureInfo> ParticleRuntime::FeatureInfos() const
	{
		std::vector<FeatureInfo> infos;
		infos.reserve(featureKeys.size());
		for (FeatureID id = 0; id < featureKeys.size(); ++id) {
			const auto configuredMaximum = features[id].maximumLiveParticles;
			FeatureInfo info{};
			info.id = id;
			info.stableKey = featureKeys[id];
			info.maximumLiveParticles = configuredMaximum == 0 ?
			                                capacity :
			                                std::min(configuredMaximum, capacity);
			if (diagnostics.valid && id < diagnostics.registeredFeatures) {
				info.liveParticles = diagnostics.liveByFeature[id];
				info.requestedParticles = diagnostics.requestedByFeature[id];
				info.grantedParticles = diagnostics.grantedByFeature[id];
				info.droppedParticles = diagnostics.droppedByFeature[id];
				info.budgetClampedParticles = diagnostics.budgetClampedByFeature[id];
			}
			infos.push_back(std::move(info));
		}
		return infos;
	}

	PersistentListHandle ParticleRuntime::RegisterPersistentList(
		const PersistentListDesc& a_desc,
		IPersistentParticleMaintenance* a_maintenance)
	{
		if (a_desc.name.empty() ||
			a_desc.ownerFeature >= registeredFeatureCount.load(std::memory_order_acquire))
			return {};
		for (std::uint32_t index = 0; index < persistentLists.size(); ++index) {
			auto& slot = persistentLists[index];
			if (slot.registered && slot.desc.name == a_desc.name) {
				slot.desc = a_desc;
				slot.profilerName = "SSPP::PersistentMaintenance::" + slot.desc.name;
				slot.maintenance = a_maintenance;
				persistentListCollisionProviderMasks[index].store(
					a_desc.requiredCollisionProviders & kAllCollisionProviders,
					std::memory_order_release);
				RefreshCollisionProviderConfiguration();
				routeDescriptorConfigurationDirty.store(true, std::memory_order_release);
				return { index, slot.generation };
			}
		}
		if (registeredPersistentListCount >= persistentLists.size())
			return {};
		const auto index = registeredPersistentListCount++;
		auto& slot = persistentLists[index];
		slot = {};
		slot.desc = a_desc;
		slot.profilerName = "SSPP::PersistentMaintenance::" + slot.desc.name;
		slot.maintenance = a_maintenance;
		slot.generation += 1U;
		if (slot.generation == 0)
			slot.generation = 1U;
		slot.registered = true;
		persistentListCollisionProviderMasks[index].store(
			a_desc.requiredCollisionProviders & kAllCollisionProviders,
			std::memory_order_release);
		RefreshCollisionProviderConfiguration();
		routeDescriptorConfigurationDirty.store(true, std::memory_order_release);
		return { index, slot.generation };
	}

	EventRouteHandle ParticleRuntime::RegisterGPUEventConsumer(
		const EventConsumerDesc& a_desc,
		IGPUEventConsumer* a_consumer)
	{
		if (a_desc.name.empty() ||
			a_desc.ownerFeature >= registeredFeatureCount.load(std::memory_order_acquire))
			return {};
		for (std::uint32_t index = 0; index < eventConsumers.size(); ++index) {
			auto& slot = eventConsumers[index];
			if (slot.registered && slot.desc.name == a_desc.name) {
				slot.desc = a_desc;
				slot.consumer = a_consumer;
				return { index, slot.generation };
			}
		}
		if (registeredEventRouteCount >= eventConsumers.size())
			return {};
		const auto index = registeredEventRouteCount++;
		auto& slot = eventConsumers[index];
		slot = {};
		slot.desc = a_desc;
		slot.consumer = a_consumer;
		slot.generation += 1U;
		if (slot.generation == 0)
			slot.generation = 1U;
		slot.registered = true;
		return { index, slot.generation };
	}

	NewbornInitializerID ParticleRuntime::RegisterNewbornInitializer(
		std::string_view a_stableKey,
		const NewbornInitializerDesc& a_desc,
		INewbornParticleInitializer* a_initializer)
	{
		if (a_stableKey.empty() || !a_initializer)
			return kInvalidRoute;
		for (NewbornInitializerID index = 0;
			 index < registeredNewbornInitializerCount;
			 ++index) {
			auto& slot = newbornInitializers[index];
			if (!slot.registered || slot.stableKey != a_stableKey)
				continue;
			const bool changed = slot.initializer != a_initializer;
			slot.desc = a_desc;
			if (slot.desc.name.empty())
				slot.desc.name = slot.stableKey;
			if (slot.desc.profilerName.empty())
				slot.desc.profilerName = "SSPP::NewbornInitialize::" + slot.desc.name;
			slot.initializer = a_initializer;
			slot.setupPending = slot.setupPending || changed;
			initializerConfigurationGeneration.fetch_add(1, std::memory_order_release);
			if (slot.setupPending)
				initializerSetupPending.store(true, std::memory_order_release);
			return index;
		}
		if (registeredNewbornInitializerCount >= newbornInitializers.size()) {
			logger::error(
				"[SSPP 2.0] Newborn initializer '{}' rejected: the {}-initializer registry is full",
				a_stableKey,
				newbornInitializers.size());
			return kInvalidRoute;
		}
		const auto index = registeredNewbornInitializerCount++;
		auto& slot = newbornInitializers[index];
		slot = {};
		slot.stableKey = a_stableKey;
		slot.desc = a_desc;
		if (slot.desc.name.empty())
			slot.desc.name = slot.stableKey;
		if (slot.desc.profilerName.empty())
			slot.desc.profilerName = "SSPP::NewbornInitialize::" + slot.desc.name;
		slot.initializer = a_initializer;
		slot.registered = true;
		slot.setupPending = true;
		initializerConfigurationGeneration.fetch_add(1, std::memory_order_release);
		initializerSetupPending.store(true, std::memory_order_release);
		return index;
	}

	RenderClassID ParticleRuntime::RegisterRenderer(
		std::string_view a_stableKey,
		const RenderClassDesc& a_desc,
		IParticleRenderer* a_renderer)
	{
		if (a_stableKey.empty())
			return kInvalidRoute;
		for (std::uint32_t index = 0; index < registeredRenderClassCount; ++index) {
			auto& slot = renderers[index];
			if (!slot.registered || slot.stableKey != a_stableKey)
				continue;
			const bool rendererChanged = slot.renderer != a_renderer;
			slot.desc = a_desc;
			if (slot.desc.name.empty())
				slot.desc.name = slot.stableKey;
			if (slot.desc.preparationProfilerName.empty())
				slot.desc.preparationProfilerName = "SSPP::RendererPrepare::" + slot.desc.name;
			if (slot.desc.drawProfilerName.empty())
				slot.desc.drawProfilerName = "SSPP::RendererDraw::" + slot.desc.name;
			slot.renderer = a_renderer;
			slot.setupPending = slot.setupPending || rendererChanged;
			rendererConfigurationGeneration.fetch_add(1, std::memory_order_release);
			if (slot.setupPending)
				rendererSetupPending.store(true, std::memory_order_release);
			return index;
		}
		if (registeredRenderClassCount >= renderers.size()) {
			logger::error(
				"[SSPP 2.0] Render class '{}' rejected: the {}-class registry is full",
				a_stableKey,
				renderers.size());
			return kInvalidRoute;
		}
		const auto index = registeredRenderClassCount++;
		auto& slot = renderers[index];
		slot = {};
		slot.stableKey = a_stableKey;
		slot.desc = a_desc;
		if (slot.desc.name.empty())
			slot.desc.name = slot.stableKey;
		if (slot.desc.preparationProfilerName.empty())
			slot.desc.preparationProfilerName = "SSPP::RendererPrepare::" + slot.desc.name;
		if (slot.desc.drawProfilerName.empty())
			slot.desc.drawProfilerName = "SSPP::RendererDraw::" + slot.desc.name;
		slot.renderer = a_renderer;
		slot.registered = true;
		slot.setupPending = true;
		rendererConfigurationGeneration.fetch_add(1, std::memory_order_release);
		rendererSetupPending.store(true, std::memory_order_release);
		return index;
	}

	bool ParticleRuntime::SetRendererEnabled(RenderClassID a_id, bool a_enabled) noexcept
	{
		if (a_id >= registeredRenderClassCount || !renderers[a_id].registered)
			return false;
		if (renderers[a_id].desc.enabled == a_enabled)
			return true;
		renderers[a_id].desc.enabled = a_enabled;
		rendererConfigurationGeneration.fetch_add(1, std::memory_order_release);
		return true;
	}

	std::vector<RendererInfo> ParticleRuntime::RendererInfos() const
	{
		std::vector<RendererInfo> result;
		std::array<IParticleRenderer*, kRenderClassCount> callbacks{};
		result.reserve(registeredRenderClassCount);
		for (std::uint32_t index = 0; index < registeredRenderClassCount; ++index) {
			const auto& slot = renderers[index];
			if (!slot.registered)
				continue;
			RendererInfo info{};
			info.id = index;
			info.stableKey = slot.stableKey;
			info.name = slot.desc.name;
			info.drawOrder = slot.desc.drawOrder;
			info.enabled = slot.desc.enabled;
			info.rendererPresent = slot.renderer != nullptr;
			if (diagnostics.valid)
				info.visibleCount = diagnostics.visibleByRenderClass[index];
			callbacks[result.size()] = slot.renderer;
			result.push_back(std::move(info));
		}
		for (std::size_t index = 0; index < result.size(); ++index)
			result[index].ready = callbacks[index] && callbacks[index]->IsReady();
		return result;
	}

	std::vector<PersistentListInfo> ParticleRuntime::PersistentListInfos() const
	{
		std::vector<PersistentListInfo> result;
		result.reserve(registeredPersistentListCount);
		for (PersistentListID id = 0; id < registeredPersistentListCount; ++id) {
			const auto& slot = persistentLists[id];
			if (!slot.registered)
				continue;
			PersistentListInfo info{};
			info.id = id;
			info.name = slot.desc.name;
			info.ownerFeature = slot.desc.ownerFeature;
			info.capacity = slot.desc.capacity == 0 ? capacity :
			                                          std::min(slot.desc.capacity, capacity);
			info.maintenanceIntervalSeconds = slot.desc.maintenanceIntervalSeconds;
			if (diagnostics.valid)
				info.activeCount = diagnostics.activeByPersistentList[id];
			result.push_back(std::move(info));
		}
		return result;
	}

	std::vector<EventRouteInfo> ParticleRuntime::EventRouteInfos() const
	{
		std::vector<EventRouteInfo> result;
		result.reserve(registeredEventRouteCount);
		for (EventRouteID id = 0; id < registeredEventRouteCount; ++id) {
			const auto& slot = eventConsumers[id];
			if (!slot.registered)
				continue;
			EventRouteInfo info{};
			info.id = id;
			info.name = slot.desc.name;
			info.ownerFeature = slot.desc.ownerFeature;
			if (diagnostics.valid) {
				info.produced = diagnostics.eventsProducedByRoute[id];
				info.consumed = diagnostics.eventsConsumedByRoute[id];
			}
			result.push_back(std::move(info));
		}
		return result;
	}

	std::uint32_t ParticleRuntime::RequestFeatureDrain(std::uint32_t a_ownerFeature) noexcept
	{
		if (a_ownerFeature >= registeredFeatureCount.load(std::memory_order_acquire))
			return 0;
		std::uint32_t requested{};
		const auto pendingEnd = pendingFeatureDrains.begin() + pendingFeatureDrainCount;
		if (std::find(pendingFeatureDrains.begin(), pendingEnd, a_ownerFeature) == pendingEnd) {
			if (pendingFeatureDrainCount < pendingFeatureDrains.size()) {
				pendingFeatureDrains[pendingFeatureDrainCount++] = a_ownerFeature;
				++requested;
			} else {
				logger::warn(
					"[SSPP 2.0] Dynamic feature drain for owner {} rejected: queue is full",
					a_ownerFeature);
			}
		}
		if (requested != 0)
			RecordCollisionProviderActivity(0);
		return requested;
	}

	void ParticleRuntime::SetupResources(std::uint32_t a_capacity)
	{
		if (!globals::d3d::device || !globals::d3d::context)
			return;
		a_capacity = std::clamp(a_capacity, kMinimumCapacity, kMaximumCapacity);
		const bool resourcesReady = positionAge && velocityLifetime && appearance && metadata &&
		                            contactData && particleAuxiliaryState && freeIDs && counters && dynamicArenas[0] &&
		                            dynamicArenas[1] && persistentArenas[0] && persistentArenas[1] && routeArena &&
		                            routeDescriptors && eventArena && renderIndexArena && renderClassBuffer && burstUpload &&
		                            frameRequests && emissionChunks && emitterStates &&
		                            emitterCommandUpload && activeEmitterUpload && simulationProfileBuffer &&
		                            collisionRuleBuffer && particleTemplateBuffer && emissionProfileBuffer && featureBuffer && frameCB &&
		                            collisionFrameCB && emitterFrameCB && lifecycleFrameCB && renderFrameCB && simulationDispatchArgs &&
		                            classifyDispatchArgs &&
		                            emissionDispatchArgs && renderDrawArgs && renderPreparationArgs && routeDispatchArgs && eventDispatchArgs &&
		                            persistentDispatchArgs &&
		                            debugBlendState && debugDepthState && debugRasterizerState;
		if (capacity == a_capacity && resourcesReady) {
			EnsureShaders();
			UploadRouteDescriptors();
			SetupRegisteredInitializers();
			RefreshInitializerExecutionPlan();
			SetupRegisteredRenderers();
			RefreshRenderClassConfiguration();
			ready = resetCountersCS && resetDynamicCountersCS && buildSimulationArgsCS &&
			        emissionPlanCS && emissionCS && reclassifySimulationProfilesCS &&
			        resetEmitterFrameCS && applyEmitterCommandsCS && updateEmittersCS &&
			        buildEmissionArgsCS && simulationCS && classifyCS && classifyPersistentCS &&
			        buildRenderArgsCS && buildRouteArgsCS && buildFreeRouteArgsCS &&
			        transitionCS && freeClaimCS && freeMergeCS && builtinMaintenanceCS &&
			        resetPersistentDestinationCS &&
			        countEventConsumerCS &&
			        debugVS && debugPS;
			return;
		}

		ready = false;
		capacity = a_capacity;
		currentArena = 0;
		frameCounter = 0;
		readbackCursor = 0;
		readbackPending.fill(false);
		diagnostics = {};
		CreateCoreBuffers();
		CreateIndirectBuffers();
		CreateRenderStates();
		frameCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<FrameConstants>(), "SSPP::CoreFrameCB");
		collisionFrameCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<CollisionFrameConstants>(), "SSPP::CollisionFrameCB");
		emitterFrameCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<EmitterFrameConstants>(), "SSPP::EmitterFrameCB");
		lifecycleFrameCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<LifecycleFrameConstants>(), "SSPP::LifecycleFrameCB");
		renderFrameCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<RenderFrameConstants>(), "SSPP::RenderFrameCB");
		EnsureShaders();
		SetupRegisteredInitializers();
		RefreshInitializerExecutionPlan();
		SetupRegisteredRenderers();
		RefreshRenderClassConfiguration();
		InitializePool();
		resetPending.store(false, std::memory_order_release);
		configurationDirty = true;
		UploadDirtyConfiguration();
		UploadRouteDescriptors();
		ready = positionAge && velocityLifetime && appearance && metadata && contactData &&
		        particleAuxiliaryState && freeIDs && counters && dynamicArenas[0] && dynamicArenas[1] &&
		        persistentArenas[0] && persistentArenas[1] && routeArena && routeDescriptors &&
		        eventArena && renderIndexArena && renderClassBuffer && burstUpload && frameRequests &&
		        emissionChunks && emitterStates && emitterCommandUpload && activeEmitterUpload &&
		        simulationProfileBuffer && collisionRuleBuffer && particleTemplateBuffer &&
		        emissionProfileBuffer && featureBuffer && frameCB && collisionFrameCB && emitterFrameCB && lifecycleFrameCB &&
		        renderFrameCB &&
		        simulationDispatchArgs && classifyDispatchArgs &&
		        emissionDispatchArgs && renderDrawArgs && renderPreparationArgs && routeDispatchArgs &&
		        eventDispatchArgs && persistentDispatchArgs && resetCountersCS &&
		        resetDynamicCountersCS && reclassifySimulationProfilesCS &&
		        buildSimulationArgsCS && emissionPlanCS && emissionCS && resetEmitterFrameCS &&
		        applyEmitterCommandsCS &&
		        updateEmittersCS && buildEmissionArgsCS && simulationCS &&
		        classifyCS && classifyPersistentCS && buildRenderArgsCS && buildRouteArgsCS &&
		        buildFreeRouteArgsCS && transitionCS && freeClaimCS && freeMergeCS &&
		        builtinMaintenanceCS && resetPersistentDestinationCS && countEventConsumerCS &&
		        debugVS && debugPS;
		logger::info(
			"[SSPP 2.0] Shared stable-slot runtime initialized: capacity={}, ready={}",
			capacity,
			ready);
	}

	void ParticleRuntime::CreateCoreBuffers()
	{
		positionAge = CreateStructuredBuffer<float4>(capacity, "SSPP::PositionAge");
		velocityLifetime = CreateStructuredBuffer<float4>(capacity, "SSPP::VelocityLifetime");
		appearance = CreateStructuredBuffer<float4>(capacity, "SSPP::Appearance");
		metadata = CreateStructuredBuffer<std::array<std::uint32_t, 4>>(capacity, "SSPP::Metadata");
		contactData = CreateStructuredBuffer<float4>(capacity, "SSPP::ContactData");
		particleAuxiliaryState = CreateStructuredBuffer<ParticleAuxStateGPU>(
			capacity, "SSPP::ParticleAuxiliaryState");

		std::vector<std::uint32_t> freeData(capacity);
		for (std::uint32_t index = 0; index < capacity; ++index)
			freeData[index] = index;
		freeIDs = CreateStructuredBuffer<std::uint32_t>(
			capacity, "SSPP::FreeIDs", false, freeData.data());
		counters = CreateStructuredBuffer<std::uint32_t>(kCounterCount, "SSPP::Counters");
		for (std::uint32_t arena = 0; arena < 2; ++arena) {
			dynamicArenas[arena] = CreateStructuredBuffer<ParticleReferenceGPU>(
				capacity * kSimulationClassCount,
				arena == 0 ? "SSPP::DynamicArenaA" : "SSPP::DynamicArenaB");
			persistentArenas[arena] = CreateStructuredBuffer<ParticleReferenceGPU>(
				capacity * kPersistentListCount,
				arena == 0 ? "SSPP::PersistentArenaA" : "SSPP::PersistentArenaB");
		}
		routeArena = CreateStructuredBuffer<ParticleReferenceGPU>(
			capacity * kRouteCount, "SSPP::RouteArena");
		routeDescriptors = CreateStructuredBuffer<RouteDescriptorGPU>(
			kRouteCount, "SSPP::RouteDescriptors", true);
		routeDescriptorConfigurationDirty.store(true, std::memory_order_release);
		publishedPersistentListCount = 0;
		eventArena = CreateStructuredBuffer<ParticleExitEventGPU>(capacity, "SSPP::EventArena");
		renderIndexArena = CreateStructuredBuffer<std::uint32_t>(
			capacity * kRenderClassCount, "SSPP::RenderIndexArena");
		renderClassBuffer = CreateStructuredBuffer<RenderClassGPU>(
			kRenderClassCount, "SSPP::RenderClasses", true);
		rendererExecutionCount = 0;
		renderClassGPUDescriptorBytes.fill(std::byte{});
		publishedRendererConfigurationGeneration = 0;
		burstUpload = CreateStructuredBuffer<BurstRequestGPU>(
			kMaxBurstRequests, "SSPP::BurstUpload", true);
		frameRequests = CreateStructuredBuffer<BurstRequestGPU>(
			kMaxFrameRequests, "SSPP::FrameRequests");
		emissionChunks = CreateStructuredBuffer<EmissionChunkGPU>(
			kMaxEmissionChunks, "SSPP::EmissionChunks");
		emitterStates = CreateStructuredBuffer<PersistentEmitterGPU>(
			kMaxPersistentEmitters, "SSPP::EmitterStates");
		emitterCommandUpload = CreateStructuredBuffer<EmitterCommandGPU>(
			kMaxEmitterCommands, "SSPP::EmitterCommands", true);
		activeEmitterUpload = CreateStructuredBuffer<EmitterHandle>(
			kMaxPersistentEmitters, "SSPP::ActiveEmitterHandles", true);
		simulationProfileBuffer = CreateStructuredBuffer<SimulationProfileGPU>(
			kMaxSimulationProfiles, "SSPP::SimulationProfiles", true);
		collisionRuleBuffer = CreateStructuredBuffer<CollisionRuleGPU>(
			kMaxCollisionRules, "SSPP::CollisionRules", true);
		particleTemplateBuffer = CreateStructuredBuffer<ParticleTemplateGPU>(
			kMaxParticleTemplates, "SSPP::ParticleTemplates", true);
		emissionProfileBuffer = CreateStructuredBuffer<EmissionProfileGPU>(
			kMaxEmissionProfiles, "SSPP::EmissionProfiles", true);
		featureBuffer = CreateStructuredBuffer<ParticleFeatureGPU>(
			kMaxParticleFeatures, "SSPP::Features", true);
	}

	void ParticleRuntime::CreateReadbackResources()
	{
		D3D11_BUFFER_DESC staging{};
		staging.Usage = D3D11_USAGE_STAGING;
		staging.ByteWidth = sizeof(std::uint32_t) * kCounterCount;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		D3D11_QUERY_DESC query{};
		query.Query = D3D11_QUERY_EVENT;
		for (std::uint32_t index = 0; index < kReadbackSlots; ++index) {
			counterReadbacks[index] = nullptr;
			readbackQueries[index] = nullptr;
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(
				&staging, nullptr, counterReadbacks[index].put()));
			Util::SetResourceName(counterReadbacks[index].get(), "SSPP::CounterReadback%u", index);
			DX::ThrowIfFailed(globals::d3d::device->CreateQuery(
				&query, readbackQueries[index].put()));
		}
	}

	void ParticleRuntime::CreateIndirectBuffers()
	{
		CreateIndirectBuffer(
			kSimulationClassCount * 3U * sizeof(std::uint32_t),
			"SSPP::SimulationDispatchArgs",
			simulationDispatchArgs,
			simulationDispatchArgsUAV);
		CreateIndirectBuffer(
			kSimulationClassCount * 3U * sizeof(std::uint32_t),
			"SSPP::ClassifyDispatchArgs",
			classifyDispatchArgs,
			classifyDispatchArgsUAV);
		CreateIndirectBuffer(
			3U * sizeof(std::uint32_t),
			"SSPP::EmissionDispatchArgs",
			emissionDispatchArgs,
			emissionDispatchArgsUAV);
		CreateIndirectBuffer(
			kRenderClassCount * sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS),
			"SSPP::RenderDrawArgs",
			renderDrawArgs,
			renderDrawArgsUAV);
		CreateIndirectBuffer(
			kRenderClassCount * 3U * sizeof(std::uint32_t),
			"SSPP::RenderPreparationArgs",
			renderPreparationArgs,
			renderPreparationArgsUAV);
		CreateIndirectBuffer(
			kRouteCount * 3U * sizeof(std::uint32_t),
			"SSPP::RouteDispatchArgs",
			routeDispatchArgs,
			routeDispatchArgsUAV);
		CreateIndirectBuffer(
			3U * sizeof(std::uint32_t),
			"SSPP::EventDispatchArgs",
			eventDispatchArgs,
			eventDispatchArgsUAV);
		CreateIndirectBuffer(
			kPersistentListCount * 3U * sizeof(std::uint32_t),
			"SSPP::PersistentDispatchArgs",
			persistentDispatchArgs,
			persistentDispatchArgsUAV);
	}

	void ParticleRuntime::CreateRenderStates()
	{
		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].BlendEnable = true;
		blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blend, debugBlendState.put()));

		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = true;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depth, debugDepthState.put()));

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = true;
		DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&raster, debugRasterizerState.put()));
		Util::SetResourceName(debugBlendState.get(), "SSPP::DebugBlendState");
		Util::SetResourceName(debugDepthState.get(), "SSPP::DebugDepthState");
		Util::SetResourceName(debugRasterizerState.get(), "SSPP::DebugRasterizerState");
	}

	void ParticleRuntime::EnsureShaders()
	{
		const std::wstring core = L"Data\\Shaders\\ScreenSpacePhysicalParticles\\Core\\";
		const auto compile = [&](const wchar_t* a_file,
								 const std::vector<std::pair<const char*, const char*>>& a_defines,
								 const char* a_target,
								 const char* a_entry) {
			const auto path = core + a_file;
			return Util::CompileShader(path.c_str(), a_defines, a_target, a_entry);
		};
		if (!resetCountersCS)
			resetCountersCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleArgsCS.hlsl", {}, "cs_5_0", "ResetCountersMain")));
		if (!resetDynamicCountersCS)
			resetDynamicCountersCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleArgsCS.hlsl", {}, "cs_5_0", "ResetDynamicCountersMain")));
		if (!buildSimulationArgsCS)
			buildSimulationArgsCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleArgsCS.hlsl", {}, "cs_5_0", "BuildSimulationArgsMain")));
		if (!buildRenderArgsCS)
			buildRenderArgsCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleArgsCS.hlsl", {}, "cs_5_0", "BuildRenderArgsMain")));
		if (!buildRouteArgsCS)
			buildRouteArgsCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleArgsCS.hlsl", {}, "cs_5_0", "BuildRouteArgsMain")));
		if (!buildFreeRouteArgsCS)
			buildFreeRouteArgsCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleArgsCS.hlsl", {}, "cs_5_0", "BuildFreeRouteArgsMain")));
		if (!emissionPlanCS)
			emissionPlanCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEmissionPlanCS.hlsl", {}, "cs_5_0", "PlanMain")));
		if (!emissionCS)
			emissionCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEmissionCS.hlsl", {}, "cs_5_0", "EmitMain")));
		if (!resetEmitterFrameCS)
			resetEmitterFrameCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEmitterArgsCS.hlsl", {}, "cs_5_0", "ResetEmitterFrameMain")));
		if (!buildEmissionArgsCS)
			buildEmissionArgsCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEmitterArgsCS.hlsl", {}, "cs_5_0", "BuildEmissionArgsMain")));
		if (!applyEmitterCommandsCS)
			applyEmitterCommandsCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEmitterCommandCS.hlsl", {}, "cs_5_0", "ApplyEmitterCommandsMain")));
		if (!updateEmittersCS)
			updateEmittersCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEmitterUpdateCS.hlsl", {}, "cs_5_0", "UpdateEmittersMain")));
		if (!simulationCS)
			simulationCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleSimulationCS.hlsl", {}, "cs_5_0", "SimulateMain")));
		if (!reclassifySimulationProfilesCS)
			reclassifySimulationProfilesCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleProfileReclassifyCS.hlsl", {}, "cs_5_0", "ReclassifyMain")));
		if (!classifyCS)
			classifyCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleClassifyCS.hlsl", {}, "cs_5_0", "ClassifyMain")));
		if (!classifyPersistentCS)
			classifyPersistentCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleClassifyCS.hlsl", {}, "cs_5_0", "ClassifyPersistentMain")));
		if (!transitionCS)
			transitionCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleTransitionCS.hlsl", {}, "cs_5_0", "TransitionMain")));
		if (!freeClaimCS)
			freeClaimCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleTransitionCS.hlsl", {}, "cs_5_0", "ClaimFreeMain")));
		if (!freeMergeCS)
			freeMergeCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleFreeMergeCS.hlsl", {}, "cs_5_0", "MergeFreeReturnsMain")));
		if (!builtinMaintenanceCS)
			builtinMaintenanceCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleBuiltinMaintenanceCS.hlsl", {}, "cs_5_0", "MaintainMain")));
		if (!resetPersistentDestinationCS)
			resetPersistentDestinationCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleBuiltinMaintenanceCS.hlsl", {}, "cs_5_0", "ResetPersistentDestinationMain")));
		if (!countEventConsumerCS)
			countEventConsumerCS.attach(static_cast<ID3D11ComputeShader*>(
				compile(L"ParticleEventConsumerCS.hlsl", {}, "cs_5_0", "CountEventsMain")));
		if (!debugVS)
			debugVS.attach(static_cast<ID3D11VertexShader*>(
				compile(L"ParticleDebug.hlsl", { { "VSHADER", "" } }, "vs_5_0", "VSMain")));
		if (!debugPS)
			debugPS.attach(static_cast<ID3D11PixelShader*>(
				compile(L"ParticleDebug.hlsl", { { "PSHADER", "" } }, "ps_5_0", "PSMain")));
	}

	void ParticleRuntime::InitializePool()
	{
		if (!globals::d3d::context || !counters)
			return;
		std::vector<std::uint32_t> freeData(capacity);
		for (std::uint32_t index = 0; index < capacity; ++index)
			freeData[index] = index;
		globals::d3d::context->UpdateSubresource(freeIDs->resource.get(), 0, nullptr, freeData.data(), 0, 0);

		std::array<std::uint32_t, kCounterCount> initialCounters{};
		initialCounters[kFreeTop] = capacity;
		globals::d3d::context->UpdateSubresource(
			counters->resource.get(), 0, nullptr, initialCounters.data(), 0, 0);
		const std::uint32_t clear[4]{};
		globals::d3d::context->ClearUnorderedAccessViewUint(positionAge->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(velocityLifetime->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(appearance->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(metadata->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(contactData->uav.get(), clear);
		std::vector<ParticleAuxStateGPU> auxiliaryData(capacity);
		globals::d3d::context->UpdateSubresource(
			particleAuxiliaryState->resource.get(), 0, nullptr, auxiliaryData.data(), 0, 0);
		for (auto& arena : dynamicArenas)
			globals::d3d::context->ClearUnorderedAccessViewUint(arena->uav.get(), clear);
		for (auto& arena : persistentArenas)
			globals::d3d::context->ClearUnorderedAccessViewUint(arena->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(routeArena->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(eventArena->uav.get(), clear);
		globals::d3d::context->ClearUnorderedAccessViewUint(emitterStates->uav.get(), clear);
		// New readback resources isolate the reset epoch from in-flight queries.
		CreateReadbackResources();
		currentArena = 0;
		simulationProfileReclassificationPending.store(false, std::memory_order_release);
		persistentParityMask = 0;
		persistentDestinationParityMask = 0;
		routeEpoch = 1;
		frameCounter = 0;
		readbackCursor = 0;
		readbackPending.fill(false);
		readbackArena.fill(0);
		readbackPersistentParity.fill(0);
		readbackRenderClassCount.fill(0);
		readbackFeatureCount.fill(0);
		readbackFrame.fill(0);
		readbackRejectedCPUCommands.fill(0);
		readbackRejectedCPUParticles.fill(0);
		readbackRejectedEmitterCommands.fill(0);
		readbackRejectedEmitterHandles.fill(0);
		readbackActivityEpoch.fill(0);
		for (auto& perSlot : readbackRejectedCPURequestedByFeature)
			perSlot.fill(0);
		for (auto& perSlot : readbackRejectedCPUDroppedByFeature)
			perSlot.fill(0);
		rejectedCPUCommands.store(0, std::memory_order_relaxed);
		rejectedCPUParticles.store(0, std::memory_order_relaxed);
		rejectedEmitterCommands.store(0, std::memory_order_relaxed);
		rejectedEmitterHandles.store(0, std::memory_order_relaxed);
		for (auto& requested : rejectedCPURequestedByFeature)
			requested.store(0, std::memory_order_relaxed);
		for (auto& dropped : rejectedCPUDroppedByFeature)
			dropped.store(0, std::memory_order_relaxed);
		collisionProviderActivityState.store(0, std::memory_order_release);
		submittedActivityEpoch = 0;
		diagnostics = {};
		diagnostics.capacity = capacity;
		ResetBurstQueue();
		ResetEmitterRegistry();
		pendingFeatureDrainCount = 0;
		for (auto& list : persistentLists) {
			if (!list.registered)
				continue;
			list.currentArena = 0;
			list.elapsedSeconds = 0.0F;
			list.drainRequested = false;
			list.drainThisFrame = false;
			list.rebuildThisFrame = false;
		}
		std::array<IParticleRenderer*, kRenderClassCount> rendererCallbacks{};
		std::uint32_t rendererCallbackCount{};
		for (std::uint32_t index = 0; index < registeredRenderClassCount; ++index) {
			if (renderers[index].registered && renderers[index].renderer)
				rendererCallbacks[rendererCallbackCount++] = renderers[index].renderer;
		}
		for (std::uint32_t index = 0; index < rendererCallbackCount; ++index)
			rendererCallbacks[index]->Reset();
		std::array<INewbornParticleInitializer*, kMaxNewbornInitializers> initializerCallbacks{};
		std::uint32_t initializerCallbackCount{};
		for (std::uint32_t index = 0; index < registeredNewbornInitializerCount; ++index) {
			if (newbornInitializers[index].registered && newbornInitializers[index].initializer)
				initializerCallbacks[initializerCallbackCount++] =
					newbornInitializers[index].initializer;
		}
		for (std::uint32_t index = 0; index < initializerCallbackCount; ++index)
			initializerCallbacks[index]->Reset();
	}

	void ParticleRuntime::ResetEmitterRegistry()
	{
		dirtyEmitterCount = 0;
		freeEmitterSlots.clear();
		freeEmitterSlots.reserve(kMaxPersistentEmitters);
		for (std::uint32_t index = kMaxPersistentEmitters; index-- > 0;) {
			liveEmitterCollisionProviderMasks[index].store(0, std::memory_order_release);
			auto& slot = emitterSlots[index];
			slot.desired = {};
			slot.state = CPUSlotState::kFree;
			slot.pending = PendingEmitterOperation::kNone;
			slot.queued = false;
			freeEmitterSlots.push_back(index);
		}
	}

	void ParticleRuntime::ResetBurstQueue() noexcept
	{
		burstReservationCursor.store(0, std::memory_order_relaxed);
		for (auto& slot : burstQueue) {
			// A concurrent writer retains ownership and becomes stale after generation advance.
			auto expected = BurstSlotState::kReady;
			(void)slot.state.compare_exchange_strong(
				expected,
				BurstSlotState::kEmpty,
				std::memory_order_acq_rel,
				std::memory_order_relaxed);
		}
		// Publish the new producer generation only after sweeping old slots.
		burstQueueGeneration.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool ParticleRuntime::ReserveBurstSlots(std::span<std::uint32_t> a_slots) noexcept
	{
		if (a_slots.empty() || a_slots.size() > burstQueue.size())
			return false;
		const auto first = burstReservationCursor.fetch_add(
			static_cast<std::uint32_t>(a_slots.size()), std::memory_order_relaxed);
		constexpr auto queueCapacity = static_cast<std::uint32_t>(kMaxBurstRequests);
		std::size_t reserved{};
		std::uint32_t cursor = first;
		while (reserved < a_slots.size()) {
			bool found{};
			for (std::uint32_t attempt = 0; attempt < queueCapacity; ++attempt) {
				const auto index = (cursor + attempt) % queueCapacity;
				auto expected = BurstSlotState::kEmpty;
				if (!burstQueue[index].state.compare_exchange_strong(
						expected,
						BurstSlotState::kWriting,
						std::memory_order_acq_rel,
						std::memory_order_relaxed)) {
					continue;
				}
				a_slots[reserved++] = index;
				cursor = index + 1U;
				found = true;
				break;
			}
			if (found)
				continue;
			for (std::size_t index = 0; index < reserved; ++index) {
				burstQueue[a_slots[index]].state.store(
					BurstSlotState::kEmpty, std::memory_order_release);
			}
			return false;
		}
		return true;
	}

	void ParticleRuntime::PublishBurstSlot(
		std::uint32_t a_slot,
		const BurstRequestGPU& a_request,
		std::uint32_t a_generation) noexcept
	{
		auto request = a_request;
		// CPU bursts cannot mint persistent-emitter ownership.
		request.ownerEmitter = kInvalidRoute;
		request.ownerEmitterGeneration = 0;
		auto& slot = burstQueue[a_slot];
		slot.request = request;
		slot.generation = a_generation;
		slot.state.store(BurstSlotState::kReady, std::memory_order_release);
	}

	void ParticleRuntime::Reset()
	{
		// GameLoaded may run off-thread; RunFrame owns immediate-context reset work.
		resetPending.store(true, std::memory_order_release);
	}

	void ParticleRuntime::ClearShaderCache()
	{
		std::array<IParticleRenderer*, kRenderClassCount> rendererCallbacks{};
		std::uint32_t rendererCallbackCount{};
		for (std::uint32_t index = 0; index < registeredRenderClassCount; ++index) {
			if (!renderers[index].registered || !renderers[index].renderer)
				continue;
			rendererCallbacks[rendererCallbackCount++] = renderers[index].renderer;
			renderers[index].setupPending = true;
		}
		rendererSetupPending.store(true, std::memory_order_release);
		for (std::uint32_t index = 0; index < rendererCallbackCount; ++index)
			rendererCallbacks[index]->ClearShaderCache();
		std::array<INewbornParticleInitializer*, kMaxNewbornInitializers> initializerCallbacks{};
		std::uint32_t initializerCallbackCount{};
		for (std::uint32_t index = 0; index < registeredNewbornInitializerCount; ++index) {
			auto& slot = newbornInitializers[index];
			if (!slot.registered || !slot.initializer)
				continue;
			initializerCallbacks[initializerCallbackCount++] = slot.initializer;
			slot.setupPending = true;
		}
		initializerSetupPending.store(true, std::memory_order_release);
		for (std::uint32_t index = 0; index < initializerCallbackCount; ++index)
			initializerCallbacks[index]->ClearShaderCache();
		resetCountersCS = nullptr;
		resetDynamicCountersCS = nullptr;
		buildSimulationArgsCS = nullptr;
		buildRenderArgsCS = nullptr;
		emissionPlanCS = nullptr;
		emissionCS = nullptr;
		resetEmitterFrameCS = nullptr;
		applyEmitterCommandsCS = nullptr;
		updateEmittersCS = nullptr;
		buildEmissionArgsCS = nullptr;
		buildRouteArgsCS = nullptr;
		buildFreeRouteArgsCS = nullptr;
		simulationCS = nullptr;
		reclassifySimulationProfilesCS = nullptr;
		classifyCS = nullptr;
		classifyPersistentCS = nullptr;
		transitionCS = nullptr;
		freeClaimCS = nullptr;
		freeMergeCS = nullptr;
		builtinMaintenanceCS = nullptr;
		resetPersistentDestinationCS = nullptr;
		countEventConsumerCS = nullptr;
		debugVS = nullptr;
		debugPS = nullptr;
		ready = false;
	}

	void ParticleRuntime::SetSimulationProfile(
		SimulationProfileID a_id,
		const SimulationProfileGPU& a_profile)
	{
		if (a_id >= kMaxSimulationProfiles)
			throw std::out_of_range("SSPP simulation profile ID exceeds GPU capacity");
		if (!std::isfinite(a_profile.acceleration.x) ||
			!std::isfinite(a_profile.acceleration.y) ||
			!std::isfinite(a_profile.acceleration.z) || !std::isfinite(a_profile.drag) ||
			!std::isfinite(a_profile.restitution) || !std::isfinite(a_profile.friction) ||
			!std::isfinite(a_profile.settleSpeed) || !std::isfinite(a_profile.surfaceOffset)) {
			throw std::invalid_argument("SSPP simulation profile contains a non-finite value");
		}
		const bool simulationClassChanged =
			a_id < simulationProfiles.size() &&
			simulationProfiles[a_id].simulationClass < kSimulationClassCount &&
			a_profile.simulationClass < kSimulationClassCount &&
			simulationProfiles[a_id].simulationClass != a_profile.simulationClass;
		if (simulationProfiles.size() <= a_id) {
			SimulationProfileGPU invalid{};
			invalid.simulationClass = static_cast<std::uint32_t>(SimulationClass::kCount);
			simulationProfiles.resize(a_id + 1U, invalid);
		}
		auto profile = a_profile;
		profile.responseTableOffset = a_id * kCollisionRuleSlots;
		simulationProfiles[a_id] = profile;
		RecordCollisionProviderActivity(profile.queryPolicy);
		if (simulationProfileKeys.size() <= a_id)
			simulationProfileKeys.resize(a_id + 1U);
		if (collisionRules.size() < (a_id + 1U) * kCollisionRuleSlots)
			collisionRules.resize((a_id + 1U) * kCollisionRuleSlots);
		RefreshCollisionProviderConfiguration();
		configurationDirty = true;
		if (simulationClassChanged) {
			// Coalesce profile edits into one active-list rebuild next frame.
			simulationProfileReclassificationPending.store(true, std::memory_order_release);
		}
	}

	SimulationProfileID ParticleRuntime::RegisterSimulationProfile(
		std::string_view a_stableKey,
		const SimulationProfileGPU& a_profile)
	{
		if (a_stableKey.empty())
			return kInvalidRoute;
		for (SimulationProfileID id = 0; id < simulationProfileKeys.size(); ++id) {
			if (simulationProfileKeys[id] != a_stableKey)
				continue;
			SetSimulationProfile(id, a_profile);
			return id;
		}
		if (simulationProfiles.size() >= kMaxSimulationProfiles) {
			logger::error(
				"[SSPP 2.0] Simulation profile '{}' rejected: the {}-profile registry is full",
				a_stableKey,
				kMaxSimulationProfiles);
			return kInvalidRoute;
		}
		const auto id = static_cast<SimulationProfileID>(simulationProfiles.size());
		simulationProfileKeys.resize(id);
		simulationProfileKeys.emplace_back(a_stableKey);
		SetSimulationProfile(id, a_profile);
		return id;
	}

	void ParticleRuntime::SetParticleTemplate(
		ParticleTemplateID a_id,
		const ParticleTemplateGPU& a_template)
	{
		if (a_id >= kMaxParticleTemplates)
			throw std::out_of_range("SSPP particle template ID exceeds GPU capacity");
		if (particleTemplates.size() <= a_id) {
			ParticleTemplateGPU invalid{};
			invalid.simulationProfile = kInvalidRoute;
			invalid.renderClass = kInvalidRoute;
			invalid.lifecyclePhase = static_cast<std::uint32_t>(LifecyclePhase::kFree);
			invalid.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kFeatureCustom);
			particleTemplates.resize(a_id + 1U, invalid);
		}
		particleTemplates[a_id] = a_template;
		if (particleTemplateKeys.size() <= a_id)
			particleTemplateKeys.resize(a_id + 1U);
		RefreshCollisionProviderConfiguration();
		configurationDirty = true;
	}

	ParticleTemplateID ParticleRuntime::RegisterParticleTemplate(
		std::string_view a_stableKey,
		const ParticleTemplateGPU& a_template)
	{
		if (a_stableKey.empty())
			return kInvalidRoute;
		for (ParticleTemplateID id = 0; id < particleTemplateKeys.size(); ++id) {
			if (particleTemplateKeys[id] != a_stableKey)
				continue;
			SetParticleTemplate(id, a_template);
			return id;
		}
		if (particleTemplates.size() >= kMaxParticleTemplates) {
			logger::error(
				"[SSPP 2.0] Particle template '{}' rejected: the {}-template registry is full",
				a_stableKey,
				kMaxParticleTemplates);
			return kInvalidRoute;
		}
		const auto id = static_cast<ParticleTemplateID>(particleTemplates.size());
		particleTemplateKeys.resize(id);
		particleTemplateKeys.emplace_back(a_stableKey);
		SetParticleTemplate(id, a_template);
		return id;
	}

	void ParticleRuntime::SetEmissionProfile(
		EmissionProfileID a_id,
		const EmissionProfileGPU& a_profile)
	{
		if (a_id >= kMaxEmissionProfiles)
			throw std::out_of_range("SSPP emission profile ID exceeds GPU capacity");
		if (emissionProfiles.size() <= a_id) {
			EmissionProfileGPU invalid{};
			invalid.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kFeatureCustom);
			emissionProfiles.resize(a_id + 1U, invalid);
		}
		emissionProfiles[a_id] = a_profile;
		if (emissionProfileKeys.size() <= a_id)
			emissionProfileKeys.resize(a_id + 1U);
		configurationDirty = true;
	}

	EmissionProfileID ParticleRuntime::RegisterEmissionProfile(
		std::string_view a_stableKey,
		const EmissionProfileGPU& a_profile)
	{
		if (a_stableKey.empty())
			return kInvalidRoute;
		for (EmissionProfileID id = 0; id < emissionProfileKeys.size(); ++id) {
			if (emissionProfileKeys[id] != a_stableKey)
				continue;
			SetEmissionProfile(id, a_profile);
			return id;
		}
		if (emissionProfiles.size() >= kMaxEmissionProfiles) {
			logger::error(
				"[SSPP 2.0] Emission profile '{}' rejected: the {}-profile registry is full",
				a_stableKey,
				kMaxEmissionProfiles);
			return kInvalidRoute;
		}
		const auto id = static_cast<EmissionProfileID>(emissionProfiles.size());
		emissionProfileKeys.resize(id);
		emissionProfileKeys.emplace_back(a_stableKey);
		SetEmissionProfile(id, a_profile);
		return id;
	}

	void ParticleRuntime::SetCollisionRule(
		SimulationProfileID a_profileID,
		CollisionChannel a_channel,
		const CollisionRuleGPU& a_rule)
	{
		if (a_profileID >= simulationProfiles.size())
			throw std::out_of_range("SSPP collision rule references an undefined simulation profile");
		const auto slot = CollisionRuleIndex(a_channel);
		if (slot == kInvalidRoute)
			throw std::invalid_argument("SSPP collision rule requires one semantic channel");
		const auto ruleIndex = simulationProfiles[a_profileID].responseTableOffset + slot;
		if (ruleIndex >= kMaxCollisionRules)
			throw std::out_of_range("SSPP collision response table exceeds GPU capacity");
		if (collisionRules.size() <= ruleIndex)
			collisionRules.resize(ruleIndex + 1U);
		collisionRules[ruleIndex] = a_rule;
		configurationDirty = true;
	}

	void ParticleRuntime::UploadDirtyConfiguration()
	{
		if (!simulationProfileBuffer || !collisionRuleBuffer || !particleTemplateBuffer ||
			!emissionProfileBuffer || !featureBuffer)
			return;
		const auto pendingFeatureGeneration =
			featureConfigurationGeneration.load(std::memory_order_acquire);
		if (!configurationDirty &&
			uploadedFeatureConfigurationGeneration == pendingFeatureGeneration)
			return;
		auto upload = [](Buffer& a_buffer, const void* a_data, std::size_t a_bytes) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			DX::ThrowIfFailed(globals::d3d::context->Map(
				a_buffer.resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			std::memset(mapped.pData, 0, a_buffer.desc.ByteWidth);
			std::memcpy(mapped.pData, a_data, a_bytes);
			globals::d3d::context->Unmap(a_buffer.resource.get(), 0);
		};
		if (configurationDirty) {
			upload(
				*simulationProfileBuffer,
				simulationProfiles.data(),
				simulationProfiles.size() * sizeof(SimulationProfileGPU));
			upload(
				*collisionRuleBuffer,
				collisionRules.data(),
				collisionRules.size() * sizeof(CollisionRuleGPU));
			upload(
				*particleTemplateBuffer,
				particleTemplates.data(),
				particleTemplates.size() * sizeof(ParticleTemplateGPU));
			upload(
				*emissionProfileBuffer,
				emissionProfiles.data(),
				emissionProfiles.size() * sizeof(EmissionProfileGPU));
			configurationDirty = false;
		}
		if (uploadedFeatureConfigurationGeneration != pendingFeatureGeneration) {
			upload(
				*featureBuffer,
				features.data(),
				features.size() * sizeof(ParticleFeatureGPU));
			uploadedFeatureConfigurationGeneration = pendingFeatureGeneration;
		}
	}

	void ParticleRuntime::UploadRouteDescriptors()
	{
		if (!routeDescriptors ||
			!routeDescriptorConfigurationDirty.exchange(false, std::memory_order_acq_rel))
			return;
		std::array<RouteDescriptorGPU, kRouteCount> descriptors{};
		const auto persistentListCount = registeredPersistentListCount;
		for (std::uint32_t route = 0; route < kRouteCount; ++route) {
			auto& descriptor = descriptors[route];
			descriptor.baseOffset = route * capacity;
			descriptor.capacity = capacity;
			descriptor.counterOffset = kRoute + route;
			if (route < kSimulationClassCount) {
				descriptor.destinationKind =
					static_cast<std::uint32_t>(RouteDestinationKind::kDynamic);
				descriptor.destinationIndex = route;
			} else if (route < kPersistentRouteBase + kPersistentListCount) {
				descriptor.destinationKind =
					static_cast<std::uint32_t>(RouteDestinationKind::kPersistent);
				descriptor.destinationIndex = route - kPersistentRouteBase;
				const auto& list = persistentLists[descriptor.destinationIndex];
				if (list.registered) {
					descriptor.capacity = list.desc.capacity == 0 ? capacity :
					                                                std::min(list.desc.capacity, capacity);
					descriptor.ownerFeature = list.desc.ownerFeature;
				} else {
					descriptor.capacity = 0;
				}
			} else if (route == kFreeReturnRoute) {
				descriptor.destinationKind =
					static_cast<std::uint32_t>(RouteDestinationKind::kFreeReturn);
				descriptor.destinationIndex = kInvalidRoute;
			} else {
				descriptor.destinationKind =
					static_cast<std::uint32_t>(RouteDestinationKind::kQuarantine);
				descriptor.destinationIndex = kInvalidRoute;
			}
		}
		try {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			DX::ThrowIfFailed(globals::d3d::context->Map(
				routeDescriptors->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			std::memcpy(mapped.pData, descriptors.data(), sizeof(descriptors));
			globals::d3d::context->Unmap(routeDescriptors->resource.get(), 0);
			publishedPersistentListCount = persistentListCount;
		} catch (...) {
			routeDescriptorConfigurationDirty.store(true, std::memory_order_release);
			throw;
		}
	}

	void ParticleRuntime::RefreshCollisionProviderConfiguration() noexcept
	{
		std::array<std::uint32_t, kSimulationClassCount> classMasks{};
		for (const auto& profile : simulationProfiles) {
			if (profile.simulationClass >= kSimulationClassCount)
				continue;
			classMasks[profile.simulationClass] |=
				profile.queryPolicy & kAllCollisionProviders;
		}
		for (std::uint32_t index = 0; index < kSimulationClassCount; ++index) {
			simulationClassCollisionProviderMasks[index].store(
				classMasks[index], std::memory_order_release);
		}

		for (ParticleTemplateID templateID = 0;
			 templateID < particleTemplateCollisionProviderMasks.size();
			 ++templateID) {
			std::uint32_t mask{};
			if (templateID < particleTemplates.size()) {
				const auto& particleTemplate = particleTemplates[templateID];
				if (particleTemplate.simulationProfile < simulationProfiles.size()) {
					mask |= simulationProfiles[particleTemplate.simulationProfile].queryPolicy &
					        kAllCollisionProviders;
				}
				if (particleTemplate.spawnPlacement ==
					static_cast<std::uint32_t>(SpawnPlacement::kProjectToWorldSolid)) {
					mask |= static_cast<std::uint32_t>(CollisionQueryPolicy::kPerspectiveDepth) |
					        static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField);
				} else if (particleTemplate.spawnPlacement ==
						   static_cast<std::uint32_t>(SpawnPlacement::kProjectToWater)) {
					mask |= static_cast<std::uint32_t>(CollisionQueryPolicy::kWaterHeightField);
				}
				if (particleTemplate.lifecyclePhase ==
						static_cast<std::uint32_t>(LifecyclePhase::kPersistent) &&
					particleTemplate.persistentListID < kPersistentListCount) {
					mask |= persistentListCollisionProviderMasks[particleTemplate.persistentListID]
					            .load(std::memory_order_acquire);
				}
			}
			particleTemplateCollisionProviderMasks[templateID].store(
				mask & kAllCollisionProviders, std::memory_order_release);
		}
	}

	std::uint32_t ParticleRuntime::CollisionProvidersForTemplate(
		ParticleTemplateID a_template) const noexcept
	{
		if (a_template >= particleTemplateCollisionProviderMasks.size())
			return 0;
		return particleTemplateCollisionProviderMasks[a_template].load(
			std::memory_order_acquire);
	}

	void ParticleRuntime::RecordCollisionProviderActivity(std::uint32_t a_mask) noexcept
	{
		a_mask &= kAllCollisionProviders;
		auto observed = collisionProviderActivityState.load(std::memory_order_relaxed);
		for (;;) {
			const auto currentMask = static_cast<std::uint32_t>(observed >> 32U);
			const auto currentEpoch = static_cast<std::uint32_t>(observed);
			const auto nextEpoch = currentEpoch + 1U;
			const auto desired =
				(static_cast<std::uint64_t>(currentMask | a_mask) << 32U) | nextEpoch;
			if (collisionProviderActivityState.compare_exchange_weak(
					observed,
					desired,
					std::memory_order_release,
					std::memory_order_relaxed)) {
				return;
			}
		}
	}

	std::uint32_t ParticleRuntime::CollisionProviderActivityEpoch() const noexcept
	{
		return static_cast<std::uint32_t>(
			collisionProviderActivityState.load(std::memory_order_acquire));
	}

	void ParticleRuntime::ReconcileCollisionProviders(
		std::uint32_t a_sampledEpoch,
		std::uint32_t a_requiredMask) noexcept
	{
		a_requiredMask &= kAllCollisionProviders;
		auto observed = collisionProviderActivityState.load(std::memory_order_acquire);
		for (;;) {
			const auto currentEpoch = static_cast<std::uint32_t>(observed);
			const auto currentMask = static_cast<std::uint32_t>(observed >> 32U);
			// Only matching producer epochs may narrow the conservative mask.
			const auto desiredMask = currentEpoch == a_sampledEpoch ?
					a_requiredMask :
					(currentMask | a_requiredMask);
			const auto desired =
				(static_cast<std::uint64_t>(desiredMask) << 32U) | currentEpoch;
			if (desired == observed || collisionProviderActivityState.compare_exchange_weak(
					observed,
					desired,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return;
			}
		}
	}

	void ParticleRuntime::SetupRegisteredRenderers()
	{
		if (!rendererSetupPending.exchange(false, std::memory_order_acq_rel))
			return;
		struct PendingSetup
		{
			RenderClassID id{};
			IParticleRenderer* renderer{};
		};
		std::array<PendingSetup, kRenderClassCount> pending{};
		std::uint32_t pendingCount{};
		for (RenderClassID id = 0; id < registeredRenderClassCount; ++id) {
			auto& slot = renderers[id];
			if (!slot.registered || !slot.setupPending || !slot.renderer)
				continue;
			pending[pendingCount++] = { id, slot.renderer };
			// Clear first so a re-entrant cache clear can re-arm the slot.
			slot.setupPending = false;
		}

		for (std::uint32_t index = 0; index < pendingCount; ++index) {
			auto* renderer = pending[index].renderer;
			try {
				renderer->SetupResources();
			} catch (...) {
				for (std::uint32_t retry = index; retry < pendingCount; ++retry) {
					const auto& failed = pending[retry];
					if (failed.id < registeredRenderClassCount &&
						renderers[failed.id].renderer == failed.renderer) {
						renderers[failed.id].setupPending = true;
					}
				}
				rendererSetupPending.store(true, std::memory_order_release);
				throw;
			}
		}
	}

	void ParticleRuntime::SetupRegisteredInitializers()
	{
		if (!initializerSetupPending.exchange(false, std::memory_order_acq_rel))
			return;
		struct PendingSetup
		{
			NewbornInitializerID id{};
			INewbornParticleInitializer* initializer{};
		};
		std::array<PendingSetup, kMaxNewbornInitializers> pending{};
		std::uint32_t pendingCount{};
		for (NewbornInitializerID id = 0; id < registeredNewbornInitializerCount; ++id) {
			auto& slot = newbornInitializers[id];
			if (!slot.registered || !slot.setupPending || !slot.initializer)
				continue;
			pending[pendingCount++] = { id, slot.initializer };
			slot.setupPending = false;
		}

		for (std::uint32_t index = 0; index < pendingCount; ++index) {
			auto* initializer = pending[index].initializer;
			try {
				initializer->SetupResources();
			} catch (...) {
				for (std::uint32_t retry = index; retry < pendingCount; ++retry) {
					const auto& failed = pending[retry];
					if (failed.id < registeredNewbornInitializerCount &&
						newbornInitializers[failed.id].initializer == failed.initializer) {
						newbornInitializers[failed.id].setupPending = true;
					}
				}
				initializerSetupPending.store(true, std::memory_order_release);
				throw;
			}
		}
	}

	void ParticleRuntime::RefreshInitializerExecutionPlan()
	{
		const auto pendingGeneration =
			initializerConfigurationGeneration.load(std::memory_order_acquire);
		if (publishedInitializerConfigurationGeneration == pendingGeneration)
			return;

		std::array<InitializerExecutionSlot, kMaxNewbornInitializers> plan{};
		const auto planCount = registeredNewbornInitializerCount;
		const auto snapshotGeneration =
			initializerConfigurationGeneration.load(std::memory_order_relaxed);
		for (NewbornInitializerID id = 0; id < planCount; ++id) {
			const auto& source = newbornInitializers[id];
			auto& destination = plan[id];
			destination.id = id;
			destination.initializer = source.initializer;
			destination.desc = source.desc;
		}
		initializerExecutionPlan = std::move(plan);
		initializerExecutionCount = planCount;
		publishedInitializerConfigurationGeneration = snapshotGeneration;
	}

	void ParticleRuntime::RefreshRenderClassConfiguration()
	{
		if (!renderClassBuffer)
			return;

		const auto pendingGeneration =
			rendererConfigurationGeneration.load(std::memory_order_acquire);
		const bool registryChanged =
			publishedRendererConfigurationGeneration != pendingGeneration;
		if (registryChanged) {
			std::array<RendererExecutionSlot, kRenderClassCount> plan{};
			const auto planCount = registeredRenderClassCount;
			const auto snapshotGeneration =
				rendererConfigurationGeneration.load(std::memory_order_relaxed);
			for (RenderClassID renderClass = 0; renderClass < planCount; ++renderClass) {
				const auto& source = renderers[renderClass];
				auto& destination = plan[renderClass];
				destination.renderClass = renderClass;
				destination.renderer = source.renderer;
				destination.desc = source.desc;
			}
			std::stable_sort(
				plan.begin(),
				plan.begin() + planCount,
				[](const RendererExecutionSlot& a_left, const RendererExecutionSlot& a_right) {
					if (a_left.desc.drawOrder != a_right.desc.drawOrder)
						return a_left.desc.drawOrder < a_right.desc.drawOrder;
					return a_left.renderClass < a_right.renderClass;
				});
			rendererExecutionPlan = std::move(plan);
			rendererExecutionCount = planCount;
			publishedRendererConfigurationGeneration = snapshotGeneration;
		}

		std::array<RenderClassGPU, kRenderClassCount> gpuDescriptors{};
		bool changed = registryChanged;
		for (std::uint32_t index = 0; index < rendererExecutionCount; ++index) {
			auto& slot = rendererExecutionPlan[index];
			RenderClassGPU gpu{};
			gpu.enabled = slot.desc.enabled && slot.renderer && slot.renderer->IsReady() ? 1U : 0U;
			gpu.vertexCountPerInstance = slot.desc.vertexCountPerInstance;
			gpu.startVertexLocation = slot.desc.startVertexLocation;
			gpu.preparationThreads = std::min(slot.desc.preparationThreads, 1024U);
			gpu.cullRadiusScale = std::isfinite(slot.desc.cullRadiusScale) ?
			                          std::max(slot.desc.cullRadiusScale, 0.0F) :
			                          1.0F;
			gpu.maximumDistance = std::isfinite(slot.desc.maximumDistance) ?
			                          std::max(slot.desc.maximumDistance, 0.0F) :
			                          0.0F;
			gpu.cullingFlags = slot.desc.cullingFlags &
			                   (static_cast<std::uint32_t>(RenderCullFlags::kFrustum) |
								   static_cast<std::uint32_t>(RenderCullFlags::kDistance));
			const auto descriptorOffset = slot.renderClass * sizeof(RenderClassGPU);
			changed = changed ||
			          std::memcmp(
					  renderClassGPUDescriptorBytes.data() + descriptorOffset,
					  &gpu,
					  sizeof(gpu)) != 0;
			gpuDescriptors[slot.renderClass] = gpu;
			slot.enabled = gpu.enabled != 0;
		}
		if (!changed)
			return;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		DX::ThrowIfFailed(globals::d3d::context->Map(
			renderClassBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		std::memcpy(mapped.pData, gpuDescriptors.data(), sizeof(gpuDescriptors));
		globals::d3d::context->Unmap(renderClassBuffer->resource.get(), 0);
		std::memcpy(
			renderClassGPUDescriptorBytes.data(),
			gpuDescriptors.data(),
			sizeof(gpuDescriptors));
	}

	PersistentEmitterGPU ParticleRuntime::BuildEmitterGPU(
		const EmitterDesc& a_desc,
		std::uint32_t a_generation) noexcept
	{
		PersistentEmitterGPU emitter{};
		emitter.origin = a_desc.origin;
		emitter.strength = std::max(a_desc.strength, 0.0F);
		emitter.forward = Normalize(a_desc.forward);
		emitter.seed = a_desc.seed;
		emitter.right = Normalize(a_desc.right);
		emitter.particleTemplate = a_desc.particleTemplate;
		emitter.vertical = Normalize(a_desc.vertical);
		emitter.emissionProfile = a_desc.emissionProfile;
		emitter.boundsMin = a_desc.boundsMin;
		emitter.particlesPerSecond = std::max(a_desc.particlesPerSecond, 0.0F);
		emitter.boundsMax = a_desc.boundsMax;
		emitter.targetAlive = a_desc.targetAlive;
		emitter.maximumAlive = a_desc.maximumAlive;
		if (emitter.maximumAlive != 0)
			emitter.targetAlive = std::min(emitter.targetAlive, emitter.maximumAlive);
		emitter.generation = a_generation;
		emitter.priority = a_desc.priority;
		emitter.ownerFeature = a_desc.ownerFeature;
		emitter.ownerCell = a_desc.ownerCell;
		emitter.flags = a_desc.flags;
		emitter.state = static_cast<std::uint32_t>(EmitterState::kActive);
		return emitter;
	}

	EmitterHandle ParticleRuntime::CreateEmitter(const EmitterDesc& a_desc) noexcept
	{
		if (a_desc.particleTemplate >= kMaxParticleTemplates ||
			a_desc.emissionProfile >= kMaxEmissionProfiles ||
			a_desc.ownerFeature >= registeredFeatureCount.load(std::memory_order_acquire)) {
			rejectedEmitterCommands.fetch_add(1, std::memory_order_relaxed);
			return {};
		}

		if (freeEmitterSlots.empty()) {
			rejectedEmitterCommands.fetch_add(1, std::memory_order_relaxed);
			return {};
		}
		const auto index = freeEmitterSlots.back();
		freeEmitterSlots.pop_back();
		auto& slot = emitterSlots[index];
		slot.generation += 1U;
		if (slot.generation == 0)
			slot.generation = 1U;
		slot.desired = a_desc;
		slot.state = CPUSlotState::kLive;
		slot.pending = PendingEmitterOperation::kCreate;
		if (!slot.queued) {
			if (dirtyEmitterCount >= dirtyEmitterSlots.size()) {
				slot.state = CPUSlotState::kFree;
				slot.pending = PendingEmitterOperation::kNone;
				freeEmitterSlots.push_back(index);
				rejectedEmitterCommands.fetch_add(1, std::memory_order_relaxed);
				return {};
			}
			slot.queued = true;
			dirtyEmitterSlots[dirtyEmitterCount++] = index;
		}
		const auto providerMask =
			(a_desc.flags & static_cast<std::uint32_t>(EmitterFlags::kEnabled)) != 0 ?
				CollisionProvidersForTemplate(a_desc.particleTemplate) :
				0U;
		liveEmitterCollisionProviderMasks[index].store(providerMask, std::memory_order_release);
		RecordCollisionProviderActivity(providerMask);
		return { index, slot.generation };
	}

	bool ParticleRuntime::UpdateEmitter(
		EmitterHandle a_handle,
		const EmitterDesc& a_desc) noexcept
	{
		if (!a_handle.Valid() || a_handle.index >= kMaxPersistentEmitters ||
			a_desc.particleTemplate >= kMaxParticleTemplates ||
			a_desc.emissionProfile >= kMaxEmissionProfiles ||
			a_desc.ownerFeature >= registeredFeatureCount.load(std::memory_order_acquire)) {
			rejectedEmitterHandles.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		auto& slot = emitterSlots[a_handle.index];
		if (slot.state != CPUSlotState::kLive || slot.generation != a_handle.generation) {
			rejectedEmitterHandles.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (!slot.queued) {
			if (dirtyEmitterCount >= dirtyEmitterSlots.size()) {
				rejectedEmitterCommands.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			slot.queued = true;
			slot.pending = PendingEmitterOperation::kUpdate;
			dirtyEmitterSlots[dirtyEmitterCount++] = a_handle.index;
		} else if (slot.pending != PendingEmitterOperation::kCreate) {
			slot.pending = PendingEmitterOperation::kUpdate;
		}
		slot.desired = a_desc;
		const auto providerMask =
			(a_desc.flags & static_cast<std::uint32_t>(EmitterFlags::kEnabled)) != 0 ?
				CollisionProvidersForTemplate(a_desc.particleTemplate) :
				0U;
		liveEmitterCollisionProviderMasks[a_handle.index].store(
			providerMask, std::memory_order_release);
		RecordCollisionProviderActivity(providerMask);
		return true;
	}

	bool ParticleRuntime::DestroyEmitter(EmitterHandle a_handle) noexcept
	{
		if (!a_handle.Valid() || a_handle.index >= kMaxPersistentEmitters) {
			rejectedEmitterHandles.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		auto& slot = emitterSlots[a_handle.index];
		if (slot.state != CPUSlotState::kLive || slot.generation != a_handle.generation) {
			rejectedEmitterHandles.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		slot.state = CPUSlotState::kPendingDestroy;
		if (slot.queued && slot.pending == PendingEmitterOperation::kCreate) {
			// Cancel a create generation that was never uploaded.
			slot.pending = PendingEmitterOperation::kCancelCreate;
		} else {
			slot.pending = PendingEmitterOperation::kDestroy;
			if (!slot.queued) {
				if (dirtyEmitterCount >= dirtyEmitterSlots.size()) {
					slot.state = CPUSlotState::kLive;
					slot.pending = PendingEmitterOperation::kNone;
					rejectedEmitterCommands.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				slot.queued = true;
				dirtyEmitterSlots[dirtyEmitterCount++] = a_handle.index;
			}
		}
		liveEmitterCollisionProviderMasks[a_handle.index].store(0, std::memory_order_release);
		RecordCollisionProviderActivity(0);
		return true;
	}

	bool ParticleRuntime::QueueBurst(const BurstRequestGPU& a_request) noexcept
	{
		if (a_request.count == 0)
			return false;
		const auto featureCount = registeredFeatureCount.load(std::memory_order_acquire);
		const bool validFeature = a_request.ownerFeature < featureCount;
		if (!validFeature || a_request.particleTemplate >= kMaxParticleTemplates) {
			rejectedCPUCommands.fetch_add(1, std::memory_order_relaxed);
			rejectedCPUParticles.fetch_add(a_request.count, std::memory_order_relaxed);
			if (validFeature) {
				rejectedCPURequestedByFeature[a_request.ownerFeature].fetch_add(
					a_request.count, std::memory_order_relaxed);
				rejectedCPUDroppedByFeature[a_request.ownerFeature].fetch_add(
					a_request.count, std::memory_order_relaxed);
			}
			return false;
		}
		const auto generation = burstQueueGeneration.load(std::memory_order_acquire);
		std::array<std::uint32_t, 1> slots{};
		if (!ReserveBurstSlots(slots)) {
			rejectedCPUCommands.fetch_add(1, std::memory_order_relaxed);
			rejectedCPUParticles.fetch_add(a_request.count, std::memory_order_relaxed);
			rejectedCPURequestedByFeature[a_request.ownerFeature].fetch_add(
				a_request.count, std::memory_order_relaxed);
			rejectedCPUDroppedByFeature[a_request.ownerFeature].fetch_add(
				a_request.count, std::memory_order_relaxed);
			return false;
		}
		// Publish provider demand before the burst becomes visible to the consumer.
		RecordCollisionProviderActivity(
			CollisionProvidersForTemplate(a_request.particleTemplate));
		PublishBurstSlot(
			slots.front(),
			a_request,
			generation);
		return true;
	}

	bool ParticleRuntime::QueueDebugBurst(const float3& a_origin, const float3& a_forward) noexcept
	{
		BurstRequestGPU base{};
		base.origin = a_origin;
		base.forward = Normalize(a_forward);
		const auto referenceUp = std::abs(base.forward.z) > 0.98F ?
		                             float3{ 0.0F, 1.0F, 0.0F } :
		                             float3{ 0.0F, 0.0F, 1.0F };
		base.right = Normalize(base.forward.Cross(referenceUp));
		base.vertical = Normalize(base.right.Cross(base.forward));
		base.count = 24;
		base.ownerFeature = kDebugFeature;
		base.initialSpeed = { { 120.0F, 480.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F };
		base.horizontalAngleDegrees = { { -35.0F, 35.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F };
		base.verticalAngleDegrees = { { -20.0F, 35.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F };
		base.lifetime = { { 1.5F, 4.0F }, static_cast<std::uint32_t>(DistributionType::kNormal), 1.0F };
		base.radius = { { 3.0F, 8.0F }, static_cast<std::uint32_t>(DistributionType::kUniform), 1.0F };
		base.opacity = { { 0.55F, 0.95F }, static_cast<std::uint32_t>(DistributionType::kUniform), 1.0F };

		static constexpr std::array<ParticleTemplateID, 4> templates{
			kDebugNoCollisionTemplate,
			kDebugWorldTemplate,
			kDebugWorldWaterTemplate,
			kDebugWorldPlayerTemplate
		};
		std::array<BurstRequestGPU, templates.size()> requests{};
		static constexpr std::array<float, 4> lateralOffsets{ -24.0F, -8.0F, 8.0F, 0.0F };
		for (std::uint32_t index = 0; index < requests.size(); ++index) {
			requests[index] = base;
			requests[index].origin = {
				a_origin.x + base.right.x * lateralOffsets[index],
				a_origin.y + base.right.y * lateralOffsets[index],
				a_origin.z + base.right.z * lateralOffsets[index]
			};
			requests[index].seed = static_cast<std::uint32_t>(
				frameCounter * 0x9E3779B9ULL + 0x51A7E123ULL + index * 0x85EBCA6BU);
			requests[index].particleTemplate = templates[index];
		}

		const auto generation = burstQueueGeneration.load(std::memory_order_acquire);
		std::array<std::uint32_t, requests.size()> slots{};
		if (!ReserveBurstSlots(slots)) {
			rejectedCPUCommands.fetch_add(requests.size(), std::memory_order_relaxed);
			rejectedCPUParticles.fetch_add(base.count * requests.size(), std::memory_order_relaxed);
			rejectedCPURequestedByFeature[kDebugFeature].fetch_add(
				base.count * requests.size(), std::memory_order_relaxed);
			rejectedCPUDroppedByFeature[kDebugFeature].fetch_add(
				base.count * requests.size(), std::memory_order_relaxed);
			return false;
		}
		std::uint32_t providerMask{};
		for (const auto particleTemplate : templates)
			providerMask |= CollisionProvidersForTemplate(particleTemplate);
		RecordCollisionProviderActivity(providerMask);
		for (std::size_t index = 0; index < requests.size(); ++index)
			PublishBurstSlot(slots[index], requests[index], generation);
		return true;
	}

	std::uint32_t ParticleRuntime::UploadBurstRequests()
	{
		auto& requests = burstUploadScratch;
		std::uint32_t count{};
		std::uint32_t consumedProviderMask{};
		const auto generation = burstQueueGeneration.load(std::memory_order_acquire);
		for (auto& slot : burstQueue) {
			auto expected = BurstSlotState::kReady;
			if (!slot.state.compare_exchange_strong(
					expected,
					BurstSlotState::kReading,
					std::memory_order_acq_rel,
					std::memory_order_relaxed)) {
				continue;
			}
			if (slot.generation == generation && count < requests.size()) {
				requests[count++] = slot.request;
				consumedProviderMask |=
					CollisionProvidersForTemplate(slot.request.particleTemplate);
			}
			slot.state.store(BurstSlotState::kEmpty, std::memory_order_release);
		}
		if (consumedProviderMask != 0)
			RecordCollisionProviderActivity(consumedProviderMask);
		if (count == 0)
			return 0;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		DX::ThrowIfFailed(globals::d3d::context->Map(
			burstUpload->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		std::memcpy(mapped.pData, requests.data(), count * sizeof(BurstRequestGPU));
		globals::d3d::context->Unmap(burstUpload->resource.get(), 0);
		return count;
	}

	ParticleRuntime::EmitterUploadCounts ParticleRuntime::UploadEmitterCommands()
	{
		auto& commands = emitterCommandScratch;
		auto& activeEmitterHandles = activeEmitterScratch;
		EmitterUploadCounts result{};
		for (std::uint32_t dirtyIndex = 0; dirtyIndex < dirtyEmitterCount; ++dirtyIndex) {
			const auto emitterIndex = dirtyEmitterSlots[dirtyIndex];
			if (emitterIndex >= emitterSlots.size())
				continue;
			auto& slot = emitterSlots[emitterIndex];
			slot.queued = false;

			EmitterCommandType operation{};
			bool emitCommand = true;
			switch (slot.pending) {
			case PendingEmitterOperation::kCreate:
				operation = EmitterCommandType::kCreate;
				break;
			case PendingEmitterOperation::kUpdate:
				operation = EmitterCommandType::kUpdate;
				break;
			case PendingEmitterOperation::kDestroy:
				operation = EmitterCommandType::kDestroy;
				break;
			case PendingEmitterOperation::kCancelCreate:
			case PendingEmitterOperation::kNone:
			default:
				emitCommand = false;
				break;
			}

			if (emitCommand && result.commands < commands.size()) {
				auto& command = commands[result.commands++];
				command = {};
				command.emitter = BuildEmitterGPU(slot.desired, slot.generation);
				command.emitterIndex = emitterIndex;
				command.operation = static_cast<std::uint32_t>(operation);
			}

			const bool releaseSlot = slot.state == CPUSlotState::kPendingDestroy;
			slot.pending = PendingEmitterOperation::kNone;
			if (releaseSlot) {
				slot.desired = {};
				slot.state = CPUSlotState::kFree;
				freeEmitterSlots.push_back(emitterIndex);
			}
		}
		dirtyEmitterCount = 0;

		for (std::uint32_t emitterIndex = 0; emitterIndex < emitterSlots.size(); ++emitterIndex) {
			if (emitterSlots[emitterIndex].state == CPUSlotState::kLive) {
				activeEmitterHandles[result.activeEmitters++] = {
					emitterIndex, emitterSlots[emitterIndex].generation
				};
			}
		}

		auto upload = [](Buffer& a_buffer, const void* a_data, std::size_t a_bytes) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			DX::ThrowIfFailed(globals::d3d::context->Map(
				a_buffer.resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			std::memcpy(mapped.pData, a_data, a_bytes);
			globals::d3d::context->Unmap(a_buffer.resource.get(), 0);
		};
		if (result.commands != 0) {
			upload(
				*emitterCommandUpload,
				commands.data(),
				result.commands * sizeof(EmitterCommandGPU));
		}
		if (result.activeEmitters != 0) {
			upload(
				*activeEmitterUpload,
				activeEmitterHandles.data(),
				result.activeEmitters * sizeof(EmitterHandle));
		}
		return result;
	}

	void ParticleRuntime::CopyBurstRequestsToUnifiedArena(std::uint32_t a_requestCount)
	{
		if (a_requestCount == 0)
			return;
		D3D11_BOX sourceBox{};
		sourceBox.right = static_cast<UINT>(a_requestCount * sizeof(BurstRequestGPU));
		sourceBox.bottom = 1;
		sourceBox.back = 1;
		globals::d3d::context->CopySubresourceRegion(
			frameRequests->resource.get(),
			0,
			0,
			0,
			0,
			burstUpload->resource.get(),
			0,
			&sourceBox);
	}

	std::uint32_t ParticleRuntime::CurrentCounterBase() const noexcept
	{
		return currentArena == 0 ? kDynamicA : kDynamicB;
	}

	std::uint32_t ParticleRuntime::OutputCounterBase() const noexcept
	{
		return currentArena == 0 ? kDynamicB : kDynamicA;
	}

	void ParticleRuntime::ResetTransientCounters(
		const FrameConstants& a_constants,
		const EmitterFrameConstants& a_emitterConstants,
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		frameCB->Update(a_constants);
		emitterFrameCB->Update(a_emitterConstants);
		lifecycleFrameCB->Update(a_lifecycleConstants);
		RenderFrameConstants renderConstants{};
		renderConstants.registeredClassCount = rendererExecutionCount;
		renderFrameCB->Update(renderConstants);
		ID3D11Buffer* cb = frameCB->CB();
		ID3D11Buffer* emitterCB = emitterFrameCB->CB();
		ID3D11Buffer* lifecycleCB = lifecycleFrameCB->CB();
		ID3D11Buffer* renderCB = renderFrameCB->CB();
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[7] = counters->uav.get();
		globals::d3d::context->CSSetConstantBuffers(0, 1, &cb);
		globals::d3d::context->CSSetConstantBuffers(2, 1, &emitterCB);
		globals::d3d::context->CSSetConstantBuffers(3, 1, &lifecycleCB);
		globals::d3d::context->CSSetConstantBuffers(4, 1, &renderCB);
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		globals::d3d::context->CSSetShader(resetCountersCS.get(), nullptr, 0);
		globals::d3d::context->Dispatch(1, 1, 1);
		globals::d3d::context->CSSetShader(resetEmitterFrameCS.get(), nullptr, 0);
		globals::d3d::context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	}

	void ParticleRuntime::ResetDynamicOutputCounters(const FrameConstants& a_constants)
	{
		frameCB->Update(a_constants);
		ID3D11Buffer* frame = frameCB->CB();
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[7] = counters->uav.get();
		auto* context = globals::d3d::context;
		context->CSSetConstantBuffers(0, 1, &frame);
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(resetDynamicCountersCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	}

	void ParticleRuntime::ReclassifyDynamicParticles(FrameConstants& a_constants)
	{
		if (!simulationProfileReclassificationPending.exchange(
				false, std::memory_order_acq_rel))
			return;

		// Reclassification dispatches over current compact lists, not pool capacity.
		BuildSimulationArguments(a_constants);

		auto* context = globals::d3d::context;
		ID3D11ShaderResourceView* srvs[9]{};
		srvs[0] = dynamicArenas[currentArena]->srv.get();
		srvs[1] = simulationProfileBuffer->srv.get();
		srvs[2] = metadata->srv.get();
		srvs[7] = routeDescriptors->srv.get();
		srvs[8] = particleAuxiliaryState->srv.get();
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = dynamicArenas[1U - currentArena]->uav.get();
		uavs[5] = routeArena->uav.get();
		uavs[6] = eventArena->uav.get();
		uavs[7] = counters->uav.get();
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		context->CSSetConstantBuffers(3, 1, &lifecycle);
		context->CSSetShader(reclassifySimulationProfilesCS.get(), nullptr, 0);

		globals::profiler->BeginPass("SSPP::ProfileReclassification");
		for (std::uint32_t simulationClass = 0;
			 simulationClass < kSimulationClassCount;
			 ++simulationClass) {
			auto constants = a_constants;
			constants.dispatchClass = simulationClass;
			frameCB->Update(constants);
			ID3D11Buffer* frame = frameCB->CB();
			context->CSSetConstantBuffers(0, 1, &frame);
			context->DispatchIndirect(
				simulationDispatchArgs.get(), simulationClass * 3U * sizeof(std::uint32_t));
		}

		ID3D11UnorderedAccessView* nullUAVs[8]{};
		ID3D11ShaderResourceView* nullSRVs[std::size(srvs)]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);

		// The old input counters become this frame's simulation destinations.
		currentArena = 1U - currentArena;
		a_constants.inputCounterBase = CurrentCounterBase();
		a_constants.outputCounterBase = OutputCounterBase();
		ResetDynamicOutputCounters(a_constants);
		globals::profiler->EndPass();
	}

	void ParticleRuntime::ApplyEmitterCommands(const EmitterFrameConstants& a_constants)
	{
		if (a_constants.emitterCommandCount == 0)
			return;
		emitterFrameCB->Update(a_constants);
		ID3D11Buffer* emitterCB = emitterFrameCB->CB();
		ID3D11ShaderResourceView* srvs[]{ emitterCommandUpload->srv.get() };
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = emitterStates->uav.get();
		uavs[7] = counters->uav.get();
		globals::d3d::context->CSSetConstantBuffers(2, 1, &emitterCB);
		globals::d3d::context->CSSetShaderResources(0, 1, srvs);
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		globals::d3d::context->CSSetShader(applyEmitterCommandsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::EmitterCommands");
		globals::d3d::context->Dispatch(
			(a_constants.emitterCommandCount + kThreads - 1U) / kThreads, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		ID3D11ShaderResourceView* nullSRVs[1]{};
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		globals::d3d::context->CSSetShaderResources(0, 1, nullSRVs);
	}

	void ParticleRuntime::BuildSimulationArguments(const FrameConstants& a_constants)
	{
		frameCB->Update(a_constants);
		LifecycleFrameConstants inputLifecycle{};
		inputLifecycle.eventCapacity = capacity;
		inputLifecycle.routeEpoch = routeEpoch;
		inputLifecycle.persistentDestinationParityMask = persistentParityMask;
		inputLifecycle.persistentListCount = publishedPersistentListCount;
		lifecycleFrameCB->Update(inputLifecycle);
		ID3D11Buffer* cb = frameCB->CB();
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = simulationDispatchArgsUAV.get();
		uavs[1] = persistentDispatchArgsUAV.get();
		uavs[7] = counters->uav.get();
		globals::d3d::context->CSSetConstantBuffers(0, 1, &cb);
		globals::d3d::context->CSSetConstantBuffers(3, 1, &lifecycle);
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		globals::d3d::context->CSSetShader(buildSimulationArgsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::BuildIndirectArgs");
		globals::d3d::context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	}

	ParticleRuntime::CollisionFrameConstants ParticleRuntime::BuildCollisionFrameConstants() const noexcept
	{
		CollisionFrameConstants constants{};
		auto& sceneDepth = ScreenSpaceCollisionDepth::GetSingleton();
		auto& heightField = ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton();
		auto& player = ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton();

		constants.sceneDepthAvailable = globals::state &&
		                                        sceneDepth.ReadyForFrame(globals::state->frameCount) && sceneDepth.SRV() ?
		                                    1U :
		                                    0U;
		constants.solidHeightAvailable = heightField.Ready() && heightField.SRV() ? 1U : 0U;
		constants.waterHeightAvailable = heightField.WaterReady() && heightField.WaterSRV() ? 1U : 0U;
		constants.playerCapsuleCount = player.Ready() && player.SRV() ?
		                                   std::min(player.Count(), PlayerCollisionProvider::kMaximumVolumes) :
		                                   0U;
		constants.solidCenter = heightField.Center();
		constants.solidHalfExtent = heightField.HalfExtent();
		constants.solidTexelSize = heightField.TexelSize();
		constants.waterCenter = heightField.WaterCenter();
		constants.waterHalfExtent = heightField.WaterHalfExtent();
		constants.waterTexelSize = heightField.WaterTexelSize();
		constants.playerVelocity = constants.playerCapsuleCount > 0 ? player.Velocity() : float3{};
		constants.collisionRuleCount = static_cast<std::uint32_t>(collisionRules.size());
		return constants;
	}

	void ParticleRuntime::Simulate(const FrameConstants& a_constants)
	{
		auto* context = globals::d3d::context;
		auto& sceneDepth = ScreenSpaceCollisionDepth::GetSingleton();
		auto& heightField = ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton();
		auto& player = ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton();
		const auto collisionConstants = BuildCollisionFrameConstants();
		collisionFrameCB->Update(collisionConstants);
		ID3D11ShaderResourceView* srvs[10]{
			dynamicArenas[currentArena]->srv.get(),
			simulationProfileBuffer->srv.get(),
			collisionRuleBuffer->srv.get(),
			collisionConstants.sceneDepthAvailable ? sceneDepth.SRV() : nullptr,
			collisionConstants.solidHeightAvailable ? heightField.SRV() : nullptr,
			collisionConstants.waterHeightAvailable ? heightField.WaterSRV() : nullptr,
			collisionConstants.playerCapsuleCount > 0 ? player.SRV() : nullptr,
			routeDescriptors->srv.get(),
			particleAuxiliaryState->srv.get(),
			appearance->srv.get()
		};
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
		ID3D11UnorderedAccessView* uavs[8]{
			positionAge->uav.get(),
			velocityLifetime->uav.get(),
			nullptr,
			metadata->uav.get(),
			contactData->uav.get(),
			routeArena->uav.get(),
			eventArena->uav.get(),
			counters->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(simulationCS.get(), nullptr, 0);
		ID3D11Buffer* perFrame = *globals::game::perFrame;
		context->CSSetConstantBuffers(12, 1, &perFrame);
		ID3D11Buffer* collisionFrame = collisionFrameCB->CB();
		context->CSSetConstantBuffers(1, 1, &collisionFrame);
		ID3D11Buffer* lifecycleFrame = lifecycleFrameCB->CB();
		context->CSSetConstantBuffers(3, 1, &lifecycleFrame);
		static constexpr const char* names[kSimulationClassCount]{
			"SSPP::Simulation::NoCollision",
			"SSPP::Simulation::World",
			"SSPP::Simulation::WorldWater",
			"SSPP::Simulation::WorldPlayer"
		};
		for (std::uint32_t simulationClass = 0; simulationClass < kSimulationClassCount; ++simulationClass) {
			auto constants = a_constants;
			constants.dispatchClass = simulationClass;
			frameCB->Update(constants);
			ID3D11Buffer* cb = frameCB->CB();
			context->CSSetConstantBuffers(0, 1, &cb);
			globals::profiler->BeginPass(names[simulationClass]);
			context->DispatchIndirect(
				simulationDispatchArgs.get(), simulationClass * 3U * sizeof(std::uint32_t));
			globals::profiler->EndPass();
		}
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		ID3D11ShaderResourceView* nullSRVs[std::size(srvs)]{};
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
	}

	void ParticleRuntime::UpdatePersistentEmitters(
		const FrameConstants& a_constants,
		const EmitterFrameConstants& a_emitterConstants)
	{
		if (a_emitterConstants.activeEmitterCount == 0)
			return;
		auto* context = globals::d3d::context;
		frameCB->Update(a_constants);
		emitterFrameCB->Update(a_emitterConstants);
		ID3D11Buffer* frame = frameCB->CB();
		ID3D11Buffer* emitterFrame = emitterFrameCB->CB();
		ID3D11ShaderResourceView* srvs[]{
			activeEmitterUpload->srv.get(), emissionProfileBuffer->srv.get()
		};
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = emitterStates->uav.get();
		uavs[1] = frameRequests->uav.get();
		uavs[7] = counters->uav.get();
		context->CSSetConstantBuffers(0, 1, &frame);
		context->CSSetConstantBuffers(2, 1, &emitterFrame);
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(updateEmittersCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::EmitterUpdate");
		context->Dispatch(
			(a_emitterConstants.activeEmitterCount + kThreads - 1U) / kThreads, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		ID3D11ShaderResourceView* nullSRVs[2]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		context->CSSetShaderResources(0, 2, nullSRVs);
	}

	void ParticleRuntime::PlanAndInitialize(
		const FrameConstants& a_constants,
		const EmitterFrameConstants& a_emitterConstants)
	{
		auto* context = globals::d3d::context;
		frameCB->Update(a_constants);
		emitterFrameCB->Update(a_emitterConstants);
		ID3D11Buffer* cb = frameCB->CB();
		ID3D11Buffer* emitterFrame = emitterFrameCB->CB();
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetConstantBuffers(2, 1, &emitterFrame);
		ID3D11Buffer* lifecycleFrame = lifecycleFrameCB->CB();
		context->CSSetConstantBuffers(3, 1, &lifecycleFrame);

		ID3D11ShaderResourceView* planningSRVs[]{
			frameRequests->srv.get(),
			particleTemplateBuffer->srv.get(),
			simulationProfileBuffer->srv.get(),
			featureBuffer->srv.get()
		};
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(planningSRVs)), planningSRVs);
		ID3D11UnorderedAccessView* planUAVs[8]{};
		planUAVs[1] = emitterStates->uav.get();
		planUAVs[2] = emissionChunks->uav.get();
		planUAVs[7] = counters->uav.get();
		context->CSSetUnorderedAccessViews(0, 8, planUAVs, nullptr);
		const auto requestUpperBound = std::min(
			kMaxFrameRequests,
			a_emitterConstants.cpuRequestCount + a_emitterConstants.activeEmitterCount);
		globals::profiler->BeginPass("SSPP::EmissionPlanning");
		if (requestUpperBound != 0) {
			context->CSSetShader(emissionPlanCS.get(), nullptr, 0);
			context->Dispatch(1, 1, 1);
		}
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullPlanUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullPlanUAVs, nullptr);
		ID3D11ShaderResourceView* nullPlanningSRVs[std::size(planningSRVs)]{};
		context->CSSetShaderResources(
			0, static_cast<UINT>(std::size(nullPlanningSRVs)), nullPlanningSRVs);

		// The planner produces a bounded GPU work-chunk count.
		ID3D11UnorderedAccessView* argumentUAVs[8]{};
		argumentUAVs[0] = emissionDispatchArgsUAV.get();
		argumentUAVs[7] = counters->uav.get();
		context->CSSetConstantBuffers(2, 1, &emitterFrame);
		context->CSSetUnorderedAccessViews(0, 8, argumentUAVs, nullptr);
		context->CSSetShader(buildEmissionArgsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::BuildIndirectArgs");
		context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullArgumentUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullArgumentUAVs, nullptr);

		ID3D11ShaderResourceView* emissionSRVs[8]{
			frameRequests->srv.get(),
			emissionChunks->srv.get(),
			freeIDs->srv.get(),
			particleTemplateBuffer->srv.get(),
			nullptr,
			nullptr,
			nullptr,
			routeDescriptors->srv.get()
		};
		context->CSSetShaderResources(0, 8, emissionSRVs);
		ID3D11UnorderedAccessView* emissionUAVs[8]{
			positionAge->uav.get(),
			velocityLifetime->uav.get(),
			appearance->uav.get(),
			metadata->uav.get(),
			contactData->uav.get(),
			routeArena->uav.get(),
			particleAuxiliaryState->uav.get(),
			counters->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, 8, emissionUAVs, nullptr);
		context->CSSetShader(emissionCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::ParticleInitialization");
		context->DispatchIndirect(emissionDispatchArgs.get(), 0);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		ID3D11ShaderResourceView* nullSRVs[8]{};
		context->CSSetShaderResources(0, 8, nullSRVs);
	}

	void ParticleRuntime::InitializeFeatureNewborns(
		const FrameConstants& a_constants,
		const EmitterFrameConstants& a_emitterConstants)
	{
		if (a_emitterConstants.readyInitializerMask == 0)
			return;

		auto& sceneDepth = ScreenSpaceCollisionDepth::GetSingleton();
		auto& heightField = ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton();
		const auto collisionConstants = BuildCollisionFrameConstants();
		collisionFrameCB->Update(collisionConstants);
		frameCB->Update(a_constants);
		emitterFrameCB->Update(a_emitterConstants);
		auto* context = globals::d3d::context;
		ID3D11Buffer* perFrame = *globals::game::perFrame;

		for (std::uint32_t index = 0; index < initializerExecutionCount; ++index) {
			const auto& slot = initializerExecutionPlan[index];
			const auto id = slot.id;
			if ((a_emitterConstants.readyInitializerMask & (1U << id)) == 0 ||
				!slot.initializer)
				continue;
			UnbindComputeResources();
			NewbornInitializationContext initializerContext{};
			initializerContext.context = context;
			initializerContext.frameRequests = frameRequests->srv.get();
			initializerContext.emissionChunks = emissionChunks->srv.get();
			initializerContext.freeIDs = freeIDs->srv.get();
			initializerContext.particleTemplates = particleTemplateBuffer->srv.get();
			initializerContext.sceneDepth =
				collisionConstants.sceneDepthAvailable ? sceneDepth.SRV() : nullptr;
			initializerContext.solidHeight =
				collisionConstants.solidHeightAvailable ? heightField.SRV() : nullptr;
			initializerContext.waterHeight =
				collisionConstants.waterHeightAvailable ? heightField.WaterSRV() : nullptr;
			initializerContext.routeDescriptors = routeDescriptors->srv.get();
			initializerContext.simulationProfiles = simulationProfileBuffer->srv.get();
			initializerContext.positionAge = positionAge->uav.get();
			initializerContext.velocityLifetime = velocityLifetime->uav.get();
			initializerContext.appearance = appearance->uav.get();
			initializerContext.metadata = metadata->uav.get();
			initializerContext.contactData = contactData->uav.get();
			initializerContext.routeArena = routeArena->uav.get();
			initializerContext.auxiliaryState = particleAuxiliaryState->uav.get();
			initializerContext.counters = counters->uav.get();
			initializerContext.frameConstants = frameCB->CB();
			initializerContext.collisionConstants = collisionFrameCB->CB();
			initializerContext.emitterConstants = emitterFrameCB->CB();
			initializerContext.lifecycleConstants = lifecycleFrameCB->CB();
			initializerContext.gamePerFrameConstants = perFrame;
			initializerContext.dispatchArguments = emissionDispatchArgs.get();
			globals::profiler->BeginPass(slot.desc.profilerName);
			slot.initializer->Dispatch(initializerContext);
			globals::profiler->EndPass();
			UnbindComputeResources();
		}
	}

	void ParticleRuntime::MaintainPersistentLists(
		const FrameConstants& a_constants,
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		auto* context = globals::d3d::context;
		ID3D11UnorderedAccessView* nullUAVs[8]{};

		const auto listCount =
			std::min(registeredPersistentListCount, publishedPersistentListCount);
		struct ExecutionSlot
		{
			IPersistentParticleMaintenance* maintenance{};
			std::uint32_t currentArena{};
			std::uint32_t builtinPolicy{};
			float elapsedSeconds{};
			bool registered{};
			bool rebuildThisFrame{};
			bool drainThisFrame{};
		};
		std::array<ExecutionSlot, kPersistentListCount> lists{};
		for (std::uint32_t listID = 0; listID < listCount; ++listID) {
			const auto& source = persistentLists[listID];
			lists[listID] = {
				source.maintenance,
				source.currentArena,
				static_cast<std::uint32_t>(source.desc.builtinPolicy),
				source.elapsedSeconds,
				source.registered,
				source.rebuildThisFrame,
				source.drainThisFrame
			};
		}
		for (std::uint32_t listID = 0; listID < listCount; ++listID) {
			const auto& slot = lists[listID];
			if (!slot.registered || !slot.rebuildThisFrame)
				continue;
			auto listConstants = a_lifecycleConstants;
			listConstants.maintenanceList = listID;
			listConstants.maintenancePolicy = slot.drainThisFrame ? 3U :
			                                                        slot.builtinPolicy;
			listConstants.maintenanceElapsed = slot.elapsedSeconds;
			lifecycleFrameCB->Update(listConstants);
			ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
			context->CSSetConstantBuffers(3, 1, &lifecycle);

			ID3D11UnorderedAccessView* resetUAVs[8]{};
			resetUAVs[7] = counters->uav.get();
			context->CSSetUnorderedAccessViews(0, 8, resetUAVs, nullptr);
			context->CSSetShader(resetPersistentDestinationCS.get(), nullptr, 0);
			context->Dispatch(1, 1, 1);
			context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);

			if (!slot.drainThisFrame && slot.maintenance && slot.maintenance->IsReady()) {
				UnbindComputeResources();
				ParticleMaintenanceContext maintenanceContext{};
				maintenanceContext.inputReferences =
					persistentArenas[slot.currentArena]->srv.get();
				maintenanceContext.inputBase = listID * capacity;
				maintenanceContext.inputCounterOffset =
					(slot.currentArena != 0 ? kPersistentB : kPersistentA) + listID;
				maintenanceContext.particleCapacity = capacity;
				maintenanceContext.dispatchArguments = persistentDispatchArgs.get();
				maintenanceContext.dispatchArgumentOffset = listID * 3U * sizeof(std::uint32_t);
				maintenanceContext.positionAge = positionAge->uav.get();
				maintenanceContext.velocityLifetime = velocityLifetime->uav.get();
				maintenanceContext.appearance = appearance->uav.get();
				maintenanceContext.metadata = metadata->uav.get();
				maintenanceContext.contactData = contactData->uav.get();
				maintenanceContext.auxiliaryState = particleAuxiliaryState->srv.get();
				maintenanceContext.emitterStates = emitterStates->srv.get();
				maintenanceContext.routeDescriptors = routeDescriptors->srv.get();
				maintenanceContext.routeArena = routeArena->uav.get();
				maintenanceContext.counters = counters->uav.get();
				maintenanceContext.frameConstants = frameCB->CB();
				maintenanceContext.lifecycleConstants = lifecycle;
				maintenanceContext.keepRoute = kPersistentRouteBase + listID;
				maintenanceContext.elapsedSeconds = slot.elapsedSeconds;
				// Feature callbacks own profiler scopes; the profiler is not stack-aware.
				slot.maintenance->Dispatch(maintenanceContext);
				UnbindComputeResources();
				continue;
			}

			ID3D11ShaderResourceView* srvs[9]{};
			srvs[0] = persistentArenas[slot.currentArena]->srv.get();
			srvs[1] = metadata->srv.get();
			srvs[2] = simulationProfileBuffer->srv.get();
			srvs[3] = contactData->srv.get();
			srvs[7] = routeDescriptors->srv.get();
			context->CSSetShaderResources(0, 9, srvs);
			ID3D11UnorderedAccessView* maintenanceUAVs[8]{};
			maintenanceUAVs[0] = positionAge->uav.get();
			maintenanceUAVs[1] = velocityLifetime->uav.get();
			const auto destinationArena =
				(persistentDestinationParityMask >> listID) & 1U;
			maintenanceUAVs[2] = persistentArenas[destinationArena]->uav.get();
			maintenanceUAVs[3] = particleAuxiliaryState->uav.get();
			maintenanceUAVs[5] = routeArena->uav.get();
			maintenanceUAVs[7] = counters->uav.get();
			context->CSSetUnorderedAccessViews(0, 8, maintenanceUAVs, nullptr);
			frameCB->Update(a_constants);
			ID3D11Buffer* frame = frameCB->CB();
			context->CSSetConstantBuffers(0, 1, &frame);
			context->CSSetShader(builtinMaintenanceCS.get(), nullptr, 0);
			globals::profiler->BeginPass(persistentLists[listID].profilerName);
			context->DispatchIndirect(
				persistentDispatchArgs.get(), listID * 3U * sizeof(std::uint32_t));
			globals::profiler->EndPass();
			context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
			ID3D11ShaderResourceView* nullSRVs[9]{};
			context->CSSetShaderResources(0, 9, nullSRVs);
		}
		lifecycleFrameCB->Update(a_lifecycleConstants);
	}

	void ParticleRuntime::BuildRouteArguments(
		const FrameConstants& a_constants,
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		frameCB->Update(a_constants);
		lifecycleFrameCB->Update(a_lifecycleConstants);
		ID3D11Buffer* frame = frameCB->CB();
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = routeDispatchArgsUAV.get();
		uavs[1] = eventDispatchArgsUAV.get();
		uavs[7] = counters->uav.get();
		auto* context = globals::d3d::context;
		context->CSSetConstantBuffers(0, 1, &frame);
		context->CSSetConstantBuffers(3, 1, &lifecycle);
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(buildRouteArgsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::BuildIndirectArgs");
		context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	}

	void ParticleRuntime::BuildFreeReturnArgument(
		const FrameConstants& a_constants,
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		frameCB->Update(a_constants);
		lifecycleFrameCB->Update(a_lifecycleConstants);
		ID3D11Buffer* frame = frameCB->CB();
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = routeDispatchArgsUAV.get();
		uavs[7] = counters->uav.get();
		auto* context = globals::d3d::context;
		context->CSSetConstantBuffers(0, 1, &frame);
		context->CSSetConstantBuffers(3, 1, &lifecycle);
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(buildFreeRouteArgsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::BuildIndirectArgs");
		context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	}

	void ParticleRuntime::TransitionRoutes(
		const FrameConstants& a_constants,
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		auto* context = globals::d3d::context;
		ID3D11ShaderResourceView* srvs[]{
			nullptr, metadata->srv.get(), routeDescriptors->srv.get()
		};
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = dynamicArenas[1U - currentArena]->uav.get();
		uavs[1] = persistentArenas[0]->uav.get();
		uavs[2] = persistentArenas[1]->uav.get();
		uavs[3] = particleAuxiliaryState->uav.get();
		uavs[4] = routeArena->uav.get();
		uavs[7] = counters->uav.get();
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(transitionCS.get(), nullptr, 0);
		frameCB->Update(a_constants);
		ID3D11Buffer* frame = frameCB->CB();
		context->CSSetConstantBuffers(0, 1, &frame);
		globals::profiler->BeginPass("SSPP::TransitionRouting");
		const auto dispatchRoute = [&](std::uint32_t route) {
			auto lifecycleConstants = a_lifecycleConstants;
			lifecycleConstants.dispatchRoute = route;
			lifecycleFrameCB->Update(lifecycleConstants);
			ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
			context->CSSetConstantBuffers(3, 1, &lifecycle);
			context->CSSetShader(
				route == kFreeReturnRoute ? freeClaimCS.get() : transitionCS.get(),
				nullptr,
				0);
			context->DispatchIndirect(
				routeDispatchArgs.get(), route * 3U * sizeof(std::uint32_t));
		};
		dispatchRoute(kQuarantineRoute);
		dispatchRoute(kFreeReturnRoute);
		for (std::uint32_t list = 0; list < a_lifecycleConstants.persistentListCount; ++list)
			dispatchRoute(kPersistentRouteBase + list);
		for (std::uint32_t simulationClass = 0; simulationClass < kSimulationClassCount; ++simulationClass)
			dispatchRoute(kDynamicRouteBase + simulationClass);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		ID3D11ShaderResourceView* nullSRVs[3]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		context->CSSetShaderResources(0, 3, nullSRVs);
		persistentParityMask = persistentDestinationParityMask;
		for (std::uint32_t listID = 0; listID < publishedPersistentListCount; ++listID) {
			auto& slot = persistentLists[listID];
			if (slot.rebuildThisFrame)
				slot.currentArena = (persistentParityMask >> listID) & 1U;
			slot.elapsedSeconds = slot.rebuildThisFrame ? 0.0F : slot.elapsedSeconds;
			slot.drainThisFrame = false;
			slot.rebuildThisFrame = false;
		}
	}

	void ParticleRuntime::ConsumeEvents(
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		auto* context = globals::d3d::context;
		lifecycleFrameCB->Update(a_lifecycleConstants);
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		context->CSSetConstantBuffers(3, 1, &lifecycle);
		ID3D11UnorderedAccessView* nullUAVs[8]{};

		const auto consumerCount = registeredEventRouteCount;
		struct ExecutionSlot
		{
			IGPUEventConsumer* consumer{};
			bool registered{};
			bool countOnly{};
		};
		std::array<ExecutionSlot, kEventRouteCount> consumers{};
		for (std::uint32_t route = 0; route < consumerCount; ++route) {
			const auto& source = eventConsumers[route];
			consumers[route] = {
				source.consumer,
				source.registered,
				source.desc.countOnly
			};
		}
		for (std::uint32_t route = 0; route < consumerCount; ++route) {
			const auto& slot = consumers[route];
			if (!slot.registered)
				continue;
			auto routeConstants = a_lifecycleConstants;
			routeConstants.dispatchRoute = route;
			lifecycleFrameCB->Update(routeConstants);
			lifecycle = lifecycleFrameCB->CB();
			if (slot.consumer && slot.consumer->IsReady()) {
				UnbindComputeResources();
				GPUEventConsumerContext consumerContext{};
				consumerContext.events = eventArena->srv.get();
				consumerContext.counters = counters->uav.get();
				consumerContext.dispatchArguments = eventDispatchArgs.get();
				consumerContext.lifecycleConstants = lifecycle;
				slot.consumer->Consume(consumerContext);
				UnbindComputeResources();
			} else if (slot.countOnly) {
				ID3D11ShaderResourceView* srvs[]{ eventArena->srv.get() };
				ID3D11UnorderedAccessView* uavs[8]{};
				uavs[7] = counters->uav.get();
				context->CSSetConstantBuffers(3, 1, &lifecycle);
				context->CSSetShaderResources(0, 1, srvs);
				context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
				context->CSSetShader(countEventConsumerCS.get(), nullptr, 0);
				context->DispatchIndirect(eventDispatchArgs.get(), 0);
				ID3D11ShaderResourceView* nullSRVs[1]{};
				context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
				context->CSSetShaderResources(0, 1, nullSRVs);
			}
		}
	}

	void ParticleRuntime::MergeFreeReturns(
		const FrameConstants& a_constants,
		const LifecycleFrameConstants& a_lifecycleConstants)
	{
		auto* context = globals::d3d::context;
		frameCB->Update(a_constants);
		emitterFrameCB->Update(EmitterFrameConstants{});
		lifecycleFrameCB->Update(a_lifecycleConstants);
		ID3D11Buffer* frame = frameCB->CB();
		ID3D11Buffer* emitter = emitterFrameCB->CB();
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		context->CSSetConstantBuffers(0, 1, &frame);
		context->CSSetConstantBuffers(2, 1, &emitter);
		context->CSSetConstantBuffers(3, 1, &lifecycle);
		ID3D11ShaderResourceView* srvs[]{ routeArena->srv.get(), routeDescriptors->srv.get() };
		context->CSSetShaderResources(0, 2, srvs);
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = freeIDs->uav.get();
		uavs[1] = metadata->uav.get();
		uavs[2] = particleAuxiliaryState->uav.get();
		uavs[3] = emitterStates->uav.get();
		uavs[7] = counters->uav.get();
		context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		context->CSSetShader(freeMergeCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::FreeReturnMerge");
		context->DispatchIndirect(
			routeDispatchArgs.get(), kFreeReturnRoute * 3U * sizeof(std::uint32_t));
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		ID3D11ShaderResourceView* nullSRVs[2]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		context->CSSetShaderResources(0, 2, nullSRVs);
	}

	void ParticleRuntime::Classify(const FrameConstants& a_constants)
	{
		auto* context = globals::d3d::context;
		// Build classification arguments from newly produced dynamic-list counts.
		auto classifyConstants = a_constants;
		classifyConstants.inputCounterBase = a_constants.outputCounterBase;
		frameCB->Update(classifyConstants);
		RenderFrameConstants renderConstants{};
		renderConstants.registeredClassCount = rendererExecutionCount;
		renderFrameCB->Update(renderConstants);
		LifecycleFrameConstants lifecycleConstants{};
		lifecycleConstants.eventCapacity = capacity;
		lifecycleConstants.routeEpoch = routeEpoch;
		lifecycleConstants.persistentDestinationParityMask = persistentParityMask;
		lifecycleConstants.persistentListCount = publishedPersistentListCount;
		lifecycleFrameCB->Update(lifecycleConstants);
		ID3D11Buffer* cb = frameCB->CB();
		ID3D11Buffer* renderCB = renderFrameCB->CB();
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		ID3D11UnorderedAccessView* argUAVs[8]{};
		argUAVs[0] = classifyDispatchArgsUAV.get();
		argUAVs[1] = persistentDispatchArgsUAV.get();
		argUAVs[7] = counters->uav.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetConstantBuffers(3, 1, &lifecycle);
		context->CSSetConstantBuffers(4, 1, &renderCB);
		context->CSSetUnorderedAccessViews(0, 8, argUAVs, nullptr);
		context->CSSetShader(buildSimulationArgsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::BuildIndirectArgs");
		context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullArgUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullArgUAVs, nullptr);

		ID3D11ShaderResourceView* classifySRVs[]{
			dynamicArenas[1U - currentArena]->srv.get(),
			positionAge->srv.get(),
			appearance->srv.get(),
			metadata->srv.get(),
			particleAuxiliaryState->srv.get(),
			renderClassBuffer->srv.get()
		};
		context->CSSetShaderResources(0, 6, classifySRVs);
		ID3D11UnorderedAccessView* classifyUAVs[8]{};
		classifyUAVs[0] = renderIndexArena->uav.get();
		classifyUAVs[7] = counters->uav.get();
		context->CSSetUnorderedAccessViews(0, 8, classifyUAVs, nullptr);
		context->CSSetShader(classifyCS.get(), nullptr, 0);
		ID3D11Buffer* perFrame = *globals::game::perFrame;
		context->CSSetConstantBuffers(12, 1, &perFrame);
		globals::profiler->BeginPass("SSPP::ClassifyCull");
		for (std::uint32_t simulationClass = 0; simulationClass < kSimulationClassCount; ++simulationClass) {
			classifyConstants.dispatchClass = simulationClass;
			frameCB->Update(classifyConstants);
			cb = frameCB->CB();
			context->CSSetConstantBuffers(0, 1, &cb);
			context->DispatchIndirect(
				classifyDispatchArgs.get(), simulationClass * 3U * sizeof(std::uint32_t));
		}
		globals::profiler->EndPass();

		context->CSSetUnorderedAccessViews(0, 8, classifyUAVs, nullptr);
		context->CSSetShader(classifyPersistentCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::ClassifyCull");
		for (std::uint32_t listID = 0; listID < publishedPersistentListCount; ++listID) {
			const auto parity = (persistentParityMask >> listID) & 1U;
			classifyConstants.inputCounterBase = parity != 0 ? kPersistentB : kPersistentA;
			frameCB->Update(classifyConstants);
			cb = frameCB->CB();
			context->CSSetConstantBuffers(0, 1, &cb);
			lifecycleConstants.maintenanceList = listID;
			lifecycleFrameCB->Update(lifecycleConstants);
			lifecycle = lifecycleFrameCB->CB();
			context->CSSetConstantBuffers(3, 1, &lifecycle);
			classifySRVs[0] = persistentArenas[parity]->srv.get();
			context->CSSetShaderResources(0, 6, classifySRVs);
			context->DispatchIndirect(
				persistentDispatchArgs.get(), listID * 3U * sizeof(std::uint32_t));
		}
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		ID3D11ShaderResourceView* nullSRVs[6]{};
		context->CSSetShaderResources(0, 6, nullSRVs);
	}

	void ParticleRuntime::BuildRenderArguments(const FrameConstants& a_constants)
	{
		frameCB->Update(a_constants);
		RenderFrameConstants renderConstants{};
		renderConstants.registeredClassCount = rendererExecutionCount;
		renderFrameCB->Update(renderConstants);
		LifecycleFrameConstants lifecycleConstants{};
		lifecycleConstants.eventCapacity = capacity;
		lifecycleConstants.routeEpoch = routeEpoch;
		lifecycleConstants.persistentDestinationParityMask = persistentParityMask;
		lifecycleConstants.persistentListCount = publishedPersistentListCount;
		lifecycleFrameCB->Update(lifecycleConstants);
		ID3D11Buffer* cb = frameCB->CB();
		ID3D11Buffer* renderCB = renderFrameCB->CB();
		ID3D11Buffer* lifecycle = lifecycleFrameCB->CB();
		ID3D11UnorderedAccessView* uavs[8]{};
		uavs[0] = renderDrawArgsUAV.get();
		uavs[1] = renderPreparationArgsUAV.get();
		uavs[7] = counters->uav.get();
		globals::d3d::context->CSSetConstantBuffers(0, 1, &cb);
		globals::d3d::context->CSSetConstantBuffers(3, 1, &lifecycle);
		globals::d3d::context->CSSetConstantBuffers(4, 1, &renderCB);
		ID3D11ShaderResourceView* renderClassSRV = renderClassBuffer->srv.get();
		globals::d3d::context->CSSetShaderResources(0, 1, &renderClassSRV);
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
		globals::d3d::context->CSSetShader(buildRenderArgsCS.get(), nullptr, 0);
		globals::profiler->BeginPass("SSPP::BuildIndirectArgs");
		globals::d3d::context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		ID3D11ShaderResourceView* nullSRV{};
		globals::d3d::context->CSSetShaderResources(0, 1, &nullSRV);
	}

	void ParticleRuntime::RunFrame(float a_deltaTime)
	{
		if (!ready) {
			EnsureShaders();
			ready = resetCountersCS && resetDynamicCountersCS && buildSimulationArgsCS &&
			        emissionPlanCS && emissionCS && reclassifySimulationProfilesCS &&
			        resetEmitterFrameCS && applyEmitterCommandsCS && updateEmittersCS &&
			        buildEmissionArgsCS && simulationCS && classifyCS && classifyPersistentCS &&
			        buildRenderArgsCS && buildRouteArgsCS && buildFreeRouteArgsCS &&
			        transitionCS && freeClaimCS && freeMergeCS && builtinMaintenanceCS &&
			        resetPersistentDestinationCS && countEventConsumerCS &&
			        debugVS && debugPS;
			if (!ready)
				return;
		}
		if (resetPending.exchange(false, std::memory_order_acq_rel))
			InitializePool();
		SetupRegisteredInitializers();
		RefreshInitializerExecutionPlan();
		SetupRegisteredRenderers();
		RefreshRenderClassConfiguration();
		UploadDirtyConfiguration();
		UploadRouteDescriptors();
		// Snapshot the provider epoch before draining producer work.
		submittedActivityEpoch = CollisionProviderActivityEpoch();
		const auto requestCount = UploadBurstRequests();
		const auto emitterUploads = UploadEmitterCommands();
		CopyBurstRequestsToUnifiedArena(requestCount);
		FrameConstants constants{};
		constants.capacity = capacity;
		constants.listSegmentCapacity = capacity;
		constants.inputCounterBase = CurrentCounterBase();
		constants.outputCounterBase = OutputCounterBase();
		constants.simulationProfileCount = static_cast<std::uint32_t>(simulationProfiles.size());
		constants.particleTemplateCount = static_cast<std::uint32_t>(particleTemplates.size());
		constants.renderClassCount = rendererExecutionCount;
		constants.registeredFeatureCount =
			registeredFeatureCount.load(std::memory_order_acquire);
		constants.deltaTime = std::clamp(a_deltaTime, 0.0F, 1.0F / 20.0F);
		EmitterFrameConstants emitterConstants{};
		emitterConstants.activeEmitterCount = emitterUploads.activeEmitters;
		emitterConstants.cpuRequestCount = requestCount;
		emitterConstants.emitterCommandCount = emitterUploads.commands;
		emitterConstants.emissionProfileCount = static_cast<std::uint32_t>(emissionProfiles.size());
		emitterConstants.deltaTime = constants.deltaTime;
		emitterConstants.planningFeature = constants.registeredFeatureCount == 0 ?
		                                       0U :
		                                       static_cast<std::uint32_t>(
										 frameCounter % constants.registeredFeatureCount);
		for (std::uint32_t index = 0; index < initializerExecutionCount; ++index) {
			const auto& initializer = initializerExecutionPlan[index];
			if (initializer.initializer && initializer.initializer->IsReady())
				emitterConstants.readyInitializerMask |= 1U << initializer.id;
		}
		routeEpoch += 1U;
		if (routeEpoch == 0)
			routeEpoch = 1U;
		LifecycleFrameConstants lifecycleConstants{};
		lifecycleConstants.eventCapacity = capacity;
		lifecycleConstants.routeEpoch = routeEpoch;
		lifecycleConstants.persistentDestinationParityMask = persistentParityMask;
		if (pendingFeatureDrainCount != 0) {
			lifecycleConstants.retiringOwnerFeature = pendingFeatureDrains.front();
			std::move(
				pendingFeatureDrains.begin() + 1,
				pendingFeatureDrains.begin() + pendingFeatureDrainCount,
				pendingFeatureDrains.begin());
			--pendingFeatureDrainCount;
			// Arm persistent lists only in their owner's retirement frame.
			for (auto& list : persistentLists) {
				if (!list.registered ||
					list.desc.ownerFeature != lifecycleConstants.retiringOwnerFeature)
					continue;
				list.drainRequested = true;
			}
		}
		lifecycleConstants.persistentListCount = publishedPersistentListCount;
		for (std::uint32_t listID = 0; listID < publishedPersistentListCount; ++listID) {
			auto& list = persistentLists[listID];
			list.elapsedSeconds += constants.deltaTime;
			const auto interval = std::max(list.desc.maintenanceIntervalSeconds, 0.0F);
			const bool drainRequested = list.drainRequested;
			list.drainRequested = false;
			const bool cadenceDue = constants.deltaTime > 0.0F &&
			                        (interval <= 0.0F || list.elapsedSeconds >= interval);
			const bool due = drainRequested || cadenceDue;
			list.rebuildThisFrame = due;
			list.drainThisFrame = drainRequested;
			if (!due)
				continue;
			const auto destination = 1U - list.currentArena;
			if (destination != 0)
				lifecycleConstants.persistentDestinationParityMask |= 1U << listID;
			else
				lifecycleConstants.persistentDestinationParityMask &= ~(1U << listID);
		}
		persistentDestinationParityMask = lifecycleConstants.persistentDestinationParityMask;
		lifecycleFrameCB->Update(lifecycleConstants);

		ResetTransientCounters(constants, emitterConstants, lifecycleConstants);
		ApplyEmitterCommands(emitterConstants);
		ReclassifyDynamicParticles(constants);
		BuildSimulationArguments(constants);
		MaintainPersistentLists(constants, lifecycleConstants);
		Simulate(constants);
		UpdatePersistentEmitters(constants, emitterConstants);
		PlanAndInitialize(constants, emitterConstants);
		InitializeFeatureNewborns(constants, emitterConstants);
		BuildRouteArguments(constants, lifecycleConstants);
		TransitionRoutes(constants, lifecycleConstants);
		ConsumeEvents(lifecycleConstants);
		// Refresh free-return arguments after route-overflow recovery.
		BuildFreeReturnArgument(constants, lifecycleConstants);
		MergeFreeReturns(constants, lifecycleConstants);
		Classify(constants);
		BuildRenderArguments(constants);
		UnbindComputeResources();
		PrepareAndDrawRenderers();
		currentArena = 1U - currentArena;
		++frameCounter;
		QueueDiagnosticsReadback();
	}

	void ParticleRuntime::PrepareAndDrawRenderers()
	{
		if (!ready || !renderDrawArgs || !renderPreparationArgs || !renderIndexArena)
			return;
		auto* context = globals::d3d::context;
		ID3D11Buffer* perFrame = *globals::game::perFrame;
		ParticleRenderContext renderContext{
			context,
			positionAge->srv.get(),
			velocityLifetime->srv.get(),
			appearance->srv.get(),
			metadata->srv.get(),
			contactData->srv.get(),
			renderIndexArena->srv.get(),
			counters->srv.get(),
			frameCB->CB(),
			perFrame,
			renderPreparationArgs.get(),
			renderDrawArgs.get(),
			0,
			capacity,
			0,
			0,
			0
		};

		for (std::uint32_t index = 0; index < rendererExecutionCount; ++index) {
			const auto& invocation = rendererExecutionPlan[index];
			if (!invocation.enabled || invocation.desc.preparationThreads == 0)
				continue;
			globals::profiler->BeginPass(invocation.desc.preparationProfilerName);
			ID3D11ShaderResourceView* srvs[]{
				renderContext.positionAge,
				renderContext.velocityLifetime,
				renderContext.appearance,
				renderContext.metadata,
				renderContext.contactData,
				renderContext.visibleParticleIDs,
				renderContext.counters
			};
			context->CSSetShaderResources(0, 7, srvs);
			context->CSSetConstantBuffers(0, 1, &renderContext.frameConstants);
			context->CSSetConstantBuffers(12, 1, &renderContext.gamePerFrameConstants);
			renderContext.renderClass = invocation.renderClass;
			renderContext.visibleSegmentBase = invocation.renderClass * capacity;
			renderContext.preparationArgumentOffset =
				invocation.renderClass * 3U * sizeof(std::uint32_t);
			renderContext.drawArgumentOffset =
				invocation.renderClass * sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
			invocation.renderer->Prepare(renderContext);
			globals::profiler->EndPass();
			UnbindComputeResources();
		}

		for (std::uint32_t index = 0; index < rendererExecutionCount; ++index) {
			const auto& invocation = rendererExecutionPlan[index];
			globals::profiler->BeginPass(invocation.desc.drawProfilerName);
			if (invocation.enabled) {
				ID3D11ShaderResourceView* srvs[]{
					renderContext.positionAge,
					renderContext.velocityLifetime,
					renderContext.appearance,
					renderContext.metadata,
					renderContext.contactData,
					renderContext.visibleParticleIDs
				};
				context->VSSetShaderResources(0, 6, srvs);
				context->VSSetConstantBuffers(12, 1, &renderContext.gamePerFrameConstants);
				renderContext.renderClass = invocation.renderClass;
				renderContext.visibleSegmentBase = invocation.renderClass * capacity;
				renderContext.preparationArgumentOffset =
					invocation.renderClass * 3U * sizeof(std::uint32_t);
				renderContext.drawArgumentOffset =
					invocation.renderClass * sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
				invocation.renderer->Draw(renderContext);
			}
			globals::profiler->EndPass();
			ID3D11ShaderResourceView* nullSRVs[6]{};
			context->VSSetShaderResources(0, 6, nullSRVs);
		}
	}

	void ParticleRuntime::DrawBuiltinDebug(const ParticleRenderContext& a_context)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || !globals::deferred || !a_context.context)
			return;
		const auto mainTarget = globals::deferred->forwardRenderTargets[0];
		auto& main = renderer->GetRuntimeData().renderTargets[mainTarget];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		ID3D11RenderTargetView* rtv = main.RTV;
		ID3D11DepthStencilView* dsv = depth.readOnlyViews[0] ? depth.readOnlyViews[0] : depth.views[0];
		if (!rtv)
			return;

		auto* context = a_context.context;
		context->OMSetRenderTargets(1, &rtv, dsv);
		auto resolution = float2{
			static_cast<float>(globals::game::graphicsState->screenWidth),
			static_cast<float>(globals::game::graphicsState->screenHeight)
		};
		resolution = Util::ConvertToDynamic(resolution, true);
		const D3D11_VIEWPORT viewport{ 0.0F, 0.0F, resolution.x, resolution.y, 0.0F, 1.0F };
		context->RSSetViewports(1, &viewport);
		context->RSSetState(debugRasterizerState.get());
		context->OMSetBlendState(debugBlendState.get(), nullptr, 0xFFFFFFFFU);
		context->OMSetDepthStencilState(debugDepthState.get(), 0);
		ID3D11Buffer* nullVB{};
		UINT stride{};
		UINT offset{};
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 1, &nullVB, &stride, &offset);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->VSSetShader(debugVS.get(), nullptr, 0);
		context->PSSetShader(debugPS.get(), nullptr, 0);
		a_context.DrawInstancedIndirect();
	}

	void ParticleRuntime::QueueDiagnosticsReadback()
	{
		if (!counters)
			return;

		// Poll pending queries without forcing a GPU flush.
		for (std::uint32_t slot = 0; slot < kReadbackSlots; ++slot)
			(void)ConsumeDiagnosticsReadback(slot);

		if (frameCounter % kReadbackCadence != 0)
			return;

		std::uint32_t slot = kReadbackSlots;
		for (std::uint32_t offset = 0; offset < kReadbackSlots; ++offset) {
			const auto candidate = (readbackCursor + offset) % kReadbackSlots;
			if (!readbackPending[candidate]) {
				slot = candidate;
				break;
			}
		}
		if (slot == kReadbackSlots)
			return;

		globals::d3d::context->CopyResource(counterReadbacks[slot].get(), counters->resource.get());
		globals::d3d::context->End(readbackQueries[slot].get());
		readbackArena[slot] = currentArena;
		readbackPersistentParity[slot] = persistentParityMask;
		readbackRenderClassCount[slot] = rendererExecutionCount;
		readbackFeatureCount[slot] =
			registeredFeatureCount.load(std::memory_order_acquire);
		readbackFrame[slot] = frameCounter;
		readbackRejectedCPUCommands[slot] = rejectedCPUCommands.load(std::memory_order_relaxed);
		readbackRejectedCPUParticles[slot] = rejectedCPUParticles.load(std::memory_order_relaxed);
		readbackRejectedEmitterCommands[slot] =
			rejectedEmitterCommands.load(std::memory_order_relaxed);
		readbackRejectedEmitterHandles[slot] =
			rejectedEmitterHandles.load(std::memory_order_relaxed);
		readbackActivityEpoch[slot] = submittedActivityEpoch;
		for (FeatureID feature = 0; feature < kMaxParticleFeatures; ++feature) {
			readbackRejectedCPURequestedByFeature[slot][feature] =
				rejectedCPURequestedByFeature[feature].load(std::memory_order_relaxed);
			readbackRejectedCPUDroppedByFeature[slot][feature] =
				rejectedCPUDroppedByFeature[feature].load(std::memory_order_relaxed);
		}
		readbackPending[slot] = true;
		readbackCursor = (readbackCursor + 1U) % kReadbackSlots;
	}

	bool ParticleRuntime::ConsumeDiagnosticsReadback(std::uint32_t a_slot)
	{
		if (!readbackPending[a_slot])
			return true;
		if (globals::d3d::context->GetData(
				readbackQueries[a_slot].get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
			return false;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(globals::d3d::context->Map(
				counterReadbacks[a_slot].get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			return false;
		}
		const auto* values = static_cast<const std::uint32_t*>(mapped.pData);
		DiagnosticsSnapshot snapshot{};
		snapshot.sampledFrame = readbackFrame[a_slot];
		snapshot.capacity = capacity;
		snapshot.freeCount = values[kFreeTop];
		snapshot.allocatedCount = values[kAllocated];
		snapshot.highWaterMark = values[kHighWater];
		const auto dynamicBase = readbackArena[a_slot] == 0 ? kDynamicA : kDynamicB;
		for (std::uint32_t index = 0; index < kSimulationClassCount; ++index) {
			snapshot.activeBySimulationClass[index] = values[dynamicBase + index];
			snapshot.dynamicCount += snapshot.activeBySimulationClass[index];
		}
		for (std::uint32_t index = 0; index < kPersistentListCount; ++index) {
			const auto counterBase = ((readbackPersistentParity[a_slot] >> index) & 1U) != 0 ?
			                             kPersistentB :
			                             kPersistentA;
			snapshot.activeByPersistentList[index] = values[counterBase + index];
			snapshot.persistentCount += snapshot.activeByPersistentList[index];
			snapshot.persistentTransitions[index] = values[kPersistentTransitions + index];
		}
		for (std::uint32_t index = 0; index < kSimulationClassCount; ++index)
			snapshot.dynamicTransitions[index] = values[kDynamicTransitions + index];
		for (std::uint32_t index = 0; index < kRenderClassCount; ++index) {
			snapshot.visibleByRenderClass[index] = values[kRender + index];
			snapshot.visibleCount += snapshot.visibleByRenderClass[index];
		}
		snapshot.registeredFeatures =
			std::min(readbackFeatureCount[a_slot], kMaxParticleFeatures);
		for (FeatureID feature = 0; feature < snapshot.registeredFeatures; ++feature) {
			snapshot.liveByFeature[feature] = values[kFeatureLive + feature];
			snapshot.requestedByFeature[feature] = values[kFeatureRequested + feature] +
			                                       readbackRejectedCPURequestedByFeature[a_slot][feature];
			snapshot.grantedByFeature[feature] = values[kFeatureGranted + feature];
			snapshot.droppedByFeature[feature] = values[kFeatureDropped + feature] +
			                                     readbackRejectedCPUDroppedByFeature[a_slot][feature];
			snapshot.budgetClampedByFeature[feature] =
				values[kFeatureBudgetClamped + feature];
		}
		snapshot.registeredRenderClasses = readbackRenderClassCount[a_slot];
		snapshot.culledCount = values[kFrameCulled];
		snapshot.renderCandidates = values[kRenderCandidates];
		snapshot.frustumCulled = values[kRenderFrustumCulled];
		snapshot.distanceCulled = values[kRenderDistanceCulled];
		snapshot.disabledOrMissingRenderers = values[kRenderDisabledOrMissing];
		snapshot.invalidRenderClasses = values[kRenderInvalidClass];
		snapshot.renderStaleReferences = values[kRenderStaleReferences];
		snapshot.renderBinOverflows = values[kRenderBinOverflows];
		snapshot.burstRequests = values[kBurstRequests] + readbackRejectedCPUCommands[a_slot];
		snapshot.requestedParticles = values[kRequestedParticles] + readbackRejectedCPUParticles[a_slot];
		snapshot.grantedParticles = values[kGrantedParticles];
		snapshot.droppedParticles = values[kDroppedParticles] + readbackRejectedCPUParticles[a_slot];
		snapshot.recycledParticles = values[kRecycledParticles];
		snapshot.invalidProfiles = values[kInvalidProfiles];
		snapshot.listOverflows = values[kListOverflows];
		snapshot.eventOverflows = values[kEventOverflows];
		for (std::uint32_t index = 0; index < std::size(snapshot.collisionQueries); ++index) {
			snapshot.collisionQueries[index] = values[kCollisionQueries + index];
			snapshot.collisionHits[index] = values[kCollisionHits + index];
		}
		for (std::uint32_t index = 0; index < std::size(snapshot.collisionResponses); ++index)
			snapshot.collisionResponses[index] = values[kCollisionResponses + index];
		snapshot.invalidCollisionRules = values[kInvalidCollisionRules];
		snapshot.activeEmitters = values[kFrameActiveEmitterCount];
		snapshot.ownedParticles = values[kFrameOwnedParticleCount];
		snapshot.emitterCreatesApplied = values[kEmitterCreatesApplied];
		snapshot.emitterUpdatesApplied = values[kEmitterUpdatesApplied];
		snapshot.emitterDestroysApplied = values[kEmitterDestroysApplied];
		snapshot.persistentRequests = values[kPersistentRequests];
		snapshot.persistentParticlesRequested = values[kPersistentParticlesRequested];
		snapshot.persistentParticlesGranted = values[kPersistentParticlesGranted];
		snapshot.persistentParticlesDropped = values[kPersistentParticlesDropped];
		snapshot.invalidEmitterCommands = values[kInvalidEmitterCommands];
		snapshot.requestArenaOverflows = values[kRequestArenaOverflows];
		snapshot.emissionChunkOverflows = values[kEmissionChunkOverflows];
		snapshot.emitterBudgetClamps = values[kEmitterBudgetClamps];
		snapshot.ownerGenerationMismatches = values[kOwnerGenerationMismatches];
		snapshot.staleReferences = values[kStaleReferences];
		snapshot.duplicateRoutes = values[kDuplicateRoutes];
		snapshot.invalidDestinations = values[kInvalidDestinations];
		snapshot.quarantinedParticles = values[kQuarantinedParticles];
		snapshot.freeReturns = values[kFreeReturns];
		snapshot.maintenanceRuns = values[kMaintenanceRuns];
		for (std::uint32_t index = 0; index < kEventRouteCount; ++index) {
			snapshot.eventsProducedByRoute[index] = values[kEventsProducedByRoute + index];
			snapshot.eventsConsumedByRoute[index] = values[kEventsConsumedByRoute + index];
			snapshot.eventsProduced += snapshot.eventsProducedByRoute[index];
			snapshot.eventsConsumed += snapshot.eventsConsumedByRoute[index];
		}
		snapshot.rejectedEmitterCommands = readbackRejectedEmitterCommands[a_slot];
		snapshot.rejectedEmitterHandles = readbackRejectedEmitterHandles[a_slot];
		snapshot.valid = true;
		globals::d3d::context->Unmap(counterReadbacks[a_slot].get(), 0);
		// Publish only snapshots newer than the current sampled frame.
		if (!diagnostics.valid || snapshot.sampledFrame > diagnostics.sampledFrame) {
			diagnostics = snapshot;
			std::uint32_t requiredProviders{};
			for (std::uint32_t simulationClass = 0;
				 simulationClass < kSimulationClassCount;
				 ++simulationClass) {
				if (snapshot.activeBySimulationClass[simulationClass] != 0) {
					requiredProviders |= simulationClassCollisionProviderMasks[simulationClass].load(
						std::memory_order_acquire);
				}
			}
			for (PersistentListID id = 0; id < publishedPersistentListCount; ++id) {
				if (snapshot.activeByPersistentList[id] != 0) {
					requiredProviders |=
						persistentListCollisionProviderMasks[id].load(std::memory_order_acquire);
				}
			}
			for (const auto& providerMask : liveEmitterCollisionProviderMasks)
				requiredProviders |= providerMask.load(std::memory_order_acquire);
			ReconcileCollisionProviders(
				static_cast<std::uint32_t>(readbackActivityEpoch[a_slot]),
				requiredProviders);
		}
		readbackPending[a_slot] = false;
		return true;
	}

	void ParticleRuntime::UnbindComputeResources() const
	{
		ID3D11ShaderResourceView* nullSRVs[16]{};
		ID3D11UnorderedAccessView* nullUAVs[8]{};
		globals::d3d::context->CSSetShaderResources(0, 16, nullSRVs);
		globals::d3d::context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
		globals::d3d::context->CSSetShader(nullptr, nullptr, 0);
	}

}
