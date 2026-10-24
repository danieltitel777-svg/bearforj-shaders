#include "PhysicalBlood.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/ScreenSpacePhysicalParticles/ScreenSpacePhysicalParticles.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/TerrainShadows.h"
#include "Features/VolumetricShadows.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>

// This translation unit implements the Physical Blood adapter to SSPP. It
// registers feature ownership, one flying-particle template, a terminal event
// route, and two renderer callbacks. SSPP owns airborne droplets only; collision
// events transfer solid splats and water deposits into Physical Blood resources
// before SSPP releases the source slots. Gameplay threads publish bursts through
// one atomic owner/template word; D3D work remains on the render thread.

namespace
{
	using namespace ScreenSpacePhysicalParticles::Core;
	constexpr std::uint64_t kInvalidSharedBurstRouting =
		(static_cast<std::uint64_t>(kInvalidRoute) << 32U) | kInvalidRoute;

	// Summary: Publishes an owner and particle-template ID as one atomic value.
	// Args:
	//   a_ownerFeature: SSPP feature that owns the burst.
	//   a_particleTemplate: SSPP template used during GPU expansion.
	// Returns: The packed routing word.
	constexpr std::uint64_t PackSharedBurstRouting(
		FeatureID a_ownerFeature,
		ParticleTemplateID a_particleTemplate) noexcept
	{
		return (static_cast<std::uint64_t>(a_ownerFeature) << 32U) | a_particleTemplate;
	}

	// Summary: Converts a validated Physical Blood range into SSPP GPU form.
	// Args:
	//   a_range: User-facing range and distribution mode.
	// Returns: The equivalent SSPP scalar distribution.
	ScalarDistributionGPU MakeSharedDistribution(
		const PhysicalBlood::DistributionRange& a_range)
	{
		auto minimum = std::isfinite(a_range.minimum) ? a_range.minimum : 0.0F;
		auto maximum = std::isfinite(a_range.maximum) ? a_range.maximum : minimum;
		if (minimum > maximum)
			std::swap(minimum, maximum);
		ScalarDistributionGPU result{};
		result.range = { minimum, maximum };
		switch (a_range.distribution) {
		case PhysicalBlood::DistributionMode::kUniform:
			result.type = static_cast<std::uint32_t>(DistributionType::kUniform);
			break;
		case PhysicalBlood::DistributionMode::kNormal:
			result.type = static_cast<std::uint32_t>(DistributionType::kNormal);
			break;
		case PhysicalBlood::DistributionMode::kBiasLow:
			result.type = static_cast<std::uint32_t>(DistributionType::kBiasLow);
			break;
		case PhysicalBlood::DistributionMode::kBiasHigh:
			result.type = static_cast<std::uint32_t>(DistributionType::kBiasHigh);
			break;
		}
		result.shape = 1.0F;
		return result;
	}
}

class PhysicalBlood::SharedRenderer final :
	public ScreenSpacePhysicalParticles::Core::IParticleRenderer
{
public:
	SharedRenderer(PhysicalBlood& a_owner, bool a_splat) :
		owner(a_owner), splat(a_splat)
	{}

	[[nodiscard]] bool IsReady() const noexcept override
	{
		return owner.SharedRendererReady(splat);
	}

	void Prepare(
		const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context) override
	{
		owner.PrepareSharedRenderer(splat, a_context);
	}

	void Draw(
		const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context) override
	{
		owner.DrawSharedRenderer(splat, a_context);
	}

private:
	PhysicalBlood& owner;
	bool splat{};
};

class PhysicalBlood::SharedTerminalEventConsumer final :
	public ScreenSpacePhysicalParticles::Core::IGPUEventConsumer
{
public:
	explicit SharedTerminalEventConsumer(PhysicalBlood& a_owner) :
		owner(a_owner) {}

	[[nodiscard]] bool IsReady() const noexcept override
	{
		return owner.SharedTerminalEventConsumerReady();
	}

	void Consume(
		const ScreenSpacePhysicalParticles::Core::GPUEventConsumerContext& a_context) override
	{
		owner.ConsumeSharedTerminalEvents(a_context);
	}

private:
	PhysicalBlood& owner;
};

PhysicalBlood::PhysicalBlood()
{
	ResetBloodMeshProfiles(settings.defaultEmission);
}

PhysicalBlood::~PhysicalBlood() = default;

// Summary: Normalizes a direction with a stable nonzero fallback.
// Args:
//   a_value: Direction candidate.
// Returns: A unit-length direction.
RE::NiPoint3 PhysicalBlood::NormalizeDirection(const RE::NiPoint3& a_value) noexcept
{
	if (a_value.SqrLength() <= 0.000001F) {
		RE::NiPoint3 fallback{ 0.0F, 1.0F, 0.25F };
		fallback.Unitize();
		return fallback;
	}
	auto normalized = a_value;
	normalized.Unitize();
	return normalized;
}

