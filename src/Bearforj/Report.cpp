#include "Bearforj/Report.h"
#include "Bearforj/Analyzer.h"
#include "Bearforj/Compat.h"
#include "Bearforj/Context.h"
#include "Bearforj/PerfAssistant.h"
#include "Bearforj/WeatherPresets.h"

#include "Feature.h"
#include "Globals.h"
#include "I18n/I18n.h"

#include <d3d11.h>
#include <fstream>
#include <stb_image_write.h>

namespace Bearforj::Report
{
	namespace
	{
		bool g_wasDown = false, g_rebind = false;
		double g_toastUntil = 0;
		std::string g_toast;

		double Now() { return RE::GetDurationOfApplicationRunTime() / 1000.0; }

		bool Foreground()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		std::filesystem::path Dir()
		{
			std::error_code ec;
			std::filesystem::path d = Compat::GetConfig().reportDir;
			if (!d.empty() && (std::filesystem::create_directories(d, ec), std::filesystem::is_directory(d, ec)))
				return d;
			d = logger::log_directory().value_or(std::filesystem::path{}) / "Bearforj" / "reports";
			std::filesystem::create_directories(d, ec);
			return d;
		}

		bool Screenshot(const std::filesystem::path& a_png)
		{
			auto* sc = globals::d3d::swapChain;
			auto* dev = globals::d3d::device;
			auto* ctx = globals::d3d::context;
			if (!sc || !dev || !ctx)
				return false;
			winrt::com_ptr<ID3D11Texture2D> back;
			if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())))
				return false;
			D3D11_TEXTURE2D_DESC d{};
			back->GetDesc(&d);
			if (d.SampleDesc.Count != 1)
				return false;
			D3D11_TEXTURE2D_DESC sd = d;
			sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0, sd.MipLevels = 1, sd.ArraySize = 1;
			winrt::com_ptr<ID3D11Texture2D> st;
			if (FAILED(dev->CreateTexture2D(&sd, nullptr, st.put())))
				return false;
			ctx->CopyResource(st.get(), back.get());
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m)))
				return false;
			std::vector<uint8_t> rgb((size_t)d.Width * d.Height * 3);
			bool ok = true;
			for (UINT y = 0; y < d.Height && ok; ++y) {
				const auto* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
				for (UINT x = 0; x < d.Width; ++x) {
					float r, g, b;
					if (!Analyzer::ReadPixel(row, (int)x, d.Format, r, g, b)) {
						ok = false;
						break;
					}
					auto* o = &rgb[((size_t)y * d.Width + x) * 3];
					o[0] = (uint8_t)std::lround(std::clamp(r, 0.0f, 1.0f) * 255), o[1] = (uint8_t)std::lround(std::clamp(g, 0.0f, 1.0f) * 255), o[2] = (uint8_t)std::lround(std::clamp(b, 0.0f, 1.0f) * 255);
				}
			}
			ctx->Unmap(st.get(), 0);
			return ok && stbi_write_png(a_png.string().c_str(), (int)d.Width, (int)d.Height, 3, rgb.data(), (int)d.Width * 3) != 0;
		}

		std::vector<std::string> LastLogLines(size_t a_n)
		{
			std::vector<std::string> lines;
			std::ifstream in(logger::log_directory().value_or(std::filesystem::path{}) / "Bearforj.log");
			for (std::string l; std::getline(in, l);) {
				lines.push_back(l);
				if (lines.size() > a_n)
					lines.erase(lines.begin());
			}
			return lines;
		}

		void Capture()
		{
			const auto dir = Dir();
			const auto stamp = std::format("{:%Y%m%d_%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
			const auto base = dir / ("MishMish_" + stamp);
			const bool png = Screenshot(base.string() + ".png");

			nlohmann::json j;
			const auto& pc = Perf::GetContext();
			const auto& ps = Perf::GetStats();
			j["time"] = stamp;
			j["gpu"] = { { "name", pc.gpu }, { "vramGB", pc.vramGB }, { "vendor", pc.vendor }, { "width", pc.width }, { "height", pc.height }, { "upscaler", pc.upscaler }, { "frameGen", pc.fg ? pc.frameGen : "off" } };
			j["fps"] = { { "base", ps.baseFps }, { "shown", ps.shownFps }, { "avgMs", ps.avgMs }, { "p99Ms", ps.p99Ms }, { "stutters", ps.stutters } };
			for (auto& c : Perf::GetCosts())
				j["featureMs"][c.feature] = c.ms;
			const auto& cx = Context::Get();
			auto* sky = globals::game::sky;
			j["context"] = { { "weatherSlot", WeatherPresets::kSlotIds[std::clamp(cx.weatherSlot, 0, (int)WeatherPresets::kSlotCount - 1)] },
				{ "weatherEditorID", sky && sky->currentWeather && sky->currentWeather->GetFormEditorID() ? sky->currentWeather->GetFormEditorID() : "" },
				{ "hour", cx.hour }, { "night", cx.night }, { "locFlags", cx.loc }, { "trees", cx.trees }, { "conifers", cx.conifers }, { "cold", cx.cold } };
			j["preset"] = WeatherPresets::ActivePresetName();
			for (auto& p : Compat::GetProfiles())
				if (p.active)
					j["activeProfiles"].push_back(p.name);
			for (auto& c : Compat::GetConflictInfos())
				j["conflicts"].push_back({ { "id", c.id }, { "mod", c.mod }, { "feature", c.feature } });
			for (auto* f : Feature::GetFeatureList()) {
				if (f->IsHiddenUnreleased())
					continue;
				nlohmann::json s;
				if (f->loaded)
					f->SaveSettings(s);
				j["features"][f->GetShortName()] = { { "loaded", f->loaded }, { "version", f->version }, { "failed", f->failedLoadedMessage }, { "settings", s } };
			}
			j["log"] = LastLogLines(60);
			j["screenshot"] = png;
			std::ofstream(base.string() + ".json") << j.dump(2);

			std::ofstream t(base.string() + ".txt");
			t << "Mish Mish report " << stamp << "\n";
			t << std::format("GPU {} ({:.0f} GB) {}x{} | upscaler {} | FG {}\n", pc.gpu, pc.vramGB, pc.width, pc.height, pc.upscaler, pc.fg ? pc.frameGen : "off");
			t << std::format("fps base {:.1f} shown {:.1f} | avg {:.2f} ms p99 {:.2f} ms | stutters {}\n", ps.baseFps, ps.shownFps, ps.avgMs, ps.p99Ms, ps.stutters);
			t << "weather " << j["context"]["weatherSlot"].get<std::string>() << " (" << j["context"]["weatherEditorID"].get<std::string>() << "), preset " << j["preset"].get<std::string>() << "\n\nFeature ms:\n";
			for (auto& c : Perf::GetCosts())
				t << std::format("  {:<32} {:.2f}\n", c.feature, c.ms);
			t << "\nConflicts:\n";
			for (auto& c : Compat::GetConflictInfos())
				t << "  " << c.id << " : " << c.mod << "\n";
			t << "\nLast log lines:\n";
			for (auto& l : j["log"])
				t << "  " << l.get<std::string>() << "\n";
			t << "\n(full settings in the .json)\n";

			g_toast = std::format("{} {}", T("bearforj.report.saved", "Mish Mish saved a report:"), base.filename().string()) + (png ? "" : std::string(" ") + T("bearforj.report.no_png", "(no screenshot: unsupported screen format)"));
			g_toastUntil = Now() + 3.5;
			logger::info("[Bearforj] Mish Mish report: {}", base.string());
		}
	}

	void BeginRebind() { g_rebind = true; }
	bool Rebinding() { return g_rebind; }

	std::string KeyName(int a_vk)
	{
		if (a_vk >= VK_F1 && a_vk <= VK_F24)
			return std::format("F{}", a_vk - VK_F1 + 1);
		char buf[64]{};
		const UINT sc = MapVirtualKeyA((UINT)a_vk, MAPVK_VK_TO_VSC);
		if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0)
			return buf;
		return std::format("VK 0x{:02X}", a_vk);
	}

	void OnFrame()
	{
		auto& cfg = Compat::GetConfig();
		if (g_rebind) {
			for (int vk = 0x08; vk <= 0xFE; ++vk) {
				if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
					continue;
				if (GetAsyncKeyState(vk) & 0x8000) {
					if (vk != VK_ESCAPE)
						cfg.reportKey = vk;
					g_rebind = false;
					g_wasDown = true;  // don't fire on the binding press
					Compat::SaveConfig();
					break;
				}
			}
			return;
		}
		if (!cfg.devReport || !Foreground())
			return;
		const bool down = (GetAsyncKeyState(cfg.reportKey) & 0x8000) != 0;
		if (down && !g_wasDown)
			Capture();  // nothing is captured unless the key is pressed
		g_wasDown = down;
	}

	bool ToastActive(std::string& a_text, float& a_alpha)
	{
		const double left = g_toastUntil - Now();
		if (left <= 0)
			return false;
		a_text = g_toast;
		a_alpha = (float)std::clamp(left / 0.5, 0.0, 1.0);
		return true;
	}
}
