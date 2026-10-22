#pragma once

/**
 * @brief Bearforj 0.6 "Mish Mish performance assistant".
 *
 * - Per-feature GPU ms from the existing D3D11 timestamp Profiler (sampled: 2 s capture every 10 s,
 *   continuous while the Performance page is open).
 * - Context: GPU name/vendor/VRAM (DXGI), resolution, upscaler (TAA/FSR/DLSS; XeSS if present),
 *   frame generation (Open/CS FSR-FG / DLSS-G, external DLSSG->FSR3 / Optiscaler / Lossless).
 * - Base fps = game simulation rate (generated frames are inserted after the game loop),
 *   with p99 frame time and stutter count, not only the average.
 * - Advisor: costliest features, cost-ms / visual-value score, suggestions with one-click "Do it"
 *   and rollback. Never changes anything unasked.
 * - Bearforj.log next to the SKSE logs + Export report.
 */
namespace Bearforj::Perf
{
	struct Context
	{
		std::string gpu;
		uint32_t vendor = 0;   // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
		double vramGB = 0;
		uint32_t width = 0, height = 0;
		std::string upscaler;  // "TAA", "FSR", "DLSS", "XeSS", "none"
		std::string frameGen;  // "", "FSR3 FG (Bearforj)", "DLSS-G (Bearforj)", external name...
		bool fg = false;
	};

	struct FeatureCost
	{
		std::string feature;   // short name
		std::string name;      // display name
		float ms = 0;          // avg GPU ms
		int value = 3;         // visual value 1..5
		float ratio = 0;       // ms per value point (higher = better candidate to cut)
	};

	enum class Action
	{
		kPerfProfile,  // feature->ApplyPerformanceProfile(Performance)
		kTurnOff,      // feature's own "Enabled" setting -> false
		kInfo          // text only (e.g. switch upscaler quality)
	};

	struct Suggestion
	{
		std::string feature, text;
		Action action = Action::kInfo;
		float savesMs = 0;
	};

	struct Stats
	{
		float baseFps = 0, shownFps = 0, avgMs = 0, p99Ms = 0;
		int stutters = 0;        // frames > 2.5x median in the last window
		float targetFps = 50;    // threshold for the current FG state
		bool low = false;        // sustained below target
	};

	struct Config
	{
		float thresholdFG = 35.0f, thresholdNoFG = 50.0f;
		bool monitor = true;
		int logEverySec = 30;
	};

	Config& GetConfig();
	const Context& GetContext();
	const Stats& GetStats();
	std::vector<FeatureCost> GetCosts();
	/** @brief Suggestions to save enough ms to reach a_targetFps (0 = current threshold). */
	std::vector<Suggestion> Plan(float a_targetFps);
	/** @brief Applies one suggestion; snapshot kept for Rollback(). */
	bool Apply(const Suggestion& a_s);
	bool CanRollback();
	void Rollback();

	/** @brief Once after data loaded: context + startup section of Bearforj.log. */
	void OnDataLoaded();
	/** @brief Every frame (render thread). */
	void OnFrame(bool a_pageOpen);
	/** @brief Writes a full report; returns its path. */
	std::string ExportReport();
}
