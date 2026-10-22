#include "BearforjLook.h"
#include "Bearforj/TipUI.h"

#include "I18n/I18n.h"
#include "Features/ProceduralAurora.h"
#include "Bearforj/WeatherPresets.h"
#include "State.h"
#include <chrono>
#include "Utils/UI.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BearforjLook::Settings, Enable, TonemapStrength, TonemapMix, Warmth, NightLift, NightTint, AuroraSnow, AuroraTint)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HairBacklighting::Settings, Enable, Strength, ScatterWidth, EdgeFalloff, InteriorGlow, Absorption, DarkBoost, DarkThreshold, HeadOcclusion)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PseudoSunBounce::Settings, groundAlbedo, intensity, wallAlbedo, windowWidth)

namespace
{
	/** 0 at day, 1 at deep night, smooth over dusk (18-21h) and dawn (4-7h). */
	float NightFactor(float a_hour)
	{
		const auto ramp = [](float e0, float e1, float x) { float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
		return std::max(ramp(18.0f, 21.0f, a_hour), 1.0f - ramp(4.0f, 7.0f, a_hour));
	}

	bool AuroraVisible(RE::Sky* a_sky)
	{
		if (!a_sky || !a_sky->auroraRoot || a_sky->auroraRoot->GetAppCulled())
			return false;
		const auto aurora = [](RE::TESWeather* w) { return w && w->data.flags.any(RE::TESWeather::WeatherDataFlag::kPermAurora, RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun); };
		return aurora(a_sky->currentWeather) || (aurora(a_sky->lastWeather) && a_sky->currentWeatherPct < 1.0f);
	}
}

void BearforjLook::LoadSettings(json& o_json) { settings = o_json; }
void BearforjLook::SaveSettings(json& o_json) { o_json = settings; }

void BearforjLook::DrawSettings()
{
	BFUI::Checkbox(T("bearforj_look.enable", "Enable"), &settings.Enable);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("bearforj_look.enable_tt", "Bearforj default look. Turn off if your weather mod has its own tonemap and the picture looks too contrasty. Perf: ~0."));
	BFUI::SliderFloat(T("bearforj_look.tonemap", "Crimson filmic look"), &settings.TonemapStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("bearforj_look.tonemap_tt", "How much warm ACES-like curve is mixed over the game tonemap. 0.3-0.4 vanilla weathers, 0.05-0.15 with CS weather mods (NAT.CS, Obsidian CS). Perf: ~0."));
	BFUI::SliderFloat(T("bearforj_look.tonemap_mix", "Bearforj tonemap mix"), &settings.TonemapMix, 0.0f, 100.0f, "%.0f%%");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("bearforj_look.tonemap_mix_tt", "How much of the Bearforj tonemap is used on top of the game/weather tonemap. 100% with vanilla weathers; 15-40% with CS weather mods that ship their own filmic tonemap (NAT.CS, Obsidian CS, Vespro CS), so it is not applied twice. Weather profiles and presets set it automatically. Perf: ~0."));
	BFUI::SliderFloat(T("bearforj_look.warmth", "Warmth"), &settings.Warmth, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("bearforj_look.warmth_tt", "Warm tint of highlights. 0.3-0.5 recommended. Perf: ~0."));
	BFUI::SliderFloat(T("bearforj_look.night_lift", "Night Moonlight Lift"), &settings.NightLift, 0.0f, 1.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("bearforj_look.night_lift_tt", "Raises the moonlight base at night (keeps hue). 0.3-0.5; lower if your weather already brightens nights (Cathedral CS). Perf: ~0."));
	BFUI::ColorEdit3(T("bearforj_look.night_tint", "Moonlight Tint"), &settings.NightTint.x);
	BFUI::SliderFloat(T("bearforj_look.aurora_snow", "Aurora Light on Snow"), &settings.AuroraSnow, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("bearforj_look.aurora_snow_tt", "Green glow on snow while an aurora is visible. 0.4-0.8. Perf: ~0."));
	BFUI::ColorEdit3(T("bearforj_look.aurora_tint", "Aurora Tint"), &settings.AuroraTint.x);
}

