#include "WaterWaves.h"

#include "Bearforj/TipUI.h"
#include "Features/Wind/Wind.h"
#include "SubmersionWetness.h"

#include "../I18n/I18n.h"
#include "../Utils/UI.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <algorithm>
#include <cmath>

#define I18N_KEY_PREFIX "feature.water_waves."

using namespace WaterWaveMath;

namespace
{
	// One hull per vertex layout. Light count, refraction, and the flags that
	// do not change VS_OUTPUT share that hull. Flowmap and LOD are not tessellated.
	uint32_t CanonicalTessDescriptor(uint32_t a_descriptor)
	{
		using Flag = SIE::ShaderCache::WaterShaderFlags;
		constexpr uint32_t kDrop =
			static_cast<uint32_t>(Flag::Reflections) |
			static_cast<uint32_t>(Flag::Refractions) |
			static_cast<uint32_t>(Flag::Depth) |
			static_cast<uint32_t>(Flag::Interior) |
			static_cast<uint32_t>(Flag::Cubemap);
		uint32_t desc = a_descriptor & ~kDrop;
		const uint32_t technique = (desc >> 11) & 0xF;
		if (technique < 8)
			desc &= ~(0xFu << 11);
		return desc;
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WaterWaves::Settings,
	Enabled,
	Amplitude,
	Chop,
	ShoreLength,
	SeaScale,
	InteriorScale,
	MaxTessFactor,
	TessRange,
	Foam,
	Detail)

namespace
{
	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::waterWaves.OnSetup(a_shader, a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::waterWaves.OnRestore();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_BindShore
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::waterWaves.OnLightingSetup();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void CopySlot(UINT a_slot)
	{
		auto context = globals::d3d::context;
		ID3D11Buffer* cb = nullptr;
		context->VSGetConstantBuffers(a_slot, 1, &cb);
		context->HSSetConstantBuffers(a_slot, 1, &cb);
		context->DSSetConstantBuffers(a_slot, 1, &cb);
		if (cb)
			cb->Release();
	}
}

void WaterWaves::PostPostLoad()
{
	vr = globals::game::isVR;
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x7, BSWaterShader_RestoreGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x6, BSLightingShader_BindShore>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Water Waves] Installed BSWaterShader SetupGeometry/RestoreGeometry");
}

void WaterWaves::DataLoaded()
{
	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = 16;
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	const float zeros[4]{};
	D3D11_SUBRESOURCE_DATA init{ zeros, 0, 0 };
	DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cb, &init, tessCB.put()));
	Util::SetResourceName(tessCB.get(), "WaterWaves Tess");

	D3D11_TEXTURE2D_DESC shoreDesc{};
	shoreDesc.Width = kShoreCells;
	shoreDesc.Height = kShoreCells;
	shoreDesc.MipLevels = 1;
	shoreDesc.ArraySize = 1;
	shoreDesc.Format = DXGI_FORMAT_R32_FLOAT;
	shoreDesc.SampleDesc.Count = 1;
	shoreDesc.Usage = D3D11_USAGE_DEFAULT;
	shoreDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	DX::ThrowIfFailed(globals::d3d::device->CreateTexture2D(&shoreDesc, nullptr, shoreTexture.put()));
	D3D11_SHADER_RESOURCE_VIEW_DESC shoreView{};
	shoreView.Format = DXGI_FORMAT_R32_FLOAT;
	shoreView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shoreView.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(shoreTexture.get(), &shoreView, shoreSRV.put()));
	Util::SetResourceName(shoreTexture.get(), "WaterWaves Shore");
	Util::SetResourceName(shoreSRV.get(), "WaterWaves Shore SRV");
	shoreCPU.assign(static_cast<size_t>(kShoreCells * kShoreCells), -1.0e8f);
}

WaterWaveMath::Params WaterWaves::MakeParams(bool a_flowmap) const
{
	Params params{};
	params.enabled = loaded && settings.Enabled;
	params.flowmap = a_flowmap;
	params.time = globals::state ? globals::state->timer : 0.0f;
	params.amplitude = std::max(settings.Amplitude, 0.0f);
	params.chop = std::clamp(settings.Chop, 0.0f, 1.5f);
	params.scale = 0.0f;
	if (params.enabled) {
		params.scale = std::max(Util::IsInterior() ? settings.InteriorScale : settings.SeaScale, 0.0f);
	}
	params.windSpeed = smoothedWind.load(std::memory_order_relaxed);
	params.windAngle = smoothedAngle.load(std::memory_order_relaxed);
	return params;
}