// Summary: Replaces only the feature-owned splat ring with an empty ring.
// Args:
//   a_capacity: Requested record count, clamped to the supported range.
// Returns: Nothing; existing resources remain alive if creation throws.
void PhysicalBlood::RecreateOwnedSplatResources(std::uint32_t a_capacity)
{
	a_capacity = std::clamp(
		a_capacity,
		static_cast<std::uint32_t>(kMinimumOwnedSplatCapacity),
		static_cast<std::uint32_t>(kMaximumOwnedSplatCapacity));
	auto* device = globals::d3d::device;

	const auto splatDesc = StructuredBufferDesc<OwnedSplatGPU>(a_capacity, true, false);
	auto replacementSplats = std::make_unique<Buffer>(
		splatDesc, nullptr, "PhysicalBlood::OwnedSplats");
	D3D11_SHADER_RESOURCE_VIEW_DESC splatSRV{};
	splatSRV.Format = DXGI_FORMAT_UNKNOWN;
	splatSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	splatSRV.Buffer.NumElements = a_capacity;
	replacementSplats->CreateSRV(splatSRV);
	D3D11_UNORDERED_ACCESS_VIEW_DESC splatUAV{};
	splatUAV.Format = DXGI_FORMAT_UNKNOWN;
	splatUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	splatUAV.Buffer.NumElements = a_capacity;
	replacementSplats->CreateUAV(splatUAV);

	const std::uint32_t initialState[kOwnedSplatStateWords]{};
	D3D11_SUBRESOURCE_DATA stateData{};
	stateData.pSysMem = initialState;
	const auto stateDesc = StructuredBufferDesc<std::uint32_t>(
		kOwnedSplatStateWords, true, false);
	auto replacementState = std::make_unique<Buffer>(
		stateDesc, &stateData, "PhysicalBlood::OwnedSplatState");
	D3D11_SHADER_RESOURCE_VIEW_DESC stateSRV{};
	stateSRV.Format = DXGI_FORMAT_UNKNOWN;
	stateSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	stateSRV.Buffer.NumElements = kOwnedSplatStateWords;
	replacementState->CreateSRV(stateSRV);
	D3D11_UNORDERED_ACCESS_VIEW_DESC stateUAV{};
	stateUAV.Format = DXGI_FORMAT_UNKNOWN;
	stateUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	stateUAV.Buffer.NumElements = kOwnedSplatStateWords;
	replacementState->CreateUAV(stateUAV);
	D3D11_UNORDERED_ACCESS_VIEW_DESC frameCounterUAVDesc = stateUAV;
	frameCounterUAVDesc.Buffer.FirstElement = kOwnedSplatStateWords - 1U;
	frameCounterUAVDesc.Buffer.NumElements = 1U;
	winrt::com_ptr<ID3D11UnorderedAccessView> replacementFrameCounterUAV;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(
		replacementState->resource.get(),
		&frameCounterUAVDesc,
		replacementFrameCounterUAV.put()));
	Util::SetResourceName(
		replacementFrameCounterUAV.get(), "PhysicalBlood::OwnedSplatFrameCounter UAV");

	const D3D11_DRAW_INSTANCED_INDIRECT_ARGS emptyDraws[2]{
		{ 4U, 0U, 0U, 0U }, { 4U, 0U, 0U, 0U }
	};
	D3D11_SUBRESOURCE_DATA drawArgsData{};
	drawArgsData.pSysMem = emptyDraws;
	D3D11_BUFFER_DESC drawArgsDesc{};
	drawArgsDesc.ByteWidth = sizeof(emptyDraws);
	drawArgsDesc.Usage = D3D11_USAGE_DEFAULT;
	drawArgsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	drawArgsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS |
	                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	winrt::com_ptr<ID3D11Buffer> replacementDrawArguments;
	DX::ThrowIfFailed(device->CreateBuffer(
		&drawArgsDesc, &drawArgsData, replacementDrawArguments.put()));
	Util::SetResourceName(
		replacementDrawArguments.get(), "PhysicalBlood::OwnedSplatDrawArguments");
	D3D11_UNORDERED_ACCESS_VIEW_DESC drawArgsUAV{};
	drawArgsUAV.Format = DXGI_FORMAT_R32_TYPELESS;
	drawArgsUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	drawArgsUAV.Buffer.NumElements = drawArgsDesc.ByteWidth / sizeof(std::uint32_t);
	drawArgsUAV.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	winrt::com_ptr<ID3D11UnorderedAccessView> replacementDrawArgumentsUAV;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(
		replacementDrawArguments.get(),
		&drawArgsUAV,
		replacementDrawArgumentsUAV.put()));
	Util::SetResourceName(
		replacementDrawArgumentsUAV.get(), "PhysicalBlood::OwnedSplatDrawArguments UAV");

	ownedSplats = std::move(replacementSplats);
	ownedSplatState = std::move(replacementState);
	ownedSplatFrameCounterUAV = std::move(replacementFrameCounterUAV);
	ownedSplatDrawArguments = std::move(replacementDrawArguments);
	ownedSplatDrawArgumentsUAV = std::move(replacementDrawArgumentsUAV);
	allocatedOwnedSplatCapacity = a_capacity;
}

