#include "Bearforj/PerfAssistant.h"
#include "Bearforj/Compat.h"

#include "Feature.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Utils/Format.h"
#include "Profiler.h"
#include "State.h"

#include <dxgi.h>
#include <fstream>

namespace Bearforj::Perf
{
	namespace
	{
		Config g_cfg;
		Context g_ctx;
		Stats g_stats;
		std::deque<float> g_frames;  // game frame times (ms)
		float g_lowFor = 0, g_nextCapture = 0, g_captureUntil = 0, g_nextLog = 0;
		std::vector<std::pair<std::string, nlohmann::json>> g_undo;  // feature short name, settings before

		std::filesystem::path LogPath() { return logger::log_directory().value_or(std::filesystem::path{}) / "Bearforj.log"; }

		void Log(const std::string& a_line, bool a_truncate = false)
		{
			std::ofstream out(LogPath(), a_truncate ? std::ios::trunc : std::ios::app);
			if (out)
				out << std::format("[{:%H:%M:%S}] ", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())) << a_line << "\n";
		}

		Feature* Find(const std::string& a_short)
		{
			for (auto* f : Feature::GetFeatureList())
				if (f->GetShortName() == a_short)
					return f;
			return nullptr;
		}

		// How much each feature adds to the picture (1 = subtle .. 5 = defines the look). Unknown = 3.
		int VisualValue(const std::string& n)
		{
			static const std::map<std::string, int> v = {
				{ "ScreenSpaceGI", 4 }, { "Skylighting", 5 }, { "ScreenSpaceShadows", 3 }, { "LightLimitFix", 5 },
				{ "TerrainShadows", 4 }, { "CloudShadows", 3 }, { "WetnessEffects", 4 }, { "VolumetricLighting", 4 },
				{ "ExponentialHeightFog", 3 }, { "ScreenSpacePhysicalParticles", 4 }, { "TerrainDeformation", 3 },
				{ "SubsurfaceScattering", 3 }, { "DynamicCubemaps", 3 }, { "TerrainBlending", 2 }, { "GrassCollision", 2 },
				{ "GrassLighting", 3 }, { "RainRefraction", 2 }, { "LODBlending", 2 }, { "IBL", 3 }, { "BearforjLook", 5 },
				{ "Upscaling", 5 }, { "PhysicalSky", 4 }, { "SubmersionWetness", 2 }, { "ContactShadows", 2 },
			};
			auto it = v.find(n);
			return it == v.end() ? 3 : it->second;
		}

