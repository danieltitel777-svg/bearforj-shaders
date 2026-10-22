#include "BearforjMenu.h"

#include <array>
#include <functional>
#include <map>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <vector>

#include "Bearforj/Analyzer.h"
#include "Bearforj/PerfAssistant.h"
#include "Bearforj/Report.h"
#include "Bearforj/Compat.h"
#include "Bearforj/TipUI.h"
#include "Bearforj/WeatherPresets.h"
#include "Features/BearforjLook.h"
#include "Feature.h"
#include "FeatureCategories.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

namespace
{
	// ---------------------------------------------------------------- style (strict B/W, VoiceOver)
	constexpr ImU32 kBlack = IM_COL32(0, 0, 0, 255);
	constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
	constexpr ImU32 kText = IM_COL32(230, 230, 230, 255);
	constexpr ImU32 kDim = IM_COL32(138, 138, 138, 255);
	constexpr ImU32 kBorder = IM_COL32(58, 58, 58, 255);
	constexpr ImU32 kOutline = IM_COL32(189, 189, 189, 255);
	constexpr ImU32 kSelect = IM_COL32(38, 38, 38, 255);

	struct StyleScope
	{
		int colors = 0;
		int vars = 0;
		void Col(ImGuiCol c, ImU32 v)
		{
			ImGui::PushStyleColor(c, v);
			++colors;
		}
		void Var(ImGuiStyleVar s, float v)
		{
			ImGui::PushStyleVar(s, v);
			++vars;
		}
		void Var(ImGuiStyleVar s, ImVec2 v)
		{
			ImGui::PushStyleVar(s, v);
			++vars;
		}
		~StyleScope()
		{
			ImGui::PopStyleColor(colors);
			ImGui::PopStyleVar(vars);
		}
	};

	void PushBlackWhite(StyleScope& s)
	{
		s.Col(ImGuiCol_WindowBg, kBlack);
		s.Col(ImGuiCol_ChildBg, kBlack);
		s.Col(ImGuiCol_PopupBg, IM_COL32(13, 13, 13, 255));
		s.Col(ImGuiCol_Border, kBorder);
		s.Col(ImGuiCol_Text, kText);
		s.Col(ImGuiCol_TextDisabled, kDim);
		s.Col(ImGuiCol_FrameBg, IM_COL32(10, 10, 10, 255));
		s.Col(ImGuiCol_FrameBgHovered, IM_COL32(28, 28, 28, 255));
		s.Col(ImGuiCol_FrameBgActive, IM_COL32(40, 40, 40, 255));
		s.Col(ImGuiCol_Button, kBlack);
		s.Col(ImGuiCol_ButtonHovered, IM_COL32(22, 22, 22, 255));
		s.Col(ImGuiCol_ButtonActive, IM_COL32(48, 48, 48, 255));
		s.Col(ImGuiCol_Header, kSelect);
		s.Col(ImGuiCol_HeaderHovered, IM_COL32(30, 30, 30, 255));
		s.Col(ImGuiCol_HeaderActive, IM_COL32(48, 48, 48, 255));
		s.Col(ImGuiCol_CheckMark, kText);
		s.Col(ImGuiCol_SliderGrab, kWhite);
		s.Col(ImGuiCol_SliderGrabActive, kText);
		s.Col(ImGuiCol_Separator, kBorder);
		s.Col(ImGuiCol_Tab, kBlack);
		s.Col(ImGuiCol_TabHovered, IM_COL32(28, 28, 28, 255));
		s.Col(ImGuiCol_TabSelected, kBlack);
		s.Col(ImGuiCol_TabSelectedOverline, kText);
		s.Col(ImGuiCol_ScrollbarBg, kBlack);
		s.Col(ImGuiCol_ScrollbarGrab, kBorder);
		s.Col(ImGuiCol_ScrollbarGrabHovered, kDim);
		s.Col(ImGuiCol_ScrollbarGrabActive, kText);
		s.Col(ImGuiCol_PlotHistogram, kText);
		s.Col(ImGuiCol_TextSelectedBg, IM_COL32(80, 80, 80, 255));
		s.Var(ImGuiStyleVar_FrameRounding, 0.0f);
		s.Var(ImGuiStyleVar_ChildRounding, 0.0f);
		s.Var(ImGuiStyleVar_WindowRounding, 0.0f);
		s.Var(ImGuiStyleVar_PopupRounding, 0.0f);
		s.Var(ImGuiStyleVar_ChildBorderSize, 1.0f);
		s.Var(ImGuiStyleVar_FrameBorderSize, 1.0f);
	}

	// ---------------------------------------------------------------- images (swappable files)
	struct Image
	{
		ID3D11ShaderResourceView* srv = nullptr;
		ImVec2 size{};
		void Release()
		{
			if (srv)
				srv->Release();
			srv = nullptr;
		}
	};

	enum ImageId : size_t
	{
		kLogo,
		kHeadHappy,
		kHeadGrumpy,
		kThinking,
		kSurprised,
		kHappy,
		kProud,
		kWink,
		kGeneration,
		kSettingsEmote,
		kPushSheet,
		kWalkSheet,
		kWorried,
		kImageCount
	};

	constexpr std::array<const char*, kImageCount> kImageFiles = {
		"logo.png", "head_happy.png", "head_grumpy.png",
		"emotions/thinking.png", "emotions/surprised.png", "emotions/happy.png", "emotions/proud.png", "emotions/wink.png",
		"emotes/generation.png", "emotes/settings.png", "anim/push_sheet.png", "anim/walk_sheet.png", "head_worried.png"
	};

	std::array<Image, kImageCount> g_images;
	bool g_imagesLoaded = false;

	void LoadImages()
	{
		if (g_imagesLoaded || !globals::d3d::device)
			return;
		g_imagesLoaded = true;
		const auto root = Util::PathHelpers::GetInterfacePath() / "CommunityShaders" / "Bearforj";
		for (size_t i = 0; i < kImageCount; ++i) {
			const auto path = root / kImageFiles[i];
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
				continue;
			if (!Util::LoadTextureFromFile(globals::d3d::device, path.string().c_str(), &g_images[i].srv, g_images[i].size))
				logger::warn("[Bearforj UI] failed to load {}", path.string());
		}
		// Missing grumpy head falls back to the happy one (drawn dimmed), missing happy to the logo.
	}

	const Image* Img(ImageId id)
	{
		if (g_images[id].srv)
			return &g_images[id];
		if (id == kHeadGrumpy && g_images[kHeadHappy].srv)
			return &g_images[kHeadHappy];
		if ((id == kHeadHappy || id == kHeadGrumpy) && g_images[kLogo].srv)
			return &g_images[kLogo];
		return nullptr;
	}

	void DrawImageFit(const Image* img, ImVec2 box, float alpha = 1.0f)
	{
		if (!img || img->size.x <= 0 || img->size.y <= 0) {
			ImGui::Dummy(box);
			return;
		}
		const float s = std::min(box.x / img->size.x, box.y / img->size.y);
		const ImVec2 sz{ img->size.x * s, img->size.y * s };
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddImage((ImTextureID)img->srv, { p.x + (box.x - sz.x) * 0.5f, p.y + (box.y - sz.y) * 0.5f },
			{ p.x + (box.x + sz.x) * 0.5f, p.y + (box.y + sz.y) * 0.5f }, { 0, 0 }, { 1, 1 }, IM_COL32(255, 255, 255, (int)(alpha * 255)));
		ImGui::Dummy(box);
	}

	// ---------------------------------------------------------------- fonts (looked up every frame)
	struct FontScope
	{
		bool pushed = false;
		FontScope(ImFont* f, float size = 0.0f)
		{
			if (f) {
				ImGui::PushFont(f, size);
				pushed = true;
			}
		}
		~FontScope()
		{
			if (pushed)
				ImGui::PopFont();
		}
	};

	/** @brief Letter-spaced mono label ("SYS.01 // ...") in the VoiceOver style. */
	void MonoLabel(const char* text, ImU32 col = kDim)
	{
		FontScope f(ThemeManager::FindMonoFont(), ImGui::GetFontSize() * 0.72f);
		auto* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float start = p.x;
		const float spacing = 1.5f;
		char buf[8];
		for (const char* c = text; *c;) {
			const char* next = c + 1;
			while (*next && (static_cast<unsigned char>(*next) & 0xC0) == 0x80)
				++next;
			const size_t n = std::min<size_t>(next - c, sizeof(buf) - 1);
			std::memcpy(buf, c, n);
			buf[n] = 0;
			dl->AddText(p, col, buf);
			p.x += ImGui::CalcTextSize(buf).x + spacing;
			c = next;
		}
		ImGui::Dummy({ p.x - start, ImGui::GetTextLineHeight() });
	}

	// ---------------------------------------------------------------- tabs / feature grouping
	enum Tab : int
	{
		kCharacter,
		kSkyWeather,
		kLighting,
		kLandscape,
		kParticles,
		kWater,
		kPostProcessing,
		kSystem,
		kTabCount
	};