// Summary: Allocates SSPP-facing buffers and process-stable callback adapters.
// Args: None.
// Returns: Nothing.
void PhysicalBlood::SetupSharedResources()
{
	// Close hook-side publication until registration republishes a complete route.
	sharedBurstRouting.store(kInvalidSharedBurstRouting, std::memory_order_release);
	terminalEventCB = std::make_unique<ConstantBuffer>(
		ConstantBufferDesc<TerminalEventCB>(), "PhysicalBlood::TerminalEventCB");
	sharedRenderCB = std::make_unique<ConstantBuffer>(
		ConstantBufferDesc<SharedRenderCB>(), "PhysicalBlood::SharedRenderCB");
	settings.maximumSplats = std::clamp(
		settings.maximumSplats,
		kMinimumOwnedSplatCapacity,
		kMaximumOwnedSplatCapacity);
	maximumSplatsDraft = settings.maximumSplats;
	RecreateOwnedSplatResources(static_cast<std::uint32_t>(settings.maximumSplats));

	// SSPP plans retain these non-owning callback addresses across rebuilds.
	if (!sharedSplatRenderer)
		sharedSplatRenderer = std::make_unique<SharedRenderer>(*this, true);
	if (!sharedAirborneRenderer)
		sharedAirborneRenderer = std::make_unique<SharedRenderer>(*this, false);
	if (!sharedTerminalEventConsumer)
		sharedTerminalEventConsumer = std::make_unique<SharedTerminalEventConsumer>(*this);
	sharedOwnerFeature = ScreenSpacePhysicalParticles::Core::kInvalidRoute;
	sharedRegistrationReady = false;
}

// Summary: Registers Physical Blood ownership, routes, profiles, and renderers with SSPP.
// Args: None.
// Returns: Nothing; readiness remains false if any registration fails.
void PhysicalBlood::RegisterSharedRuntime()
{
	using namespace ScreenSpacePhysicalParticles::Core;
	auto& runtime = ParticleRuntime::GetSingleton();
	if (!runtime.Ready() || !sharedSplatRenderer || !sharedAirborneRenderer ||
		!sharedTerminalEventConsumer)
		return;

	sharedOwnerFeature = runtime.RegisterFeature("PhysicalBlood");
	if (sharedOwnerFeature == kInvalidRoute)
		return;

	EventConsumerDesc terminalEventDesc{};
	terminalEventDesc.name = "PhysicalBlood.Terminal";
	terminalEventDesc.ownerFeature = sharedOwnerFeature;
	sharedTerminalEventRoute = runtime.RegisterGPUEventConsumer(
		terminalEventDesc, sharedTerminalEventConsumer.get());

	RenderClassDesc splatRendererDesc{};
	splatRendererDesc.name = "Physical Blood splats";
	splatRendererDesc.drawOrder = 100U;
	// A nonzero value schedules the feature-owned ring head update.
	splatRendererDesc.preparationThreads = 1U;
	splatRendererDesc.cullingFlags = 0U;
	splatRendererDesc.preparationProfilerName = "PhysicalBlood::SplatPrepare";
	splatRendererDesc.drawProfilerName = "PhysicalBlood::ParticleRender";
	sharedSplatRenderClass = runtime.RegisterRenderer(
		"PhysicalBlood.BloodSplat", splatRendererDesc, sharedSplatRenderer.get());

	RenderClassDesc airborneRendererDesc{};
	airborneRendererDesc.name = "Physical Blood airborne";
	airborneRendererDesc.drawOrder = 110U;
	airborneRendererDesc.cullRadiusScale = 4.0F;
	airborneRendererDesc.cullingFlags =
		static_cast<std::uint32_t>(RenderCullFlags::kFrustum);
	airborneRendererDesc.preparationProfilerName = "PhysicalBlood::AirbornePrepare";
	airborneRendererDesc.drawProfilerName = "PhysicalBlood::ParticleRender";
	sharedAirborneRenderClass = runtime.RegisterRenderer(
		"PhysicalBlood.BloodAirborne",
		airborneRendererDesc,
		sharedAirborneRenderer.get());

	const auto configurationReady = ConfigureSharedSimulation();

	sharedRegistrationReady = sharedTerminalEventRoute.Valid() && configurationReady &&
	                          sharedFlyingTemplate != kInvalidRoute &&
	                          sharedSplatRenderClass != kInvalidRoute &&
	                          sharedAirborneRenderClass != kInvalidRoute;
	sharedBurstRouting.store(
		sharedRegistrationReady ?
			PackSharedBurstRouting(sharedOwnerFeature, sharedFlyingTemplate) :
			kInvalidSharedBurstRouting,
		std::memory_order_release);
	sharedConfiguredGravity = settings.gravity;
	sharedConfiguredDrag = settings.drag;
	sharedConfiguredEnabled = settings.enabled;
	(void)runtime.SetRendererEnabled(sharedSplatRenderClass, settings.enabled);
	(void)runtime.SetRendererEnabled(sharedAirborneRenderClass, settings.enabled);
}

