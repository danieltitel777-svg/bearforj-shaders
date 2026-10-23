#include "Bearforj/Compat.h"

#include <dxgi.h>
#include <fstream>
#include <mutex>

#include "Bearforj/WeatherPresets.h"
#include "Feature.h"
#include "Globals.h"
#include "State.h"
#include "Utils/FileSystem.h"


namespace Bearforj::Compat
{
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, safeMode, hardwarePreset, conflictWarnings, advancedMode,
	autoDetectProfiles, compileAnimation, weatherPresets, presetSlots, presetLock, hardwarePresetDone, appliedSignature, safeModeDisabled, dismissedConflicts, devReport, reportKey, reportDir)
	namespace
	{
		Config g_config;
		std::vector<ModProfile> g_profiles;
		std::vector<std::string> g_conflicts;
		std::vector<ConflictInfo> g_conflictInfos;
		std::mutex g_safeMutex;
		std::vector<std::string> g_safeEvents;
		std::string g_hwPreset;

		std::filesystem::path Root() { return Util::PathHelpers::GetCommunityShaderPath() / "Bearforj"; }
		std::filesystem::path ConfigPath() { return Root() / "Compat.json"; }

		std::string Lower(std::string s)
		{
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			return s;
		}

		nlohmann::json ReadJson(const std::filesystem::path& p)
		{
			std::ifstream in(p);
			if (!in)
				return {};
			return nlohmann::json::parse(in, nullptr, false, true);
		}

		void LoadDir(const std::filesystem::path& dir, bool selected)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(dir, ec))
				return;
			for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.path().extension() != ".json")
					continue;
				auto j = ReadJson(e.path());
				if (!j.is_object()) {
					logger::warn("[Bearforj Compat] bad profile {}", e.path().string());
					continue;
				}
				ModProfile p;
				p.id = j.value("id", e.path().stem().string());
				p.name = j.value("name", p.id);
				p.kind = j.value("kind", "weather");
				p.url = j.value("url", "");
				p.notes = j.value("notes", "");
				p.file = e.path().string();
				if (j.contains("plugins") && j["plugins"].is_array())
					for (auto& s : j["plugins"])
						if (s.is_string())
							p.plugins.push_back(s.get<std::string>());
				p.patch = j.value("patch", nlohmann::json::object());
				p.selected = selected;
				// a Selected/ copy replaces the library entry with the same id
				auto it = std::ranges::find_if(g_profiles, [&](auto& o) { return o.id == p.id; });
				if (it != g_profiles.end()) {
					if (selected)
						*it = p;
				} else
					g_profiles.push_back(std::move(p));
			}
		}

		bool PluginPresent(const std::string& name)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			return dh && dh->LookupModByName(name) != nullptr;
		}

		void Detect()
		{
			const bool anySelected = std::ranges::any_of(g_profiles, [](auto& p) { return p.selected; });
			for (auto& p : g_profiles) {
				p.detected = std::ranges::any_of(p.plugins, PluginPresent);
				p.active = anySelected ? p.selected : (g_config.autoDetectProfiles && p.detected);
			}
		}

		struct ConflictRule
		{
			const char* id;
			const char* feature;
			int severity;
			std::vector<const char*> patterns;  // lowercase substrings of plugin / SKSE dll file names
		};

		// Weather, lighting and LL mods are supported (profiles), they are NOT conflicts.
		const std::vector<ConflictRule>& Rules()
		{
			static const std::vector<ConflictRule> rules = {
				{ "deformable_terrain", "TerrainDeformation", 2, { "deformableterrain", "nmn_deform", "nmndeform" } },
				{ "footprints", "TerrainDeformation", 1, { "footprints" } },
				{ "embers", "PhysicalEffectParticles", 1, { "embersxd", "embers xd", "embershd", "embers hd" } },
				{ "splashes", "GPUFootstepParticles", 1, { "splashesofskyrim", "splashes of skyrim", "splashesofstorms", "splashes of storms" } },
				{ "fog_mod", "ExponentialHeightFog", 1, { "volumetricfog", "supremefog", "supreme and volumetric fog" } },
				{ "wet_and_cold", "SubmersionWetness", 1, { "wetandcold", "wet and cold" } },
			};
			return rules;
		}

		void AddConflict(const char* a_id, const std::string& a_mod, const char* a_feature, int a_sev)
		{
			for (auto& c : g_conflictInfos)
				if (c.id == a_id)
					return;  // one entry per rule
			g_conflictInfos.push_back({ a_id, a_mod, a_feature, a_sev });
			g_conflicts.push_back(a_mod);
		}

		void FindConflicts()
		{
			g_conflicts.clear();
			g_conflictInfos.clear();
			std::error_code ec;
			const auto root = std::filesystem::current_path(ec);
			auto has = [&](const char* f) { return std::filesystem::exists(root / f, ec); };
			// ENB is only active when its d3d11.dll/dxgi.dll wrapper is present; leftover enbseries/enblocal
			// configs alone (an uninstalled ENB) were a false positive in 0.6.1.
			if ((has("enbseries.ini") || has("enblocal.ini")) && (has("d3d11.dll") || has("dxgi.dll")) && !has("ReShade.ini"))
				AddConflict("enb", "ENB (d3d11.dll + enbseries.ini)", "", 2);
			if (has("ReShade.ini") || has("reshade-shaders"))
				AddConflict("reshade", "ReShade (ReShade.ini)", "BearforjLook", 1);
			const auto self = Lower(Util::PathHelpers::GetCurrentModuleRealPath().filename().string());
			std::vector<std::string> names;  // original-case file names
			const auto plugins = Util::PathHelpers::GetDataPath() / "SKSE" / "Plugins";
			for (auto& e : std::filesystem::directory_iterator(plugins, ec)) {
				if (e.path().extension() != ".dll")
					continue;
				const auto n = Lower(e.path().filename().string());
				if (n == self)
					continue;
				if (n.find("communityshaders") != std::string::npos || n.find("openshaders") != std::string::npos || n.find("bottledshaders") != std::string::npos)
					AddConflict("second_shader_dll", e.path().filename().string(), "", 2);
				names.push_back(e.path().filename().string());
			}
			if (auto* dh = RE::TESDataHandler::GetSingleton()) {
				for (auto* f : dh->files)
					if (f && f->compileIndex != 0xFF)  // loaded plugins only (light plugins use 0xFE)
						names.emplace_back(f->GetFilename());
			}
			for (auto& name : names) {
				const auto n = Lower(name);
				for (auto& r : Rules())
					for (auto* p : r.patterns)
						if (n.find(p) != std::string::npos)
							AddConflict(r.id, name, r.feature, r.severity);
			}
			for (auto& c : g_conflictInfos)
				logger::warn("[Bearforj Compat] conflict '{}': {} (our feature: {})", c.id, c.mod, c.feature.empty() ? "-" : c.feature);
		}

		void ApplyHardwarePreset()
		{
			auto* device = globals::d3d::device;
			if (!device)
				return;
			IDXGIDevice* dxgi = nullptr;
			if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) || !dxgi)
				return;
			IDXGIAdapter* adapter = nullptr;
			DXGI_ADAPTER_DESC desc{};
			if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter) {
				adapter->GetDesc(&desc);
				adapter->Release();
			}
			dxgi->Release();
			const double gb = desc.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
			const auto profile = gb >= 11.5 ? Feature::PerfProfile::Quality : gb >= 7.5 ? Feature::PerfProfile::Balanced : Feature::PerfProfile::Performance;
			g_hwPreset = std::format("{} ({:.0f} GB VRAM)", gb >= 11.5 ? "Ultra/Quality" : gb >= 7.5 ? "High/Balanced" : "Medium/Performance", gb);
			if (g_config.hardwarePresetDone)
				return;
			if (gb < 11.5)  // high-end (e.g. RX 6950 XT 16 GB): keep the shipped all-ON defaults
				Feature::ApplyPerformanceProfileToAll(profile);
			g_config.hardwarePresetDone = true;
			logger::info("[Bearforj Compat] first-run hardware preset: {}", g_hwPreset);
		}

		std::string Signature()
		{
			std::string s;
			for (auto& p : g_profiles)
				if (p.active)
					s += p.id + "|" + p.patch.dump() + ";";
			return s;
		}
	}

	Config& GetConfig() { return g_config; }

	void SaveConfig()
	{
		std::error_code ec;
		std::filesystem::create_directories(Root(), ec);
		std::ofstream out(ConfigPath());
		if (out)
			out << nlohmann::json(g_config).dump(2);
	}

	const std::vector<ModProfile>& GetProfiles() { return g_profiles; }
	const std::vector<std::string>& GetConflicts() { return g_conflicts; }
	const std::vector<ConflictInfo>& GetConflictInfos() { return g_conflictInfos; }
	std::vector<ConflictInfo> GetVisibleConflicts()
	{
		std::vector<ConflictInfo> out;
		for (auto& c : g_conflictInfos)
			if (std::find(g_config.dismissedConflicts.begin(), g_config.dismissedConflicts.end(), c.id) == g_config.dismissedConflicts.end())
				out.push_back(c);
		return out;
	}
	void DismissConflict(const std::string& a_id)
	{
		g_config.dismissedConflicts.push_back(a_id);
		SaveConfig();
	}
	std::string GetHardwarePresetName() { return g_hwPreset; }

	void ApplyActiveProfiles(bool a_force)
	{
		const auto sig = Signature();
		if (!a_force && sig == g_config.appliedSignature)
			return;
		for (auto& p : g_profiles) {
			if (!p.active || !p.patch.is_object())
				continue;
			for (auto& [featureName, patch] : p.patch.items()) {
				if (featureName == "_disable" && patch.is_array()) {
					for (auto& n : patch)
						if (n.is_string())
							globals::state->SetFeatureBootEnabled(n.get<std::string>(), false);
					continue;
				}
				for (auto* f : Feature::GetFeatureList()) {
					if (f->GetShortName() != featureName || !f->loaded)
						continue;
					nlohmann::json j;
					f->SaveSettings(j);
					j.merge_patch(patch);
					f->LoadSettings(j);
				}
			}
			logger::info("[Bearforj Compat] applied profile {}", p.id);
		}
		g_config.appliedSignature = sig;
		SaveConfig();
		globals::state->Save();
	}

	void OnDataLoaded()
	{
		std::error_code ec;
		// First run: FOMOD-installed CompatDefaults.json seeds the config (e.g. Advanced menu option).
		auto j = ReadJson(std::filesystem::exists(ConfigPath(), ec) ? ConfigPath() : Root() / "CompatDefaults.json");
		if (j.is_object())
			g_config = j;
		g_profiles.clear();
		LoadDir(Root() / "Profiles" / "Library", false);
		LoadDir(Root() / "Profiles" / "Selected", true);
		Detect();
		WeatherPresets::Load();
		if (g_config.conflictWarnings)
			FindConflicts();
		if (g_config.hardwarePreset)
			ApplyHardwarePreset();
		ApplyActiveProfiles(false);
		SaveConfig();
	}

	namespace
	{
		std::unordered_map<std::string, int> g_compileFailures;  // feature short name -> count (guarded by g_safeMutex)
	}

	int GetCompileFailures(const std::string& a_feature)
	{
		std::scoped_lock lock(g_safeMutex);
		auto it = g_compileFailures.find(a_feature);
		return it == g_compileFailures.end() ? 0 : it->second;
	}

	void OnShaderCompileFailed(const std::string& a_path)
	{
		const auto path = Lower(a_path);
		for (auto* f : Feature::GetFeatureList()) {
			const auto ln = Lower(f->GetShortName());
			if (path.find("\\" + ln + "\\") != std::string::npos || path.find("/" + ln + "/") != std::string::npos) {
				std::scoped_lock lock(g_safeMutex);
				++g_compileFailures[f->GetShortName()];
				break;
			}
		}
		if (!g_config.safeMode)
			return;
		for (auto* f : Feature::GetFeatureList()) {
			if (f->IsCore() || f->IsAlwaysEnabled())
				continue;
			const auto n = f->GetShortName();
			if (path.find("\\" + Lower(n) + "\\") == std::string::npos && path.find("/" + Lower(n) + "/") == std::string::npos)
				continue;
			{
				std::scoped_lock lock(g_safeMutex);
				if (std::ranges::find(g_safeEvents, n) != g_safeEvents.end())
					return;
				g_safeEvents.push_back(n);
			}
			logger::error("[Bearforj Safe Mode] shader {} failed to compile; feature {} is turned off from the next start", a_path, n);
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([n]() {
					globals::state->SetFeatureBootEnabled(n, false);
					if (std::ranges::find(g_config.safeModeDisabled, n) == g_config.safeModeDisabled.end())
						g_config.safeModeDisabled.push_back(n);
					SaveConfig();
				});
			}
			return;
		}
	}

	std::vector<std::string> GetSafeModeEvents()
	{
		std::scoped_lock lock(g_safeMutex);
		return g_safeEvents;
	}
}
