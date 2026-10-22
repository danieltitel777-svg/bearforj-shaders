#pragma once
// Bearforj 0.6: minimal stand-ins for the Community Shaders CS Editor browser widgets and Tabler icon
// font that the advanced-skin editor (jiayev/skyrim-community-shaders@advanced-skin) was written against.
// Open Shaders 2.19 has neither, so icons render as short text labels with the regular UI font.

#include <imgui.h>
#include <imgui_stdlib.h>

#include "Utils/UI.h"

#define ICON_TI_ALERT_TRIANGLE "!"
#define ICON_TI_ARROW_BACK_UP "<"
#define ICON_TI_ARROW_FORWARD_UP ">"
#define ICON_TI_CHEVRON_RIGHT ">"
#define ICON_TI_CROSSHAIR "+"
#define ICON_TI_DROPLET "~"
#define ICON_TI_FOLDER_OPEN "..."
#define ICON_TI_REFRESH "R"
#define ICON_TI_RESTORE "<<"
#define ICON_TI_TOOLS "T"
#define ICON_TI_USER "@"
#define ICON_TI_X "x"
#define ICON_TI_TRASH "Del"
#define ICON_TI_DEVICE_FLOPPY "Save"
#define ICON_TI_COPY "Copy"



#include <cstdarg>
#include <string>

namespace Util
{
	inline constexpr ImGuiHoveredFlags kTooltipWhenDisabled = ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled;

	inline bool SegmentedControl(const char* a_id, const char* const* a_labels, int a_count, int& a_selected, int a_marked = -1, bool a_muted = false)
	{
		(void)a_marked;
		(void)a_muted;
		bool changed = false;
		ImGui::PushID(a_id);
		for (int i = 0; i < a_count; ++i) {
			if (i > 0)
				ImGui::SameLine(0.0f, 2.0f);
			if (ImGui::RadioButton(a_labels[i], a_selected == i)) {
				changed = a_selected != i;
				a_selected = i;
			}
		}
		ImGui::PopID();
		return changed;
	}

	inline bool StatusBanner(const char* a_icon, const char* a_message, const ImVec4& a_color)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, a_color);
		ImGui::TextWrapped("%s %s", a_icon ? a_icon : "", a_message);
		ImGui::PopStyleColor();
		return ImGui::IsItemClicked();
	}

	inline void Explainer(const char* a_label, const char* a_text)
	{
		if (ImGui::TreeNode(a_label)) {
			ImGui::TextWrapped("%s", a_text);
			ImGui::TreePop();
		}
	}

	inline void ToolbarDivider(bool a_continueLine = true)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		if (a_continueLine)
			ImGui::SameLine();
	}

	inline void DrawIconCircle(ImVec2 center, float radius, ImU32 color, bool filled)
	{
		auto* dl = ImGui::GetWindowDrawList();
		if (filled)
			dl->AddCircleFilled(center, radius, color);
		else
			dl->AddCircle(center, radius, color);
	}

	inline void DrawInlineIndicatorDot(ImU32 color, bool filled)
	{
		const float h = ImGui::GetTextLineHeight();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(h, h));
		DrawIconCircle(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.25f, color, filled);
	}

	inline void PushTintedFrameStyle(const ImVec4& color)
	{
		ImGui::PushStyleColor(ImGuiCol_Border, color);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
	}

	inline void PopTintedFrameStyle()
	{
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	namespace Text
	{
		inline void Secondary(const char* fmt, ...)
		{
			va_list args;
			va_start(args, fmt);
			ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetDisabled());
			ImGui::TextV(fmt, args);
			ImGui::PopStyleColor();
			va_end(args);
		}
		inline void WrappedSecondary(const char* fmt, ...)
		{
			va_list args;
			va_start(args, fmt);
			ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetDisabled());
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextV(fmt, args);
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			va_end(args);
		}
	}
}

namespace Util::Colors
{
	/** @brief Accent / secondary are CS-only theme slots; map to Open equivalents. */
	inline ImVec4 GetAccent() { return GetInfo(); }
	inline ImVec4 GetSecondary() { return GetDisabled(); }
}