// Summary: Registers the flying profile, collision responses, and particle template.
// Args: None.
// Returns: True when every required SSPP object was registered.
bool PhysicalBlood::ConfigureSharedSimulation()
{
	using namespace ScreenSpacePhysicalParticles::Core;
	if (!sharedTerminalEventRoute.Valid() ||
		sharedSplatRenderClass == kInvalidRoute || sharedAirborneRenderClass == kInvalidRoute) {
		return false;
	}

	auto& runtime = ParticleRuntime::GetSingleton();
	SimulationProfileGPU profile{};
	profile.acceleration = { 0.0F, 0.0F, -settings.gravity };
	profile.drag = settings.drag;
	profile.collisionMask =
		static_cast<std::uint32_t>(CollisionChannel::kWorldSolid) |
		static_cast<std::uint32_t>(CollisionChannel::kWater);
	profile.queryPolicy =
		static_cast<std::uint32_t>(CollisionQueryPolicy::kPerspectiveDepth) |
		static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField) |
		static_cast<std::uint32_t>(CollisionQueryPolicy::kWaterHeightField);
	profile.simulationClass = static_cast<std::uint32_t>(SimulationClass::kWorldWater);
	sharedFlyingProfile = runtime.RegisterSimulationProfile("PhysicalBlood.Flying", profile);
	if (sharedFlyingProfile == kInvalidRoute)
		return false;

	CollisionRuleGPU solidRule{};
	solidRule.response = static_cast<std::uint32_t>(CollisionResponse::kEmitEventAndKill);
	solidRule.eventRoute = sharedTerminalEventRoute.index;
	solidRule.eventPayloadID = kInvalidRoute;
	solidRule.minimumParticleAge = 0.12F;
	solidRule.collisionRadiusScale = 0.0F;
	solidRule.separationOffset = 0.15F;
	runtime.SetCollisionRule(sharedFlyingProfile, CollisionChannel::kWorldSolid, solidRule);

	CollisionRuleGPU waterRule{};
	waterRule.response = static_cast<std::uint32_t>(CollisionResponse::kEmitEventAndKill);
	waterRule.eventRoute = sharedTerminalEventRoute.index;
	waterRule.eventPayloadID = kInvalidRoute;
	waterRule.collisionRadiusScale = 0.0F;
	runtime.SetCollisionRule(sharedFlyingProfile, CollisionChannel::kWater, waterRule);

	ParticleTemplateGPU particleTemplate{};
	particleTemplate.simulationProfile = sharedFlyingProfile;
	particleTemplate.renderClass = sharedAirborneRenderClass;
	particleTemplate.lifecyclePhase = static_cast<std::uint32_t>(LifecyclePhase::kDynamic);
	particleTemplate.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
	sharedFlyingTemplate = runtime.RegisterParticleTemplate(
		"PhysicalBlood.Flying", particleTemplate);
	return sharedFlyingTemplate != kInvalidRoute;
}

// Summary: Applies changed enable, gravity, drag, and splat-lifetime settings to SSPP.
// Args: None.
// Returns: Nothing.
void PhysicalBlood::RefreshSharedRuntimeConfiguration()
{
	using namespace ScreenSpacePhysicalParticles::Core;
	auto& runtime = ParticleRuntime::GetSingleton();
	if (!sharedRegistrationReady) {
		RegisterSharedRuntime();
		return;
	}
	if (sharedConfiguredEnabled != settings.enabled) {
		(void)runtime.SetRendererEnabled(sharedSplatRenderClass, settings.enabled);
		(void)runtime.SetRendererEnabled(sharedAirborneRenderClass, settings.enabled);
		sharedConfiguredEnabled = settings.enabled;
	}
	if (sharedConfiguredGravity == settings.gravity &&
		sharedConfiguredDrag == settings.drag)
		return;

	if (!ConfigureSharedSimulation()) {
		sharedRegistrationReady = false;
		sharedBurstRouting.store(kInvalidSharedBurstRouting, std::memory_order_release);
		return;
	}
	sharedBurstRouting.store(
		PackSharedBurstRouting(sharedOwnerFeature, sharedFlyingTemplate),
		std::memory_order_release);

	sharedConfiguredGravity = settings.gravity;
	sharedConfiguredDrag = settings.drag;
}

// Summary: Initializes the dye field center on an integer texel boundary.
// Args: None.
// Returns: Nothing.
void PhysicalBlood::InitializeWaterFieldCenter()
{
	if (waterFieldCenterInitialized)
		return;
	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	constexpr float texelSize =
		kWaterFieldExtent / static_cast<float>(kWaterFieldSize);
	waterFieldCenter = {
		std::round(camera.x / texelSize) * texelSize,
		std::round(camera.y / texelSize) * texelSize
	};
	previousWaterFieldCenter = waterFieldCenter;
	waterFieldCenterInitialized = true;
}

