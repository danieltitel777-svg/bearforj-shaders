#include "Bearforj/PicturePreset.h"

#include "Features/CSUtility.h"
#include "Features/CloudShadows.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/PostProcessing/Camera.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "Utils/FileSystem.h"

#include <cmath>
#include <fstream>

namespace Bearforj::PicturePreset
{
	namespace
	{
		struct Display
		{
			float intensity = 1.0f;
			float customColorContribution = 0.30f;
			float densityContribution = 0.75f;
			float densityScale = 1.0f;
			float windSpeedScale = 1.0f;
			float windFallingSpeed = 0.0f;
			float phaseContribution = 0.80f;
			float phaseScattering = 0.65f;
			float rangeFactor = 1.2f;
		};

		Display g_display{};
		bool g_resolutionApplied = false;
		bool g_displayApplied = false;

		constexpr const char* kBlock = "Volumetric Lighting Display";

		float ReadFloat(const nlohmann::json& a_block, const char* a_key, float a_fallback)
		{
			const auto it = a_block.find(a_key);
			if (it == a_block.end() || !it->is_number())
				return a_fallback;
			return it->get<float>();
		}

		bool Near(float a_value, float a_expected)
		{
			return std::abs(a_value - a_expected) <= 0.001f;
		}

		bool UserHas(const nlohmann::json& a_user, const char* a_feature, const char* a_key)
		{
			const auto feature = a_user.find(a_feature);
			return feature != a_user.end() && feature->is_object() && feature->contains(a_key);
		}

		nlohmann::json LoadUserSettings()
		{
			std::ifstream in{ Util::PathHelpers::GetSettingsUserPath() };
			if (!in.is_open())
				return {};
			try {
				nlohmann::json user;
				in >> user;
				return user.is_object() ? user : nlohmann::json::object();
			} catch (const std::exception& e) {
				logger::warn("Picture preset left the 1440p column in place; SettingsUser.json could not be read ({})", e.what());
				return {};
			}
		}

		void ApplyOne(const char* a_name, float a_value, bool& a_pending)
		{
			auto* setting = RE::GetINISetting(a_name);
			if (!setting) {
				a_pending = true;
				return;
			}
			setting->data.f = a_value;
		}
	}

	void WriteDisplaySettings(nlohmann::json& a_settings)
	{
		a_settings[kBlock] = {
			{ "fVolumetricLightingIntensity", g_display.intensity },
			{ "fVolumetricLightingCustomColorContribution", g_display.customColorContribution },
			{ "fVolumetricLightingDensityContribution", g_display.densityContribution },
			{ "fVolumetricLightingDensityScale", g_display.densityScale },
			{ "fVolumetricLightingWindSpeedScale", g_display.windSpeedScale },
			{ "fVolumetricLightingWindFallingSpeed", g_display.windFallingSpeed },
			{ "fVolumetricLightingPhaseContribution", g_display.phaseContribution },
			{ "fVolumetricLightingPhaseScattering", g_display.phaseScattering },
			{ "fVolumetricLightingRangeFactor", g_display.rangeFactor },
		};
	}

	void ReadDisplaySettings(const nlohmann::json& a_settings)
	{
		const auto block = a_settings.find(kBlock);
		if (block == a_settings.end() || !block->is_object())
			return;
		g_display.intensity = ReadFloat(*block, "fVolumetricLightingIntensity", g_display.intensity);
		g_display.customColorContribution = ReadFloat(*block, "fVolumetricLightingCustomColorContribution", g_display.customColorContribution);
		g_display.densityContribution = ReadFloat(*block, "fVolumetricLightingDensityContribution", g_display.densityContribution);
		g_display.densityScale = ReadFloat(*block, "fVolumetricLightingDensityScale", g_display.densityScale);
		g_display.windSpeedScale = ReadFloat(*block, "fVolumetricLightingWindSpeedScale", g_display.windSpeedScale);
		g_display.windFallingSpeed = ReadFloat(*block, "fVolumetricLightingWindFallingSpeed", g_display.windFallingSpeed);
		g_display.phaseContribution = ReadFloat(*block, "fVolumetricLightingPhaseContribution", g_display.phaseContribution);
		g_display.phaseScattering = ReadFloat(*block, "fVolumetricLightingPhaseScattering", g_display.phaseScattering);
		g_display.rangeFactor = ReadFloat(*block, "fVolumetricLightingRangeFactor", g_display.rangeFactor);
	}