	struct TabDef
	{
		const char* key;
		const char* en;
	};
	constexpr std::array<TabDef, kTabCount> kTabs = { {
		{ "bearforj.ui.tab.character", "CHARACTER" },
		{ "bearforj.ui.tab.sky_weather", "SKY & WEATHER" },
		{ "bearforj.ui.tab.lighting", "LIGHTING" },
		{ "bearforj.ui.tab.landscape", "LANDSCAPE" },
		{ "bearforj.ui.tab.particles", "PARTICLES" },
		{ "bearforj.ui.tab.water", "WATER" },
		{ "bearforj.ui.tab.post_processing", "POST-PROCESSING" },
		{ "bearforj.ui.tab.system", "SYSTEM" },
	} };

	Tab TabFor(Feature* f)
	{
		const std::string s = f->GetShortName();
		static const std::unordered_map<std::string, Tab> overrides = {
			{ "ScreenSpacePhysicalParticles", kParticles }, { "PhysicalLeaves", kParticles }, { "PhysicalBlood", kParticles },
			{ "NativeParticlePhysics", kParticles }, { "FootstepKickParticles", kParticles }, { "GPUFootstepParticles", kParticles },
			{ "PrecipitationPhysics", kParticles }, { "FootstepParticles", kParticles },
			{ "GrassCollision", kParticles },
			{ "WetnessEffects", kSkyWeather }, { "RainRefraction", kSkyWeather }, { "WeatherPicker", kSkyWeather },
			{ "SkySync", kSkyWeather }, { "PhysicalSky", kSkyWeather }, { "CloudShadows", kSkyWeather }, { "CloudRelight", kSkyWeather },
			{ "ExponentialHeightFog", kSkyWeather }, { "Wind", kSkyWeather },
			{ "SubmersionWetness", kCharacter }, { "HairBacklighting", kCharacter },
			{ "TerrainDeformation", kLandscape },
		};
		if (auto it = overrides.find(s); it != overrides.end())
			return it->second;
		const auto c = f->GetCategory();
		if (c == FeatureCategories::kCharacters)
			return kCharacter;
		if (c == FeatureCategories::kSky)
			return kSkyWeather;
		if (c == FeatureCategories::kLighting || c == FeatureCategories::kMaterials)
			return kLighting;
		if (c == FeatureCategories::kLandscapeAndTextures || c == FeatureCategories::kFoliage)
			return kLandscape;
		if (c == FeatureCategories::kWater)
			return kWater;
		if (c == FeatureCategories::kPostProcessing || c == FeatureCategories::kDisplay)
			return kPostProcessing;
		return kSystem;
	}

	bool IsOn(Feature* f)
	{
		return f->IsAlwaysEnabled() || f->IsCore() || !globals::state->IsFeatureDisabled(f->GetShortName());
	}

	// ---------------------------------------------------------------- state
	int g_tab = kCharacter;
	std::array<std::string, kTabCount> g_selected;  // short name per tab; "#general"/"#advanced" in SYSTEM
	char g_search[64] = {};
	bool g_requestReset = false;