// Summary: Lazily compiles airborne, terminal-transfer, and owned-splat shaders.
// Args: None.
// Returns: Nothing; shader compilation failures propagate.
void PhysicalBlood::EnsureSharedShaders()
{
	std::vector<std::pair<const char*, const char*>> lightingDefines;
	if (globals::features::volumetricShadows.loaded)
		lightingDefines.emplace_back("VOLUMETRIC_SHADOWS", "");
	if (globals::features::terrainShadows.loaded)
		lightingDefines.emplace_back("TERRAIN_SHADOWS", "");
	if (globals::features::cloudShadows.loaded)
		lightingDefines.emplace_back("CLOUD_SHADOWS", "");

	if (!ownedSplatDrawArgsCS) {
		ownedSplatDrawArgsCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalBlood\\PhysicalBloodSplatStorageCS.hlsl",
			{},
			"cs_5_0",
			"BuildDrawArgumentsMain")));
	}
	if (!sharedTerminalEventCS) {
		sharedTerminalEventCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalBlood\\PhysicalBloodWaterEventCS.hlsl",
			lightingDefines,
			"cs_5_0",
			"ConsumeTerminalEventsMain")));
	}
	if (!sharedAirborneVS) {
		const std::vector<std::pair<const char*, const char*>> defines{
			{ "VSHADER", "" }, { "AIRBORNE_PASS", "" }
		};
		sharedAirborneVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalBlood\\PhysicalBloodShared.hlsl",
			defines,
			"vs_5_0",
			"VSMain")));
	}
	if (!sharedSplatVS) {
		const std::vector<std::pair<const char*, const char*>> defines{
			{ "VSHADER", "" }, { "SPLAT_PASS", "" }, { "OWNED_SPLAT_STORAGE", "" }
		};
		sharedSplatVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalBlood\\PhysicalBloodShared.hlsl",
			defines,
			"vs_5_0",
			"VSMain")));
	}
	if (!sharedAirbornePS) {
		auto defines = lightingDefines;
		defines.emplace_back("PSHADER", "");
		defines.emplace_back("AIRBORNE_PASS", "");
		sharedAirbornePS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalBlood\\PhysicalBloodShared.hlsl",
			defines,
			"ps_5_0",
			"PSMain")));
	}
	if (!sharedSplatPS) {
		std::vector<std::pair<const char*, const char*>> defines{
			{ "PSHADER", "" }, { "SPLAT_PASS", "" }, { "OWNED_SPLAT_STORAGE", "" }
		};
		if (globals::features::screenSpaceShadows.loaded)
			defines.emplace_back("SCREEN_SPACE_SHADOWS", "");
		sharedSplatPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalBlood\\PhysicalBloodShared.hlsl",
			defines,
			"ps_5_0",
			"PSMain")));
	}
}

// Summary: Converts an emission profile into one thread-safe SSPP burst request.
// Args:
//   a_origin: World-space burst origin.
//   a_direction: Desired central trajectory.
//   a_strength: Multiplier applied to particle count and initial speed.
//   a_seed: Random seed consumed by GPU burst expansion.
//   a_payloadID: Source profile identifier forwarded with particle events.
//   a_emission: Distribution and appearance settings for the burst.
// Returns: True when SSPP accepted the request.
bool PhysicalBlood::QueueSharedBurst(
	const RE::NiPoint3& a_origin,
	const RE::NiPoint3& a_direction,
	float a_strength,
	std::uint32_t a_seed,
	std::uint32_t a_payloadID,
	const EmissionSettings& a_emission)
{
	using namespace ScreenSpacePhysicalParticles::Core;
	if (!sharedEmissionReady.load(std::memory_order_acquire))
		return false;
	auto& runtime = ParticleRuntime::GetSingleton();
	const auto routing = sharedBurstRouting.load(std::memory_order_acquire);
	const auto ownerFeature = static_cast<FeatureID>(routing >> 32U);
	const auto particleTemplate = static_cast<ParticleTemplateID>(routing);
	if (ownerFeature == kInvalidRoute || particleTemplate == kInvalidRoute)
		return false;
	const auto amountScale = globals::features::screenSpacePhysicalParticles.ParticleAmountScale();
	if (amountScale <= 0.0F)
		return false;

	const auto forward = NormalizeDirection(a_direction);
	const RE::NiPoint3 referenceUp = std::abs(forward.z) > 0.98F ?
	                                     RE::NiPoint3{ 0.0F, 1.0F, 0.0F } :
	                                     RE::NiPoint3{ 0.0F, 0.0F, 1.0F };
	const auto right = forward.UnitCross(referenceUp);
	const auto vertical = right.UnitCross(forward);

	BurstRequestGPU request{};
	request.origin = { a_origin.x, a_origin.y, a_origin.z };
	request.strength = std::max(a_strength, 0.0F);
	request.forward = { forward.x, forward.y, forward.z };
	request.seed = a_seed;
	request.right = { right.x, right.y, right.z };
	request.count = static_cast<std::uint32_t>(std::clamp(
		static_cast<int>(a_emission.particlesPerHit * request.strength * amountScale),
		1,
		static_cast<int>(ParticleRuntime::kMaximumCapacity)));
	request.vertical = { vertical.x, vertical.y, vertical.z };
	request.particleTemplate = particleTemplate;
	request.initialSpeed = MakeSharedDistribution(a_emission.initialSpeed);
	request.horizontalAngleDegrees =
		MakeSharedDistribution(a_emission.horizontalAngleDegrees);
	request.verticalAngleDegrees =
		MakeSharedDistribution(a_emission.verticalAngleDegrees);
	request.lifetime = MakeSharedDistribution(a_emission.particleLifetime);
	request.radius = MakeSharedDistribution(a_emission.particleSize);
	request.opacity.range = {
		std::clamp(a_emission.opacity, 0.0F, 1.0F),
		std::clamp(a_emission.opacity, 0.0F, 1.0F)
	};
	request.opacity.type = static_cast<std::uint32_t>(DistributionType::kConstant);
	request.opacity.shape = 1.0F;
	request.generalScalar = std::max(a_emission.splatSizeMultiplier, 0.0F);
	request.payloadID = a_payloadID;
	request.ownerFeature = ownerFeature;
	request.trajectoryPolicy = static_cast<std::uint32_t>(
		EmissionTrajectoryPolicy::kPercentileHeavyTailCoupled);
	request.spawnShape = static_cast<std::uint32_t>(EmitterShape::kPoint);
	request.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
	if (!runtime.QueueBurst(request))
		return false;
	waterActivityEpoch.fetch_add(1, std::memory_order_release);
	return true;
}

