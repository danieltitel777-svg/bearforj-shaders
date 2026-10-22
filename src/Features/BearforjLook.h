#pragma once

/**
 * @brief Bearforj 0.6 built-in default look (no preset, NAT III CS / NAT compatible).
 * Night: raised moonlight base that keeps the ambient hue (brighter, not grey), aurora light on snow.
 * Post: warm ACES-like look applied after the vanilla tonemap in linear, so it matches with Linear Lighting on or off.
 * Also owns the shader data of Hair Backlighting and Pseudo Sun Bounce (restored 0.5 modules).
 */
struct BearforjLook : Feature
{
	virtual inline std::string GetName() override { return "Bearforj Look"; }
	virtual std::string GetDisplayName() override { return T("feature.bearforj_look.name", "Bearforj Look"); }
	virtual inline std::string GetShortName() override { return "BearforjLook"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return true; }

	struct Settings
	{
		bool Enable = true;
		float TonemapStrength = 0.35f;
		float TonemapMix = 100.0f;  // % of the Bearforj tonemap kept; lower it when the weather mod has its own tonemap
		float Warmth = 0.5f;
		float NightLift = 0.45f;
		float3 NightTint{ 0.70f, 0.82f, 1.0f };
		float AuroraSnow = 0.6f;
		float3 AuroraTint{ 0.30f, 1.0f, 0.60f };
	} settings;

	struct PerFrame
	{
		uint Enable;
		float TonemapStrength;
		float Warmth;
		float NightFactor;
		float3 NightTint;
		float NightLift;
		float3 AuroraTint;
		float AuroraSnow;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	virtual void RestoreDefaultSettings() override { settings = {}; }
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	PerFrame GetCommonBufferData();
};

/** @brief Bearforj Hair Backlighting: light through thin hair strands (shader: HairBacklighting.hlsli). */
struct HairBacklighting : Feature
{
	virtual inline std::string GetName() override { return "Hair Backlighting"; }
	virtual std::string GetDisplayName() override { return T("feature.hair_backlighting.name", "Hair Backlighting"); }
	virtual inline std::string GetShortName() override { return "HairBacklighting"; }
	virtual inline std::string_view GetShaderDefineName() override { return "HAIR_BACKLIGHTING"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return true; }

	struct alignas(16) Settings
	{
		uint Enable = 1;
		float Strength = 1.0f;
		float ScatterWidth = 0.35f;
		float EdgeFalloff = 2.0f;
		float InteriorGlow = 0.25f;
		float Absorption = 1.5f;
		float DarkBoost = 2.0f;
		float DarkThreshold = 0.08f;
		float HeadOcclusion = 0.6f;
		uint pad[3]{};
	} settings;
	STATIC_ASSERT_ALIGNAS_16(Settings);

	virtual void RestoreDefaultSettings() override { settings = {}; }
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	Settings GetCommonBufferData() const;
};

/** @brief Bearforj Pseudo Sun Bounce (Jiaye): faux sunlight bounced off ground and walls, the "bounce light" of the day look. */
struct PseudoSunBounce : Feature
{
	virtual inline std::string GetName() override { return "Pseudo Sun Bounce"; }
	virtual std::string GetDisplayName() override { return T("feature.pseudo_sun_bounce.name", "Pseudo Sun Bounce"); }
	virtual inline std::string GetShortName() override { return "PseudoSunBounce"; }
	virtual inline std::string_view GetShaderDefineName() override { return "PSEUDO_SUN_BOUNCE"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return true; }

	struct alignas(16) Settings
	{
		float3 groundAlbedo{ 0.25f, 0.22f, 0.18f };
		float intensity = 0.35f;
		float3 wallAlbedo{ 0.30f, 0.28f, 0.25f };
		float windowWidth = 3.0f;
	} settings;
	STATIC_ASSERT_ALIGNAS_16(Settings);

	virtual void RestoreDefaultSettings() override { settings = {}; }
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	Settings GetCommonBufferData() const;
};

/** @brief Zero placeholder for SharedData PhysSkyData (320 bytes) until Physical Sky C++ is restored; enabled = 0. */
struct alignas(16) PhysSkyDataPlaceholder
{
	float data[80]{};
};
static_assert(sizeof(PhysSkyDataPlaceholder) == 320);
