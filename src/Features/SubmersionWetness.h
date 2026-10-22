#pragma once

#include <array>
#include <mutex>
#include <unordered_map>

class ConstantBuffer;

struct alignas(16) CBUInt4
{
	uint32_t x, y, z, w;
};

/**
 * @brief Bearforj Submersion Wetness: per-actor body, armor and hair wetness.
 *
 * Each high-process actor keeps a soak state on the CPU:
 *  - water: the deepest point of the body that was under water (soak line) and how wet it still is;
 *  - rain: a whole-body coat driven by Wetness Effects' retained character wetness (the upstream
 *    character path zeroes the broad coat and keeps only drops; this restores it per actor).
 * The state is uploaded per draw into PS b9 (SubmersionPerGeometry) from a BSLightingShader SetupGeometry
 * hook, and Lighting.hlsl layers it on top of WetnessEffects / CharacterRainSpots (max, never added twice).
 */
struct SubmersionWetness : Feature
{
	/// Water surface Z at a_pos (engine water height, else a flat TESWaterSystem bound). From BearForge 0.5.
	static float WaterHeightAt(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos);


	virtual inline std::string GetName() override { return "Submersion Wetness"; }
	virtual std::string GetDisplayName() override { return T("feature.submersion_wetness.name", "Submersion Wetness"); }
	virtual inline std::string GetShortName() override { return "SubmersionWetness"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SUBMERSION_WETNESS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.submersion_wetness.description", "Characters get wet where they were in water and in the rain, then slowly dry."),
			{ T("feature.submersion_wetness.key_1", "Wet line follows how deep the actor waded"),
				T("feature.submersion_wetness.key_2", "Beads and rivulets after leaving the water"),
				T("feature.submersion_wetness.key_3", "Rain soaks skin, armor and hair"),
				T("feature.submersion_wetness.key_4", "Works with Wetness Effects' character rain drops") } };
	}

	struct Settings
	{
		bool Enabled = true;
		bool IncludeCreatures = true;
		bool RainSoak = true;
		float Strength = 1.0f;
		float BeadDensity = 1.0f;
		float HairStrength = 1.0f;
		float ArmorStrength = 0.8f;
		float RainCoatStrength = 0.7f;
		float DryTime = 180.0f;      // seconds from fully soaked to dry
		float EmergeFlowTime = 8.0f; // seconds of rivulets after the head leaves the water
		float EdgeSoftness = 6.0f;   // units
		float Range = 6144.0f;
		bool RespectShelter = true;  // actors under roofs/porches do not get rained on
		float RainSoakTime = 20.0f;  // seconds of open-sky rain to reach the full rain coat
		float SoakInTime = 2.0f;     // seconds for the submerged part to go from dry to fully soaked
	} settings;

	struct alignas(16) PerGeometry
	{
		float4 SubA;
		float4 SubB;
		float4 SubC;
	};
	static_assert(sizeof(PerGeometry) == 48);

	struct ActorState
	{
		float soakLine = 0.0f;   // above feet
		float soak = 0.0f;       // water soak 0..1
		float depth = 0.0f;      // current submersion depth
		float flow = 0.0f;       // emerge rivulets 0..1
		float waterZ = -1.0e9f;
		float feetZ = 0.0f;
		float height = 128.0f;
		float rain = 0.0f;       // retained rain soak 0..1 (rises under open sky, dries under cover)
		float skyVisibility = 1.0f;  // precipitation visibility above the head (GPU readback)
		uint32_t lastSeenFrame = 0;
	};

	virtual void SetupResources() override;
	virtual void Prepass() override;
	virtual void PostPostLoad() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;

	void OnLightingSetupGeometry(RE::BSRenderPass* a_pass);
	void OnLightingRestoreGeometry();
	/** @brief Soak line above the feet and soak amount 0..1, if this actor is tracked. */
	bool TryGetSoak(RE::FormID a_formID, float& a_line, float& a_amount) const;

private:
	void UpdateActor(RE::Actor* a_actor, float a_dt, float a_rain);
	void UpdateSkyVisibility(const std::vector<RE::Actor*>& a_actors);

	static constexpr uint32_t kMaxQueries = 64;
	static constexpr uint32_t kReadbackLatency = 3;
	winrt::com_ptr<ID3D11ComputeShader> visibilityCS;
	bool visibilityCSFailed = false;
	winrt::com_ptr<ID3D11Buffer> positionBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> positionSRV;
	winrt::com_ptr<ID3D11Buffer> visibilityBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> visibilityUAV;
	std::array<winrt::com_ptr<ID3D11Buffer>, kReadbackLatency> staging;
	std::array<std::vector<RE::FormID>, kReadbackLatency> stagingActors;
	std::array<bool, kReadbackLatency> stagingPending{};
	ConstantBuffer* queryCB = nullptr;
	uint32_t queryFrame = 0;

	std::unordered_map<RE::FormID, ActorState> actors;
	ConstantBuffer* perGeometryCB = nullptr;
	uint32_t frame = 0;
	bool boundThisDraw = false;
};
