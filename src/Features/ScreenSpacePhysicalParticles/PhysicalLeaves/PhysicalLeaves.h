#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "Features/ScreenSpacePhysicalParticles/Collision/PlayerCollisionProvider.h"
#include "Features/ScreenSpacePhysicalParticles/Core/ParticleRuntime.h"
#include "Features/ScreenSpacePhysicalParticles/ParticleTextureSet.h"

#include <array>
#include <atomic>
#include <unordered_map>
#include <winrt/base.h>

// Adapts persistent leaf fields into SSPP particles. Player capsules wake settled
// leaves into the shared flight simulation. Physical Leaves owns placement,
// sidecars, and feature shaders; SSPP owns allocation, simulation, lifecycle, and
// storage. The former grass-derived emitter path is compile-time disabled.
struct PhysicalLeaves final : Feature
{
public:
	static constexpr std::uint32_t kMaxResidentPatches = 64;
	static constexpr std::uint32_t kMaxPlayerCollisionVolumes =
		ScreenSpacePhysicalParticles::PlayerCollisionProvider::kMaximumVolumes;

	struct Settings
	{
		int leavesPerTile{ 64 };
		float particleSize{ 12.0F };
		float particleLifetime{ 45.0F };
		float gravity{ 140.0F };
		float drag{ 0.35F };
		float bounce{ 0.22F };
		float settleSpeed{ 28.0F };
		float surfaceOffset{ 0.75F };
		float minimumGroundNormalZ{ 0.35F };
		float groundSearchBelow{ 192.0F };
		float groundSearchAbove{ 64.0F };
		float screenXYTolerance{ 96.0F };
		bool playerCollision{ true };
		float capsulePadding{ 4.0F };
		float playerHorizontalImpulse{ 180.0F };
		float playerVerticalImpulse{ 220.0F };
		float playerVelocityInfluence{ 0.65F };
		float playerCollisionRestitution{ 0.25F };
		bool debugWhiteQuads{};
	};

	std::string GetName() override { return "Physical Leaves"; }
	std::string GetDisplayName() override { return T("feature.physical_leaves.name", "Physical Leaves"); }
	std::string GetShortName() override { return "PhysicalLeaves"; }
	std::string_view GetCategory() const override { return FeatureCategories::kOther; }
	// Summary: Returns the settings-menu description and key feature list.
	// Args: None.
	// Returns: Localized summary text and feature bullets.
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	// Summary: Registers the feature with SSPP and creates feature-owned resources.
	// Args: None.
	// Returns: None.
	void SetupResources() override;
	// Summary: Maintains Physical Leaves resources after each composite pass.
	// Args: None.
	// Returns: None.
	void PostComposite() override;
	// Summary: Schedules feature state reset after a game load.
	// Args: None.
	// Returns: None.
	void GameLoaded() override;
	// Summary: Releases feature-owned compiled shaders.
	// Args: None.
	// Returns: None.
	void ClearShaderCache() override;
	// Summary: Draws Physical Leaves settings and applies runtime invalidation flags.
	// Args: None.
	// Returns: None.
	void DrawSettings() override;
	// Summary: Loads and clamps Physical Leaves settings.
	// Args:
	//   a_json: Feature settings object to read.
	// Returns: None.
	void LoadSettings(json& a_json) override;
	// Summary: Writes Physical Leaves settings.
	// Args:
	//   a_json: Feature settings object to replace.
	// Returns: None.
	void SaveSettings(json& a_json) override;
	// Summary: Restores defaults and schedules particle reseeding.
	// Args: None.
	// Returns: None.
	void RestoreDefaultSettings() override;
	PhysicalLeaves();
	~PhysicalLeaves();

	Settings settings{};

private:
	struct ObservedPatch
	{
		RE::NiPoint3 boundsMin{};
		RE::NiPoint3 boundsMax{};
		std::uint32_t species{ 2 };
	};

	struct alignas(16) LeafSidecarGPU
	{
		float3 homePosition{};
		float baseRotation{};
		float3 homeNormal{ 0.0F, 0.0F, 1.0F };
		float angularVelocity{};
		std::uint32_t generation{};
		std::uint32_t state{};
		float2 groundTilt{};
	};
	STATIC_ASSERT_ALIGNAS_16(LeafSidecarGPU);
	static_assert(sizeof(LeafSidecarGPU) == 48);

	using CapsuleGPU = ScreenSpacePhysicalParticles::PlayerCollisionProvider::CapsuleGPU;

	struct alignas(16) RenderCB
	{
		std::uint32_t debugWhiteQuads{};
		std::uint32_t visibleSegmentBase{};
		std::uint32_t particleCapacity{};
		std::uint32_t pad{};
	};
	STATIC_ASSERT_ALIGNAS_16(RenderCB);

	struct alignas(16) SharedMaintenanceCB
	{
		std::uint32_t inputBase{};
		std::uint32_t inputCounterOffset{};
		std::uint32_t keepRoute{};
		std::uint32_t freeRoute{};

		std::uint32_t dynamicRouteBase{};
		std::uint32_t dynamicSimulationClass{};
		std::uint32_t persistentListID{};
		std::uint32_t particleCapacity{};

		std::uint32_t playerCapsuleCount{};
		std::uint32_t playerCollisionEnabled{};
		float capsulePadding{};
		float elapsedSeconds{};

