#pragma once

#include "Buffer.h"
#include "Features/ScreenSpacePhysicalParticles/Core/ParticleRuntime.h"
#include "I18n/I18n.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <winrt/base.h>

// Physical Blood adapts gameplay hit and temporary-effect hooks to the SSPP GPU
// runtime. Hooks read immutable profile snapshots and enqueue compact burst
// requests; they never own particles or issue rendering commands. SSPP expands
// and simulates airborne droplets. Solid and water contacts emit self-contained
// terminal events and release their SSPP slots in the same frame.
//
// The feature owns blood-specific renderers, textures, simulation profiles, and
// terminal storage: settled splats live in a configurable chronological GPU ring
// rendered by Physical Blood, while water contacts become density in a
// camera-centered field.
// The render thread registers the SSPP callbacks, consumes terminal events,
// advects dye, and binds it after Skyrim configures each water draw. Resource
// rebuilds temporarily close burst publication and preserve callback addresses.
struct PhysicalBlood final : Feature
{
public:
	static constexpr std::size_t kMaxBloodMeshProfiles = 64;
	// The camera-centered field uses 8-unit texels across an 8192-unit span.
	static constexpr std::uint32_t kWaterFieldSize = 1024;
	static constexpr float kWaterFieldExtent = 8192.0F;
	static constexpr std::size_t kWaterActivityReadbackCount = 3;
	static constexpr std::int32_t kMinimumOwnedSplatCapacity = 4096;
	static constexpr std::int32_t kDefaultOwnedSplatCapacity = 131072;
	static constexpr std::int32_t kMaximumOwnedSplatCapacity = 524288;
	static constexpr std::uint32_t kOwnedSplatStateWords = 5;

	virtual std::string GetName() override { return "Physical Blood"; }
	virtual std::string GetDisplayName() override { return T("feature.physical_blood.name", "Physical Blood"); }
	virtual std::string GetShortName() override { return "PhysicalBlood"; }
	virtual std::string_view GetShaderDefineName() override { return "PHYSICAL_BLOOD"; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return loaded && a_type == RE::BSShader::Type::Water; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kOther; }

	// Summary: Describes Physical Blood for the feature browser.
	// Args: None.
	// Returns: The localized description and key-feature list.
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.physical_blood.description", "Adds GPU-simulated blood sprays with ballistic motion and temporary ground splats."),
			{ T("feature.physical_blood.key_feature_1", "Hit-vector-aligned angular sprays"),
				T("feature.physical_blood.key_feature_2", "Compute-shader gravity and drag"),
				T("feature.physical_blood.key_feature_3", "Velocity-stretched blood droplets"),
				T("feature.physical_blood.key_feature_4", "Depth-tested temporary ground splats") }
		};
	}

	enum class DistributionMode : std::uint32_t
	{
		kUniform = 0U,
		kNormal = 1U,
		kBiasLow = 2U,
		kBiasHigh = 3U
	};

	struct DistributionRange
	{
		float minimum{};
		float maximum{ 1.0F };
		DistributionMode distribution{ DistributionMode::kUniform };
		bool operator==(const DistributionRange&) const = default;
	};

	struct EmissionSettings
	{
		int particlesPerHit{ 72 };
		DistributionRange initialSpeed{ 360.0F, 1040.0F, DistributionMode::kBiasLow };
		DistributionRange horizontalAngleDegrees{ -15.0F, 15.0F, DistributionMode::kNormal };
		DistributionRange verticalAngleDegrees{ -12.0F, 12.0F, DistributionMode::kNormal };
		DistributionRange particleLifetime{ 1.12F, 2.16F, DistributionMode::kNormal };
		DistributionRange particleSize{ 1.3F, 4.0F, DistributionMode::kBiasLow };
		float splatSizeMultiplier{ 1.5F };
		float opacity{ 0.88F };
		bool operator==(const EmissionSettings&) const = default;
	};

	struct Settings
	{
		bool enabled{ true };
		bool hijackBloodMeshes{ true };
		EmissionSettings defaultEmission{};
		float gravity{ 980.0F };
		float drag{ 0.55F };
		float splatLifetime{ 14.0F };
		std::int32_t maximumSplats{ kDefaultOwnedSplatCapacity };
		float waterUpdateRate{ 30.0F };
		float waterFlowSpeed{ 180.0F };
		float waterDiffusion{ 0.12F };
		float waterDissipation{ 0.08F };
		float waterImpactSizeMultiplier{ 1.0F };
		float waterImpactStrength{ 1.0F };
	};

	// Summary: Initializes the immutable default On Hit and blood-NIF profiles.
	// Args: None.
	// Returns: A feature instance with no allocated GPU resources.
	PhysicalBlood();
	~PhysicalBlood();

	Settings settings{};

	// Summary: Creates blood render state, textures, water fields, and SSPP adapters.
	// Args: None.
	// Returns: Nothing; throws if a required resource cannot be created.
	virtual void SetupResources() override;

	// Summary: Installs emission hooks and the post-water-setup binding callback.
	// Args: None.
	// Returns: Nothing; marks the feature unavailable when SSPP is not loaded.
	virtual void PostPostLoad() override;

	// Summary: Requests render-thread cleanup after a game load transition.
	// Args: None.
	// Returns: Nothing.
	virtual void GameLoaded() override;

	// Summary: Schedules water-density simulation at the configured update rate.
	// Args: None.
	// Returns: Nothing.
	virtual void EarlyPrepass() override;

	// Summary: Applies pending resets and executes scheduled water advection.
	// Args: None.
	// Returns: Nothing.
	virtual void PostComposite() override;

	// Summary: Binds the current blood-density field for an imminent water draw.
	// Args: None.
	// Returns: Nothing.
	void BindWaterShaderResources();

	// Summary: Releases compiled shaders so the next frame recompiles them.
	// Args: None.
	// Returns: Nothing.
	virtual void ClearShaderCache() override;

	// Summary: Draws Physical Blood settings and publishes edited profiles.
	// Args: None.
	// Returns: Nothing.
	virtual void DrawSettings() override;

	// Summary: Validates settings JSON and publishes a complete hook profile snapshot.
	// Args:
	//   a_json: Feature settings object to read.
	// Returns: Nothing.
	virtual void LoadSettings(json& a_json) override;

	// Summary: Serializes global settings and every emission profile.
	// Args:
	//   a_json: Destination feature settings object.
	// Returns: Nothing.
	virtual void SaveSettings(json& a_json) override;

	// Summary: Restores global settings and built-in profiles to defaults.
	// Args: None.
	// Returns: Nothing.
	virtual void RestoreDefaultSettings() override;