		void DetectContext()
		{
			Context c;
			if (auto* device = globals::d3d::device) {
				IDXGIDevice* dxgi = nullptr;
				if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) && dxgi) {
					IDXGIAdapter* adapter = nullptr;
					if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter) {
						DXGI_ADAPTER_DESC d{};
						adapter->GetDesc(&d);
						c.gpu = Util::WStringToString(d.Description);
						c.vendor = d.VendorId;
						c.vramGB = d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
						adapter->Release();
					}
					dxgi->Release();
				}
			}
			if (globals::state) {
				c.width = (uint32_t)globals::state->screenSize.x;
				c.height = (uint32_t)globals::state->screenSize.y;
			}
			auto& up = globals::features::upscaling;
			if (up.loaded) {
				switch (up.GetUpscaleMethod()) {
				case Upscaling::UpscaleMethod::kDLSS: c.upscaler = "DLSS"; break;
				case Upscaling::UpscaleMethod::kFSR: c.upscaler = "FSR"; break;
				case Upscaling::UpscaleMethod::kTAA: c.upscaler = "TAA"; break;
				default: c.upscaler = "none"; break;
				}
				switch (up.GetFrameGenMethod()) {
				case Upscaling::FrameGenMethod::kFSR: c.frameGen = "FSR3 FG (Bearforj/Open Upscaling)"; break;
				case Upscaling::FrameGenMethod::kDLSSG: c.frameGen = "DLSS-G (Bearforj/Open Upscaling)"; break;
				default: break;
				}
			} else
				c.upscaler = "none";
			if (GetModuleHandleW(L"libxess.dll"))
				c.upscaler += " + XeSS (external)";
			// external frame generation injectors
			for (auto [mod, name] : std::initializer_list<std::pair<const wchar_t*, const char*>>{
					 { L"dlssg_to_fsr3_amd_is_better.dll", "DLSSG->FSR3 (external)" }, { L"nvngx_dlssg.dll", "DLSS-G (external)" },
					 { L"OptiScaler.dll", "OptiScaler FG (external)" }, { L"ffx_frameinterpolation_x64.dll", "FSR3 FG (external)" },
					 { L"LosslessScaling.dll", "Lossless Scaling (external)" } })
				if (GetModuleHandleW(mod) && c.frameGen.empty())
					c.frameGen = name;
			c.fg = !c.frameGen.empty();
			g_ctx = c;
		}

		std::string ContextLine()
		{
			return std::format("GPU {} ({:.0f} GB, vendor 0x{:04X}) | {}x{} | upscaler {} | frame gen {}", g_ctx.gpu, g_ctx.vramGB, g_ctx.vendor,
				g_ctx.width, g_ctx.height, g_ctx.upscaler, g_ctx.fg ? g_ctx.frameGen : "off");
		}
	}

	Config& GetConfig() { return g_cfg; }
	const Context& GetContext() { return g_ctx; }
	const Stats& GetStats() { return g_stats; }

	std::vector<FeatureCost> GetCosts()
	{
		std::map<std::string, float> ms;
		if (globals::profiler)
			for (const auto& r : globals::profiler->GetStatistics()) {
				if (!r.valid || !r.hasGpu)
					continue;
				const auto sep = r.name.find("::");
				if (sep == std::string::npos || r.name.find("::", sep + 2) != std::string::npos)
					continue;  // top-level feature passes only (nested passes would double count)
				ms[r.name.substr(0, sep)] += r.avgMs;
			}
		std::vector<FeatureCost> out;
		for (auto& [n, t] : ms) {
			FeatureCost c{ n, n, t, VisualValue(n), 0 };
			if (auto* f = Find(n))
				c.name = f->GetName();
			c.ratio = t / (float)c.value;
			out.push_back(c);
		}
		std::ranges::sort(out, [](auto& a, auto& b) { return a.ms > b.ms; });
		return out;
	}

	std::vector<Suggestion> Plan(float a_target)
	{
		const float target = a_target > 0 ? a_target : g_stats.targetFps;
		const float need = g_stats.avgMs > 0 ? std::max(0.0f, g_stats.avgMs - 1000.0f / target) : 0.0f;
		auto costs = GetCosts();
		std::ranges::sort(costs, [](auto& a, auto& b) { return a.ratio > b.ratio; });
		std::vector<Suggestion> out;
		float saved = 0;
		for (auto& c : costs) {
			if (c.ms < 0.15f || c.feature == "Upscaling" || c.feature == "BearforjLook")
				continue;
			auto* f = Find(c.feature);
			if (!f)
				continue;
			nlohmann::json j;
			f->SaveSettings(j);
			if (!f->MatchesPerformanceProfile(Feature::PerfProfile::Performance)) {
				const float s = c.ms * 0.4f;  // estimate
				out.push_back({ c.feature, std::format("{}: {} ({:.2f} ms, value {}/5) -> performance settings, saves ~{:.1f} ms", c.name,
											   T("bearforj.perf.lower", "lower quality"), c.ms, c.value, s), Action::kPerfProfile, s });
				saved += s;
			} else if (j.contains("Enabled") && j["Enabled"].is_boolean() && j["Enabled"].get<bool>() && c.value <= 3) {
				out.push_back({ c.feature, std::format("{}: {} ({:.2f} ms, value {}/5)", c.name, T("bearforj.perf.off", "turn off"), c.ms, c.value), Action::kTurnOff, c.ms });
				saved += c.ms;
			}
			if (need > 0 && saved >= need)
				break;
		}
		if (g_ctx.upscaler.starts_with("TAA") || g_ctx.upscaler == "none")
			out.push_back({ "", g_ctx.vendor == 0x10DE ? T("bearforj.perf.up_dlss", "Upscaling: DLSS Quality gives the most fps per quality.") :
			                                             T("bearforj.perf.up_fsr", "Upscaling: FSR (or XeSS) Quality - your GPU has no DLSS."), Action::kInfo, 0 });
		if (!g_ctx.fg && g_stats.baseFps > 40)
			out.push_back({ "", T("bearforj.perf.fg", "Frame generation (FSR3 FG in Upscaling) doubles shown fps; base fps is high enough for it."), Action::kInfo, 0 });
		return out;
	}

	bool Apply(const Suggestion& s)
	{
		auto* f = Find(s.feature);
		if (!f || s.action == Action::kInfo)
			return false;
		nlohmann::json before;
		f->SaveSettings(before);
		g_undo.emplace_back(s.feature, before);
		if (s.action == Action::kPerfProfile)
			f->ApplyPerformanceProfile(Feature::PerfProfile::Performance);
		else {
			auto j = before;
			j["Enabled"] = false;
			f->LoadSettings(j);
		}
		Log("advisor applied: " + s.text);
		return true;
	}

	bool CanRollback() { return !g_undo.empty(); }
	void Rollback()
	{
		while (!g_undo.empty()) {
			auto& [n, j] = g_undo.back();
			if (auto* f = Find(n))
				f->LoadSettings(j);
			Log("advisor rollback: " + n);
			g_undo.pop_back();
		}
	}

	void OnDataLoaded()
	{
		DetectContext();
		Log("Bearforj Shaders 0.6 - session start", true);
		Log(ContextLine());
		for (auto* f : Feature::GetFeatureList()) {
			if (f->IsHiddenUnreleased())
				continue;
			if (f->loaded)
				Log(std::format("feature OK     {} {}", f->GetShortName(), f->version));
			else
				Log(std::format("feature FAILED {} - {}", f->GetShortName(), f->failedLoadedMessage.empty() ? "not installed / disabled at boot" : f->failedLoadedMessage));
		}
		for (auto& c : Compat::GetConflictInfos())
			Log(std::format("conflict {} : {} (our feature: {})", c.id, c.mod, c.feature.empty() ? "-" : c.feature));
	}

	void OnFrame(bool a_pageOpen)
	{
		const float dt = RE::GetSecondsSinceLastFrame() * 1000.0f;  // game loop = base frame (FG frames are not simulated)
		if (dt > 0.1f && dt < 1000.0f) {
			g_frames.push_back(dt);
			if (g_frames.size() > 600)
				g_frames.pop_front();
		}
		if (g_frames.size() < 30)
			return;
		std::vector<float> v(g_frames.begin(), g_frames.end());
		double sum = 0;
		for (float x : v)
			sum += x;
		auto s = v;
		std::ranges::sort(s);
		const float median = s[s.size() / 2];
		g_stats.avgMs = (float)(sum / v.size());
		g_stats.p99Ms = s[std::min(s.size() - 1, (size_t)(s.size() * 0.99))];
		g_stats.stutters = (int)std::ranges::count_if(v, [&](float x) { return x > median * 2.5f; });
		g_stats.baseFps = 1000.0f / g_stats.avgMs;
		g_stats.shownFps = g_stats.baseFps * (g_ctx.fg ? 2.0f : 1.0f);
		g_stats.targetFps = g_ctx.fg ? g_cfg.thresholdFG : g_cfg.thresholdNoFG;
		const bool lowNow = g_stats.baseFps < g_stats.targetFps || g_stats.p99Ms > 2.0f * 1000.0f / g_stats.targetFps;
		g_lowFor = lowNow ? g_lowFor + dt / 1000.0f : 0.0f;
		g_stats.low = g_lowFor > 5.0f;

		// sampled GPU capture (timestamps cost a little, so not always on)
		const float now = RE::GetDurationOfApplicationRunTime() / 1000.0f;
		if (globals::profiler && g_cfg.monitor) {
			if (a_pageOpen || now < g_captureUntil)
				globals::profiler->RequestCapture(Profiler::CaptureMode::GPU);
			else if (now >= g_nextCapture) {
				g_captureUntil = now + 2.0f;
				g_nextCapture = now + 10.0f;
				globals::profiler->SetUserEnabled(true);
			}
		}
		if (g_cfg.logEverySec > 0 && now >= g_nextLog) {
			g_nextLog = now + (float)g_cfg.logEverySec;
			std::string line = std::format("perf base {:.0f} fps (shown {:.0f}), avg {:.1f} ms, p99 {:.1f} ms, stutters {} |", g_stats.baseFps, g_stats.shownFps, g_stats.avgMs, g_stats.p99Ms, g_stats.stutters);
			for (auto& c : GetCosts())
				line += std::format(" {} {:.2f}", c.feature, c.ms);
			Log(line);
		}
	}

	std::string ExportReport()
	{
		const auto path = LogPath().parent_path() / std::format("Bearforj_Report_{:%Y%m%d_%H%M%S}.txt", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
		std::ofstream out(path);
		out << "Bearforj Shaders 0.6 report\n" << ContextLine() << "\n\n";
		out << std::format("base fps {:.1f}, shown {:.1f}, avg {:.2f} ms, p99 {:.2f} ms, stutters {}\n\nFeatures:\n", g_stats.baseFps, g_stats.shownFps, g_stats.avgMs, g_stats.p99Ms, g_stats.stutters);
		const auto costs = GetCosts();
		for (auto* f : Feature::GetFeatureList()) {
			if (f->IsHiddenUnreleased())
				continue;
			float ms = 0;
			for (auto& c : costs)
				if (c.feature == f->GetShortName())
					ms = c.ms;
			const int fails = Compat::GetCompileFailures(f->GetShortName());
			out << std::format("  {:<32} {:<8} {:>6.2f} ms  compile failures {}  {}\n", f->GetShortName(), f->loaded ? "loaded" : "FAILED", ms, fails, f->loaded ? "" : f->failedLoadedMessage);
		}
		out << "\nConflicts:\n";
		for (auto& c : Compat::GetConflictInfos())
			out << "  " << c.id << " : " << c.mod << "\n";
		out << "\nSafe mode disabled:\n";
		for (auto& s : Compat::GetConfig().safeModeDisabled)
			out << "  " << s << "\n";
		Log("report exported: " + path.string());
		return path.string();
	}
}