	bool ApplyDisplaySettings()
	{
		static bool announced = false;
		bool pending = false;
		ApplyOne("fVolumetricLightingIntensity:Display", g_display.intensity, pending);
		ApplyOne("fVolumetricLightingCustomColorContribution:Display", g_display.customColorContribution, pending);
		ApplyOne("fVolumetricLightingDensityContribution:Display", g_display.densityContribution, pending);
		ApplyOne("fVolumetricLightingDensityScale:Display", g_display.densityScale, pending);
		ApplyOne("fVolumetricLightingWindSpeedScale:Display", g_display.windSpeedScale, pending);
		ApplyOne("fVolumetricLightingWindFallingSpeed:Display", g_display.windFallingSpeed, pending);
		ApplyOne("fVolumetricLightingPhaseContribution:Display", g_display.phaseContribution, pending);
		ApplyOne("fVolumetricLightingPhaseScattering:Display", g_display.phaseScattering, pending);
		ApplyOne("fVolumetricLightingRangeFactor:Display", g_display.rangeFactor, pending);
		if (pending && !announced) {
			announced = true;
			logger::info("Volumetric lighting display settings are not in memory yet; the picture preset will retry");
		}
		return !pending;
	}

	void TryApplyResolutionPreset()
	{
		if (!globals::state)
			return;
		const int width = static_cast<int>(globals::state->screenSize.x);
		const int height = static_cast<int>(globals::state->screenSize.y);
		if (width < 640 || height < 480)
			return;
		if (!g_displayApplied)
			g_displayApplied = ApplyDisplaySettings();
		if (g_resolutionApplied)
			return;
		g_resolutionApplied = true;
		if (height < 2160 && width < 3840)
			return;

		const auto user = LoadUserSettings();
		auto& bloom = globals::features::csUtility.settings.bloomEnhancement.Default;
		if (!UserHas(user, "CS Utility", "bloomEnhancement")) {
			if (Near(bloom.EnhancementIntensity, 0.40f))
				bloom.EnhancementIntensity = 0.35f;
			if (Near(bloom.HaloRadius, 3.5f))
				bloom.HaloRadius = 3.0f;
			if (Near(bloom.HaloSpread, 0.70f))
				bloom.HaloSpread = 0.65f;
			if (Near(bloom.CompressionThreshold, 0.15f))
				bloom.CompressionThreshold = 0.18f;
		}

		auto& rays = globals::features::volumetricLighting.settings;
		if (!UserHas(user, "Volumetric Lighting", "ExteriorQuality") && rays.ExteriorQuality == 2)
			rays.ExteriorQuality = 1;
		if (!UserHas(user, "Volumetric Lighting", "InteriorQuality") && rays.InteriorQuality == 2)
			rays.InteriorQuality = 1;

		auto& sky = globals::features::physicalSky.settings;
		if (!UserHas(user, "Physical Sky", "enableVolumetricClouds") && sky.enableVolumetricClouds)
			sky.enableVolumetricClouds = false;
		if (!UserHas(user, "Physical Sky", "cloudMaxStep") && sky.cloudMaxStep == 40)
			sky.cloudMaxStep = 32;

		auto& shadows = globals::features::cloudShadows.settings;
		if (!UserHas(user, "Cloud Shadows", "Opacity") && Near(shadows.Opacity, 0.2f))
			shadows.Opacity = 0.85f;

		bool savedNoise = false;
		if (const auto post = user.find("Post Processing"); post != user.end() && post->is_object()) {
			if (const auto camera = post->find("Camera"); camera != post->end() && camera->is_object()) {
				if (const auto cameraSettings = camera->find("settings"); cameraSettings != camera->end() && cameraSettings->is_object())
					savedNoise = cameraSettings->contains("NoiseStrength");
			}
		}
		if (!savedNoise) {
			if (auto* camera = globals::features::postProcessing.GetPipelineFeature<Camera>(PostProcessing::FeaturePipelineIndex::Camera)) {
				if (Near(camera->settings.NoiseStrength, 0.03f))
					camera->settings.NoiseStrength = 0.02f;
			}
		}
		logger::info("Picture preset: 4K column applied where the saved settings were still the 1440p defaults");
	}
}
