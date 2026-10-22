#include "Bearforj/WeatherPresets.h"

#include <fstream>
#include <nlohmann/json.hpp>

#include "Bearforj/Compat.h"
#include "Globals.h"
#include "Utils/FileSystem.h"

namespace Bearforj::WeatherPresets
{
	namespace
	{
		std::vector<Preset> g_presets;
		Look g_blended{};
		bool g_blendInit = false;
		std::string g_activeName;

		std::filesystem::path Root() { return Util::PathHelpers::GetCommunityShaderPath() / "Bearforj" / "Presets"; }

		Look ReadLook(const nlohmann::json& j, Look base)
		{
			if (!j.is_object())
				return base;
			const auto& l = j.contains("BearforjLook") ? j["BearforjLook"] : j;
			base.TonemapStrength = l.value("TonemapStrength", base.TonemapStrength);
			base.Warmth = l.value("Warmth", base.Warmth);
			base.NightLift = l.value("NightLift", base.NightLift);
			base.AuroraSnow = l.value("AuroraSnow", base.AuroraSnow);
			base.TonemapMix = l.value("TonemapMix", base.TonemapMix);
			return base;
		}

		const Preset* Find(const std::string& id)
		{
			for (auto& p : g_presets)
				if (p.id == id)
					return &p;
			return nullptr;
		}

		std::string ActivePack()
		{
			// pack of the first active weather profile ("weather_nat_cs3" -> "nat_cs3")
			for (auto& p : Compat::GetProfiles())
				if (p.active && p.kind == "weather")
					return p.id.starts_with("weather_") ? p.id.substr(8) : p.id;
			return "vanilla";
		}

		Look Lerp(const Look& a, const Look& b, float t)
		{
			auto l = [t](float x, float y) { return x + (y - x) * t; };
			return { l(a.TonemapStrength, b.TonemapStrength), l(a.Warmth, b.Warmth), l(a.NightLift, b.NightLift), l(a.AuroraSnow, b.AuroraSnow), l(a.TonemapMix, b.TonemapMix) };
		}
	}

	std::map<std::string, std::string>& Assignments() { return Compat::GetConfig().presetSlots; }
	std::string& Lock() { return Compat::GetConfig().presetLock; }
	bool& Enabled() { return Compat::GetConfig().weatherPresets; }
	const std::vector<Preset>& GetPresets() { return g_presets; }
	std::string ActivePresetName() { return g_activeName; }

	void Load()
	{
		g_presets.clear();
		std::error_code ec;
		for (auto& packDir : std::filesystem::directory_iterator(Root(), ec)) {
			if (!packDir.is_directory())
				continue;
			for (auto& e : std::filesystem::directory_iterator(packDir.path(), ec)) {
				if (e.path().extension() != ".json")
					continue;
				std::ifstream in(e.path());
				auto j = nlohmann::json::parse(in, nullptr, false, true);
				if (!j.is_object())
					continue;
				Preset p;
				p.pack = packDir.path().filename().string();
				p.name = j.value("name", e.path().stem().string());
				p.id = p.pack + "/" + e.path().stem().string();
				p.file = e.path().string();
				p.day = ReadLook(j, {});
				p.hasNight = j.contains("night");
				p.night = p.hasNight ? ReadLook(j["night"], p.day) : p.day;
				g_presets.push_back(std::move(p));
			}
		}
		std::ranges::sort(g_presets, [](auto& a, auto& b) { return a.id < b.id; });
		logger::info("[Bearforj Presets] {} weather presets loaded", g_presets.size());
	}

