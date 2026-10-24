#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "Features/ScreenSpacePhysicalParticles/Core/ParticleRuntime.h"
#include "Features/ScreenSpacePhysicalParticles/ParticleTextureSet.h"

#include <array>
#include <atomic>
#include <unordered_map>
#include <winrt/base.h>

struct PhysicalWorldParticles final : Feature
{
	static constexpr std::uint32_t kKindCount = 6;

	std::string GetName() override { return "Physical World Particles"; }
	std::string GetDisplayName() override
	{
		return T("feature.physical_world_particles.name", "Physical World Particles");
	}
	std::string GetShortName() override { return "PhysicalWorldParticles"; }
	std::string_view GetCategory() const override { return FeatureCategories::kOther; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	void PostPostLoad() override;
	void SetupResources() override;
	void PostComposite() override;
	void ClearShaderCache() override;
	void DrawSettings() override;
	PhysicalWorldParticles();
	~PhysicalWorldParticles();

	// Summary: Queues spell-colored sparks when a magic projectile hits anything.
	// Args:
	//   a_projectile: Impacting missile, flame, beam, cone, or grenade.
	//   a_position: World-space impact position.
	// Returns: None.
	void OnMagicImpact(RE::Projectile* a_projectile, const RE::NiPoint3& a_position);

private:
	enum class Kind : std::uint32_t
	{
		kMagic,
		kFluff,
		kDandelion,
		kSpores,
		kFireflies,
		kMoths
	};

	class SharedRenderer;

	struct alignas(16) RenderCB
	{
		std::uint32_t mode{};
		std::uint32_t visibleSegmentBase{};
		std::uint32_t particleCapacity{};
		std::uint32_t pad{};
	};

	[[nodiscard]] std::uint32_t KindFromClass(std::uint32_t a_renderClass) const noexcept;
	void RegisterRuntime();
	void EnsureGraphics();
	void SpawnHandSparks(float a_deltaTime);
	void SpawnAmbient();
	[[nodiscard]] bool QueueKind(
		Kind a_kind,
		const RE::NiPoint3& a_origin,
		const RE::NiPoint3& a_forward,
		std::uint32_t a_count,
		float a_generalScalar,
		float a_radiusScale = 1.0F);
	[[nodiscard]] std::uint32_t TakeBudget(std::uint32_t a_count);

	std::array<ScreenSpacePhysicalParticles::ParticleTextureSet, kKindCount> textures;
	std::array<ScreenSpacePhysicalParticles::Core::RenderClassID, kKindCount> renderClasses{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::array<ScreenSpacePhysicalParticles::Core::ParticleTemplateID, kKindCount> templates{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute,
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::array<ScreenSpacePhysicalParticles::Core::EmissionProfileGPU, kKindCount> emissions{};
	ScreenSpacePhysicalParticles::Core::FeatureID owner{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	ScreenSpacePhysicalParticles::Core::PersistentListHandle magicRest;
	std::unique_ptr<SharedRenderer> renderer;
	std::unique_ptr<ConstantBuffer> renderCB;
	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> pixelShader;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	winrt::com_ptr<ID3D11BlendState> alphaBlend;
	winrt::com_ptr<ID3D11BlendState> additiveBlend;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	std::atomic<std::uint32_t> magicTemplate{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::atomic<std::uint32_t> magicOwner{
		ScreenSpacePhysicalParticles::Core::kInvalidRoute
	};
	std::atomic<std::uint32_t> spawnBudget{ 48 };
	std::unordered_map<std::uint32_t, std::pair<float, RE::NiPoint3>> impactThrottle;
	float handAccumulator[2]{};
	float ambientTimer{};
	std::uint32_t randomState{ 0xC0FFEEU };
	bool registered{};
};