bool WaterWaves::ReadSharedWind(float& a_speed, float& a_angle) const
{
	auto& wind = globals::features::wind;
	if (!wind.loaded)
		return false;
	const float blend = std::clamp(wind.windFieldTransitionBlend, 0.0f, 1.0f);
	const float3 transitionVelocity = wind.windFieldTransition.direction * wind.windFieldTransition.speed;
	const float3 selected = wind.windFieldSelectedVelocity;
	const float3 velocity = transitionVelocity + (selected - transitionVelocity) * blend;
	const float speed = std::hypot(velocity.x, velocity.y);
	if (!std::isfinite(speed))
		return false;
	a_speed = speed;
	if (speed > 0.0001f)
		a_angle = std::atan2(velocity.y, velocity.x);
	else if (auto* sky = RE::Sky::GetSingleton(); sky && std::isfinite(sky->windAngle))
		a_angle = sky->windAngle;
	else
		a_angle = smoothedAngle.load(std::memory_order_relaxed);
	return true;
}

float WaterWaves::RainTarget() const
{
	auto* sky = RE::Sky::GetSingleton();
	if (!sky)
		return 0.0f;
	const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 1.0f;
	auto weight = [&](const RE::TESWeather* weather, bool current) {
		if (!weather || !weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy))
			return 0.0f;
		if (current) {
			const float fadeIn = static_cast<float>(weather->data.precipitationBeginFadeIn) * (1.0f / 255.0f);
			if (fadeIn >= 1.0f)
				return pct >= 1.0f ? 1.0f : 0.0f;
			return std::clamp((pct - fadeIn) / (1.0f - fadeIn), 0.0f, 1.0f);
		}
		const float fadeOut = static_cast<float>(weather->data.precipitationEndFadeOut) * (1.0f / 255.0f);
		if (fadeOut <= 0.0f)
			return pct <= 0.0f ? 1.0f : 0.0f;
		return 1.0f - std::clamp(pct / fadeOut, 0.0f, 1.0f);
	};
	return std::clamp(weight(sky->currentWeather, true) + weight(sky->lastWeather, false), 0.0f, 1.0f);
}

void WaterWaves::UpdateWeather()
{
	float targetSpeed = 0.0f;
	float targetAngle = smoothedAngle.load(std::memory_order_relaxed);
	if (!ReadSharedWind(targetSpeed, targetAngle)) {
		if (auto* sky = RE::Sky::GetSingleton()) {
			if (std::isfinite(sky->windSpeed))
				targetSpeed = sky->windSpeed;
			if (std::isfinite(sky->windAngle))
				targetAngle = sky->windAngle;
		}
	}
	targetSpeed = std::clamp(targetSpeed, 0.0f, 1.5f);
	if (!std::isfinite(targetAngle))
		targetAngle = 0.0f;
	const float targetRain = RainTarget();
	float dt = 0.0f;
	if (!(globals::game::ui && globals::game::ui->GameIsPaused()))
		dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	if (!weatherInit.load(std::memory_order_relaxed)) {
		smoothedWind.store(targetSpeed, std::memory_order_relaxed);
		smoothedAngle.store(targetAngle, std::memory_order_relaxed);
		smoothedRain.store(targetRain, std::memory_order_relaxed);
		weatherInit.store(true, std::memory_order_relaxed);
		return;
	}
	const float k = 1.0f - std::exp(-dt * 1.4f);
	const float speed = smoothedWind.load(std::memory_order_relaxed);
	smoothedWind.store(speed + (targetSpeed - speed) * k, std::memory_order_relaxed);
	const float angle = smoothedAngle.load(std::memory_order_relaxed);
	const float currentX = std::cos(angle);
	const float currentY = std::sin(angle);
	const float targetX = std::cos(targetAngle);
	const float targetY = std::sin(targetAngle);
	const float mixedX = currentX + (targetX - currentX) * k;
	const float mixedY = currentY + (targetY - currentY) * k;
	if (std::hypot(mixedX, mixedY) > 1.0e-4f)
		smoothedAngle.store(std::atan2(mixedY, mixedX), std::memory_order_relaxed);
	const float rain = smoothedRain.load(std::memory_order_relaxed);
	smoothedRain.store(rain + (targetRain - rain) * k, std::memory_order_relaxed);
}