	Slot CurrentSlot()
	{
		auto* sky = globals::game::sky;
		if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || !sky->currentWeather)
			return kClear;
		auto* w = sky->currentWeather;
		using F = RE::TESWeather::WeatherDataFlag;
		const std::string edid = w->GetFormEditorID() ? w->GetFormEditorID() : "";
		auto has = [&](const char* s) {
			std::string a = edid, b = s;
			std::ranges::transform(a, a.begin(), ::tolower);
			return a.find(b) != std::string::npos;
		};
		const float wind = w->data.windSpeed / 255.0f;
		if (has("ash") || has("volcan"))
			return kAsh;
		if (w->data.flags.any(F::kSnow))
			return wind > 0.6f || has("blizzard") ? kBlizzard : kSnow;
		if (w->data.flags.any(F::kRainy))
			return w->data.thunderLightningFrequency > 0 || has("storm") || has("thunder") ? kStorm : kRain;
		const bool night = sky->currentGameHour < 5.0f || sky->currentGameHour > 20.5f;
		if (night && w->data.flags.any(F::kPermAurora, F::kAuroraFollowsSun))
			return kAuroraNight;
		if (w->fogData.dayFar > 0.0f && w->fogData.dayFar < 12000.0f || has("fog"))
			return kFog;
		if (w->data.flags.any(F::kCloudy))
			return kCloudy;
		return kClear;
	}

	void Apply(Look& io_look, float a_nightFactor, float a_deltaTime)
	{
		if (!Enabled()) {
			g_activeName.clear();
			g_blendInit = false;
			return;
		}
		const Slot slot = CurrentSlot();
		const Preset* preset = nullptr;
		if (!Lock().empty())
			preset = Find(Lock());
		if (!preset) {
			const auto it = Assignments().find(kSlotIds[slot]);
			const std::string pick = it == Assignments().end() ? "Auto" : it->second;
			preset = pick == "Auto" ? Find(ActivePack() + "/" + kSlotIds[slot]) : Find(pick);
		}
		Look target = io_look;
		if (preset)
			target = Lerp(preset->day, preset->night, std::clamp(a_nightFactor, 0.0f, 1.0f));
		g_activeName = preset ? preset->id : std::string();
		if (!g_blendInit) {
			g_blended = target;
			g_blendInit = true;
		}
		// ~4 s exponential blend so weather transitions never pop
		const float t = 1.0f - std::exp(-std::clamp(a_deltaTime, 0.0f, 0.25f) / 1.3f);
		g_blended = Lerp(g_blended, target, t);
		io_look = g_blended;
	}

	std::vector<Preset>& MutablePresets() { return g_presets; }

	bool IsAutumnOrForest()
	{
		auto* sky = globals::game::sky;
		if (!sky || !sky->currentWeather || !sky->currentWeather->GetFormEditorID())
			return false;
		std::string e = sky->currentWeather->GetFormEditorID();
		std::ranges::transform(e, e.begin(), ::tolower);
		for (const char* k : { "autumn", "fall", "forest", "rift", "falkreath", "pine" })
			if (e.find(k) != std::string::npos)
				return true;
		return false;
	}

	bool SavePreset(const Preset& p)
	{
		auto look = [](const Look& l) { return nlohmann::json{ { "TonemapStrength", l.TonemapStrength }, { "Warmth", l.Warmth }, { "NightLift", l.NightLift }, { "AuroraSnow", l.AuroraSnow }, { "TonemapMix", l.TonemapMix } }; };
		nlohmann::json j{ { "name", p.name }, { "BearforjLook", look(p.day) } };
		if (p.hasNight)
			j["night"] = { { "BearforjLook", look(p.night) } };
		std::ofstream out(p.file);
		if (!out)
			return false;
		out << j.dump(2);
		return true;
	}

	bool SaveUserPreset(const std::string& a_name, const Look& a_look)
	{
		std::error_code ec;
		std::filesystem::create_directories(Root() / "User", ec);
		std::ofstream out(Root() / "User" / (a_name + ".json"));
		if (!out)
			return false;
		out << nlohmann::json{ { "name", a_name }, { "BearforjLook", { { "TonemapStrength", a_look.TonemapStrength }, { "Warmth", a_look.Warmth }, { "NightLift", a_look.NightLift }, { "AuroraSnow", a_look.AuroraSnow }, { "TonemapMix", a_look.TonemapMix } } } }.dump(2);
		out.close();
		Load();
		return true;
	}

	bool ExportIni(const std::string& a_path)
	{
		std::ofstream out(a_path);
		if (!out)
			return false;
		out << "; Bearforj weather preset slots\n[Slots]\n";
		for (auto id : kSlotIds) {
			auto it = Assignments().find(id);
			out << id << "=" << (it == Assignments().end() ? "Auto" : it->second) << "\n";
		}
		out << "[Lock]\npreset=" << Lock() << "\n";
		return true;
	}

	bool ImportIni(const std::string& a_path)
	{
		std::ifstream in(a_path);
		if (!in)
			return false;
		std::string line, section;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == ';')
				continue;
			if (line[0] == '[') {
				section = line;
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			auto k = line.substr(0, eq), v = line.substr(eq + 1);
			if (!v.empty() && v.back() == '\r')
				v.pop_back();
			if (section == "[Slots]")
				Assignments()[k] = v;
			else if (section == "[Lock]" && k == "preset")
				Lock() = v;
		}
		Compat::SaveConfig();
		return true;
	}
}
