#pragma once

/**
 * @brief Developer-mode "Mish Mish report" (hotkey, default F12).
 * On key press only: PNG of the final frame (captured before Bearforj UI draws) + JSON + TXT report
 * into Compat::Config::reportDir (fallback <SKSE logs>/Bearforj/reports), timestamped.
 */
namespace Bearforj::Report
{
	/** @brief Every frame from Menu::DrawOverlay, before ImGui draws. Polls the hotkey (edge) and captures. */
	void OnFrame();
	/** @brief Draws the confirmation toast (inside the ImGui frame). Returns the wink image request. */
	bool ToastActive(std::string& a_text, float& a_alpha);
	/** @brief Menu: wait for the next key press and bind it. */
	void BeginRebind();
	bool Rebinding();
	std::string KeyName(int a_vk);
	/** @brief Key the report listens to: the menu screenshot binding when it is a single key, else reportKey. */
	int ActiveKey();
	/** @brief True while the report hotkey also saves the screenshot (menu binding should not capture again). */
	bool OwnsHotkey();
}
