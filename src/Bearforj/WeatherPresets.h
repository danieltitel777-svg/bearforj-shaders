#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

/**
 * @brief Bearforj 0.6 per-weather look presets (beta).
 *
 * Presets live in CommunityShaders/Bearforj/Presets/<Pack>/<name>.json and hold Bearforj Look values
 * (TonemapStrength, Warmth, NightLift, AuroraSnow) plus an optional "night" override block.
 * Each weather slot (clear, cloudy, rain, storm, snow, blizzard, fog, ash, aurora night) is either
 * "Auto" (the pack of the active weather profile, file <slot>.json) or any preset from any pack.
 * A manual lock forces one preset everywhere. The current weather is classified every frame
 * from TESWeather flags/data and the result is blended smoothly.
 */
namespace Bearforj::WeatherPresets
{
	enum Slot : int
	{
		kClear,
		kCloudy,
		kRain,
		kStorm,
		kSnow,
		kBlizzard,
		kFog,
		kAsh,
		kAuroraNight,
		kSlotCount
	};
	inline constexpr std::array<const char*, kSlotCount> kSlotIds = { "clear", "cloudy", "rain", "storm", "snow", "blizzard", "fog", "ash", "aurora_night" };

	struct Look
	{
		float TonemapStrength = 0.32f, Warmth = 0.45f, NightLift = 0.42f, AuroraSnow = 0.6f, TonemapMix = 100.0f;
	};

	struct Preset
	{
		std::string id;  // "<Pack>/<name>"
		std::string pack, name, file;
		Look day, night;
		bool hasNight = false;
	};

	void Load();
	const std::vector<Preset>& GetPresets();
	/** @brief Slot assignments: slot id -> preset id or "Auto". Stored in Compat.json. */
	std::map<std::string, std::string>& Assignments();
	std::string& Lock();
	bool& Enabled();

	/** @brief Classifies the current weather. */
	Slot CurrentSlot();
	/** @brief Called per frame by Bearforj Look: blends @p io_look (user values) toward the preset. */
	void Apply(Look& io_look, float a_nightFactor, float a_deltaTime);
	/** @brief Name of the preset currently in effect (for the menu). */
	std::string ActivePresetName();
	/** @brief Autumn/forest weather (by editor ID) for extra falling leaves. */
	bool IsAutumnOrForest();

	/** @brief Mutable presets for direct editing in the menu (live: edits apply next frame). */
	std::vector<Preset>& MutablePresets();
	/** @brief Writes an edited preset back to its file. */
	bool SavePreset(const Preset& a_preset);
	bool SaveUserPreset(const std::string& a_name, const Look& a_look);
	bool ExportIni(const std::string& a_path);
	bool ImportIni(const std::string& a_path);
}