// Summary: Checks resources needed by the airborne or splat renderer.
// Args:
//   a_splat: Selects the persistent-splat renderer when true.
// Returns: True when the selected renderer can execute.
bool PhysicalBlood::SharedRendererReady(bool a_splat) const noexcept
{
	if (!sharedRegistrationReady || !bloodDiffuseSRV || !bloodNormalSRV ||
		!bloodBlendSRV || !bloodBlendAltAlphaSRV || !blendState || !depthState ||
		!rasterizerState || !bloodAtlasSampler)
		return false;
	if (a_splat)
		return ownedSplatDrawArgsCS && sharedSplatVS && sharedSplatPS &&
		       ownedSplats && ownedSplatState && ownedSplatState->srv &&
		       ownedSplatFrameCounterUAV && ownedSplatDrawArguments &&
		       ownedSplatDrawArgumentsUAV && sharedRenderCB;
	return sharedAirborneVS && sharedAirbornePS && sharedRenderCB;
}

// Summary: Retires the chronological splat-ring head and builds two wrapped draws.
// Args:
//   a_splat: Selects the feature-owned splat renderer when true.
//   a_context: SSPP render-stage context used for command submission.
// Returns: Nothing.
void PhysicalBlood::PrepareSharedRenderer(
	bool a_splat,
	const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context)
{
	if (!a_splat || !SharedRendererReady(true) || !a_context.context)
		return;

	SharedRenderCB constants{};
	constants.ownedSplatCapacity = allocatedOwnedSplatCapacity;
	constants.currentTime = globals::state ? globals::state->timer : 0.0F;
	constants.splatLifetime = std::max(settings.splatLifetime, 0.001F);
	sharedRenderCB->Update(constants);
	ID3D11Buffer* renderCB = sharedRenderCB->CB();
	a_context.context->CSSetConstantBuffers(1, 1, &renderCB);
	ID3D11ShaderResourceView* splats = ownedSplats->srv.get();
	ID3D11UnorderedAccessView* outputs[]{
		ownedSplatState->uav.get(), ownedSplatDrawArgumentsUAV.get()
	};
	a_context.context->CSSetShaderResources(0, 1, &splats);
	a_context.context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
	a_context.context->CSSetShader(ownedSplatDrawArgsCS.get(), nullptr, 0);
	a_context.context->Dispatch(1, 1, 1);
	ID3D11ShaderResourceView* nullSRV{};
	ID3D11UnorderedAccessView* nullUAVs[2]{};
	a_context.context->CSSetShaderResources(0, 1, &nullSRV);
	a_context.context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
}