		float3 playerVelocity{};
		float playerVelocityInfluence{};

		float playerHorizontalImpulse{};
		float playerVerticalImpulse{};
		float particleCollisionRadiusScale{ 0.15F };
		float depenetrationOffset{ 0.25F };

		std::array<CapsuleGPU, kMaxPlayerCollisionVolumes> playerCapsules{};
	};
	STATIC_ASSERT_ALIGNAS_16(SharedMaintenanceCB);
	static_assert(sizeof(SharedMaintenanceCB) == 112);

	struct alignas(16) SharedNewbornCB
	{
		std::uint32_t expectedInitializer{
			ScreenSpacePhysicalParticles::Core::kInvalidRoute
		};
		float minimumGroundNormalZ{};
		float groundSearchBelow{};
		float groundSearchAbove{};

		float screenXYTolerance{};
		float surfaceOffset{};
		float2 pad{};
	};
	STATIC_ASSERT_ALIGNAS_16(SharedNewbornCB);
	static_assert(sizeof(SharedNewbornCB) == 32);

	class SharedRenderer;
	class SharedMaintenance;
	class SharedNewbornInitializer;

	// Deprecated grass-derived seeding retained behind
	// PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING.
	void ScanLoadedGrassNeighborhood();
	void ScanLoadedTrees();
	void ConfigureCanopyEmission();
	[[nodiscard]] std::uint32_t QueueGrassRegion(
		std::uint32_t a_sceneKey,
		const RE::NiPoint3& a_boundsMin,
		const RE::NiPoint3& a_boundsMax);
	[[nodiscard]] std::uint64_t MakePatchKey(
		std::uint32_t a_sceneKey,
		std::int32_t a_tileX,
		std::int32_t a_tileY) const noexcept;

	void SetupSharedRuntime();
	void SetupSharedGraphicsResources();
	void ConfigureSharedRuntime();
	void SynchronizeSharedEmitters();
	void RetireSharedEmitters();
	void ResetSharedClientState();
	void ResetSharedRuntimeState();
	void EnsureSharedShaders();
	[[nodiscard]] bool QueueDebugLeafField() noexcept;
	[[nodiscard]] ScreenSpacePhysicalParticles::Core::EmitterDesc BuildSharedEmitterDesc(
		std::uint64_t a_patchKey,
		const ObservedPatch& a_patch) const;
	[[nodiscard]] bool SharedRendererReady() const noexcept;
	[[nodiscard]] bool SharedMaintenanceReady() const noexcept;
	[[nodiscard]] bool SharedNewbornReady() const noexcept;
	void DispatchSharedMaintenance(
		const ScreenSpacePhysicalParticles::Core::ParticleMaintenanceContext& a_context);
	void DispatchSharedNewborn(
		const ScreenSpacePhysicalParticles::Core::NewbornInitializationContext& a_context);
	void DrawSharedParticles(
		const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context);

	std::unordered_map<std::uint64_t, ObservedPatch> observedPatches;
	std::unordered_map<std::uint64_t, ScreenSpacePhysicalParticles::Core::EmitterHandle> sharedEmitters;
	std::atomic_bool resetPending{};
	std::uint64_t emittedGrassBatchCount{};
	std::uint64_t debugLeafFieldSpawnCount{};
	std::uint32_t debugLeafFieldSequence{};
	std::uint32_t lastWorldspaceKey{};
	std::uint32_t lastGrassScanCellKey{};
	std::uint32_t grassScanCountdown{};
	std::uint32_t activeRegionIndex{ 5 };
	bool sharedConfigured{};
	ParticleTextureSet leafTextures;
	std::array<ScreenSpacePhysicalParticles::Core::EmissionProfileID, 5> canopyEmissionProfiles{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::ParticleTemplateID canopyTemplate{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};

	ScreenSpacePhysicalParticles::Core::FeatureID sharedOwnerFeature{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::PersistentListHandle sharedGroundedList{};
	ScreenSpacePhysicalParticles::Core::NewbornInitializerID sharedNewbornInitializerID{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::SimulationProfileID sharedSimulationProfile{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::ParticleTemplateID sharedParticleTemplate{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::ParticleTemplateID sharedDebugParticleTemplate{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::EmissionProfileID sharedEmissionProfile{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::RenderClassID sharedRenderClass{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::unique_ptr<SharedRenderer> sharedRenderer;
	std::unique_ptr<SharedMaintenance> sharedMaintenance;
	std::unique_ptr<SharedNewbornInitializer> sharedNewbornInitializer;
	std::unique_ptr<Buffer> sharedSidecars;
	std::uint32_t sharedSidecarCapacity{};
	std::unique_ptr<ConstantBuffer> sharedMaintenanceCB;
	std::unique_ptr<ConstantBuffer> sharedNewbornCB;
	winrt::com_ptr<ID3D11ComputeShader> sharedMaintenanceCS;
	winrt::com_ptr<ID3D11ComputeShader> sharedNewbornCS;
	winrt::com_ptr<ID3D11VertexShader> sharedVertexShader;
	winrt::com_ptr<ID3D11PixelShader> sharedPixelShader;

	std::unique_ptr<ConstantBuffer> renderCB;
	winrt::com_ptr<ID3D11ShaderResourceView> leafTextureSRV;
	winrt::com_ptr<ID3D11SamplerState> leafSampler;

	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
};