private:
	class SharedRenderer;
	class SharedTerminalEventConsumer;

	enum class NifMatchMode : std::uint8_t
	{
		kOnHit,
		kFilename,
		kExactPath
	};

	struct BloodMeshProfile
	{
		std::string modelPath;
		NifMatchMode matchMode{ NifMatchMode::kFilename };
		bool enabled{ true };
		bool suppressOriginal{ true };
		bool builtIn{};
		EmissionSettings emission{};
		bool operator==(const BloodMeshProfile&) const = default;
	};
	using BloodMeshProfileList = std::vector<BloodMeshProfile>;

	struct alignas(16) OwnedSplatGPU
	{
		float3 position{};
		float spawnTime{};

		float3 impactVelocity{};
		float expiryTime{};

		float3 surfaceNormal{ 0.0F, 0.0F, 1.0F };
		float radius{};

		float opacity{ 1.0F };
		float sizeMultiplier{ 1.0F };
		float variation{};
		std::uint32_t serial{};

		float3 ambientIrradiance{};
		float pad0{};

		float3 directionalDiffuse{};
		float pad1{};
	};
	STATIC_ASSERT_ALIGNAS_16(OwnedSplatGPU);
	static_assert(sizeof(OwnedSplatGPU) == 96);

	struct alignas(16) WaterFieldCB
	{
		float2 center{};
		float2 previousCenter{};
		float extent{ kWaterFieldExtent };
		float flowStep{};
		float diffusionBlend{};
		float decayMultiplier{ 1.0F };
		float pad{};
		float flowOffsetX{};
		float flowOffsetY{};
		float flowWidth{};
		float flowHeight{};
		std::uint32_t densityActive{};
		float2 inverseDimensions{};
	};
	STATIC_ASSERT_ALIGNAS_16(WaterFieldCB);
	static_assert(sizeof(WaterFieldCB) == 64);

	struct alignas(16) SharedRenderCB
	{
		std::uint32_t visibleSegmentBase{};
		std::uint32_t particleCapacity{};
		std::uint32_t renderClass{};
		std::uint32_t ownedSplatCapacity{};

		float currentTime{};
		float splatLifetime{};
		std::uint32_t ownedSplatSegment{};
		float pad{};
	};
	STATIC_ASSERT_ALIGNAS_16(SharedRenderCB);
	static_assert(sizeof(SharedRenderCB) == 32);

	struct alignas(16) TerminalEventCB
	{
		float2 center{};
		float extent{ kWaterFieldExtent };
		float impactSizeMultiplier{ 1.0F };

		float impactStrength{ 1.0F };
		float currentTime{};
		float splatLifetime{};
		std::uint32_t splatCapacity{ kDefaultOwnedSplatCapacity };
	};
	STATIC_ASSERT_ALIGNAS_16(TerminalEventCB);
	static_assert(sizeof(TerminalEventCB) == 32);

	struct WaterActivityReadback
	{
		winrt::com_ptr<ID3D11Texture1D> texture;
		std::uint64_t epoch{};
		bool pending{};
	};

	// Hook threads read immutable profiles while the UI atomically publishes replacements.
	std::atomic<std::shared_ptr<const BloodMeshProfileList>> bloodMeshProfiles{
		std::make_shared<const BloodMeshProfileList>()
	};
	std::size_t selectedBloodMeshProfileIndex{};
	std::size_t selectedEmissionDistributionIndex{};
	std::int32_t maximumSplatsDraft{ kDefaultOwnedSplatCapacity };
	std::string pendingBloodMeshPath;
	std::string bloodMeshProfileMessage;
	bool addingBloodMeshProfile{};
	bool bloodMeshProfileMessageIsError{};
	std::atomic_bool hookFeatureEnabled{ true };
	std::atomic_bool hookNifHijackingEnabled{ true };
	std::atomic_bool runtimeResetPending{};
	// A matching zero readback may retire density; newer bursts invalidate it.
	std::atomic<std::uint64_t> waterActivityEpoch{};
	std::atomic<std::uint32_t> nextSeed{ 0xB100D123U };
	bool shaderFailureLogged{};
	bool runtimeEnabledLastFrame{ true };
	float2 waterFieldCenter{};
	float2 previousWaterFieldCenter{};
	float waterUpdateAccumulator{};
	float sharedWaterUpdateDelta{};
	std::uint64_t observedWaterActivityEpoch{};
	bool waterFieldCenterInitialized{};
	bool sharedWaterUpdatePending{};
	bool waterDensityActive{};

	std::unique_ptr<ConstantBuffer> waterFieldCB;
	std::unique_ptr<ConstantBuffer> sharedRenderCB;
	std::unique_ptr<ConstantBuffer> terminalEventCB;
	std::unique_ptr<Buffer> ownedSplats;
	std::unique_ptr<Buffer> ownedSplatState;
	winrt::com_ptr<ID3D11UnorderedAccessView> ownedSplatFrameCounterUAV;
	winrt::com_ptr<ID3D11Buffer> ownedSplatDrawArguments;
	winrt::com_ptr<ID3D11UnorderedAccessView> ownedSplatDrawArgumentsUAV;
	std::uint32_t allocatedOwnedSplatCapacity{};
	std::unique_ptr<SharedRenderer> sharedSplatRenderer;
	std::unique_ptr<SharedRenderer> sharedAirborneRenderer;
	std::unique_ptr<SharedTerminalEventConsumer> sharedTerminalEventConsumer;
	ScreenSpacePhysicalParticles::Core::EventRouteHandle sharedTerminalEventRoute{};
	ScreenSpacePhysicalParticles::Core::SimulationProfileID sharedFlyingProfile{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::ParticleTemplateID sharedFlyingTemplate{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::RenderClassID sharedSplatRenderClass{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::RenderClassID sharedAirborneRenderClass{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::uint32_t sharedOwnerFeature{ ScreenSpacePhysicalParticles::Core::kInvalidRoute };
	// Atomic owner/template pair published only after registration completes.
	std::atomic<std::uint64_t> sharedBurstRouting{
		(static_cast<std::uint64_t>(ScreenSpacePhysicalParticles::Core::kInvalidRoute) << 32U) |
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::atomic_bool sharedEmissionReady{};
	bool sharedRegistrationReady{};
	float sharedConfiguredGravity{};
	float sharedConfiguredDrag{};
	bool sharedConfiguredEnabled{};

	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11SamplerState> bloodAtlasSampler;
	winrt::com_ptr<ID3D11Texture2D> bloodDiffuseTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodDiffuseSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodNormalTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodNormalSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodBlendTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodBlendSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodBlendAltAlphaTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodBlendAltAlphaSRV;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> waterDensityTexture;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> waterDensitySRV;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 2> waterDensityUAV;
	winrt::com_ptr<ID3D11Texture2D> waterSourceTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> waterSourceSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> waterSourceUAV;
	winrt::com_ptr<ID3D11Texture1D> waterActivityTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> waterActivityUAV;
	std::array<WaterActivityReadback, kWaterActivityReadbackCount> waterActivityReadbacks;
	std::uint32_t nextWaterActivityReadback{};
	std::uint32_t nextWaterActivityReadbackToRead{};
	winrt::com_ptr<ID3D11ComputeShader> waterAdvectCS;
	winrt::com_ptr<ID3D11ComputeShader> ownedSplatDrawArgsCS;
	winrt::com_ptr<ID3D11ComputeShader> sharedTerminalEventCS;
	winrt::com_ptr<ID3D11VertexShader> sharedAirborneVS;
	winrt::com_ptr<ID3D11PixelShader> sharedAirbornePS;
	winrt::com_ptr<ID3D11VertexShader> sharedSplatVS;
	winrt::com_ptr<ID3D11PixelShader> sharedSplatPS;
	winrt::com_ptr<ID3D11SamplerState> waterLinearSampler;
	std::uint32_t currentWaterDensity{};

	struct ActorProcessHitHook
	{
		static void thunk(RE::Actor* a_target, RE::HitData& a_hitData);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ParticleEffectSpawnHook
	{
		static RE::BSTempEffectParticle* thunk(
			RE::TESObjectCELL* a_cell,
			float a_lifetime,
			const char* a_modelName,
			const RE::NiMatrix3& a_rotation,
			const RE::NiPoint3& a_position,
			float a_scale,
			std::uint32_t a_flags,
			RE::NiAVObject* a_target);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void UpdateWaterField(float a_deltaTime);
	void PollWaterActivityReadbacks();
	void QueueWaterActivityReadback(std::uint64_t a_epoch);
	void SetWaterDensityActive(bool a_active);
	void EnsureShaders();
	void SetupSharedResources();
	void RecreateOwnedSplatResources(std::uint32_t a_capacity);
	void EnsureSharedShaders();
	void RegisterSharedRuntime();
	void RefreshSharedRuntimeConfiguration();
	[[nodiscard]] bool ConfigureSharedSimulation();
	void InitializeWaterFieldCenter();
	[[nodiscard]] bool QueueSharedBurst(
		const RE::NiPoint3& a_origin,
		const RE::NiPoint3& a_direction,
		float a_strength,
		std::uint32_t a_seed,
		std::uint32_t a_payloadID,
		const EmissionSettings& a_emission);
	[[nodiscard]] bool SharedRendererReady(bool a_splat) const noexcept;
	void PrepareSharedRenderer(
		bool a_splat,
		const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context);
	void DrawSharedRenderer(
		bool a_splat,
		const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context);
	[[nodiscard]] bool SharedTerminalEventConsumerReady() const noexcept;
	void ConsumeSharedTerminalEvents(
		const ScreenSpacePhysicalParticles::Core::GPUEventConsumerContext& a_context);
	void ResetRuntimeState();
	bool QueueResolvedHit(
		const RE::HitData& a_hitData,
		RE::Actor* a_aggressor,
		RE::Actor* a_target,
		const EmissionSettings& a_emission);
	[[nodiscard]] bool HandleEffectSpawn(
		const RE::NiPoint3& a_position,
		const RE::NiPoint3& a_direction,
		std::string_view a_modelName);
	void ResetBloodMeshProfiles(const EmissionSettings& a_emission);
	[[nodiscard]] std::shared_ptr<const BloodMeshProfileList> LoadBloodMeshProfiles() const noexcept;
	void PublishBloodMeshProfiles(BloodMeshProfileList a_profiles);
	[[nodiscard]] static std::string BloodMeshProfileLabel(const BloodMeshProfile& a_profile);
	[[nodiscard]] static bool IsValidNifPath(std::string_view a_normalizedPath);
	[[nodiscard]] static EmissionSettings LoadEmissionSettings(const json& a_json, const EmissionSettings& a_defaults);
	static void SaveEmissionSettings(json& a_json, const EmissionSettings& a_emission);

	[[nodiscard]] static RE::NiPoint3 NormalizeDirection(const RE::NiPoint3& a_value) noexcept;
	std::uint32_t NextRandom() noexcept;
};