bool WaterWaves::CellUsesFlowmap(const RE::TESObjectREFR* a_ref) const
{
	auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
	if (!cell)
		return false;
	auto* extra = cell->extraList.GetByType<RE::ExtraCellWaterType>();
	return extra && extra->water && extra->water->flags.any(RE::TESWaterForm::Flag::kEnableFlowmap);
}

float WaterWaves::DisplacedHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos, float a_flatHeight) const
{
	if (!(a_flatHeight > -1.0e8f))
		return a_flatHeight;
	if (CellUsesFlowmap(a_ref))
		return a_flatHeight;
	const Params params = MakeParams(false);
	if (!params.enabled)
		return a_flatHeight;
	float camX = 0.0f;
	float camY = 0.0f;
	const auto eye = Util::GetEyePosition(0);
	camX = a_pos.x - eye.x;
	camY = a_pos.y - eye.y;
	const float* landGrid = (shoreReady && !shoreCPU.empty()) ? shoreCPU.data() : nullptr;
	const float land = SampleLandZ(a_pos.x, a_pos.y, shoreOriginX, shoreOriginY, kShoreCell, kShoreCells, landGrid);
	const float landGap = land < -1.0e7f ? 1.0e6f : (a_flatHeight - land);
	const float damp = ShoreDamp(landGap, settings.ShoreLength);
	const Sample sample = Evaluate(a_pos.x, a_pos.y, camX, camY, params, false);
	return a_flatHeight + sample.height * damp;
}

