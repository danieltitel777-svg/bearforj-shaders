#pragma once

#include <nlohmann/json.hpp>

namespace Bearforj::PicturePreset
{
	/** @brief Writes the nine fVolumetricLighting* display keys into settings JSON. */
	void WriteDisplaySettings(nlohmann::json& a_settings);
	/** @brief Reads those keys from a merged settings object. Missing keys keep the 1440p preset. */
	void ReadDisplaySettings(const nlohmann::json& a_settings);
	/** @brief Copies the keys onto the game's in-memory display settings. Does not write Skyrim.ini. Returns false if a key is not in memory yet. */
	bool ApplyDisplaySettings();
	/**
	 * @brief On a 4K frame, and only for values the user has not saved, switches the 1440p
	 * picture defaults to the 4K column. No-ops until the real render size is known.
	 */
	void TryApplyResolutionPreset();
}
