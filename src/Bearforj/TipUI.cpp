#include "Bearforj/TipUI.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "I18n/I18n.h"
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
		while (!label.empty() && label.back() == ' ')
			label.pop_back();
		auto it = g_map.find(g_feature + "|" + label);
		if (it == g_map.end())
			return;
		const char* text = T(it->second.c_str(), "");
		if (!text || !*text)
			return;
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}
