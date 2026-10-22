#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

/**
 * @brief Bearforj 0.6 compatibility layer (beta/testing).
 *
 * - Mod profiles: JSON files in CommunityShaders/Bearforj/Profiles/. FOMOD puts the chosen ones in
 *   Profiles/Selected/; the whole library sits in Profiles/Library/. If Selected/ is empty the library
 *   is auto-detected by plugin presence (esp/esm/esl names). A profile is a JSON merge-patch per feature
 *   short name, applied once per profile set (user tweaks afterwards are kept).
 * - Safe mode: a Bearforj/CS feature whose shader fails to compile is turned off for the next start.
 * - Hardware preset: first run picks Quality/Balanced/Performance by dedicated VRAM.
 * - Startup conflicts: ENB, ReShade, second Community/Open Shaders DLL.
 * - Advanced mode: hides the debug/advanced pages unless on.
 * - Compile animation: Mish Mish pushing the shader-compile bar.
 */
namespace Bearforj::Compat
{
	struct ModProfile
	{
		std::string id, name, kind, url, notes, file;
		std::vector<std::string> plugins;
		nlohmann::json patch;
		bool selected = false;  // installed by FOMOD
		bool detected = false;  // plugin present
		bool active = false;
	};

	struct Config
	{
		bool safeMode = true;
		bool hardwarePreset = true;
		bool conflictWarnings = true;
		bool advancedMode = false;
		bool autoDetectProfiles = true;
		bool compileAnimation = true;
		bool weatherPresets = true;
		std::map<std::string, std::string> presetSlots;
		std::string presetLock;
		bool hardwarePresetDone = false;
		std::string appliedSignature;
		std::vector<std::string> safeModeDisabled;
		std::vector<std::string> dismissedConflicts;
		bool devReport = true;          // Mish Mish report hotkey (screenshot + settings/perf report)
		int reportKey = 0x7B;           // VK_F12
		std::string reportDir;          // empty: <SKSE logs>/Bearforj/reports
	};

	/** @brief A detected conflict. Bearforj never disables anything by itself; it only explains. */
	struct ConflictInfo
	{
		std::string id;       // stable id, also the translation key suffix
		std::string mod;      // file that triggered it (plugin / dll / ini)
		std::string feature;  // our feature it touches (short name) or empty
		int severity = 1;     // 2 = red, 1 = yellow
	};

	Config& GetConfig();
	void SaveConfig();

	/** @brief Called from kDataLoaded: loads config, profiles, detects conflicts, applies profiles and first-run preset. */
	void OnDataLoaded();
	const std::vector<ModProfile>& GetProfiles();
	/** @brief Re-applies all active profiles now (menu button). */
	void ApplyActiveProfiles(bool a_force);
	const std::vector<std::string>& GetConflicts();
	const std::vector<ConflictInfo>& GetConflictInfos();
	/** @brief Conflicts not dismissed with "don't show again". */
	std::vector<ConflictInfo> GetVisibleConflicts();
	void DismissConflict(const std::string& a_id);
	std::string GetHardwarePresetName();

	/** @brief Called by ShaderCache on a compile failure. Thread-safe. */
	void OnShaderCompileFailed(const std::string& a_path);
	/** @brief Features switched off by safe mode during this session. */
	std::vector<std::string> GetSafeModeEvents();
	/** @brief Shader compile failures attributed to a feature this session. */
	int GetCompileFailures(const std::string& a_feature);
}
