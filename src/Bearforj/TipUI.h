#pragma once

#include <imgui.h>
#include <string>
#include <utility>

/**
 * @brief Bearforj tooltip wrappers. Every feature settings widget goes through BFUI::<Widget>, which calls the
 * ImGui widget and then shows the generated Bearforj tooltip (what it does, range, perf cost; ru/en) on hover.
 * Lookup key: "<current feature short name>|<visible label>" from Bearforj/Tooltips.json.
 */
namespace BFUI
{
	/** @brief Feature whose settings are being drawn (set by the Bearforj menu). */
	void SetContext(const std::string& a_featureShortName);
	/** @brief Shows the tooltip for the last item if hovered. */
	void Tip(const char* a_label);

#define BFUI_WRAP(NAME)                                                     \
	template <class... A>                                                   \
	inline auto NAME(const char* a_label, A&&... a)                         \
	{                                                                       \
		auto r = ImGui::NAME(a_label, std::forward<A>(a)...);               \
		Tip(a_label);                                                       \
		return r;                                                           \
	}
	BFUI_WRAP(SliderFloat)
	BFUI_WRAP(SliderInt)
	BFUI_WRAP(Checkbox)
	BFUI_WRAP(DragFloat)
	BFUI_WRAP(DragInt)
	BFUI_WRAP(Combo)
	BFUI_WRAP(ColorEdit3)
	BFUI_WRAP(ColorEdit4)
	BFUI_WRAP(SliderFloat2)
	BFUI_WRAP(SliderFloat3)
	BFUI_WRAP(SliderAngle)
	BFUI_WRAP(InputFloat)
	BFUI_WRAP(RadioButton)
	BFUI_WRAP(SliderScalar)
#undef BFUI_WRAP
}
