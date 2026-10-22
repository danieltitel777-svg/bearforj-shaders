#include "Bearforj/TipUI.h"

#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "I18n/I18n.h"
#include "Menu/BearforjMenu.h"
#include "Utils/FileSystem.h"

namespace BFUI
{
	namespace
	{
		std::string g_feature;
		std::unordered_map<std::string, std::string> g_map;
		bool g_loaded = false;

		void Load()
		{
			g_loaded = true;
			std::ifstream in(Util::PathHelpers::GetCommunityShaderPath() / "Bearforj" / "Tooltips.json");
			auto j = nlohmann::json::parse(in, nullptr, false);
			if (!j.is_object())
				return;
			for (auto& [k, v] : j.items())
				if (v.is_string())
					g_map[k] = v.get<std::string>();
		}

		const char* Tune(const std::string& a_label)
		{
			std::string l = a_label;
			for (char& c : l)
				c = (char)std::tolower((unsigned char)c);
			auto has = [&](const char* needle) { return l.find(needle) != std::string::npos; };
			if (has("enable") || has("toggle") || has("включ") || has("выключ"))
				return T("bearforj.tune.enable", "Turns this part on or off. If the picture does not change, restart the game, then press Save.");
			if (has("sample") || has("quality") || has("step") || has("slice") || has("resolution") || has("качеств") || has("шаг") || has("разреш"))
				return T("bearforj.tune.quality", "Quality versus speed. Raise it a little at a time: the picture gets cleaner and the frame rate drops. Leave it near the default if you are unsure.");
			if (has("radius") || has("distance") || has("range") || has("width") || has("size") || has("радиус") || has("дальн") || has("ширин") || has("размер"))
				return T("bearforj.tune.reach", "How far the effect reaches. Larger values cover more of the scene and usually cost more. Move in small steps and watch the picture.");
			if (has("intensity") || has("strength") || has("power") || has("amount") || has("scale") || has("boost") || has("сил") || has("мощн") || has("множ"))
				return T("bearforj.tune.strength", "How strong the effect is. Higher is more visible. If it looks fake, lower it toward the default.");
			if (has("threshold") || has("cutoff") || has(" min") || has("max") || has("порог") || has("мин") || has("макс"))
				return T("bearforj.tune.limit", "The point where the effect starts or stops. Nudge it until the artifact (halos, flicker, missing detail) goes away, then stop.");
			if (has("speed") || has("rate") || has("time") || has("duration") || has("fade") || has("скорост") || has("врем"))
				return T("bearforj.tune.speed", "How fast it changes. Faster reacts sooner and can flicker; slower looks smoother.");
			if (has("color") || has("colour") || has("tint") || has("цвет") || has("оттен"))
				return T("bearforj.tune.color", "Color of this effect. Small shifts read as natural; big shifts look like a filter.");
			if (has("debug") || has("show") || has("visual") || has("overlay") || has("отлад"))
				return T("bearforj.tune.debug", "A debug view. Turn it off while playing or the picture stays technical.");
			if (has("mode") || has("type") || has("method") || has("preset") || has("режим") || has("тип"))
				return T("bearforj.tune.mode", "Picks how the effect is calculated. Try the next entry, look at the picture, and keep the one that stays sharp without killing the frame rate.");
			return T("bearforj.tune.generic", "Tune this in small steps and watch the game picture. Stay near the default unless you know you want more or less of the effect, then press Save.");
		}
	}

	void SetContext(const std::string& a_featureShortName) { g_feature = a_featureShortName; }

	void Tip(const char* a_label)
	{
		if (!a_label || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			return;
		if (!g_loaded)
			Load();
		std::string label = a_label;
		if (auto p = label.find("##"); p != std::string::npos)
			label.resize(p);
		while (!label.empty() && (label.back() == ' ' || label.back() == ':'))
			label.pop_back();
		const char* text = nullptr;
		if (auto it = g_map.find(g_feature + "|" + label); it != g_map.end())
			text = T(it->second.c_str(), "");
		const bool specific = text && *text;
		if (!specific)
			text = Tune(label);
		ImGui::BeginTooltip();
		if (auto* icon = BearforjMenu::TooltipMascot()) {
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(p, { p.x + 48.0f, p.y + 48.0f }, IM_COL32(255, 255, 255, 255), 6.0f);
			ImGui::GetWindowDrawList()->AddImage((ImTextureID)icon, p, { p.x + 48.0f, p.y + 48.0f });
			ImGui::Dummy({ 48.0f, 48.0f });
			ImGui::SameLine();
		}
		ImGui::BeginGroup();
		if (!specific)
			ImGui::TextUnformatted(label.c_str());
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetFontSize() * 28.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::EndTooltip();
	}
}