// Summary: Draws one SSPP-visible segment as airborne droplets or settled splats.
// Args:
//   a_splat: Selects splat shaders when true and airborne shaders otherwise.
//   a_context: SSPP render context and indirect draw command.
// Returns: Nothing.
void PhysicalBlood::DrawSharedRenderer(
	bool a_splat,
	const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context)
{
	if (!SharedRendererReady(a_splat) || !a_context.context || !globals::game::renderer ||
		!globals::deferred)
		return;

	auto* context = a_context.context;
	auto* renderer = globals::game::renderer;
	const auto mainTarget = globals::deferred->forwardRenderTargets[0];
	auto& main = renderer->GetRuntimeData().renderTargets[mainTarget];
	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ID3D11RenderTargetView* rtv = main.RTV;
	ID3D11DepthStencilView* dsv =
		depth.readOnlyViews[0] ? depth.readOnlyViews[0] : depth.views[0];
	if (!rtv)
		return;
	context->OMSetRenderTargets(1, &rtv, dsv);

	auto resolution = float2{
		static_cast<float>(globals::game::graphicsState->screenWidth),
		static_cast<float>(globals::game::graphicsState->screenHeight)
	};
	resolution = Util::ConvertToDynamic(resolution, true);
	const D3D11_VIEWPORT viewport{
		0.0F, 0.0F, resolution.x, resolution.y, 0.0F, 1.0F
	};
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterizerState.get());
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFFU);
	context->OMSetDepthStencilState(depthState.get(), 0);
	ID3D11Buffer* nullVB{};
	UINT stride{};
	UINT offset{};
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 1, &nullVB, &stride, &offset);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	SharedRenderCB constants{};
	constants.visibleSegmentBase = a_context.visibleSegmentBase;
	constants.particleCapacity = a_context.particleCapacity;
	constants.renderClass = a_context.renderClass;
	constants.ownedSplatCapacity = allocatedOwnedSplatCapacity;
	constants.currentTime = globals::state ? globals::state->timer : 0.0F;
	constants.splatLifetime = std::max(settings.splatLifetime, 0.001F);
	sharedRenderCB->Update(constants);
	ID3D11Buffer* renderCB = sharedRenderCB->CB();
	context->VSSetConstantBuffers(1, 1, &renderCB);
	context->VSSetConstantBuffers(12, 1, &a_context.gamePerFrameConstants);
	context->PSSetConstantBuffers(12, 1, &a_context.gamePerFrameConstants);
	ID3D11Buffer* sharedDataBuffers[]{
		globals::state->sharedDataCB ? globals::state->sharedDataCB->CB() : nullptr,
		globals::state->featureDataCB ? globals::state->featureDataCB->CB() : nullptr
	};
	context->PSSetConstantBuffers(5, 2, sharedDataBuffers);
	ID3D11SamplerState* samplers[]{
		waterLinearSampler.get(), bloodAtlasSampler.get()
	};
	context->PSSetSamplers(0, static_cast<UINT>(std::size(samplers)), samplers);
	ID3D11ShaderResourceView* bloodTextures[]{
		bloodDiffuseSRV.get(),
		bloodNormalSRV.get(),
		bloodBlendSRV.get(),
		bloodBlendAltAlphaSRV.get()
	};
	context->PSSetShaderResources(
		1, static_cast<UINT>(std::size(bloodTextures)), bloodTextures);

	if (a_splat && globals::features::screenSpaceShadows.loaded) {
		auto* texture = globals::features::screenSpaceShadows.screenSpaceShadowsTexture;
		ID3D11ShaderResourceView* contactShadows = texture ? texture->srv.get() : nullptr;
		context->PSSetShaderResources(45, 1, &contactShadows);
	} else if (!a_splat) {
		if (globals::features::volumetricShadows.loaded) {
			auto& shadows = globals::features::volumetricShadows;
			ID3D11ShaderResourceView* shadowMap =
				shadows.shadowCopySRV ? shadows.shadowCopySRV : shadows.shadowView;
			context->PSSetShaderResources(18, 1, &shadowMap);
			ID3D11ShaderResourceView* shadowTransforms =
				globals::deferred->directionalShadowLights ?
					globals::deferred->directionalShadowLights->srv.get() :
					nullptr;
			context->PSSetShaderResources(98, 1, &shadowTransforms);
		}
		if (globals::features::terrainShadows.loaded) {
			auto& texture = globals::features::terrainShadows.texShadowHeight;
			ID3D11ShaderResourceView* terrainShadows = texture ? texture->srv.get() : nullptr;
			context->PSSetShaderResources(60, 1, &terrainShadows);
		}
		if (globals::features::cloudShadows.loaded) {
			auto* texture = globals::features::cloudShadows.texCubemapCloudOccCopy;
			ID3D11ShaderResourceView* cloudShadows = texture ? texture->srv.get() : nullptr;
			context->PSSetShaderResources(25, 1, &cloudShadows);
		}
	}

	if (a_splat) {
		ID3D11ShaderResourceView* splatResources[]{
			ownedSplats->srv.get(), ownedSplatState->srv.get()
		};
		context->VSSetShaderResources(
			0, static_cast<UINT>(std::size(splatResources)), splatResources);
		context->VSSetShader(sharedSplatVS.get(), nullptr, 0);
		context->PSSetShader(sharedSplatPS.get(), nullptr, 0);
		context->DrawInstancedIndirect(ownedSplatDrawArguments.get(), 0);
		constants.ownedSplatSegment = 1U;
		sharedRenderCB->Update(constants);
		context->VSSetConstantBuffers(1, 1, &renderCB);
		context->DrawInstancedIndirect(
			ownedSplatDrawArguments.get(), sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS));
	} else {
		context->VSSetShader(sharedAirborneVS.get(), nullptr, 0);
		context->PSSetShader(sharedAirbornePS.get(), nullptr, 0);
		a_context.DrawInstancedIndirect();
	}

	if (a_splat) {
		ID3D11ShaderResourceView* nullSRVs[2]{};
		context->VSSetShaderResources(0, 2, nullSRVs);
	}
	ID3D11ShaderResourceView* nullBloodTextures[std::size(bloodTextures)]{};
	context->PSSetShaderResources(
		1, static_cast<UINT>(std::size(nullBloodTextures)), nullBloodTextures);
}