BearforjLook::PerFrame BearforjLook::GetCommonBufferData()
{
	PerFrame data{};
	data.Enable = loaded && settings.Enable;
	data.TonemapStrength = settings.TonemapStrength * std::clamp(settings.TonemapMix, 0.0f, 100.0f) / 100.0f;
	data.Warmth = settings.Warmth;
	data.NightTint = settings.NightTint;
	data.NightLift = settings.NightLift;
	data.AuroraTint = settings.AuroraTint;
	const auto sky = globals::game::sky;
	const bool exterior = sky && sky->mode.get() == RE::Sky::Mode::kFull;
	data.NightFactor = exterior ? NightFactor(sky->currentGameHour) : 0.0f;
	float aurora = 0.0f;
	if (exterior) {
		auto& procAurora = globals::features::proceduralAurora;
		aurora = procAurora.loaded ? procAurora.GetVanillaVisibility() : (AuroraVisible(sky) ? 1.0f : 0.0f);
	}
	// Bearforj weather presets (beta): blend the look toward the preset of the current weather slot.
	if (exterior) {
		static auto last = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::chrono::duration<float>(now - last).count();
		last = now;
		Bearforj::WeatherPresets::Look look{ settings.TonemapStrength, settings.Warmth, settings.NightLift, settings.AuroraSnow, settings.TonemapMix };
		Bearforj::WeatherPresets::Apply(look, data.NightFactor, dt);
		data.TonemapStrength = look.TonemapStrength * std::clamp(look.TonemapMix, 0.0f, 100.0f) / 100.0f;
		data.Warmth = look.Warmth;
		data.NightLift = look.NightLift;
		data.AuroraSnow = look.AuroraSnow * data.NightFactor * aurora;
		return data;
	}
	data.AuroraSnow = settings.AuroraSnow * data.NightFactor * aurora;
	return data;
}

void HairBacklighting::LoadSettings(json& o_json) { settings = o_json; }
void HairBacklighting::SaveSettings(json& o_json) { o_json = settings; }
void HairBacklighting::DrawSettings()
{
	bool enable = settings.Enable;
	if (BFUI::Checkbox(T("hair_backlighting.enable", "Enable"), &enable))
		settings.Enable = enable;
	BFUI::SliderFloat(T("hair_backlighting.strength", "Strength"), &settings.Strength, 0.0f, 3.0f);
	BFUI::SliderFloat(T("hair_backlighting.scatter_width", "Scatter Width"), &settings.ScatterWidth, 0.05f, 1.0f);
	BFUI::SliderFloat(T("hair_backlighting.edge_falloff", "Edge Falloff"), &settings.EdgeFalloff, 0.5f, 8.0f);
	BFUI::SliderFloat(T("hair_backlighting.interior_glow", "Interior Glow"), &settings.InteriorGlow, 0.0f, 1.0f);
	BFUI::SliderFloat(T("hair_backlighting.absorption", "Absorption"), &settings.Absorption, 0.0f, 5.0f);
	BFUI::SliderFloat(T("hair_backlighting.dark_boost", "Dark Hair Boost"), &settings.DarkBoost, 1.0f, 5.0f);
	BFUI::SliderFloat(T("hair_backlighting.dark_threshold", "Dark Hair Threshold"), &settings.DarkThreshold, 0.0f, 0.5f);
	BFUI::SliderFloat(T("hair_backlighting.head_occlusion", "Head Occlusion"), &settings.HeadOcclusion, 0.0f, 1.0f);
}
HairBacklighting::Settings HairBacklighting::GetCommonBufferData() const
{
	auto s = settings;
	s.Enable = loaded && settings.Enable;
	return s;
}

void PseudoSunBounce::LoadSettings(json& o_json) { settings = o_json; }
void PseudoSunBounce::SaveSettings(json& o_json) { o_json = settings; }
void PseudoSunBounce::DrawSettings()
{
	BFUI::SliderFloat(T("pseudo_sun_bounce.intensity", "Intensity"), &settings.intensity, 0.0f, 2.0f);
	BFUI::ColorEdit3(T("pseudo_sun_bounce.ground_albedo", "Ground Albedo"), &settings.groundAlbedo.x);
	BFUI::ColorEdit3(T("pseudo_sun_bounce.wall_albedo", "Wall Albedo"), &settings.wallAlbedo.x);
	BFUI::SliderFloat(T("pseudo_sun_bounce.window_width", "Window Width"), &settings.windowWidth, 0.5f, 10.0f);
}
PseudoSunBounce::Settings PseudoSunBounce::GetCommonBufferData() const
{
	auto s = settings;
	if (!loaded)
		s.intensity = 0.0f;
	return s;
}
