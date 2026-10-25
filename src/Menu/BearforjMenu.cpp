#include "BearforjMenu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Feature.h"
#include "FeatureCategories.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/FontAtlasState.h"
#include "Menu/ThemeManager.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

// Bearforj 0.6 look (black & white, logo, Mish Mish) drawn over the vb 1.0 menu code.
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
		kSword,
		kIdea,
		kSinging,
		kPointing,
		kImageCount
	};

	constexpr std::array<const char*, kImageCount> kImageFiles = {
		"logo.png",
		"emotions/thumbs_up.png",
		"emotions/thinking.png",
		"emotions/thinking.png",
		"emotions/surprised.png",
		"emotions/cheering.png",
		"emotions/proud.png",
		"emotions/wink.png",
		"emotions/hourglass.png",
		"emotions/wrench.png",
		"emotions/sleeping.png",
		"emotions/waving.png",
		"emotions/bug.png",
		"emotions/sword.png",
		"emotions/idea.png",
		"emotions/singing.png",
		"emotions/pointing.png",
	};

	std::array<Image, kImageCount> g_images;
	bool g_imagesLoaded = false;

	void LoadImages()
	{
		if (g_imagesLoaded || !globals::d3d::device)
			return;
		g_imagesLoaded = true;
		const auto root = Util::PathHelpers::GetInterfacePath() / "Bearforj";
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
			{ "ScreenSpacePhysicalParticles", kParticles }, { "PhysicalBlood", kParticles }, { "PhysicalLeaves", kParticles },
			{ "PhysicalWorldParticles", kParticles }, { "FootstepParticles", kParticles }, { "GrassCollision", kParticles },
			{ "WetnessEffects", kSkyWeather }, { "RainRefraction", kSkyWeather }, { "RainRendering", kSkyWeather },
			{ "SkySync", kSkyWeather }, { "CloudShadows", kSkyWeather }, { "ExponentialHeightFog", kSkyWeather },
			{ "ProceduralSun", kSkyWeather }, { "ProceduralAurora", kSkyWeather }, { "VolumetricLighting", kSkyWeather },
			{ "HairBacklighting", kCharacter }, { "HairSpecular", kCharacter }, { "Skin", kCharacter }, { "FurShells", kCharacter },
			{ "SnowDeformation", kLandscape }, { "WaterWaves", kWater }, { "UnifiedWater", kWater },
		};
		if (auto it = overrides.find(s); it != overrides.end())
			return it->second;
		const auto c = f->GetCategory();
		if (c == FeatureCategories::kCharacters)
			return kCharacter;
		if (c == FeatureCategories::kSky)
			return kSkyWeather;
		if (c == FeatureCategories::kLighting || c == FeatureCategories::kMaterials || c == FeatureCategories::kShadows)
			return kLighting;
		if (c == FeatureCategories::kLandscapeAndTextures || c == FeatureCategories::kGrassAndFoliage)
			return kLandscape;
		if (c == FeatureCategories::kWater)
			return kWater;
		if (c == FeatureCategories::kPostProcessing || c == FeatureCategories::kDisplay)
			return kPostProcessing;
		return kSystem;
	}

	bool IsOn(Feature* f)
	{
		if (f->IsAlwaysEnabled())
			return true;
		if (f->loaded && f->SupportsLiveToggle())
			return f->IsEnabled();
		return !globals::state->IsFeatureDisabled(f->GetShortName());
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

	// ---------------------------------------------------------------- compile overlay: static Mish Mish beside the bar
	struct CompileFade
	{
		ImageId shown = kHeadHappy;
		ImageId previous = kHeadHappy;
		double swapped = -1.0;
		float lastPct = -1.0f;
		double lastMove = 0.0;
		double cheerUntil = 0.0;
	};

	ImageId CompileFace(CompileFade& st, float pct, double now)
	{
		if (st.lastPct < 0.0f || pct + 0.05f < st.lastPct) {
			st.lastPct = pct;
			st.lastMove = now;
			st.cheerUntil = 0.0;
		}
		if (pct > st.lastPct + 0.0004f) {
			if (now - st.lastMove > 0.6)
				st.cheerUntil = now + 1.15;
			st.lastPct = pct;
			st.lastMove = now;
		}
		const bool failed = globals::shaderCache && globals::shaderCache->GetCurrentFailedCount() > 0;
		if (pct >= 0.999f)
			return kHappy;
		const double stall = now - st.lastMove;
		if (failed && stall > 2.0)
			return kWorried;
		if (now < st.cheerUntil)
			return kHappy;
		if (stall > 8.0)
			return kPushSheet;
		if (stall > 4.0)
			return kThinking;
		if (stall > 1.8)
			return kGeneration;
		return kHeadHappy;
	}

	void DrawStillSprite(ImDrawList* dl, const Image* img, ImVec2 box0, float box, float alpha)
	{
		if (!dl || !img || !img->srv || img->size.x <= 0.0f || img->size.y <= 0.0f || alpha <= 0.0f)
			return;
		const float s = box / std::max(img->size.x, img->size.y);
		const float w = img->size.x * s;
		const float h = img->size.y * s;
		const ImVec2 o{ box0.x + (box - w) * 0.5f, box0.y + (box - h) * 0.5f };
		dl->AddImage((ImTextureID)img->srv, o, { o.x + w, o.y + h }, { 0, 0 }, { 1, 1 }, IM_COL32(255, 255, 255, (int)(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)));
	}

	void DrawCompileBar(float a_percent, const char* a_overlay, const char* a_caption)
	{
		const float scale = std::max(Util::GetUIScale(), 0.5f);
		const float mascot = 112.0f * scale;
		const float gap = 12.0f * scale;
		const float barW = 520.0f * scale;
		const float barH = ImGui::GetFrameHeight() * 0.95f;
		const float pct = std::clamp(std::isfinite(a_percent) ? a_percent : 0.0f, 0.0f, 1.0f);
		static CompileFade fade;
		static bool logged = false;
		const double now = ImGui::GetTime();
		const ImageId face = CompileFace(fade, pct, now);
		if (face != fade.shown) {
			fade.previous = fade.shown;
			fade.shown = face;
			fade.swapped = now;
		}
		const float fadeSec = 0.22f;
		const float t = fadeSec <= 0.0f || fade.swapped < 0.0 ? 1.0f : std::clamp((float)((now - fade.swapped) / fadeSec), 0.0f, 1.0f);
		const Image* cur = Img(fade.shown);
		const Image* prev = Img(fade.previous);
		if (!logged) {
			logged = true;
			logger::info("[Bearforj UI] compile overlay: static Mish Mish {} px (sprite {})", (int)mascot, cur ? "loaded" : "MISSING");
		}

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float rowH = std::max(mascot, barH);
		const ImVec2 sprite0{ origin.x, origin.y + (rowH - mascot) * 0.5f };
		const ImVec2 bar0{ origin.x + mascot + gap, origin.y + (rowH - barH) * 0.5f };
		const ImVec2 bar1{ bar0.x + barW, bar0.y + barH };
		auto* dl = ImGui::GetWindowDrawList();
		if (t < 1.0f)
			DrawStillSprite(dl, prev, sprite0, mascot, 1.0f - t);
		DrawStillSprite(dl, cur, sprite0, mascot, t);

		const float fillX = bar0.x + barW * pct;
		dl->AddRectFilled(bar0, bar1, kBlack, barH * 0.5f);
		if (pct > 0.0f)
			dl->AddRectFilled(bar0, { std::max(fillX, bar0.x + barH), bar1.y }, kText, barH * 0.5f);
		dl->AddRect(bar0, bar1, kOutline, barH * 0.5f);
		if (a_overlay && *a_overlay) {
			const ImVec2 ts = ImGui::CalcTextSize(a_overlay);
			const ImVec2 tp{ bar0.x + (barW - ts.x) * 0.5f, bar0.y + (barH - ts.y) * 0.5f };
			dl->PushClipRect(bar0, { fillX, bar1.y }, true);
			dl->AddText(tp, kBlack, a_overlay);
			dl->PopClipRect();
			dl->PushClipRect({ fillX, bar0.y }, bar1, true);
			dl->AddText(tp, kText, a_overlay);
			dl->PopClipRect();
		}

		ImGui::Dummy({ mascot + gap + barW, rowH + 6.0f });
		if (a_caption && *a_caption)
			MonoLabel(a_caption, kDim);
	}

	// ---------------------------------------------------------------- sections
	void DrawHeader(Menu& menu)
	{
		const Image* logo = Img(kLogo);
		const float box = 168.0f * std::max(Util::GetUIScale(), 0.5f);
		const float aspect = logo && logo->size.y > 0.0f ? logo->size.x / logo->size.y : 1.0f;
		const float logoW = box * aspect;
		const float logoH = box;
		DrawImageFit(logo, { logoW, logoH });
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::Dummy({ 0.0f, logoH * 0.18f });
		MonoLabel("SYS.01 // SHADERS-ONLINE");
		{
			FontScope title(menu.GetFont(Menu::FontRole::Title), ImGui::GetFontSize() * 1.25f);
			ImGui::TextColored(ImColor(kWhite), "%s", T("bearforj.ui.brand", "Bearforj Shaders"));
		}
		ImGui::EndGroup();

		// Right side: version badge + language switch (EN default, RU full).
		auto* i18n = I18n::GetSingleton();
		const std::string current = i18n->GetCurrentLocale();
		const float bw = ImGui::GetFrameHeight() * 1.6f;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw * 2 - 70.0f - 120.0f);
		ImGui::BeginGroup();
		ImGui::Dummy({ 0, logoH * 0.25f });
		{
			StyleScope s;
			s.Col(ImGuiCol_Border, kOutline);
			FontScope title(menu.GetFont(Menu::FontRole::Title), ImGui::GetFontSize() * 0.8f);
			ImGui::Button(Plugin::VERSION_LABEL.data(), { 110.0f, 0 });
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
				MenuFonts::GetAtlasState().pendingFontReload = true;  // atlas rebuild; all font pointers are re-acquired
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
			pseudo("#advanced", T("bearforj.ui.advanced", "Advanced"));
		}

		for (auto* f : features) {
			const std::string shortName = f->GetShortName();
			const bool locked = f->IsAlwaysEnabled();
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
			if (!f->SupportsLiveToggle() && f->loaded != on)
				name += std::format("  ({})", T("bearforj.ui.restart_pending", "restart needed"));
			ImGui::TextColored(ImColor(on ? kText : IM_COL32(122, 122, 122, 255)), "%s", name.c_str());
			const float sw = ImGui::GetFrameHeight() * 0.8f * 1.9f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sw - 6.0f);
			ImGui::SetCursorPosY(rowStart.y + (row - ImGui::GetFrameHeight() * 0.8f) * 0.5f);
			if (Switch("##sw", on, locked))
				f->ToggleAtBootSetting();
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
			menu.BeginFeatureEditTracking(feature);
			const bool editedBefore = ImGui::GetCurrentContext()->ActiveIdHasBeenEditedThisFrame;
			feature->DrawSettings();
			menu.EndFeatureEditTracking(feature, !editedBefore && ImGui::GetCurrentContext()->ActiveIdHasBeenEditedThisFrame);
			menu.IgnoreSettingEditsThisFrame();
			ImGui::PopID();
		} else {
			EmoteCard(kThinking, "OFFLINE // FEATURE", T("bearforj.ui.not_loaded", "Feature is not loaded."));
			feature->DrawUnloadedUI();
		}
	}

	void ResetAllToDefaults()
	{
		for (auto* f : Feature::GetFeatureList()) {
			if (f->loaded)
				f->RestoreDefaultSettings();
			if (!f->IsAlwaysEnabled() && globals::state->IsFeatureDisabled(f->GetShortName()) != f->IsDisabledByDefault())
				f->ToggleAtBootSetting();
		}
	}

	void DrawFooter()
	{
		const float w = 190.0f;
		if (Pill(T("bearforj.ui.save", "Save"), w))
			Menu::GetSingleton()->SaveSettings();
		ImGui::SameLine();
		if (Pill(T("bearforj.ui.reset_defaults", "Reset to defaults"), w))
			g_requestReset = true;
		ImGui::SameLine();
		auto* cache = globals::shaderCache;
		ImGui::BeginDisabled(!cache);
		if (Pill(T("bearforj.ui.clear_cache", "Clear shader cache"), w))
			Util::RequestClearShaderCacheConfirmation();
		ImGui::EndDisabled();

		// Loading state: Mish Mish at work + compile progress.
		if (cache) {
			const auto total = cache->GetTotalTasks();
			const auto done = cache->GetCompletedTasks();
			const bool compiling = cache->IsCompiling() && total > 0;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 360.0f);
			if (compiling)
				DrawImageFit(Img(kGeneration) ? Img(kGeneration) : Img(kHappy), { ImGui::GetFrameHeight() * 1.6f, ImGui::GetFrameHeight() * 1.6f });
			else
				ImGui::Dummy({ ImGui::GetFrameHeight() * 1.6f, 1 });
			ImGui::SameLine();
			const std::string overlay = compiling ? std::format("{} {}/{}", T("bearforj.ui.compiling", "COMPILING"), done, total) : T("bearforj.ui.ready", "READY");
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(overlay.c_str());
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

	void DrawCompileOverlay(float a_percent, const char* a_overlay, const char* a_caption)
	{
		LoadImages();
		StyleScope style;
		PushBlackWhite(style);
		DrawCompileBar(a_percent, a_overlay, a_caption);
	}

	ID3D11ShaderResourceView* TooltipMascot()
	{
		LoadImages();
		if (const Image* face = Img(kThinking))
			return face->srv;
		if (const Image* face = Img(kHeadHappy))
			return face->srv;
		return nullptr;
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