// Summary: Checks resources required to own terminal splats and water deposits.
// Args: None.
// Returns: True when the GPU terminal-event consumer can execute.
bool PhysicalBlood::SharedTerminalEventConsumerReady() const noexcept
{
	return sharedRegistrationReady && sharedTerminalEventCS && waterSourceUAV &&
	       waterActivityUAV && terminalEventCB && ownedSplats && ownedSplatState &&
	       ownedSplatFrameCounterUAV && waterLinearSampler &&
	       waterFieldCenterInitialized;
}

// Summary: Transfers solid contacts to the splat ring and water contacts to dye.
// Args:
//   a_context: Event buffers, counters, and indirect dispatch arguments from SSPP.
// Returns: Nothing.
void PhysicalBlood::ConsumeSharedTerminalEvents(
	const ScreenSpacePhysicalParticles::Core::GPUEventConsumerContext& a_context)
{
	if (!SharedTerminalEventConsumerReady() || !a_context.events || !a_context.counters ||
		!a_context.dispatchArguments)
		return;

	TerminalEventCB constants{};
	constants.center = waterFieldCenter;
	constants.extent = kWaterFieldExtent;
	constants.impactSizeMultiplier = settings.waterImpactSizeMultiplier;
	constants.impactStrength = settings.waterImpactStrength;
	constants.currentTime = globals::state ? globals::state->timer : 0.0F;
	constants.splatLifetime = std::max(settings.splatLifetime, 0.001F);
	constants.splatCapacity = allocatedOwnedSplatCapacity;
	terminalEventCB->Update(constants);

	ID3D11Buffer* lifecycle = a_context.lifecycleConstants;
	ID3D11Buffer* terminal = terminalEventCB->CB();
	auto* context = globals::d3d::context;
	context->CSSetConstantBuffers(3, 1, &lifecycle);
	context->CSSetConstantBuffers(7, 1, &terminal);
	ID3D11Buffer* sharedDataBuffers[]{
		globals::state->sharedDataCB ? globals::state->sharedDataCB->CB() : nullptr,
		globals::state->featureDataCB ? globals::state->featureDataCB->CB() : nullptr
	};
	context->CSSetConstantBuffers(5, 2, sharedDataBuffers);
	ID3D11SamplerState* sampler = waterLinearSampler.get();
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetShaderResources(0, 1, &a_context.events);
	if (globals::features::volumetricShadows.loaded) {
		auto& shadows = globals::features::volumetricShadows;
		ID3D11ShaderResourceView* shadowMap =
			shadows.shadowCopySRV ? shadows.shadowCopySRV : shadows.shadowView;
		context->CSSetShaderResources(18, 1, &shadowMap);
		ID3D11ShaderResourceView* shadowTransforms =
			globals::deferred->directionalShadowLights ?
				globals::deferred->directionalShadowLights->srv.get() :
				nullptr;
		context->CSSetShaderResources(98, 1, &shadowTransforms);
	}
	if (globals::features::terrainShadows.loaded) {
		auto& texture = globals::features::terrainShadows.texShadowHeight;
		ID3D11ShaderResourceView* terrainShadows = texture ? texture->srv.get() : nullptr;
		context->CSSetShaderResources(60, 1, &terrainShadows);
	}
	if (globals::features::cloudShadows.loaded) {
		auto* texture = globals::features::cloudShadows.texCubemapCloudOccCopy;
		ID3D11ShaderResourceView* cloudShadows = texture ? texture->srv.get() : nullptr;
		context->CSSetShaderResources(25, 1, &cloudShadows);
	}
	ID3D11UnorderedAccessView* uavs[8]{};
	uavs[0] = waterSourceUAV.get();
	uavs[1] = waterActivityUAV.get();
	uavs[2] = ownedSplats->uav.get();
	uavs[3] = ownedSplatState->uav.get();
	uavs[7] = a_context.counters;
	const std::uint32_t clear[4]{};
	context->ClearUnorderedAccessViewUint(ownedSplatFrameCounterUAV.get(), clear);
	context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
	context->CSSetShader(sharedTerminalEventCS.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalBlood::TerminalTransfer");
	context->DispatchIndirect(
		a_context.dispatchArguments,
		a_context.dispatchArgumentOffset);
	globals::profiler->EndPass();
	ID3D11UnorderedAccessView* nullUAVs[8]{};
	context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	QueueWaterActivityReadback(waterActivityEpoch.load(std::memory_order_acquire));
}