namespace Icons
{
	enum class Family
	{
		FontAwesome,
		Tabler,
		GameIcons
	};

	struct GlyphRef
	{
		Family family = Family::Tabler;
		const char* utf8 = nullptr;
		[[nodiscard]] constexpr bool IsValid() const noexcept { return utf8 && utf8[0] != '\0'; }
		[[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }
	};

	[[nodiscard]] constexpr GlyphRef TI(const char* utf8) noexcept { return { Family::Tabler, utf8 }; }

	/** @brief No icon font in Open Shaders: keeps the current font. */
	class FontGuard
	{
	public:
		explicit FontGuard(GlyphRef, float = 1.0f) {}
		FontGuard(const FontGuard&) = delete;
		FontGuard& operator=(const FontGuard&) = delete;
	};
}

namespace SceneActionIcons
{
	inline constexpr Icons::GlyphRef kDelete = Icons::TI(ICON_TI_TRASH);
	inline constexpr Icons::GlyphRef kSave = Icons::TI(ICON_TI_DEVICE_FLOPPY);
	inline constexpr Icons::GlyphRef kCopy = Icons::TI(ICON_TI_COPY);
}

namespace BrowserUI
{
	inline float IconButtonSize() { return ImGui::GetFrameHeight(); }

	inline bool IconButton(const char* id, Icons::GlyphRef glyph, const char* tooltip, bool active = false, ImU32 color = 0)
	{
		ImGui::PushID(id);
		int colors = 0;
		if (color) {
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			++colors;
		}
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			++colors;
		}
		const bool pressed = ImGui::Button(glyph.utf8 ? glyph.utf8 : "?");
		ImGui::PopStyleColor(colors);
		if (tooltip && tooltip[0] && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
		ImGui::PopID();
		return pressed;
	}

	inline ImU32 DestructiveIconColor() { return ImGui::GetColorU32(Util::Colors::GetError()); }

	inline bool Chip(const char* id, const char* label, Icons::GlyphRef icon = {}, const ImVec4* iconColor = nullptr, const ImVec4* tint = nullptr)
	{
		(void)iconColor;
		ImGui::PushID(id);
		if (tint)
			ImGui::PushStyleColor(ImGuiCol_Button, *tint);
		std::string text = icon ? std::string(icon.utf8) + " " + label : std::string(label);
		const bool pressed = ImGui::SmallButton(text.c_str());
		if (tint)
			ImGui::PopStyleColor();
		ImGui::PopID();
		return pressed;
	}

	inline void InlineBadge(const char* label, const ImVec4& tint)
	{
		ImGui::SameLine();
		ImGui::TextColored(tint, "[%s]", label);
	}

	inline void MetaText(const char* text, float maxWidth = 0.0f)
	{
		(void)maxWidth;
		ImGui::TextDisabled("%s", text);
	}

	inline bool Link(const char* id, const char* label, float maxWidth = 0.0f)
	{
		(void)maxWidth;
		ImGui::PushID(id);
		ImGui::TextColored(Util::Colors::GetInfo(), "%s", label);
		const bool clicked = ImGui::IsItemClicked();
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::PopID();
		return clicked;
	}

	inline void SectionLabel(const char* label, bool alignRight = false)
	{
		(void)alignRight;
		ImGui::SeparatorText(label);
	}

	inline void EmptyState(const char* title, const char* hint = nullptr)
	{
		ImGui::TextDisabled("%s", title);
		if (hint)
			ImGui::TextWrapped("%s", hint);
	}

	inline bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint, float width, bool ctrlFFocus,
		ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* userData = nullptr)
	{
		(void)ctrlFFocus;
		ImGui::SetNextItemWidth(width);
		return ImGui::InputTextWithHint(id, hint, buffer, bufferSize, flags, callback, userData);
	}

	class RowShadeScope
	{
	public:
		RowShadeScope() = default;
		RowShadeScope(const RowShadeScope&) = delete;
		RowShadeScope& operator=(const RowShadeScope&) = delete;
	};

	inline void RightAlign(float width)
	{
		const float avail = ImGui::GetContentRegionAvail().x;
		if (avail > width)
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
	}
}