void WaterWaves::ApplyShared(float& a_displacedHeight, float& a_cameraBelow, float& a_scale, float& a_amplitude, float& a_chop, float& a_shore, float& a_windSpeed, float& a_windAngle, float& a_shoreOriginX, float& a_shoreOriginY, float& a_shoreCellSize, float& a_shoreCellCount, float& a_foam, float& a_detail, float& a_murk, float& a_storm)
{
	a_displacedHeight = -RE::NI_INFINITY;
	a_cameraBelow = 0.0f;
	a_scale = 0.0f;
	a_amplitude = 0.0f;
	a_chop = 0.0f;
	a_shore = std::max(settings.ShoreLength, 1.0f);
	a_windSpeed = 0.0f;
	a_windAngle = 0.0f;
	a_foam = 0.0f;
	a_detail = 0.0f;
	a_murk = 0.0f;
	a_storm = 0.0f;
	a_shoreOriginX = 0.0f;
	a_shoreOriginY = 0.0f;
	a_shoreCellSize = 0.0f;
	a_shoreCellCount = static_cast<float>(kShoreCells);
	if (!loaded || !settings.Enabled)
		return;
	UpdateWeather();
	const WaterWaveMath::WaveResponse weather = WaterWaveMath::WaveResponseAt(smoothedWind.load(std::memory_order_relaxed));
	const float rain = smoothedRain.load(std::memory_order_relaxed);
	a_foam = std::clamp(settings.Foam, 0.0f, 2.0f);
	a_detail = std::clamp(settings.Detail, 0.0f, 2.0f);
	a_storm = std::clamp(weather.storm, 0.0f, 1.0f);
	a_murk = std::clamp(weather.storm * 0.55f + rain * 0.85f, 0.0f, 1.0f);
	UpdateShoreGrid();
	a_shoreOriginX = shoreOriginX;
	a_shoreOriginY = shoreOriginY;
	a_shoreCellSize = shoreReady ? kShoreCell : 0.0f;
	a_shoreCellCount = static_cast<float>(kShoreCells);
	const Params params = MakeParams(false);
	a_scale = params.scale;
	a_amplitude = params.amplitude;
	a_chop = params.chop;
	a_windSpeed = params.windSpeed;
	a_windAngle = params.windAngle;

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;
	const auto eye = Util::GetEyePosition(0);
	RE::NiPoint3 eyePos{ eye.x, eye.y, eye.z };
	// WaterHeightAt adds the displacement once, including on a flowmap where that addition is zero.
	const float surface = SubmersionWetness::WaterHeightAt(player, eyePos);
	if (surface > -1.0e8f) {
		a_displacedHeight = surface - eye.z;
		if (eye.z < surface - 0.5f)
			a_cameraBelow = 1.0f;
	}

	if (!tessCB)
		return;
	const float data[4] = {
		std::clamp(settings.MaxTessFactor, 1.0f, 8.0f),
		std::max(settings.TessRange, 1.0f),
		params.enabled ? 1.0f : 0.0f,
		0.0f
	};
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(globals::d3d::context->Map(tessCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		std::memcpy(mapped.pData, data, sizeof(data));
		globals::d3d::context->Unmap(tessCB.get(), 0);
	}
}

void WaterWaves::OnSetup(RE::BSShader* a_shader, RE::BSRenderPass*)
{
	if (tessBound)
		OnRestore();
	auto context = globals::d3d::context;
	if (!context || !loaded || !settings.Enabled)
		return;
	BindShore();

	ID3D11ShaderResourceView* depth = nullptr;
	context->PSGetShaderResources(7, 1, &depth);
	context->VSSetShaderResources(7, 1, &depth);
	ID3D11SamplerState* samp = nullptr;
	context->PSGetSamplers(7, 1, &samp);
	context->VSSetSamplers(7, 1, &samp);

	const uint32_t desc = globals::state->modifiedVertexDescriptor;
	const uint32_t technique = (desc >> 11) & 0xF;
	const bool flowmap = (desc & static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::Flowmap)) != 0;
	const bool lod = technique == static_cast<uint32_t>(SIE::ShaderCache::WaterShaderTechniques::Lod);
	if (vr || flowmap || lod || !a_shader || !globals::shaderCache->IsEnabled()) {
		if (depth)
			depth->Release();
		if (samp)
			samp->Release();
		return;
	}
	auto* shaders = GetTessShaders(a_shader, desc);
	if (!shaders) {
		if (depth)
			depth->Release();
		if (samp)
			samp->Release();
		return;
	}

	ID3D11Buffer* cb = tessCB.get();
	context->HSSetConstantBuffers(7, 1, &cb);
	context->DSSetConstantBuffers(7, 1, &cb);
	for (UINT slot : { 0u, 1u, 2u, 5u, 12u })
		CopySlot(slot);
	context->HSSetShaderResources(7, 1, &depth);
	context->DSSetShaderResources(7, 1, &depth);
	context->HSSetSamplers(7, 1, &samp);
	context->DSSetSamplers(7, 1, &samp);
	if (depth)
		depth->Release();
	if (samp)
		samp->Release();

	context->HSSetShader(shaders->hs.get(), nullptr, 0);
	context->DSSetShader(shaders->ds.get(), nullptr, 0);
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	shadowState.topology = REX::W32::D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
	shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	tessBound = true;
}

void WaterWaves::OnLightingSetup()
{
	if (!loaded || !settings.Enabled)
		return;
	BindShore();
}

void WaterWaves::BindShore() const
{
	auto* context = globals::d3d::context;
	if (!context || !shoreSRV)
		return;
	ID3D11ShaderResourceView* srv = shoreSRV.get();
	context->VSSetShaderResources(59, 1, &srv);
	context->HSSetShaderResources(59, 1, &srv);
	context->DSSetShaderResources(59, 1, &srv);
	context->PSSetShaderResources(59, 1, &srv);
}

void WaterWaves::FillShoreRow(int a_y)
{
	auto* tes = RE::TES::GetSingleton();
	for (int x = 0; x < kShoreCells; ++x) {
		float land = -1.0e8f;
		if (tes) {
			const float wx = shoreOriginX + static_cast<float>(x) * kShoreCell;
			const float wy = shoreOriginY + static_cast<float>(a_y) * kShoreCell;
			RE::NiPoint3 probe{ wx, wy, 0.0f };
			float height = 0.0f;
			if (tes->GetLandHeight(probe, height))
				land = height;
		}
		shoreCPU[static_cast<size_t>(a_y * kShoreCells + x)] = land;
	}
}

