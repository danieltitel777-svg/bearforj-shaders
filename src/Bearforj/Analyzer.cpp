#include "Bearforj/Analyzer.h"

#include <d3d11.h>
#include <dxgi.h>
#include <nlohmann/json.hpp>

#include "Feature.h"
#include "Features/BearforjLook.h"
#include "Globals.h"
#include "I18n/I18n.h"

namespace Bearforj::Analyzer
{
	namespace
	{
		constexpr int kFrames = 8, kGW = 256, kGH = 144, kPatch = 128;
		int g_left = 0;
		Result g_result;
		struct Frame
		{
			std::vector<float> r, g, b;  // grid samples, display-encoded 0..1
			std::vector<float> patch;    // center patch luma, full res
		};
		std::vector<Frame> g_frames;
		std::string g_format;

		float Half(uint16_t h)
		{
			const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
			uint32_t f;
			if (e == 0)
				f = s;  // flush denormals
			else if (e == 31)
				f = s | 0x7F800000u | (m << 13);
			else
				f = s | ((e + 112) << 23) | (m << 13);
			float out;
			std::memcpy(&out, &f, 4);
			return out;
		}
		float Encode(float lin)
		{
			lin = std::clamp(std::isfinite(lin) ? lin : 0.0f, 0.0f, 1.0f);
			return lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
		}

		bool Read(const uint8_t* row, int x, DXGI_FORMAT fmt, float& r, float& g, float& b)
		{
			switch (fmt) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				r = row[x * 4] / 255.0f, g = row[x * 4 + 1] / 255.0f, b = row[x * 4 + 2] / 255.0f;
				return true;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				b = row[x * 4] / 255.0f, g = row[x * 4 + 1] / 255.0f, r = row[x * 4 + 2] / 255.0f;
				return true;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				{
					uint32_t v;
					std::memcpy(&v, row + x * 4, 4);
					r = (v & 1023) / 1023.0f, g = ((v >> 10) & 1023) / 1023.0f, b = ((v >> 20) & 1023) / 1023.0f;
					return true;  // HDR10 PQ is approximated as display-encoded
				}
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				{
					uint16_t h[3];
					std::memcpy(h, row + x * 8, 6);
					r = Encode(Half(h[0])), g = Encode(Half(h[1])), b = Encode(Half(h[2]));  // scRGB, 1.0 = SDR white
					return true;
				}
			default:
				return false;
			}
		}

		float Luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

