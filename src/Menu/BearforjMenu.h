#pragma once

class Menu;
struct ImFont;
struct ID3D11ShaderResourceView;

/**
 * @brief Bearforj Shaders 0.6 main window: strict black & white, category tabs on top,
 * feature list (with Mish Mish heads + toggles) on the left, feature settings on the right,
 * Save / Reset to defaults / Clear shader cache in the footer.
 *
 * Images are loaded from Data/Interface/CommunityShaders/Bearforj/ so they can be swapped
 * without rebuilding: logo.png (full helmet, uncropped) and emotions/*.png (straight RGBA).
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
	/** @brief Shader-compile overlay: a small static Mish Mish beside the bar. @p a_caption is the status line under it. */
	void DrawCompileOverlay(float a_percent, const char* a_overlay, const char* a_caption = nullptr);
	/** @brief Emotion sprite for setting tooltips. Null until the menu images have loaded. */
	ID3D11ShaderResourceView* TooltipMascot();
	/** @brief Forces the images to be re-read from disk on the next frame. */
	void ReloadAssets();
	/** @brief B/W style around the whole main window (title bar, background, borders). Pair with PopWindowStyle after End(). */
	void PushWindowStyle();
	void PopWindowStyle();
}