	/** @brief Pill switch; returns true when clicked. */
	bool Switch(const char* id, bool on, bool locked)
	{
		const float h = ImGui::GetFrameHeight() * 0.8f, w = h * 1.9f;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::BeginDisabled(locked);
		const bool clicked = ImGui::InvisibleButton(id, { w, h });
		ImGui::EndDisabled();
		auto* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, { p.x + w, p.y + h }, on ? kText : kBlack, h * 0.5f);
		dl->AddRect(p, { p.x + w, p.y + h }, kOutline, h * 0.5f);
		const float r = h * 0.35f;
		const float cx = on ? p.x + w - h * 0.5f : p.x + h * 0.5f;
		dl->AddCircleFilled({ cx, p.y + h * 0.5f }, r, on ? kBlack : kDim);
		return clicked;
	}

	bool Pill(const char* label, float width)
	{
		StyleScope s;
		s.Var(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
		s.Col(ImGuiCol_Border, kOutline);
		return ImGui::Button(label, { width, ImGui::GetFrameHeight() * 1.15f });
	}

	void EmoteCard(ImageId emote, const char* tag, const char* text)
	{
		ImGui::BeginGroup();
		DrawImageFit(Img(emote), { 72, 72 });
		ImGui::SameLine();
		ImGui::BeginGroup();
		MonoLabel(tag);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::EndGroup();
	}

	void FeatureTooltip(Feature* f, bool on, bool locked)
	{
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			return;
		ImGui::BeginTooltip();
		DrawImageFit(Img(kThinking), { 56, 56 });
		ImGui::SameLine();
		ImGui::BeginGroup();
		MonoLabel("TIP // HINT");
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
		auto summary = f->GetFeatureSummary();
		if (!summary.first.empty())
			ImGui::TextUnformatted(summary.first.c_str());
		ImGui::TextDisabled("%s", locked ? T("bearforj.ui.toggle_locked", "Core feature, it cannot be turned off.") :
		                          on   ? T("bearforj.ui.toggle_tt_on", "Feature is on. Turning it off applies after a game restart.") :
		                                 T("bearforj.ui.toggle_tt_off", "Feature is off. Turning it on applies after a game restart."));
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::EndTooltip();
	}

	// ---------------------------------------------------------------- Bearforj 0.6 compat / beta page
	void DrawProfileRow(const Bearforj::Compat::ModProfile& p)
	{
		ImGui::PushID(p.id.c_str());
		const float row = ImGui::GetFrameHeight() * 1.2f;
		DrawImageFit(Img(p.active ? kHeadHappy : kHeadGrumpy), { row, row }, p.active ? 1.0f : 0.5f);
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextColored(ImColor(p.active ? kText : kDim), "%s", p.name.c_str());
		const char* state = p.selected ? T("bearforj.compat.state_fomod", "chosen in installer") :
		                    p.detected ? T("bearforj.compat.state_detected", "detected by plugin") :
		                                 T("bearforj.compat.state_off", "not found");
		MonoLabel(state);
		ImGui::EndGroup();
		if (ImGui::IsItemHovered() && (!p.notes.empty() || !p.url.empty())) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(420.0f);
			if (!p.notes.empty())
				ImGui::TextUnformatted(p.notes.c_str());
			if (!p.url.empty())
				ImGui::TextDisabled("%s", p.url.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		ImGui::PopID();
	}

	void DrawWeatherPresets()
	{
		namespace WP = Bearforj::WeatherPresets;
		ImGui::Spacing();
		MonoLabel(T("bearforj.presets.title", "WEATHER // PRESETS"));
		bool changed = ImGui::Checkbox(T("bearforj.presets.enable", "Switch the look by current weather"), &WP::Enabled());
		ImGui::TextDisabled("%s: %s  //  %s: %s", T("bearforj.presets.now_weather", "Now"), WP::kSlotIds[WP::CurrentSlot()],
			T("bearforj.presets.now_preset", "preset"), WP::ActivePresetName().empty() ? "-" : WP::ActivePresetName().c_str());
		const auto& presets = WP::GetPresets();
		auto combo = [&](const char* label, std::string& value, const char* empty) {
			const char* preview = value.empty() ? empty : value.c_str();
			if (ImGui::BeginCombo(label, preview)) {
				if (ImGui::Selectable(empty, value.empty() || value == "Auto")) {
					value = empty[0] == 'A' ? "Auto" : "";
					changed = true;
				}
				for (auto& p : presets)
					if (ImGui::Selectable(p.id.c_str(), value == p.id)) {
						value = p.id;
						changed = true;
					}
				ImGui::EndCombo();
			}
		};
		ImGui::SetNextItemWidth(260.0f);
		combo(T("bearforj.presets.lock", "Lock one preset (no auto-switch)"), WP::Lock(), "-");
		ImGui::BeginDisabled(!WP::Lock().empty());
		for (auto id : WP::kSlotIds) {
			auto& v = WP::Assignments()[id];
			if (v.empty())
				v = "Auto";
			ImGui::SetNextItemWidth(260.0f);
			combo(std::format("{}##slot", T(std::format("bearforj.presets.slot.{}", id).c_str(), id)).c_str(), v, "Auto");
		}
		ImGui::EndDisabled();
		// direct editing of any preset (live preview; Save writes the file)
		{
			static std::string editId;
			auto& all = WP::MutablePresets();
			ImGui::SetNextItemWidth(260.0f);
			if (ImGui::BeginCombo(T("bearforj.presets.edit", "Edit preset"), editId.empty() ? "-" : editId.c_str())) {
				for (auto& p : all)
					if (ImGui::Selectable(p.id.c_str(), editId == p.id))
						editId = p.id;
				ImGui::EndCombo();
			}
			auto it = std::ranges::find_if(all, [&](auto& p) { return p.id == editId; });
			if (it != all.end()) {
				auto sliders = [](const char* tag, WP::Look& l) {
					ImGui::PushID(tag);
					MonoLabel(tag);
					ImGui::SliderFloat(T("bearforj_look.tonemap", "Warm ACES Look"), &l.TonemapStrength, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat(T("bearforj_look.warmth", "Warmth"), &l.Warmth, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat(T("bearforj_look.night_lift", "Night Moonlight Lift"), &l.NightLift, 0.0f, 1.5f, "%.2f");
					ImGui::SliderFloat(T("bearforj_look.aurora_snow", "Aurora Light on Snow"), &l.AuroraSnow, 0.0f, 2.0f, "%.2f");
					ImGui::SliderFloat(T("bearforj_look.tonemap_mix", "Bearforj tonemap mix"), &l.TonemapMix, 0.0f, 100.0f, "%.0f%%");
					ImGui::PopID();
				};
				sliders(T("bearforj.presets.day", "DAY"), it->day);
				ImGui::Checkbox(T("bearforj.presets.has_night", "Separate night values"), &it->hasNight);
				if (it->hasNight)
					sliders(T("bearforj.presets.night", "NIGHT"), it->night);
				else
					it->night = it->day;
				if (Pill(T("bearforj.presets.save_edit", "Save preset"), 160.0f))
					WP::SavePreset(*it);
				ImGui::SameLine();
				if (Pill(T("bearforj.presets.revert", "Revert from file"), 180.0f))
					WP::Load();
			}
		}
		static char name[48] = "my_look";
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputText("##presetname", name, sizeof(name));
		ImGui::SameLine();
		if (Pill(T("bearforj.presets.save", "Save current look as preset"), 240.0f)) {
			auto& st = globals::features::bearforjLook.settings;
			WP::SaveUserPreset(name, { st.TonemapStrength, st.Warmth, st.NightLift, st.AuroraSnow, st.TonemapMix });
		}
		const auto iniPath = (Util::PathHelpers::GetCommunityShaderPath() / "Bearforj" / "Presets" / "BearforjPresetSlots.ini").string();
		if (Pill(T("bearforj.presets.export", "Export .ini"), 140.0f))
			WP::ExportIni(iniPath);
		ImGui::SameLine();
		if (Pill(T("bearforj.presets.import", "Import .ini"), 140.0f))
			WP::ImportIni(iniPath);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(iniPath.c_str());
		if (changed)
			Bearforj::Compat::SaveConfig();
	}

	/** @brief Post-processing tab: all weather/post presets grouped by set (pack), click = lock. */
	void DrawPresetBrowser()
	{
		namespace WP = Bearforj::WeatherPresets;
		ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.presets.tab", "Look presets (weather)"));
		ImGui::Separator();
		if (WP::GetPresets().empty())
			WP::Load();
		const auto& presets = WP::GetPresets();
		std::map<std::string, std::vector<const WP::Preset*>> packs;
		for (auto& p : presets)
			packs[p.pack].push_back(&p);
		ImGui::TextDisabled("%s: %zu  //  %s: %zu", T("bearforj.presets.count_sets", "Sets"), packs.size(),
			T("bearforj.presets.count_presets", "Presets"), presets.size());
		if (presets.empty())
			ImGui::TextWrapped("%s", T("bearforj.presets.none", "No presets found in CommunityShaders/Bearforj/Presets. Choose a preset set in the installer."));
		bool changed = false;
		for (auto& [pack, list] : packs) {
			if (!ImGui::CollapsingHeader(std::format("{} ({})##pk{}", pack, list.size(), pack).c_str()))
				continue;
			for (auto* p : list) {
				const bool locked = WP::Lock() == p->id;
				if (ImGui::Selectable(std::format("{}##{}", p->name, p->id).c_str(), locked)) {
					WP::Lock() = locked ? "" : p->id;
					WP::Enabled() = true;
					changed = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(p->id.c_str());
					ImGui::TextDisabled("%s %.2f  %s %.2f  %s %.2f  %s %.0f%%", T("bearforj_look.tonemap", "Warm ACES Look"), p->day.TonemapStrength,
						T("bearforj_look.warmth", "Warmth"), p->day.Warmth, T("bearforj_look.night_lift", "Night Moonlight Lift"), p->day.NightLift,
						T("bearforj_look.tonemap_mix", "Bearforj tonemap mix"), p->day.TonemapMix);
					ImGui::TextUnformatted(locked ? T("bearforj.presets.click_unlock", "Click to unlock (back to auto by weather).") :
					                                T("bearforj.presets.click_lock", "Click to use this preset everywhere."));
					ImGui::EndTooltip();
				}
			}
		}
		if (changed)
			Bearforj::Compat::SaveConfig();
		DrawWeatherPresets();
	}

	void DrawCompatPage()
	{
		using namespace Bearforj;
		auto& c = Compat::GetConfig();
		ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.compat.title", "Compatibility & Beta"));
		ImGui::Separator();
		for (const char* kind : { "weather", "storm", "post", "lighting" }) {
			ImGui::Spacing();
			const std::string_view k = kind;
			MonoLabel(k == "weather" ? T("bearforj.compat.weather", "WEATHER // PROFILES") : k == "storm" ? T("bearforj.compat.storm", "STORMS & LIGHTNING // PROFILES") :
			          k == "post"    ? T("bearforj.compat.post", "POST-PROCESSING // PROFILES") : T("bearforj.compat.lighting", "LIGHTING // PROFILES"));
			for (auto& p : Compat::GetProfiles())
				if (p.kind == kind)
					DrawProfileRow(p);
		}
		ImGui::Spacing();
		if (Pill(T("bearforj.compat.reapply", "Re-apply active profiles"), 240.0f))
			Compat::ApplyActiveProfiles(true);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("bearforj.compat.reapply_tt", "Writes the profile values over your current settings for these features."));

		DrawWeatherPresets();

		ImGui::Spacing();
		MonoLabel(T("bearforj.compat.beta", "BETA // TESTING"));
		bool changed = false;
		changed |= ImGui::Checkbox(T("bearforj.compat.auto_detect", "Auto-detect weather/lighting mods by plugin"), &c.autoDetectProfiles);
		changed |= ImGui::Checkbox(T("bearforj.compat.safe_mode", "Safe mode: turn off a feature whose shader fails to compile"), &c.safeMode);
		changed |= ImGui::Checkbox(T("bearforj.compat.hw_preset", "Pick graphics preset by video card on first run"), &c.hardwarePreset);
		changed |= ImGui::Checkbox(T("bearforj.compat.conflicts", "Warn about ENB / ReShade / second shader DLL"), &c.conflictWarnings);
		changed |= ImGui::Checkbox(T("bearforj.compat.advanced", "Advanced mode (debug tools and sliders)"), &c.advancedMode);
		changed |= ImGui::Checkbox(T("bearforj.compat.anim", "Mish Mish animation while compiling shaders"), &c.compileAnimation);
		ImGui::SeparatorText(T("bearforj.report.title", "Developer: Mish Mish report"));
		changed |= ImGui::Checkbox(T("bearforj.report.enable", "Report hotkey (screenshot + settings/perf report)"), &c.devReport);
		ImGui::TextUnformatted(T("bearforj.report.key", "Hotkey:"));
		ImGui::SameLine();
		if (ImGui::Button(Bearforj::Report::Rebinding() ? T("bearforj.report.press", "Press a key... (Esc = cancel)") : Bearforj::Report::KeyName(c.reportKey).c_str()))
			Bearforj::Report::BeginRebind();
		{
			static char dir[512]{};
			if (!ImGui::IsItemActive() && std::string(dir) != c.reportDir)
				strncpy_s(dir, c.reportDir.c_str(), sizeof(dir) - 1);
			ImGui::SetNextItemWidth(420.0f);
			if (ImGui::InputText(T("bearforj.report.dir", "Report folder"), dir, sizeof(dir), ImGuiInputTextFlags_EnterReturnsTrue)) {
				c.reportDir = dir;
				changed = true;
			}
			ImGui::TextDisabled("%s", T("bearforj.report.hint", "Nothing is captured until you press the key. If the folder can't be created: SKSE logs/Bearforj/reports."));
		}
		if (changed)
			Compat::SaveConfig();
		if (const auto hw = Compat::GetHardwarePresetName(); !hw.empty())
			ImGui::TextDisabled("%s: %s", T("bearforj.compat.hw_detected", "Video card preset"), hw.c_str());

		auto events = Compat::GetSafeModeEvents();
		for (auto& n : c.safeModeDisabled)
			if (std::ranges::find(events, n) == events.end())
				events.push_back(n);
		if (!events.empty()) {
			ImGui::Spacing();
			std::string list;
			for (auto& e : events)
				list += "\n- " + e;
			EmoteCard(kSurprised, "SAFE // MODE", (std::string(T("bearforj.compat.safe_list", "Turned off because a shader failed to compile (see the log):")) + list).c_str());
			if (Pill(T("bearforj.compat.safe_clear", "Forget this list"), 200.0f)) {
				c.safeModeDisabled.clear();
				Compat::SaveConfig();
			}
		}
	}

	// Conflict detector: badge in the header + popup with a worried Mish Mish. Never disables anything itself.
	std::string ConflictText(const Bearforj::Compat::ConflictInfo& c, const char* a_part)
	{
		static const std::map<std::string, std::pair<const char*, const char*>> en = {
			{ "enb", { "ENB replaces the same render passes; Bearforj effects, Linear Lighting and particles can break or double up.", "Remove ENB (d3d11.dll + enbseries) or don't use Bearforj with it." } },
			{ "reshade", { "ReShade runs after Bearforj; its tonemap / AO / bloom stack on top of ours.", "Turn off ReShade tonemap/AO/bloom, or lower Bearforj tonemap mix." } },
			{ "second_shader_dll", { "A second Community/Open Shaders DLL is installed; both hook the same shaders.", "Delete the other shader DLL, keep only Bearforj." } },
			{ "deformable_terrain", { "Another deformable-terrain mod also changes landscape height; footprints will double or flicker.", "Turn off Bearforj Terrain Deformation, or disable the other mod." } },
			{ "footprints", { "Footprints mod also leaves prints; you get two sets.", "Turn off Bearforj Terrain Deformation or the Footprints mod." } },
			{ "particle_engine", { "Another precipitation/particle engine draws rain or snow too; precipitation doubles.", "Turn off SSPP Rain/Snow, or the other mod's particles." } },
			{ "embers", { "Embers mod adds fire embers; with SSPP embers fires look overloaded.", "Lower SSPP magic/ember particles, or keep only one." } },
			{ "splashes", { "Splashes mod adds hit/rain splashes; SSPP does that too.", "Turn off SSPP Sparks/Splashes or the other mod." } },
			{ "fog_mod", { "Fog mod adds volumetric fog; with Height Fog it gets too thick.", "Lower or turn off Bearforj Height Fog." } },
			{ "wet_and_cold", { "Wet and Cold makes characters wet too; wetness doubles.", "Turn off Submersion Wetness or Wet and Cold wetness." } },
		};
		auto it = en.find(c.id);
		const char* def = it == en.end() ? "" : (std::string(a_part) == "what" ? it->second.first : it->second.second);
		return T(("bearforj.conflict." + c.id + "." + a_part).c_str(), def);
	}

	void DrawConflictBanner()
	{
		if (!Bearforj::Compat::GetConfig().conflictWarnings)
			return;
		const auto conflicts = Bearforj::Compat::GetVisibleConflicts();
		if (conflicts.empty())
			return;
		// badge
		ImGui::PushStyleColor(ImGuiCol_Button, ImColor(kWhite).Value);
		ImGui::PushStyleColor(ImGuiCol_Text, ImColor(kBlack).Value);
		const bool open = ImGui::SmallButton(std::format("! {} {}###bfconflictbadge", conflicts.size(), T("bearforj.conflict.badge", "conflicts")).c_str());
		ImGui::PopStyleColor(2);
		static bool shownOnce = false;
		if (open || !shownOnce) {
			ImGui::OpenPopup("###bfconflicts");
			shownOnce = true;
		}
		ImGui::SetNextWindowSize({ 560, 0 }, ImGuiCond_Appearing);
		if (ImGui::BeginPopupModal(std::format("{}###bfconflicts", T("bearforj.conflict.title", "Mish Mish is worried")).c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			DrawImageFit(Img(kWorried), { 72, 72 });
			ImGui::SameLine();
			ImGui::TextWrapped("%s", T("bearforj.conflict.intro", "These mods fight with Bearforj. Nothing was turned off - you decide."));
			ImGui::Separator();
			for (auto& c : conflicts) {
				ImGui::PushID(c.id.c_str());
				ImGui::TextColored(c.severity >= 2 ? ImVec4(1, 0.35f, 0.35f, 1) : ImVec4(1, 0.85f, 0.3f, 1), "%s %s", c.severity >= 2 ? "[!!]" : "[!]", c.mod.c_str());
				ImGui::TextWrapped("%s %s", T("bearforj.conflict.what", "What:"), ConflictText(c, "what").c_str());
				ImGui::TextWrapped("%s %s", T("bearforj.conflict.fix", "Do:"), ConflictText(c, "fix").c_str());
				if (!c.feature.empty())
					ImGui::TextDisabled("%s %s", T("bearforj.conflict.feature", "Our feature:"), c.feature.c_str());
				if (ImGui::SmallButton(T("bearforj.conflict.dismiss", "Don't show again")))
					Bearforj::Compat::DismissConflict(c.id);
				ImGui::Separator();
				ImGui::PopID();
			}
			if (ImGui::Button(T("bearforj.conflict.close", "OK")))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	// ---------------------------------------------------------------- compile overlay: Mish Mish pushes the bar
	void DrawCompileBar(float a_percent, const char* a_overlay)
	{
		const float w = 520.0f, barH = ImGui::GetFrameHeight() * 0.9f;
		const bool sheetOk = g_images[kPushSheet].srv != nullptr && g_images[kPushSheet].size.y > 0;
		const bool anim = Bearforj::Compat::GetConfig().compileAnimation && sheetOk;
		static bool loggedAnim = false;
		if (!loggedAnim) {
			loggedAnim = true;
			logger::info("[Bearforj UI] compile overlay: Mish Mish animation {} (setting {}, push_sheet {})", anim ? "ON" : "OFF",
				Bearforj::Compat::GetConfig().compileAnimation ? "on" : "off", sheetOk ? "loaded" : "MISSING");
		}
		// Full-body Mish Mish: push_sheet.png is 10 frames of 160x200 (black ink on transparent).
		const int frames = 10;
		const float spriteH = anim ? barH * 5.0f : 0.0f, spriteW = spriteH * 0.8f;
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float rowH = std::max(spriteH, barH);
		const ImVec2 bar0{ p0.x + spriteW * 0.6f, p0.y + rowH - barH };
		const ImVec2 bar1{ bar0.x + w, bar0.y + barH };
		auto* dl = ImGui::GetWindowDrawList();
		const float pct = std::clamp(std::isfinite(a_percent) ? a_percent : 0.0f, 0.0f, 1.0f);
		const float fillX = bar0.x + w * pct;
		dl->AddRectFilled(bar0, bar1, kBlack, barH * 0.5f);
		if (pct > 0.0f)
			dl->AddRectFilled(bar0, { std::max(fillX, bar0.x + barH), bar1.y }, kText, barH * 0.5f);
		dl->AddRect(bar0, bar1, kOutline, barH * 0.5f);
		const ImVec2 ts = ImGui::CalcTextSize(a_overlay);
		const ImVec2 tp{ bar0.x + (w - ts.x) * 0.5f, bar0.y + (barH - ts.y) * 0.5f };
		dl->PushClipRect(bar0, { fillX, bar1.y }, true);
		dl->AddText(tp, kBlack, a_overlay);
		dl->PopClipRect();
		dl->PushClipRect({ fillX, bar0.y }, bar1, true);
		dl->AddText(tp, kText, a_overlay);
		dl->PopClipRect();
		if (anim) {
			// Mood: progress jump -> happy; no progress 2.5 s -> strains (grumpy, faster, sweat);
			// 8 s+ -> angry (stomps, steam); done -> celebrates (jumps, happy).
			static float lastPct = -1.0f;
			static double lastMove = 0.0, happyUntil = 0.0;
			const double now = ImGui::GetTime();
			if (pct < lastPct - 0.01f || lastPct < 0.0f) {  // new compile run
				lastPct = pct;
				lastMove = now;
			}
			if (pct > lastPct + 0.0005f) {
				if (pct - lastPct > 0.02f)
					happyUntil = now + 1.2;
				lastPct = pct;
				lastMove = now;
			}
			const double stall = now - lastMove;
			const bool done = pct >= 0.999f, happy = done || now < happyUntil;
			const bool strain = !happy && stall > 2.5, angry = !happy && stall > 8.0;
			const double fps = angry ? 16.0 : strain ? 13.0 : 10.0;
			const int f = (int)(now * fps) % frames;
			float dx = 0.0f, dy = 0.0f;
			if (strain)
				dx = (float)std::sin(now * 60.0) * 1.5f - (float)std::fmod(now * 2.5, 1.0) * 3.0f;  // shaking + feet slipping back
			if (angry)
				dy = -std::abs((float)std::sin(now * 9.0)) * spriteH * 0.08f;  // stomping
			if (done)
				dy = -std::abs((float)std::sin(now * 6.0)) * spriteH * 0.25f;  // celebration hops
			const ImVec2 s0{ fillX - spriteW * 0.85f + dx, p0.y + rowH - spriteH + dy };
			// Black ink is invisible on the black overlay: give Mish Mish a paper card to stand on.
			dl->AddRectFilled({ s0.x - 4.0f, s0.y - 4.0f }, { s0.x + spriteW + 4.0f, s0.y + spriteH + 2.0f }, IM_COL32(235, 235, 235, 255), 6.0f);
			dl->AddImage((ImTextureID)g_images[kPushSheet].srv, s0, { s0.x + spriteW, s0.y + spriteH },
				{ (float)f / frames, 0.0f }, { (float)(f + 1) / frames, 1.0f });
			// mood face bubble above the head
			if (happy || strain) {
				const float hs = spriteH * 0.42f;
				const ImVec2 h0{ s0.x + spriteW * 0.55f, s0.y - hs * 0.55f };
				if (const auto* head = Img(happy ? kHeadHappy : kHeadGrumpy)) {
					dl->AddCircleFilled({ h0.x + hs * 0.5f, h0.y + hs * 0.5f }, hs * 0.58f, kBlack);
					dl->AddImage((ImTextureID)head->srv, h0, { h0.x + hs, h0.y + hs });
				}
			}
			// slipping feet: dust kicked back from under the feet while straining
			if (strain) {
				for (int i = 0; i < 2; ++i) {
					const float t = (float)std::fmod(now * 2.5 + i * 0.5, 1.0);
					const ImVec2 c{ s0.x + spriteW * 0.2f - t * spriteW * 0.6f, s0.y + spriteH - 3.0f - t * 4.0f };
					dl->AddCircleFilled(c, 1.5f + t * 3.0f, IM_COL32(200, 200, 200, (int)(180 * (1.0f - t))));
				}
			}
			// sweat drop, every ~1.6 s while straining
			if (strain && !angry) {
				const float t = (float)std::fmod(now, 1.6) / 1.6f;
				if (t < 0.5f) {
					const ImVec2 d{ s0.x + spriteW * 0.35f, s0.y + spriteH * (0.15f + t * 0.5f) };
					dl->AddCircleFilled(d, 2.5f, kText);
					dl->AddTriangleFilled({ d.x - 2.5f, d.y }, { d.x + 2.5f, d.y }, { d.x, d.y - 5.0f }, kText);
				}
			}
			// steam puffs when angry
			if (angry) {
				for (int i = 0; i < 3; ++i) {
					const float t = (float)std::fmod(now * 1.4 + i / 3.0, 1.0);
					const ImVec2 c{ s0.x + spriteW * (0.25f + 0.25f * i) + std::sin(t * 6.0f + i) * 3.0f, s0.y - t * spriteH * 0.5f };
					dl->AddCircle(c, 2.0f + t * 5.0f, IM_COL32(230, 230, 230, (int)(220 * (1.0f - t))), 12, 1.5f);
				}
			}
			// confetti-ish sparkles on completion
			if (done) {
				for (int i = 0; i < 6; ++i) {
					const float a = (float)(now * 2.0 + i * 1.047);
					const ImVec2 c{ s0.x + spriteW * 0.5f + std::cos(a) * spriteW * 0.7f, s0.y + spriteH * 0.3f + std::sin(a) * spriteH * 0.35f };
					dl->AddLine({ c.x - 3, c.y }, { c.x + 3, c.y }, kText, 1.5f);
					dl->AddLine({ c.x, c.y - 3 }, { c.x, c.y + 3 }, kText, 1.5f);
				}
			}
		}
		ImGui::Dummy({ w + spriteW * 0.6f, rowH });
	}

	/** @brief Sets one top-level settings key of a feature through its JSON round-trip; returns an undo action. */
	std::function<void()> SetFeatureValue(Feature* f, const std::string& key, const nlohmann::json& value)
	{
		nlohmann::json before;
		f->SaveSettings(before);
		nlohmann::json after = before;
		after[key] = value;
		f->LoadSettings(after);
		return [f, before]() mutable { f->LoadSettings(before); };
	}

	/** @brief Default settings JSON of a feature (restores the current values afterwards). */
	nlohmann::json DefaultsOf(Feature* f)
	{
		nlohmann::json cur, def;
		f->SaveSettings(cur);
		if (!f->HasRestoreDefaults())
			return cur;
		f->RestoreDefaultSettings();
		f->SaveSettings(def);
		f->LoadSettings(cur);
		return def;
	}

	// ---------------------------------------------------------------- health analyzer (what is NOT working)
	struct HealthRow
	{
		std::string name, status, hint;
		int level;  // 0 green, 1 yellow, 2 red
		std::function<void()> fix, undo;
	};

	std::vector<std::function<void()>> g_undo;

	void DrawHealthPage()
	{
		using namespace Bearforj;
		ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.health.title", "Health"));
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
		MonoLabel("DIAG // HEALTH");
		ImGui::Separator();
		std::vector<HealthRow> rows;
		for (auto& c : Compat::GetConflictInfos())
			rows.push_back({ c.mod, T("bearforj.health.conflict", "conflict"), ConflictText(c, "fix"), c.severity, {}, {} });
		const auto safe = Compat::GetConfig().safeModeDisabled;
		for (auto* f : Feature::GetFeatureList()) {
			if (!f->IsInMenu() || f->IsHiddenUnreleased())
				continue;
			const std::string n = f->GetShortName();
			const bool bootOn = IsOn(f);
			const int fails = Compat::GetCompileFailures(n);
			HealthRow r{ f->GetDisplayName(), "", "", 0, {}, {} };
			if (std::ranges::find(safe, n) != safe.end() && !bootOn) {
				r = { r.name, T("bearforj.health.safe_off", "off by safe mode"), T("bearforj.health.safe_hint", "Its shader failed to compile. Check the log, clear the shader cache, then turn it back on."), 2,
					[n]() { globals::state->SetFeatureBootEnabled(n, true); }, [n]() { globals::state->SetFeatureBootEnabled(n, false); } };
			} else if (fails > 0) {
				r = { r.name, std::format("{} {}", fails, T("bearforj.health.compile_fail", "shader compile failures")), T("bearforj.health.compile_hint", "Clear the shader cache; if it repeats, turn the feature off and send the log."), 2, {}, {} };
			} else if (bootOn && !f->loaded && !f->installed) {
				r = { r.name, T("bearforj.health.missing", "files missing"), T("bearforj.health.missing_hint", "Its .ini/shaders are not installed. Reinstall Bearforj Shaders."), 2, {}, {} };
			} else if (bootOn && !f->loaded && !f->failedLoadedMessage.empty()) {
				r = { r.name, f->failedLoadedMessage, T("bearforj.health.version_hint", "Version mismatch or failed load. Reinstall, do not mix files from other shader builds."), 2, {}, {} };
			} else if (bootOn && !f->loaded) {
				r = { r.name, T("bearforj.health.restart", "restart needed"), T("bearforj.health.restart_hint", "Turned on this session; restart the game."), 1, {}, {} };
			} else if (!bootOn) {
				r = { r.name, T("bearforj.health.off", "off"), T("bearforj.health.off_hint", "Turned off by you. Turn on if you want it."), 1,
					[n]() { globals::state->SetFeatureBootEnabled(n, true); }, [n]() { globals::state->SetFeatureBootEnabled(n, false); } };
			} else if (f->loaded && bootOn) {
				// enabled but no effect: its own master switch is off, or its strength is zero
				nlohmann::json j;
				f->SaveSettings(j);
				if (!j.is_object())
					continue;
				bool found = false;
				for (const char* k : { "Enable", "Enabled", "enable", "enabled" }) {
					if (j.contains(k) && (j[k].is_boolean() ? !j[k].get<bool>() : (j[k].is_number() && j[k].get<double>() == 0.0))) {
						const std::string key = k;
						const nlohmann::json on = j[k].is_boolean() ? nlohmann::json(true) : nlohmann::json(1);
						r = { r.name, T("bearforj.health.no_effect_switch", "on, but its own Enable switch is off"), T("bearforj.health.no_effect_switch_hint", "The feature costs startup time but draws nothing. Turn its Enable on, or turn the feature off."), 1,
							[f, key, on]() { g_undo.push_back(SetFeatureValue(f, key, on)); }, {} };
						found = true;
						break;
					}
				}
				if (!found) {
					for (const char* k : { "Strength", "Intensity", "strength", "intensity", "Amount", "amount" }) {
						if (j.contains(k) && j[k].is_number() && j[k].get<double>() == 0.0) {
							const std::string key = k;
							auto def = DefaultsOf(f);
							const nlohmann::json v = def.contains(k) && def[k].is_number() && def[k].get<double>() != 0.0 ? def[k] : nlohmann::json(1.0);
							r = { r.name, std::format("{} {} = 0", T("bearforj.health.no_effect_zero", "on, but"), k), std::format("{} {}", T("bearforj.health.no_effect_zero_hint", "Nothing visible at zero. Suggested:"), v.dump()), 1,
								[f, key, v]() { g_undo.push_back(SetFeatureValue(f, key, v)); }, {} };
							found = true;
							break;
						}
					}
				}
				if (!found)
					continue;  // healthy: keep the list short
			} else
				continue;
			rows.push_back(std::move(r));
		}
		std::ranges::stable_sort(rows, [](auto& a, auto& b) { return a.level > b.level; });
		if (rows.empty()) {
			EmoteCard(kProud, "OK // ALL GREEN", T("bearforj.health.ok", "Everything that is on works. Mish Mish is proud."));
			return;
		}
		ImGui::TextDisabled("%s", T("bearforj.health.legend", "Red: broken. Yellow: off or waiting for restart. Healthy features are not listed."));
		if (!g_undo.empty()) {
			if (Pill(T("bearforj.health.undo", "Undo"), 140.0f)) {
				g_undo.back()();
				g_undo.pop_back();
			}
		}
		for (size_t i = 0; i < rows.size(); ++i) {
			auto& r = rows[i];
			ImGui::PushID((int)i);
			const ImU32 col = r.level == 2 ? IM_COL32(230, 70, 70, 255) : r.level == 1 ? IM_COL32(230, 190, 60, 255) : IM_COL32(90, 200, 110, 255);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float h = ImGui::GetTextLineHeight();
			ImGui::GetWindowDrawList()->AddCircleFilled({ p.x + h * 0.5f, p.y + h * 0.5f }, h * 0.35f, col);
			ImGui::Dummy({ h, h });
			ImGui::SameLine();
			ImGui::TextColored(ImColor(kText), "%s", r.name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("// %s", r.status.c_str());
			ImGui::Indent(h + 8.0f);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ImColor(kDim), "%s", r.hint.c_str());
			ImGui::PopTextWrapPos();
			if (r.fix && ImGui::SmallButton(T("bearforj.health.apply", "Apply"))) {
				r.fix();
				if (r.undo)
					g_undo.push_back(r.undo);
			}
			ImGui::Unindent(h + 8.0f);
			ImGui::Spacing();
			ImGui::PopID();
		}
	}

	// ---------------------------------------------------------------- image analyzer
	// ---------------------------------------------------------------- Mish Mish performance assistant
	bool g_perfPageOpen = false;
	void DrawPerfPage()
	{
		namespace P = Bearforj::Perf;
		g_perfPageOpen = true;
		ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.perf.title", "Performance & Diagnostics"));
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140.0f);
		MonoLabel("DIAG // PERF");
		ImGui::Separator();
		const auto& c = P::GetContext();
		const auto& s = P::GetStats();
		ImGui::Text("%s %s (%.0f GB)", T("bearforj.perf.gpu", "GPU:"), c.gpu.c_str(), c.vramGB);
		ImGui::Text("%s %ux%u   %s %s   %s %s", T("bearforj.perf.res", "Resolution:"), c.width, c.height, T("bearforj.perf.upscaler", "Upscaler:"), c.upscaler.c_str(),
			T("bearforj.perf.fg_label", "Frame gen:"), c.fg ? c.frameGen.c_str() : T("bearforj.perf.off_label", "off"));
		ImGui::Text("%s %.0f   %s %.0f   avg %.1f ms   p99 %.1f ms   %s %d", T("bearforj.perf.base", "Base fps:"), s.baseFps, T("bearforj.perf.shown", "shown:"), s.shownFps, s.avgMs, s.p99Ms,
			T("bearforj.perf.stutters", "stutters:"), s.stutters);
		auto& cfg = P::GetConfig();
		BFUI::SliderFloat(T("bearforj.perf.thr_fg", "Minimum base fps with frame gen"), &cfg.thresholdFG, 20.0f, 90.0f, "%.0f");
		BFUI::SliderFloat(T("bearforj.perf.thr_nofg", "Minimum fps without frame gen"), &cfg.thresholdNoFG, 30.0f, 144.0f, "%.0f");
		ImGui::Checkbox(T("bearforj.perf.monitor", "Measure feature cost (GPU timers, sampled)"), &cfg.monitor);
		if (s.low)
			EmoteCard(kWorried, "PERF // LOW", std::format("{} {:.0f} < {:.0f}", T("bearforj.perf.low", "FPS is below your target. Mish Mish has ideas below - nothing changes until you press a button."), s.baseFps, s.targetFps).c_str());

		ImGui::SeparatorText(T("bearforj.perf.costs", "Feature cost (GPU ms, visual value)"));
		const auto costs = P::GetCosts();
		if (costs.empty())
			ImGui::TextDisabled("%s", T("bearforj.perf.measuring", "Measuring... keep this page open a few seconds."));
		for (size_t i = 0; i < costs.size() && i < 12; ++i) {
			auto& fc = costs[i];
			ImGui::Text("%-28s %6.2f ms   %s %d/5", fc.name.c_str(), fc.ms, T("bearforj.perf.value", "value"), fc.value);
		}

		ImGui::SeparatorText(T("bearforj.perf.advisor", "Mish Mish advisor"));
		static float target = 60.0f;
		static std::vector<P::Suggestion> plan;
		ImGui::SetNextItemWidth(160.0f);
		BFUI::SliderFloat(T("bearforj.perf.target", "Target fps"), &target, 30.0f, 144.0f, "%.0f");
		ImGui::SameLine();
		if (ImGui::Button(T("bearforj.perf.more_fps", "I want more FPS")))
			plan = P::Plan(target);
		ImGui::SameLine();
		if (ImGui::Button(T("bearforj.perf.why", "Why is it slow?")))
			plan = P::Plan(0.0f);
		for (size_t i = 0; i < plan.size(); ++i) {
			ImGui::PushID((int)i);
			ImGui::TextWrapped("- %s", plan[i].text.c_str());
			if (plan[i].action != P::Action::kInfo) {
				ImGui::SameLine();
				if (ImGui::SmallButton(T("bearforj.perf.do", "Do it")) && P::Apply(plan[i]))
					plan[i].action = P::Action::kInfo;
			}
			ImGui::PopID();
		}
		if (!plan.empty() && ImGui::Button(T("bearforj.perf.do_all", "Do all"))) {
			for (auto& sug : plan)
				if (sug.action != P::Action::kInfo && P::Apply(sug))
					sug.action = P::Action::kInfo;
		}
		if (P::CanRollback()) {
			ImGui::SameLine();
			if (ImGui::Button(T("bearforj.perf.rollback", "Undo all changes")))
				P::Rollback();
		}

		ImGui::SeparatorText(T("bearforj.perf.diag", "Diagnostics"));
		for (auto* f : Feature::GetFeatureList()) {
			if (!f->IsInMenu() || f->IsHiddenUnreleased())
				continue;
			const int fails = Bearforj::Compat::GetCompileFailures(f->GetShortName());
			const bool bad = !f->loaded || fails > 0;
			ImGui::TextColored(bad ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.6f, 1, 0.6f, 1), "%s %s", bad ? "[x]" : "[ok]", f->GetShortName().c_str());
			if (bad) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", !f->loaded ? (f->failedLoadedMessage.empty() ? T("bearforj.perf.not_loaded", "not loaded / disabled at boot") : f->failedLoadedMessage.c_str()) :
				                                       std::format("{} {}", fails, T("bearforj.perf.compile_fail", "shader compile failures")).c_str());
			}
		}
		static std::string exported;
		if (ImGui::Button(T("bearforj.perf.export", "Export report")))
			exported = P::ExportReport();
		if (!exported.empty())
			ImGui::TextDisabled("%s", exported.c_str());
		ImGui::TextDisabled("%s", T("bearforj.perf.log_hint", "Live log: Documents/My Games/Skyrim Special Edition/SKSE/Bearforj.log"));
	}

	void DrawAnalyzerPage()
	{
		namespace AZ = Bearforj::Analyzer;
		ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.analyzer.title", "Image Analyzer"));
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
		MonoLabel("DIAG // IMAGE");
		ImGui::Separator();
		ImGui::TextDisabled("%s", T("bearforj.analyzer.how", "Close nothing, just stand still: 8 frames of the game picture are read (without the menu) and rated."));
		ImGui::BeginDisabled(AZ::Busy());
		if (Pill(AZ::Busy() ? T("bearforj.analyzer.busy", "Looking...") : T("bearforj.analyzer.run", "Analyze picture"), 220.0f))
			AZ::Request();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (!g_undo.empty() && Pill(T("bearforj.health.undo", "Undo"), 140.0f)) {
			g_undo.back()();
			g_undo.pop_back();
		}
		const auto& r = AZ::GetResult();
		if (!r.valid) {
			if (!r.format.empty())
				EmoteCard(kSurprised, "ERR // FORMAT", T("bearforj.analyzer.unsupported", "This screen format cannot be read (MSAA or unusual HDR format)."));
			return;
		}
		const ImageId emo = r.score >= 90 ? kProud : r.score >= 75 ? kHappy : r.score >= 60 ? kWink : r.score >= 40 ? kThinking : kSurprised;
		ImGui::Spacing();
		ImGui::BeginGroup();
		DrawImageFit(Img(r.score >= 40 ? emo : kHeadGrumpy), { 96, 96 });
		ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::BeginGroup();
		MonoLabel(std::format("SCORE // {:03}", r.score).c_str());
		{
			FontScope big(ThemeManager::FindMonoFont(), ImGui::GetFontSize() * 2.2f);
			ImGui::TextColored(ImColor(kWhite), "%d", r.score);
		}
		ImGui::TextColored(ImColor(kText), "%s", r.verdict.c_str());
		ImGui::EndGroup();
		{
			StyleScope st;
			st.Col(ImGuiCol_PlotHistogram, kText);
			st.Col(ImGuiCol_FrameBg, kBlack);
			ImGui::PlotHistogram("##hist", r.histogram.data(), (int)r.histogram.size(), 0, T("bearforj.analyzer.histogram", "brightness histogram"), 0.0f, 0.12f, { -1, 90 });
		}
		auto metric = [](const char* name, float v, float lo, float hi, const char* fmt) {
			const bool bad = v < lo || v > hi;
			ImGui::TextColored(bad ? ImColor(IM_COL32(230, 190, 60, 255)) : ImColor(IM_COL32(90, 200, 110, 255)), bad ? "!" : "o");
			ImGui::SameLine();
			ImGui::Text(fmt, name, v);
		};
		if (ImGui::BeginTable("##metrics", 2)) {
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_clip", "Clipped highlights"), r.clip * 100, 0, 2, "%s: %.1f%%");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_crush", "Crushed shadows"), r.crush * 100, 0, 4, "%s: %.1f%%");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_exposure", "Exposure"), r.exposure, 0.25f, 0.55f, "%s: %.2f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_contrast", "Contrast"), r.contrast, 0.15f, 0.30f, "%s: %.2f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_sat", "Saturation"), r.saturation, 0.08f, 0.35f, "%s: %.2f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_cast", "Color cast (warm+/cold-)"), r.castRB, -0.06f, 0.06f, "%s: %.3f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_tint", "Green/magenta cast"), r.castGM, -0.04f, 0.04f, "%s: %.3f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_noise", "Noise"), r.noise, 0, 0.03f, "%s: %.3f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_flicker", "Flicker"), r.flicker, 0, 0.01f, "%s: %.3f");
			ImGui::TableNextColumn(); metric(T("bearforj.analyzer.m_haze", "Haze"), r.haze, 0, 0.22f, "%s: %.2f");
			ImGui::EndTable();
		}
		ImGui::Spacing();
		MonoLabel(T("bearforj.analyzer.tips", "TIPS // MISH MISH"));
		if (r.tips.empty())
			ImGui::TextDisabled("%s", T("bearforj.analyzer.no_tips", "Nothing to fix."));
		for (size_t i = 0; i < r.tips.size(); ++i) {
			const auto& t = r.tips[i];
			ImGui::PushID((int)i);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(t.text.c_str());
			ImGui::PopTextWrapPos();
			if (!t.feature.empty()) {
				ImGui::SameLine();
				const auto label = std::format("{} ({} = {:.2f})", T("bearforj.health.apply", "Apply"), t.key, t.value);
				if (ImGui::SmallButton(label.c_str())) {
					for (auto* f : Feature::GetFeatureList())
						if (f->GetShortName() == t.feature && f->loaded)
							g_undo.push_back(SetFeatureValue(f, t.key, t.value));
				}
			}
			ImGui::PopID();
		}
	}

	// ---------------------------------------------------------------- sections
	void DrawHeader(Menu& menu)
	{
		const float logo = ImGui::GetFrameHeight() * 2.6f;
		DrawImageFit(Img(kLogo), { logo, logo });
		ImGui::SameLine();
		ImGui::BeginGroup();
		MonoLabel("SYS.01 // SHADERS-ONLINE");
		{
			FontScope title(menu.GetFont(Menu::FontRole::Title), ImGui::GetFontSize() * 1.9f);
			ImGui::TextColored(ImColor(kWhite), "BearForj Shaders");
		}
		ImGui::EndGroup();

		// Right side: version badge + language switch (EN default, RU full).
		auto* i18n = I18n::GetSingleton();
		const std::string current = i18n->GetCurrentLocale();
		const float bw = ImGui::GetFrameHeight() * 1.6f;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw * 2 - 70.0f - 120.0f);
		ImGui::BeginGroup();
		ImGui::Dummy({ 0, logo * 0.25f });
		{
			StyleScope s;
			s.Col(ImGuiCol_Border, kOutline);
			ImGui::Button(Plugin::BEARFORJ_VERSION_LABEL.data(), { 110.0f, 0 });
		}
		ImGui::SameLine();
		MonoLabel(T("bearforj.ui.lang", "LANG"));
		for (const char* code : { "en", "ru" }) {
			ImGui::SameLine();
			const bool on = current.starts_with(code);
			StyleScope s;
			s.Col(ImGuiCol_Button, on ? kText : kBlack);
			s.Col(ImGuiCol_Text, on ? kBlack : kText);
			s.Col(ImGuiCol_Border, kOutline);
			std::string label = code[0] == 'e' ? "EN" : "RU";
			if (ImGui::Button(label.c_str(), { bw, 0 }) && !on) {
				i18n->SetLocale(code);
				menu.pendingFontReload = true;  // atlas rebuild; all font pointers are re-acquired
			}
		}
		ImGui::EndGroup();
	}

	void DrawTabs()
	{
		StyleScope s;
		s.Var(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 7.0f));
		s.Var(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
		FontScope mono(ThemeManager::FindMonoFont(), ImGui::GetFontSize() * 0.8f);
		for (int i = 0; i < kTabCount; ++i) {
			if (i)
				ImGui::SameLine();
			const bool on = g_tab == i;
			StyleScope t;
			t.Col(ImGuiCol_Border, on ? kText : kBorder);
			t.Col(ImGuiCol_Text, on ? kWhite : IM_COL32(154, 154, 154, 255));
			auto label = std::format("{}##bftab{}", T(kTabs[i].key, kTabs[i].en), i);
			if (ImGui::Button(label.c_str()))
				g_tab = i;
		}
	}

	std::vector<Feature*> FeaturesOf(int tab)
	{
		std::vector<Feature*> out;
		const std::string_view needle = g_search;
		for (auto* f : Feature::GetFeatureList()) {
			if (!f->IsInMenu() || f->IsHiddenUnreleased() || TabFor(f) != tab)
				continue;
			// Advanced mode off: debug/utility tools stay hidden so the menu does not scare newcomers.
			if (!Bearforj::Compat::GetConfig().advancedMode && f->GetCategory() == FeatureCategories::kUtility)
				continue;
			if (!needle.empty()) {
				std::string name = f->GetDisplayName();
				auto lower = [](std::string v) { std::ranges::transform(v, v.begin(), [](unsigned char c) { return (char)std::tolower(c); }); return v; };
				if (lower(name).find(lower(std::string(needle))) == std::string::npos)
					continue;
			}
			out.push_back(f);
		}
		std::ranges::sort(out, [](Feature* a, Feature* b) { return a->GetDisplayName() < b->GetDisplayName(); });
		return out;
	}

	void DrawFeatureList(const std::vector<Feature*>& features)
	{
		ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.ui.features", "Features"));
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110.0f);
		MonoLabel(std::format("FTR.{:02} // LIST", features.size()).c_str());
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##bfsearch", T("bearforj.ui.search", "Search"), g_search, sizeof(g_search));
		ImGui::Spacing();

		auto& selected = g_selected[g_tab];
		const float row = ImGui::GetFrameHeight() * 1.5f;

		auto pseudo = [&](const char* id, const char* label) {
			ImGui::PushID(id);
			if (ImGui::Selectable("##row", selected == id, 0, { 0, row }))
				selected = id;
			ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x + row + 8.0f);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::TextUnformatted(label);
			ImGui::PopID();
		};
		if (g_tab == kSystem) {
			if (selected.empty())
				selected = "#general";
			pseudo("#general", T("bearforj.ui.general", "General"));
			pseudo("#health", T("bearforj.health.title", "Health"));
			pseudo("#analyzer", T("bearforj.analyzer.title", "Image Analyzer"));
			pseudo("#perf", T("bearforj.perf.title", "Performance & Diagnostics"));
			pseudo("#compat", T("bearforj.compat.title", "Compatibility & Beta"));
			if (Bearforj::Compat::GetConfig().advancedMode)
				pseudo("#advanced", T("bearforj.ui.advanced", "Advanced"));
		}
		if (g_tab == kPostProcessing) {
			pseudo("#presets", T("bearforj.presets.tab", "Look presets (weather)"));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T("bearforj.presets.tab_tt", "All Bearforj look presets grouped by set. Pick one to lock it, or let the weather switch them."));
		}

		for (auto* f : features) {
			const std::string shortName = f->GetShortName();
			const bool locked = f->IsAlwaysEnabled() || f->IsCore();
			const bool on = IsOn(f);
			if (selected.empty())
				selected = shortName;
			ImGui::PushID(shortName.c_str());
			const ImVec2 rowStart = ImGui::GetCursorPos();
			if (ImGui::Selectable("##row", selected == shortName, ImGuiSelectableFlags_AllowOverlap, { 0, row }))
				selected = shortName;
			FeatureTooltip(f, on, locked);
			ImGui::SetCursorPos({ rowStart.x + 4.0f, rowStart.y + row * 0.08f });
			DrawImageFit(Img(on ? kHeadHappy : kHeadGrumpy), { row * 0.84f, row * 0.84f }, on ? 1.0f : 0.55f);
			ImGui::SameLine();
			ImGui::SetCursorPosY(rowStart.y + (row - ImGui::GetTextLineHeight()) * 0.5f);
			std::string name = f->GetDisplayName();
			if (f->loaded != on)
				name += std::format("  ({})", T("bearforj.ui.restart_pending", "restart needed"));
			ImGui::TextColored(ImColor(on ? kText : IM_COL32(122, 122, 122, 255)), "%s", name.c_str());
			const float sw = ImGui::GetFrameHeight() * 0.8f * 1.9f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sw - 6.0f);
			ImGui::SetCursorPosY(rowStart.y + (row - ImGui::GetFrameHeight() * 0.8f) * 0.5f);
			if (Switch("##sw", on, locked))
				globals::state->SetFeatureBootEnabled(shortName, !on);
			ImGui::SetCursorPos({ rowStart.x, rowStart.y + row + ImGui::GetStyle().ItemSpacing.y });
			ImGui::Dummy({ 0.0f, 0.0f });  // ImGui 1.9x: a cursor move must be followed by an item (no boundary-extension assert)
			ImGui::PopID();
		}

		if (features.empty() && g_tab != kSystem) {
			ImGui::Spacing();
			if (g_search[0])
				EmoteCard(kSurprised, "EMPTY // SEARCH", T("bearforj.ui.empty_search", "Nothing found.\nTry another word, Mish Mish is looking."));
			else
				EmoteCard(kThinking, "EMPTY // TAB", T("bearforj.ui.empty_tab", "No features in this tab yet.\nMish Mish is already thinking about it."));
		}
	}

	void DrawSettingsPanel(Menu& menu, const std::vector<Feature*>& features)
	{
		const auto& selected = g_selected[g_tab];
		if (g_tab == kSystem && selected == "#general") {
			ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.ui.general", "General"));
			menu.DrawGeneralSettings();  // Shaders / Keybindings / Interface (Behavior -> Language)
			return;
		}
		if (g_tab == kSystem && selected == "#analyzer") {
			DrawAnalyzerPage();
			return;
		}
		if (g_tab == kSystem && selected == "#perf") {
			DrawPerfPage();
			return;
		}
		if (g_tab == kSystem && selected == "#health") {
			DrawHealthPage();
			return;
		}
		if (g_tab == kSystem && selected == "#compat") {
			DrawCompatPage();
			return;
		}
		if (g_tab == kPostProcessing && selected == "#presets") {
			DrawPresetBrowser();
			return;
		}
		if (g_tab == kSystem && selected == "#advanced") {
			ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.ui.advanced", "Advanced"));
			menu.DrawAdvancedSettings();
			return;
		}
		Feature* feature = nullptr;
		size_t index = 0;
		for (size_t i = 0; i < features.size(); ++i)
			if (features[i]->GetShortName() == selected) {
				feature = features[i];
				index = i + 1;
			}
		if (!feature) {
			EmoteCard(kWink, "SELECT // FEATURE", T("bearforj.ui.empty_select", "Pick a feature on the left."));
			return;
		}
		{
			FontScope h(menu.GetFont(Menu::FontRole::Heading), ImGui::GetFontSize() * 1.45f);
			ImGui::TextColored(ImColor(kWhite), "%s", feature->GetDisplayName().c_str());
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
		MonoLabel(std::format("FEAT.{:02} // PARAMS", index).c_str());
		ImGui::Separator();
		ImGui::Spacing();
		if (feature->loaded) {
			ImGui::PushID(feature->GetShortName().c_str());
			BFUI::SetContext(feature->GetShortName());
			feature->DrawSettings();
			ImGui::PopID();
		} else {
			EmoteCard(kThinking, "OFFLINE // FEATURE", T("bearforj.ui.not_loaded", "Feature is not loaded."));
			feature->DrawUnloadedUI();
		}
	}

	void ResetAllToDefaults()
	{
		for (auto* f : Feature::GetFeatureList()) {
			if (f->loaded && f->HasRestoreDefaults())
				f->RestoreDefaultSettings();
			if (!f->IsAlwaysEnabled() && !f->IsCore())
				globals::state->SetFeatureBootEnabled(f->GetShortName(), !f->IsDisabledByDefault());
		}
	}

	void DrawFooter()
	{
		const float w = 190.0f;
		if (Pill(T("bearforj.ui.save", "Save"), w))
			globals::state->Save();
		ImGui::SameLine();
		if (Pill(T("bearforj.ui.reset_defaults", "Reset to defaults"), w))
			g_requestReset = true;
		ImGui::SameLine();
		auto* cache = globals::shaderCache;
		const bool busy = cache && (cache->IsCapturingActiveShaders() || cache->IsAwaitingMenuCloseCapture());
		ImGui::BeginDisabled(busy || !cache);
		if (Pill(T("bearforj.ui.clear_cache", "Clear shader cache"), w))
			Util::RequestClearShaderCacheConfirmation(Util::ResolveShaderCacheClearScope());
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", Util::GetClearShaderCacheTooltip());

		// Loading state: Mish Mish at work + compile progress.
		if (cache) {
			const auto total = cache->GetTotalTasks();
			const auto done = cache->GetCompletedTasks();
			const bool compiling = cache->IsCompiling() && total > 0;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 360.0f);
			if (compiling)
				DrawImageFit(Img(kGeneration), { ImGui::GetFrameHeight() * 1.3f, ImGui::GetFrameHeight() * 1.3f });
			else
				ImGui::Dummy({ ImGui::GetFrameHeight() * 1.3f, 1 });
			ImGui::SameLine();
			const std::string overlay = compiling ? std::format("{} {}/{}", T("bearforj.ui.compiling", "COMPILING"), done, total) : T("bearforj.ui.ready", "READY");
			StyleScope s;
			s.Col(ImGuiCol_PlotHistogram, IM_COL32(58, 58, 58, 255));
			s.Col(ImGuiCol_FrameBg, kBlack);
			s.Col(ImGuiCol_Border, kOutline);
			ImGui::ProgressBar(compiling ? (float)done / (float)std::max<uint64_t>(total, 1) : 1.0f, { 300.0f, 0 }, overlay.c_str());
		}

		if (g_requestReset) {
			ImGui::OpenPopup("##bfreset");
			g_requestReset = false;
		}
		if (ImGui::BeginPopupModal("##bfreset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
			EmoteCard(kSurprised, "RESET // DEFAULTS", T("bearforj.ui.reset_confirm_text", "Every feature returns to its default settings and is turned on. Press Save to write it to disk."));
			if (Pill(T("bearforj.ui.yes", "Yes, reset"), 160.0f)) {
				ResetAllToDefaults();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (Pill(T("bearforj.ui.cancel", "Cancel"), 160.0f))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
}

namespace BearforjMenu
{
	static StyleScope* g_windowStyle = nullptr;
	void PushWindowStyle()
	{
		if (!g_windowStyle) {
			g_windowStyle = new StyleScope();
			PushBlackWhite(*g_windowStyle);
			g_windowStyle->Col(ImGuiCol_TitleBg, kBlack);
			g_windowStyle->Col(ImGuiCol_TitleBgActive, kBlack);
			g_windowStyle->Col(ImGuiCol_TitleBgCollapsed, kBlack);
			g_windowStyle->Col(ImGuiCol_Border, kOutline);
		}
	}
	void PopWindowStyle()
	{
		delete g_windowStyle;  // ~StyleScope pops
		g_windowStyle = nullptr;
	}

	void DrawReportToast()
	{
		std::string text;
		float a = 0;
		if (!Bearforj::Report::ToastActive(text, a))
			return;
		const auto* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({ vp->Pos.x + vp->Size.x - 24.0f, vp->Pos.y + 24.0f }, ImGuiCond_Always, { 1.0f, 0.0f });
		ImGui::SetNextWindowBgAlpha(0.85f * a);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
		if (ImGui::Begin("##bfreporttoast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
			DrawImageFit(Img(kWink), { 40, 40 });
			ImGui::SameLine();
			ImGui::TextUnformatted(text.c_str());
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	bool ConsumePerfPageOpen()
	{
		const bool o = g_perfPageOpen;
		g_perfPageOpen = false;
		return o;
	}

	void Render(Menu& a_menu)
	{
		LoadImages();
		StyleScope style;
		PushBlackWhite(style);

		// Mockup (bearforj_ui/mockup_en_v2.png): faint big logo watermark bottom-right + tick ruler under the header.
		{
			auto* dl = ImGui::GetWindowDrawList();
			const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
			if (const auto* lg = Img(kLogo); lg && lg->size.y > 0) {
				const float h = ws.y * 0.75f, w = h * lg->size.x / lg->size.y;
				const ImVec2 a0{ wp.x + ws.x - w * 0.8f, wp.y + ws.y - h * 0.9f };
				dl->AddImage((ImTextureID)lg->srv, a0, { a0.x + w, a0.y + h }, { 0, 0 }, { 1, 1 }, IM_COL32(255, 255, 255, 18));
			}
		}

		DrawHeader(a_menu);
		{
			auto* dl = ImGui::GetWindowDrawList();
			const ImVec2 c = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			dl->AddLine({ c.x, c.y + 6 }, { c.x + w, c.y + 6 }, kBorder);
			for (float i = 0; i <= w; i += 10.0f)
				dl->AddLine({ c.x + i, c.y + 6 }, { c.x + i, c.y + 6 - (((int)i % 50) == 0 ? 6.0f : 3.0f) }, kBorder);
			ImGui::Dummy({ w, 10.0f });
		}
		DrawConflictBanner();
		ImGui::Spacing();
		DrawTabs();

		const float footer = ImGui::GetFrameHeight() * 1.15f + ImGui::GetStyle().ItemSpacing.y * 4;
		const auto features = FeaturesOf(g_tab);
		if (ImGui::BeginChild("##bfbody", { 0, -footer }, ImGuiChildFlags_Borders)) {
			const float left = std::max(300.0f, ImGui::GetContentRegionAvail().x * 0.28f);
			if (ImGui::BeginChild("##bflist", { left, 0 }, ImGuiChildFlags_Borders))
				DrawFeatureList(features);
			ImGui::EndChild();
			ImGui::SameLine();
			if (ImGui::BeginChild("##bfsettings", { 0, 0 }, ImGuiChildFlags_None))
				DrawSettingsPanel(a_menu, features);
			ImGui::EndChild();
		}
		ImGui::EndChild();
		ImGui::Spacing();
		DrawFooter();
	}

	void DrawCompileOverlay(float a_percent, const char* a_overlay)
	{
		LoadImages();
		StyleScope style;
		PushBlackWhite(style);
		DrawCompileBar(a_percent, a_overlay);
	}

	void ReleaseAssets()
	{
		for (auto& i : g_images)
			i.Release();
		g_imagesLoaded = false;
	}

	void ReloadAssets()
	{
		ReleaseAssets();
	}
}