void WaterWaves::FillShoreColumn(int a_x)
{
	auto* tes = RE::TES::GetSingleton();
	for (int y = 0; y < kShoreCells; ++y) {
		float land = -1.0e8f;
		if (tes) {
			const float wx = shoreOriginX + static_cast<float>(a_x) * kShoreCell;
			const float wy = shoreOriginY + static_cast<float>(y) * kShoreCell;
			RE::NiPoint3 probe{ wx, wy, 0.0f };
			float height = 0.0f;
			if (tes->GetLandHeight(probe, height))
				land = height;
		}
		shoreCPU[static_cast<size_t>(y * kShoreCells + a_x)] = land;
	}
}

void WaterWaves::ShiftShore(int a_dx, int a_dy)
{
	const int n = kShoreCells;
	std::vector<float> next(static_cast<size_t>(n * n), -1.0e8f);
	for (int y = 0; y < n; ++y) {
		for (int x = 0; x < n; ++x) {
			const int ox = x + a_dx;
			const int oy = y + a_dy;
			if (ox >= 0 && oy >= 0 && ox < n && oy < n)
				next[static_cast<size_t>(y * n + x)] = shoreCPU[static_cast<size_t>(oy * n + ox)];
		}
	}
	shoreCPU.swap(next);
	if (a_dx > 0) {
		for (int x = n - a_dx; x < n; ++x)
			FillShoreColumn(x);
	} else if (a_dx < 0) {
		for (int x = 0; x < -a_dx; ++x)
			FillShoreColumn(x);
	}
	if (a_dy > 0) {
		for (int y = n - a_dy; y < n; ++y)
			FillShoreRow(y);
	} else if (a_dy < 0) {
		for (int y = 0; y < -a_dy; ++y)
			FillShoreRow(y);
	}
}

void WaterWaves::UploadShoreGrid()
{
	auto* context = globals::d3d::context;
	if (!context || !shoreTexture || shoreCPU.size() != static_cast<size_t>(kShoreCells * kShoreCells))
		return;
	context->UpdateSubresource(shoreTexture.get(), 0, nullptr, shoreCPU.data(), kShoreCells * sizeof(float), 0);
}

void WaterWaves::UpdateShoreGrid()
{
	if (!shoreTexture || !RE::TES::GetSingleton()) {
		shoreReady = false;
		return;
	}
	const auto eye = Util::GetEyePosition(0);
	const float originX = std::floor(eye.x / kShoreCell) * kShoreCell - (kShoreCells / 2.0f) * kShoreCell;
	const float originY = std::floor(eye.y / kShoreCell) * kShoreCell - (kShoreCells / 2.0f) * kShoreCell;
	if (shoreCPU.size() != static_cast<size_t>(kShoreCells * kShoreCells))
		shoreCPU.assign(static_cast<size_t>(kShoreCells * kShoreCells), -1.0e8f);
	int shiftX = 0;
	int shiftY = 0;
	if (shoreReady) {
		shiftX = static_cast<int>(std::lround((originX - shoreOriginX) / kShoreCell));
		shiftY = static_cast<int>(std::lround((originY - shoreOriginY) / kShoreCell));
	}
	if (!shoreReady || std::abs(shiftX) >= kShoreCells || std::abs(shiftY) >= kShoreCells) {
		shoreOriginX = originX;
		shoreOriginY = originY;
		for (int y = 0; y < kShoreCells; ++y)
			FillShoreRow(y);
		shoreCursor = 0;
		shoreReady = true;
	} else {
		if (shiftX != 0 || shiftY != 0) {
			shoreOriginX = originX;
			shoreOriginY = originY;
			ShiftShore(shiftX, shiftY);
		}
		for (int i = 0; i < kShoreRowsPerFrame; ++i) {
			FillShoreRow(shoreCursor);
			shoreCursor = (shoreCursor + 1) % kShoreCells;
		}
	}
	UploadShoreGrid();
}

void WaterWaves::OnRestore()
{
	if (!tessBound)
		return;
	auto context = globals::d3d::context;
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	shadowState.topology = REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	tessBound = false;
}

void WaterWaves::QueueTessCompile(RE::BSShader* a_shader, uint32_t a_descriptor)
{
	a_descriptor = CanonicalTessDescriptor(a_descriptor);
	auto& slot = tessShaders[a_descriptor];
	if (slot)
		return;
	slot = std::make_shared<TessShaders>();
	CompileJob job{ a_descriptor, SIE::GetShaderSourcePath(*a_shader), SIE::GetShaderDefinesForDescriptor(*a_shader, a_descriptor), slot };
	tessQueue.push_back(std::move(job));
	if (!tessWorker.joinable())
		tessWorker = std::jthread([this](std::stop_token st) { CompileWorker(st); });
	tessCv.notify_one();
}

