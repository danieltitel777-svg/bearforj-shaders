#pragma once

#include "Feature.h"

#include <unordered_map>

// Coordinates the shared SSPP runtime and collision providers. Particle features
// register their own profiles, renderers, initializers, persistent lists, and event
// consumers with ParticleRuntime; this feature owns only shared capacity, capture
// cadence, diagnostics, and frame sequencing.
//
// Frame order:
//   1. EarlyPrepass requests perspective depth when a live profile needs it.
//   2. PostComposite finalizes captures and prepares height/capsule providers.
//   3. ParticleRuntime emits, simulates, routes, classifies, and renders particles.
//   4. Enabled debug views are converted for ImGui.
//
// Feature authors should keep a particle in SSPP only while it needs shared
// simulation, collision, or lifecycle maintenance. Terminal visual state belongs
// in feature-owned storage: use an exit-event route to snapshot the collision and
// free the SSPP slot in the same frame. Persistent SSPP lists are reserved for
// states that still require runtime work, such as grounded leaves reacting to the
// player, rather than passive decals or field deposits.
//
// Cross-module integration is limited to the generic Feature lifecycle and the
// render hooks installed by the collision providers.
struct ScreenSpacePhysicalParticlesFeature final : Feature
{
	enum class ParticleKind : std::uint8_t
	{
		kBlood,
		kLeaves,
		kMagic,
		kFluff,
		kDandelion,
		kSpores,
		kFireflies,
		kMoths
	};

	static constexpr std::int32_t kHardParticleLimit = 16384;

	struct Settings
	{
		// Shared stable-slot pool capacity. The amount slider derives this from the hard limit.
		std::int32_t particleCapacity{ 16384 };
		// Percent of the hard particle limit. Zero still keeps the minimum pool, but spawns nothing.
		std::int32_t particleAmount{ 70 };
		// Per-feature live-slot limits keyed by process-stable registration keys.
		std::unordered_map<std::string, std::uint32_t> featureParticleBudgets{};
		// Orthographic solid capture cadence and square texture resolution.
		std::int32_t orthographicRefreshFrames{ 45 };
		std::int32_t orthographicResolution{ 1024 };
		bool kindBlood{ true };
		bool kindLeaves{ true };
		bool kindMagic{ true };
		bool kindFluff{ true };
		bool kindDandelion{ true };
		bool kindSpores{ true };
		bool kindFireflies{ true };
		bool kindMoths{ true };
		// Diagnostic conversion toggles; captures are also requested by live profiles.
		bool heightFieldDebug{};
		bool waterHeightFieldDebug{};
		bool sceneDepthDebug{};
	};

	std::string GetName() override { return "Screen Space Physical Particles"; }
	std::string GetDisplayName() override
	{
		return T("feature.screen_space_physical_particles.name", "Screen-Space Physical Particles");
	}
	std::string GetShortName() override { return "ScreenSpacePhysicalParticles"; }
	std::string_view GetCategory() const override { return FeatureCategories::kOther; }
	// Summary: Builds the user-facing description and capability list.
	// Args: None.
	// Returns: Localized feature summary.
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	// Summary: Allocates shared particle and collision-provider GPU resources.
	// Args: None.
	// Returns: None.
	void SetupResources() override;
	// Summary: Installs SSPP-owned render and game-update hooks.
	// Args: None.
	// Returns: None.
	void PostPostLoad() override;
	// Summary: Revalidates capture resources after each Present callback.
	// Args: None.
	// Returns: None.
	void Reset() override;
	// Summary: Requests perspective collision depth for the upcoming frame.
	// Args: None.
	// Returns: None.
	void EarlyPrepass() override;
	// Summary: Executes providers, particle runtime work, and debug conversion.
	// Args: None.
	// Returns: None.
	void PostComposite() override;
	// Summary: Clears world-dependent particles, captures, and collision state.
	// Args: None.
	// Returns: None.
	void GameLoaded() override;
	// Summary: Invalidates shaders owned by the runtime and collision providers.
	// Args: None.
	// Returns: None.
	void ClearShaderCache() override;
	// Summary: Draws capacity, budget, stress-test, and diagnostic controls.
	// Args: None.
	// Returns: None.
	void DrawSettings() override;
	// Summary: Loads shared settings and clamps values to supported limits.
	// Args:
	//   a_json: Feature settings object.
	// Returns: None.
	void LoadSettings(json& a_json) override;
	// Summary: Serializes shared settings.
	// Args:
	//   a_json: Destination feature settings object.
	// Returns: None.
	void SaveSettings(json& a_json) override;
	// Summary: Restores shared settings without rebuilding resources immediately.
	// Args: None.
	// Returns: None.
	void RestoreDefaultSettings() override;

	// Summary: Reports whether a particle kind is allowed to spawn.
	// Args:
	//   a_kind: Blood, leaves, or one of the world-particle kinds.
	// Returns: True when this feature, the kind checkbox, and the owning feature are enabled.
	[[nodiscard]] bool IsKindEnabled(ParticleKind a_kind) const noexcept;
	// Summary: Converts the amount slider into a 0..1 spawn scale.
	// Args: None.
	// Returns: Zero when the slider is zero, otherwise percent / 100.
	[[nodiscard]] float ParticleAmountScale() const noexcept;


	Settings settings{};
	std::int32_t particleCapacityDraft{ 16384 };
	std::uint32_t budgetedFeatureCount{};
	bool featureBudgetsPending{ true };
	bool debugBurstRejected{};
};
