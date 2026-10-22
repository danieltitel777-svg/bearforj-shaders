#pragma once

class Menu;
struct ImFont;

/**
 * @brief Bearforj Shaders 0.6 main window: strict black & white, category tabs on top,
 * feature list (with Mish Mish heads + toggles) on the left, feature settings on the right,
 * Save / Reset to defaults / Clear shader cache in the footer.
 *
 * Images are loaded from Data/Interface/CommunityShaders/Bearforj/ so they can be swapped
 * without rebuilding: logo.png, head_happy.png, head_grumpy.png, emotions/{thinking,surprised,
 * happy,proud,wink}.png, emotes/{generation,settings}.png.
 *
 * Fonts are never cached across frames: every frame the mono font is looked up in the live
 * atlas (ThemeManager::FindMonoFont), so io.Fonts->Clear() in ReloadFont cannot leave a
 * dangling pointer (the 0.5 monoFont crash).
 */
namespace BearforjMenu
{
	/** @brief Draws the whole window content (call between Begin/End of the main window). */
	void Render(Menu& a_menu);
	/** @brief Releases all textures (menu shutdown / device loss). */
	void ReleaseAssets();
	/** @brief Shader-compile overlay body: B/W bar with Mish Mish pushing it. */
	void DrawCompileOverlay(float a_percent, const char* a_overlay);
	/** @brief True if the Performance page was drawn since the last call (resets it). */
	bool ConsumePerfPageOpen();
	/** @brief Mish Mish wink toast after a report hotkey press (drawn every frame, inside ImGui). */
	void DrawReportToast();
	/** @brief Forces the images to be re-read from disk on the next frame. */
	void ReloadAssets();
	/** @brief B/W style around the whole main window (title bar, background, borders). Pair with PopWindowStyle after End(). */
	void PushWindowStyle();
	void PopWindowStyle();
}