void WaterWaves::CompileWorker(std::stop_token a_stop)
{
	while (!a_stop.stop_requested()) {
		CompileJob job;
		{
			std::unique_lock lock(tessLock);
			if (!tessCv.wait(lock, a_stop, [&] { return !tessQueue.empty(); }))
				return;
			job = std::move(tessQueue.front());
			tessQueue.pop_front();
		}
		bool ok = true;
		for (const char* stage : { "HSHADER", "DSHADER" }) {
			std::vector<std::pair<const char*, const char*>> defines;
			defines.emplace_back(stage, "");
			for (const auto& [name, value] : job.defines)
				defines.emplace_back(name.c_str(), value.c_str());
			const bool hull = std::string_view(stage) == "HSHADER";
			auto* shader = Util::CompileShader(job.path.c_str(), defines, hull ? "hs_5_0" : "ds_5_0");
			if (!shader) {
				logger::warn("[Water Waves] Failed to compile {} for descriptor {:X}", stage, job.descriptor);
				ok = false;
				break;
			}
			if (hull)
				job.target->hs.attach(static_cast<ID3D11HullShader*>(shader));
			else
				job.target->ds.attach(static_cast<ID3D11DomainShader*>(shader));
		}
		job.target->state.store(ok ? 1 : 2, std::memory_order_release);
	}
}

WaterWaves::TessShaders* WaterWaves::GetTessShaders(RE::BSShader* a_shader, uint32_t a_descriptor)
{
	a_descriptor = CanonicalTessDescriptor(a_descriptor);
	std::lock_guard lock(tessLock);
	auto it = tessShaders.find(a_descriptor);
	if (it == tessShaders.end() || !it->second) {
		QueueTessCompile(a_shader, a_descriptor);
		return nullptr;
	}
	return it->second->state.load(std::memory_order_acquire) == 1 ? it->second.get() : nullptr;
}

void WaterWaves::ClearShaderCache()
{
	std::lock_guard lock(tessLock);
	tessQueue.clear();
	tessShaders.clear();
}

void WaterWaves::DrawSettings()
{
	BFUI::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	BFUI::SliderFloat(T(TKEY("amplitude"), "Wave Height"), &settings.Amplitude, 0.0f, 80.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("amplitude_tt"), "Height of the longest wave, in game units, before the weather scales it. Calm wind keeps this small. It fades to a flat plane between 4000 and 7000 units."));
	BFUI::SliderFloat(T(TKEY("chop"), "Chop"), &settings.Chop, 0.0f, 1.5f, "%.2f");
	BFUI::SliderFloat(T(TKEY("shore"), "Shore Length"), &settings.ShoreLength, 16.0f, 600.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shore_tt"), "How deep the water must be before the waves reach full height. The last 4 units at the beach stay flat, so the edge does not flicker."));
	BFUI::SliderFloat(T(TKEY("sea_scale"), "Sea Scale"), &settings.SeaScale, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("interior_scale"), "Interior Scale"), &settings.InteriorScale, 0.0f, 1.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("max_tess"), "Tessellation"), &settings.MaxTessFactor, 1.0f, 8.0f, "%.0f");
	BFUI::SliderFloat(T(TKEY("tess_range"), "Tessellation Range"), &settings.TessRange, 256.0f, 4096.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("tess_tt"), "Extra triangles near the camera, and only while the swell is large enough to need them. Calm water stays on the original mesh. At factor 1 the surface matches that mesh. Rivers and waterfalls are not moved."));
	BFUI::SliderFloat(T(TKEY("foam"), "Foam"), &settings.Foam, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("foam_tt"), "White water on the shore, around rocks, and on storm crests. Crest foam grows with the wind."));
	BFUI::SliderFloat(T(TKEY("detail"), "Detail"), &settings.Detail, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("detail_tt"), "Fine ripples on top of the swells. Two rotated samples of one normal map, so the pattern does not tile."));
}

void WaterWaves::LoadSettings(json& o_json)
{
	settings = o_json;
}

void WaterWaves::SaveSettings(json& o_json)
{
	o_json = settings;
}

void WaterWaves::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