		void Finish()
		{
			Result res;
			res.format = g_format;
			if (g_frames.empty()) {
				g_result = res;
				return;
			}
			const auto& f = g_frames.back();
			const size_t n = f.r.size();
			double sumY = 0, sumY2 = 0, sat = 0, rb = 0, gm = 0, haze = 0;
			int mids = 0, lit = 0, clip = 0, crush = 0;
			for (size_t i = 0; i < n; ++i) {
				const float y = Luma(f.r[i], f.g[i], f.b[i]);
				sumY += y, sumY2 += y * y;
				res.histogram[std::min(63, (int)(y * 64.0f))] += 1.0f;
				clip += y > 0.98f, crush += y < 0.02f;
				const float mx = std::max({ f.r[i], f.g[i], f.b[i] }), mn = std::min({ f.r[i], f.g[i], f.b[i] });
				sat += mx - mn;
				if (y > 0.2f && y < 0.8f) {
					rb += f.r[i] - f.b[i];
					gm += f.g[i] - 0.5f * (f.r[i] + f.b[i]);
					++mids;
				}
				if (y > 0.1f) {
					haze += mn;
					++lit;
				}
			}
			for (auto& h : res.histogram)
				h /= (float)n;
			res.exposure = (float)(sumY / n);
			res.contrast = (float)std::sqrt(std::max(0.0, sumY2 / n - res.exposure * res.exposure));
			res.saturation = (float)(sat / n);
			res.castRB = mids ? (float)(rb / mids) : 0.0f;
			res.castGM = mids ? (float)(gm / mids) : 0.0f;
			res.haze = lit ? (float)(haze / lit) : 0.0f;
			res.clip = clip / (float)n, res.crush = crush / (float)n;
			// noise: median |laplacian| in the full-res center patch
			std::vector<float> lap;
			for (int y = 1; y < kPatch - 1; ++y)
				for (int x = 1; x < kPatch - 1; ++x) {
					const auto& p = f.patch;
					const int i = y * kPatch + x;
					lap.push_back(std::abs(4 * p[i] - p[i - 1] - p[i + 1] - p[i - kPatch] - p[i + kPatch]));
				}
			if (!lap.empty()) {
				std::nth_element(lap.begin(), lap.begin() + lap.size() / 2, lap.end());
				res.noise = lap[lap.size() / 2];
			}
			// flicker: frame-to-frame mean luma change
			double fl = 0;
			for (size_t k = 1; k < g_frames.size(); ++k) {
				double a = 0, b = 0;
				for (size_t i = 0; i < n; ++i) {
					a += Luma(g_frames[k].r[i], g_frames[k].g[i], g_frames[k].b[i]);
					b += Luma(g_frames[k - 1].r[i], g_frames[k - 1].g[i], g_frames[k - 1].b[i]);
				}
				fl += std::abs(a - b) / n;
			}
			res.flicker = g_frames.size() > 1 ? (float)(fl / (g_frames.size() - 1)) : 0.0f;

			// score
			auto pen = [](float v, float lo, float hi, float w) { return v < lo ? (lo - v) * w : v > hi ? (v - hi) * w : 0.0f; };
			float p = 0;
			p += pen(res.clip, 0, 0.02f, 400);
			p += pen(res.crush, 0, 0.04f, 300);
			p += pen(res.exposure, 0.25f, 0.55f, 120);
			p += pen(res.contrast, 0.15f, 0.30f, 150);
			p += pen(res.saturation, 0.08f, 0.35f, 100);
			p += pen(std::abs(res.castRB), 0, 0.06f, 200) + pen(std::abs(res.castGM), 0, 0.04f, 250);
			p += pen(res.noise, 0, 0.03f, 400);
			p += pen(res.flicker, 0, 0.01f, 1500);
			p += pen(res.haze, 0, 0.22f, 150);
			res.score = std::clamp(100 - (int)std::round(p), 0, 100);
			const int s = res.score;
			res.verdict = s >= 90 ? T("bearforj.analyzer.v6", "Beautiful!") : s >= 75 ? T("bearforj.analyzer.v5", "Pretty good!") :
			              s >= 60 ? T("bearforj.analyzer.v4", "Okay.") : s >= 40 ? T("bearforj.analyzer.v3", "Meh.") :
			              s >= 20 ? T("bearforj.analyzer.v2", "Hmm, not great...") : T("bearforj.analyzer.v1", "Total garbage.");

			// tips (Bearforj Look / fog values the user can apply in one click)
			nlohmann::json look, fog;
			globals::features::bearforjLook.SaveSettings(look);
			for (auto* ft : Feature::GetFeatureList())
				if (ft->GetShortName() == "ExponentialHeightFog" && ft->loaded)
					ft->SaveSettings(fog);
			auto lv = [&](const char* k, float d) { return look.contains(k) && look[k].is_number() ? look[k].get<float>() : d; };
			const float tm = lv("TonemapStrength", 0.35f), warm = lv("Warmth", 0.5f), lift = lv("NightLift", 0.45f);
			auto add = [&](const char* key, const char* en, const char* feat, const char* k, float v) { res.tips.push_back({ T(key, en), feat, k, v }); };
			if (res.clip > 0.02f)
				add("bearforj.analyzer.tip_clip", "Highlights are clipped (white blobs). Stronger Bearforj tonemap compresses them.", "BearforjLook", "TonemapStrength", std::min(1.0f, tm + 0.1f));
			if (res.crush > 0.04f)
				add("bearforj.analyzer.tip_crush", "Shadows are crushed to black. Raise the night lift / soften the tonemap.", "BearforjLook", res.exposure < 0.2f ? "NightLift" : "TonemapStrength", res.exposure < 0.2f ? std::min(1.5f, lift + 0.15f) : std::max(0.0f, tm - 0.1f));
			if (res.contrast < 0.15f)
				add("bearforj.analyzer.tip_flat", "Picture is flat (low contrast).", "BearforjLook", "TonemapStrength", std::min(1.0f, tm + 0.1f));
			if (res.contrast > 0.30f)
				add("bearforj.analyzer.tip_harsh", "Contrast is harsh.", "BearforjLook", "TonemapStrength", std::max(0.0f, tm - 0.1f));
			if (res.castRB > 0.06f)
				add("bearforj.analyzer.tip_warm", "Too warm / orange cast.", "BearforjLook", "Warmth", std::max(0.0f, warm - 0.15f));
			if (res.castRB < -0.06f)
				add("bearforj.analyzer.tip_cold", "Too cold / blue cast.", "BearforjLook", "Warmth", std::min(1.0f, warm + 0.15f));
			if (res.haze > 0.22f && fog.contains("fogDensity"))
				add("bearforj.analyzer.tip_haze", "Hazy / washed out. Thinner height fog.", "ExponentialHeightFog", "fogDensity", fog["fogDensity"].get<float>() * 0.7f);
			// double tonemap: highlights squashed well below white with no clipping + flat contrast,
			// typical when a weather mod's own tonemap/imagespace stacks with ours
			{
				float acc = 0.0f, p99 = 1.0f;
				for (int i = 63; i >= 0; --i) {
					acc += res.histogram[i];
					if (acc >= 0.01f) {
						p99 = (i + 0.5f) / 64.0f;
						break;
					}
				}
				res.p99 = p99;
				const float mix = lv("TonemapMix", 100.0f);
				res.doubleTonemap = p99 < 0.80f && res.clip < 0.002f && res.contrast < 0.19f && mix > 50.0f && tm > 0.15f;
				if (res.doubleTonemap)
					add("bearforj.analyzer.tip_double_tm", "Looks double-tonemapped: highlights never reach white and the image is flat. The weather mod probably tonemaps too - lower Bearforj tonemap mix.", "BearforjLook", "TonemapMix", std::max(0.0f, mix - 50.0f));
			}
			const bool night = RE::Calendar::GetSingleton() && (RE::Calendar::GetSingleton()->GetHour() < 5.0f || RE::Calendar::GetSingleton()->GetHour() > 20.0f);
			if (res.exposure < 0.25f)
				add("bearforj.analyzer.tip_dark", "Underexposed (too dark). Apply lifts it; if still dark it's the weather imagespace / eye adaptation.", "BearforjLook", night ? "NightLift" : "TonemapStrength", night ? std::min(1.5f, lift + 0.15f) : std::max(0.0f, tm - 0.1f));
			if (res.exposure > 0.55f)
				add("bearforj.analyzer.tip_bright", "Overexposed (too bright). Apply compresses highlights; if still bright it's the weather imagespace / eye adaptation.", "BearforjLook", "TonemapStrength", std::min(1.0f, tm + 0.1f));
			if (res.saturation < 0.08f)
				add("bearforj.analyzer.tip_grey", "Colors are grey. Weather/ENB saturation, or a ReShade preset, may be washing them out.", "", "", 0);
			if (res.saturation > 0.35f)
				add("bearforj.analyzer.tip_oversat", "Colors are oversaturated.", "", "", 0);
			if (res.noise > 0.03f)
				add("bearforj.analyzer.tip_noise", "Noisy image. Use TAA/DLAA/FSR (Upscaling) or raise SSGI/Skylighting quality.", "", "", 0);
			if (res.flicker > 0.01f)
				add("bearforj.analyzer.tip_flicker", "Brightness flickers between frames (stand still while analyzing). If it persists: eye adaptation or SSGI temporal settings.", "", "", 0);
			res.valid = true;
			g_result = std::move(res);
		}
	}

	void Request()
	{
		g_frames.clear();
		g_left = kFrames;
	}
	bool Busy() { return g_left > 0; }
	const Result& GetResult() { return g_result; }

	bool ReadPixel(const uint8_t* a_row, int a_x, DXGI_FORMAT a_fmt, float& r, float& g, float& b) { return Read(a_row, a_x, a_fmt, r, g, b); }

	void OnFrame()
	{
		if (g_left <= 0 || !globals::d3d::swapChain || !globals::d3d::device || !globals::d3d::context)
			return;
		ID3D11Texture2D* back = nullptr;
		if (FAILED(globals::d3d::swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) {
			g_left = 0;
			return;
		}
		D3D11_TEXTURE2D_DESC d{};
		back->GetDesc(&d);
		g_format = std::to_string((int)d.Format);
		D3D11_TEXTURE2D_DESC sd = d;
		sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0, sd.MipLevels = 1, sd.ArraySize = 1;
		sd.SampleDesc = { 1, 0 };
		ID3D11Texture2D* staging = nullptr;
		bool ok = d.SampleDesc.Count == 1 && SUCCEEDED(globals::d3d::device->CreateTexture2D(&sd, nullptr, &staging));
		if (ok) {
			globals::d3d::context->CopyResource(staging, back);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(globals::d3d::context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
				Frame f;
				f.r.reserve(kGW * kGH), f.g.reserve(kGW * kGH), f.b.reserve(kGW * kGH);
				float r, g, b;
				for (int y = 0; y < kGH && ok; ++y) {
					const auto* row = (const uint8_t*)m.pData + (size_t)(((y + 0.5f) * d.Height) / kGH) * m.RowPitch;
					for (int x = 0; x < kGW; ++x) {
						if (!Read(row, (int)(((x + 0.5f) * d.Width) / kGW), d.Format, r, g, b)) {
							ok = false;
							break;
						}
						f.r.push_back(r), f.g.push_back(g), f.b.push_back(b);
					}
				}
				const int px = std::max(0, (int)d.Width / 2 - kPatch / 2), py = std::max(0, (int)d.Height / 2 - kPatch / 2);
				for (int y = 0; y < kPatch && ok; ++y) {
					const auto* row = (const uint8_t*)m.pData + (size_t)std::min<int>(py + y, d.Height - 1) * m.RowPitch;
					for (int x = 0; x < kPatch; ++x) {
						Read(row, std::min<int>(px + x, d.Width - 1), d.Format, r, g, b);
						f.patch.push_back(Luma(r, g, b));
					}
				}
				globals::d3d::context->Unmap(staging, 0);
				if (ok)
					g_frames.push_back(std::move(f));
			}
			staging->Release();
		}
		back->Release();
		if (!ok) {
			logger::warn("[Bearforj Analyzer] unsupported back buffer (format {}, msaa {})", (int)d.Format, d.SampleDesc.Count);
			g_left = 0;
			g_result = {};
			g_result.format = g_format;
			return;
		}
		if (--g_left == 0)
			Finish();
	}
}
