#pragma once

#include <dxgiformat.h>

#include <array>
#include <string>
#include <vector>

/**
 * @brief Bearforj image analyzer (beta). On request captures 8 consecutive back-buffer frames
 * (before the menu is drawn), samples them on the CPU and rates the picture 0-100:
 * histogram, clipping/crush, exposure, contrast, saturation, color cast, noise, flicker, haze.
 */
namespace Bearforj::Analyzer
{
	struct Tip
	{
		std::string text;     // already translated
		std::string feature;  // short name, empty = hint only
		std::string key;
		float value = 0.0f;
	};

	struct Result
	{
		bool valid = false;
		std::array<float, 64> histogram{};
		float p99 = 1, clip = 0, crush = 0, exposure = 0, contrast = 0, saturation = 0, castRB = 0, castGM = 0, noise = 0, flicker = 0, haze = 0;
		bool doubleTonemap = false;
		int score = 0;
		std::string verdict, format;
		std::vector<Tip> tips;
	};

	/** @brief Reads one back-buffer pixel as display-encoded 0..1 (shared with the report screenshot). */
	bool ReadPixel(const uint8_t* a_row, int a_x, DXGI_FORMAT a_fmt, float& r, float& g, float& b);
	void Request();
	bool Busy();
	/** @brief Called by Menu::DrawOverlay before ImGui draws into the back buffer. */
	void OnFrame();
	const Result& GetResult();
}
