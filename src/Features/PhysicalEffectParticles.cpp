#include "PhysicalEffectParticles.h"

#include "Deferred.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Bearforj/TipUI.h"

#include <algorithm>
#include <cmath>

#define I18N_KEY_PREFIX "feature.physical_effect_particles."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalEffectParticles::Settings,
	Enabled, Sparks, MagicParticles, FireSparks, SparkSize, SparkLifetime, Brightness,
	SparkCount, MagicCount, FireSparkRate, FireSparkRadius, MagicMaxDistance)

namespace
{
	enum Kind : uint32_t
	{
		kSpark = 0,
		kEmber = 1,
		kFrost = 2,
		kShock = 3
	};

	constexpr const wchar_t* kShaderDir = L"Data\\Shaders\\PhysicalEffects\\";

	int ElementOf(const RE::MagicItem* a_magic)
	{
		if (!a_magic)
			return -1;
		for (auto* eff : a_magic->effects) {
			if (!eff || !eff->baseEffect)
				continue;
			const auto rv = eff->baseEffect->data.resistVariable;
			if (rv == RE::ActorValue::kResistFire)
				return 0;
			if (rv == RE::ActorValue::kResistFrost)
				return 1;
			if (rv == RE::ActorValue::kResistShock)
				return 2;
		}
		return -1;
	}
}

void PhysicalEffectParticles::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PhysicalEffectParticles::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PhysicalEffectParticles::RestoreDefaultSettings()
{
	settings = {};
}

void PhysicalEffectParticles::SetupResources()
{
	auto* device = globals::d3d::device;
	D3D11_BUFFER_DESC desc{};
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(Particle);
	desc.ByteWidth = sizeof(Particle) * kMaxParticles;
	std::vector<Particle> zero(kMaxParticles);
	D3D11_SUBRESOURCE_DATA init{ zero.data(), 0, 0 };
	DX::ThrowIfFailed(device->CreateBuffer(&desc, &init, particleBuffer.put()));
	Util::SetResourceName(particleBuffer.get(), "PhysicalEffectParticles::Particles");

	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv.Buffer.NumElements = kMaxParticles;
	DX::ThrowIfFailed(device->CreateShaderResourceView(particleBuffer.get(), &srv, particleSRV.put()));
	Util::SetResourceName(particleSRV.get(), "PhysicalEffectParticles::Particles SRV");
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = kMaxParticles;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(particleBuffer.get(), &uav, particleUAV.put()));
	Util::SetResourceName(particleUAV.get(), "PhysicalEffectParticles::Particles UAV");

	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(Event);
	desc.ByteWidth = sizeof(Event) * kMaxEventsPerFrame;
	DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, eventBuffer.put()));
	Util::SetResourceName(eventBuffer.get(), "PhysicalEffectParticles::Events");
	srv.Buffer.NumElements = kMaxEventsPerFrame;
	DX::ThrowIfFailed(device->CreateShaderResourceView(eventBuffer.get(), &srv, eventSRV.put()));
	Util::SetResourceName(eventSRV.get(), "PhysicalEffectParticles::Events SRV");

	D3D11_BUFFER_DESC cb{};
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cb.ByteWidth = (sizeof(FrameCB) + 15u) & ~15u;
	DX::ThrowIfFailed(device->CreateBuffer(&cb, nullptr, frameCB.put()));
	Util::SetResourceName(frameCB.get(), "PhysicalEffectParticles::Frame");

	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	DX::ThrowIfFailed(device->CreateBlendState(&blend, blendState.put()));
	Util::SetResourceName(blendState.get(), "PhysicalEffectParticles::AdditiveBlend");

	D3D11_DEPTH_STENCIL_DESC ds{};
	ds.DepthEnable = FALSE;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&ds, depthState.put()));
	Util::SetResourceName(depthState.get(), "PhysicalEffectParticles::NoDepth");

	D3D11_RASTERIZER_DESC rs{};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.DepthClipEnable = TRUE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rs, rasterState.put()));
	Util::SetResourceName(rasterState.get(), "PhysicalEffectParticles::Raster");

	pendingEvents.reserve(kMaxEventsPerFrame);
}

void PhysicalEffectParticles::ClearShaderCache()
{
	spawnCS = nullptr;
	simulateCS = nullptr;
	particleVS = nullptr;
	particlePS = nullptr;
	shadersFailed = false;
}

void PhysicalEffectParticles::EnsureShaders()
{
	if (shadersFailed || (spawnCS && simulateCS && particleVS && particlePS))
		return;
	const std::vector<std::pair<const char*, const char*>> none;
	auto path = [](const wchar_t* a_name) { return std::wstring(kShaderDir) + a_name; };
	if (!spawnCS)
		spawnCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path(L"SpawnCS.hlsl").c_str(), none, "cs_5_0")));
	if (!simulateCS)
		simulateCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path(L"SimulateCS.hlsl").c_str(), none, "cs_5_0")));
	if (!particleVS)
		particleVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(path(L"ParticleVS.hlsl").c_str(), none, "vs_5_0")));
	if (!particlePS)
		particlePS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(path(L"ParticlePS.hlsl").c_str(), none, "ps_5_0")));
	if (!(spawnCS && simulateCS && particleVS && particlePS)) {
		shadersFailed = true;
		logger::warn("[Physical Effect Particles] Shader compilation failed; feature disabled for this session");
	}
}

void PhysicalEffectParticles::DataLoaded()
{
	if (auto* events = RE::ScriptEventSourceHolder::GetSingleton())
		events->AddEventSink<RE::TESHitEvent>(&hitSink);
}

void PhysicalEffectParticles::Emit(Event a_event)
{
	if (!loaded || !settings.Enabled || a_event.count == 0)
		return;
	a_event.count = std::min(a_event.count, kSlotsPerEvent);
	std::lock_guard lock(eventMutex);
	if (pendingEvents.size() >= kMaxEventsPerFrame)
		return;
	a_event.seed = seedCounter++ * 2654435761u;
	pendingEvents.push_back(a_event);
}

RE::BSEventNotifyControl PhysicalEffectParticles::HitSink::ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*)
{
	globals::features::physicalEffectParticles.OnHit(a_event);
	return RE::BSEventNotifyControl::kContinue;
}

void PhysicalEffectParticles::OnHit(const RE::TESHitEvent* a_event)
{
	if (!a_event || !a_event->target || !loaded || !settings.Enabled)
		return;
	auto* actor = a_event->target.get()->As<RE::Actor>();
	auto* player = globals::game::player;
	if (!actor || !player || !actor->Is3DLoaded())
		return;
	if (actor->GetPosition().GetDistance(player->GetPosition()) > 4096.0f)
		return;

	const RE::NiPoint3 pos = actor->GetPosition();
	const float height = std::max(actor->GetHeight(), 32.0f);
	float3 dir{ 0.0f, 1.0f, 0.35f };
	if (auto* cause = a_event->cause.get()) {
		const RE::NiPoint3 d = pos - cause->GetPosition();
		const float len = std::max(d.Length(), 1.0f);
		dir = float3(d.x / len, d.y / len, 0.35f);
	}
	RE::MagicItem* magic = a_event->source ? RE::TESForm::LookupByID<RE::MagicItem>(a_event->source) : nullptr;
	const float dist = actor->GetPosition().GetDistance(player->GetPosition());
	const int element = ElementOf(magic);
	const bool blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
	const bool magicNear = dist < settings.MagicMaxDistance;

	if (settings.MagicParticles && magic && magicNear && element >= 0) {
		Event m{};
		m.position[0] = pos.x;
		m.position[1] = pos.y;
		m.position[2] = pos.z + height * 0.6f;
		m.direction[0] = dir.x;
		m.direction[1] = dir.y;
		m.direction[2] = dir.z;
		m.count = std::min(settings.MagicCount, kSlotsPerEvent);
		m.brightness = settings.Brightness;
		if (element == 0) {
			m.kind = kEmber;
			m.speed = 180.0f;
			m.spread = 0.85f;
			m.size = settings.SparkSize * 1.5f;
			m.lifetime = settings.SparkLifetime * 1.4f;
			m.color[0] = 1.0f;
			m.color[1] = 0.45f;
			m.color[2] = 0.08f;
			m.color[3] = 1.0f;
		} else if (element == 1) {
			m.kind = kFrost;
			m.speed = 220.0f;
			m.spread = 0.7f;
			m.size = settings.SparkSize * 1.8f;
			m.lifetime = settings.SparkLifetime * 1.2f;
			m.color[0] = 0.75f;
			m.color[1] = 0.9f;
			m.color[2] = 1.0f;
			m.color[3] = 1.0f;
		} else {
			m.kind = kShock;
			m.speed = 420.0f;
			m.spread = 0.45f;
			m.size = settings.SparkSize;
			m.lifetime = settings.SparkLifetime * 0.45f;
			m.color[0] = 0.55f;
			m.color[1] = 0.75f;
			m.color[2] = 1.0f;
			m.color[3] = 1.0f;
		}
		Emit(m);
	}

	if (!blocked || !settings.Sparks)
		return;
	Event e{};
	e.position[0] = pos.x;
	e.position[1] = pos.y;
	e.position[2] = pos.z + height * 0.65f;
	e.direction[0] = dir.x;
	e.direction[1] = dir.y;
	e.direction[2] = 0.55f;
	e.kind = kSpark;
	e.count = std::min(settings.SparkCount, kSlotsPerEvent);
	e.speed = 320.0f;
	e.spread = 0.9f;
	e.size = settings.SparkSize;
	e.lifetime = settings.SparkLifetime;
	e.brightness = settings.Brightness * 1.4f;
	e.color[0] = 1.0f;
	e.color[1] = 0.72f;
	e.color[2] = 0.28f;
	e.color[3] = 1.0f;
	Emit(e);
}

void PhysicalEffectParticles::UpdateFireSparks(float a_dt)
{
	if (!settings.FireSparks || !settings.Sparks || a_dt <= 0.0f)
		return;
	auto* player = globals::game::player;
	auto* tes = RE::TES::GetSingleton();
	if (!player || !tes)
		return;
	fireNextScan -= a_dt;
	if (fireNextScan <= 0.0f) {
		fireNextScan = 2.0f;
		fireSources.clear();
		static RE::BGSKeyword* forgeKw[2] = {
			RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingForge"),
			RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmelter")
		};
		tes->ForEachReferenceInRange(player, std::max(settings.FireSparkRadius, 300.0f), [&](RE::TESObjectREFR* a_ref) {
			if (fireSources.size() >= 12)
				return RE::BSContainer::ForEachResult::kStop;
			auto* base = a_ref && !a_ref->IsDisabled() ? a_ref->GetBaseObject() : nullptr;
			if (!base)
				return RE::BSContainer::ForEachResult::kContinue;
			bool fire = false;
			float lift = 20.0f;
			if (auto* light = base->As<RE::TESObjectLIGH>()) {
				const auto& c = light->data.color;
				fire = !light->GetNoFlicker() && c.red > c.green && c.green > c.blue && c.red > 150 && light->data.radius >= 300;
				lift = -40.0f;
			} else if (auto* furn = base->As<RE::TESFurniture>()) {
				for (auto* kw : forgeKw)
					fire = fire || (kw && furn->HasKeyword(kw));
				lift = 60.0f;
			}
			if (fire) {
				const auto p = a_ref->GetPosition();
				fireSources.push_back(float3(p.x, p.y, p.z + lift));
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		fireTimers.resize(fireSources.size(), 0.0f);
	}
	const float interval = 1.0f / std::clamp(settings.FireSparkRate, 0.1f, 20.0f);
	for (size_t i = 0; i < fireSources.size(); ++i) {
		fireTimers[i] -= a_dt;
		if (fireTimers[i] > 0.0f)
			continue;
		fireTimers[i] = interval;
		Event e{};
		e.position[0] = fireSources[i].x;
		e.position[1] = fireSources[i].y;
		e.position[2] = fireSources[i].z;
		e.direction[2] = 1.0f;
		e.kind = kEmber;
		e.count = 4;
		e.speed = 90.0f;
		e.spread = 0.6f;
		e.size = settings.SparkSize * 1.2f;
		e.lifetime = 1.6f;
		e.brightness = settings.Brightness;
		e.color[0] = 1.0f;
		e.color[1] = 0.5f;
		e.color[2] = 0.12f;
		e.color[3] = 1.0f;
		Emit(e);
	}
}

void PhysicalEffectParticles::PostComposite()
{
	CS_GPU_PASS("PhysicalEffectParticles::Composite");
	if (!settings.Enabled || !particleBuffer || !globals::deferred || globals::game::isVR)
		return;

	const auto now = std::chrono::steady_clock::now();
	float dt = lastFrame.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - lastFrame).count();
	lastFrame = now;
	if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused())
		dt = 0.0f;
	dt = std::clamp(dt, 0.0f, 0.1f);
	UpdateFireSparks(dt);

	std::vector<Event> events;
	{
		std::lock_guard lock(eventMutex);
		events.swap(pendingEvents);
		pendingEvents.reserve(kMaxEventsPerFrame);
	}
	if (!events.empty())
		aliveTimer = 0.0f;
	else
		aliveTimer += dt;
	if (events.empty() && aliveTimer > 3.0f)
		return;

	EnsureShaders();
	if (shadersFailed)
		return;
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	for (auto& e : events) {
		e.firstSlot = nextSlot;
		nextSlot = (nextSlot + kSlotsPerEvent) % kMaxParticles;
	}

	FrameCB frame{};
	const auto& posAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	frame.cameraPosAdjust = float4(posAdjust.x, posAdjust.y, posAdjust.z, 0.0f);
	frame.deltaTime = dt;
	frame.gravity = 980.0f;
	frame.eventCount = static_cast<uint32_t>(events.size());
	frame.maxParticles = kMaxParticles;
	frame.sizeScale = 1.0f;
	frame.lifetimeScale = 1.0f;
	frame.brightness = std::max(settings.Brightness, 0.0f);
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(frameCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &frame, sizeof(frame));
			context->Unmap(frameCB.get(), 0);
		}
	}

	ID3D11Buffer* cbs[]{ frameCB.get() };
	ID3D11UnorderedAccessView* uavs[]{ particleUAV.get() };
	ID3D11UnorderedAccessView* nullUAV[]{ nullptr };
	ID3D11ShaderResourceView* nullSRV[]{ nullptr };

	if (!events.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(eventBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, events.data(), events.size() * sizeof(Event));
			context->Unmap(eventBuffer.get(), 0);
			ID3D11ShaderResourceView* srvs[]{ eventSRV.get() };
			context->CSSetConstantBuffers(0, 1, cbs);
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(spawnCS.get(), nullptr, 0);
			context->Dispatch(frame.eventCount, 1, 1);
			context->CSSetShaderResources(0, 1, nullSRV);
		}
	}

	context->CSSetConstantBuffers(0, 1, cbs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(simulateCS.get(), nullptr, 0);
	context->Dispatch((kMaxParticles + 63u) / 64u, 1, 1);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	auto& main = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[0]];
	auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(main.RTV);
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	if (!rtv || !depthSRV)
		return;

	Util::FullscreenPassScope scope(context, 1, 0, 13);
	context->OMSetRenderTargets(1, &rtv, nullptr);
	auto resolution = float2{ static_cast<float>(globals::game::graphicsState->screenWidth), static_cast<float>(globals::game::graphicsState->screenHeight) };
	resolution = Util::ConvertToDynamic(resolution, true);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, resolution.x, resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterState.get());
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthState.get(), 0);
	ID3D11Buffer* nullVB = nullptr;
	UINT stride = 0, offset = 0;
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 1, &nullVB, &stride, &offset);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	ID3D11Buffer* perFrame = *globals::game::perFrame;
	ID3D11Buffer* shared[]{ globals::state->permutationCB->CB(), globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->VSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(4, 3, shared);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	ID3D11ShaderResourceView* vsSRV[]{ particleSRV.get() };
	context->VSSetShaderResources(0, 1, vsSRV);
	ID3D11ShaderResourceView* psSRV[]{ depthSRV };
	context->PSSetShaderResources(0, 1, psSRV);
	context->VSSetShader(particleVS.get(), nullptr, 0);
	context->PSSetShader(particlePS.get(), nullptr, 0);
	context->DrawInstanced(4, kMaxParticles, 0, 0);
	context->VSSetShaderResources(0, 1, nullSRV);
	context->PSSetShaderResources(0, 1, nullSRV);
}

void PhysicalEffectParticles::DrawSettings()
{
	BFUI::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	BFUI::Checkbox(T(TKEY("sparks"), "Sparks on blocks"), &settings.Sparks);
	BFUI::Checkbox(T(TKEY("magic"), "Magic particles"), &settings.MagicParticles);
	BFUI::Checkbox(T(TKEY("fire"), "Falling sparks from fires and forges"), &settings.FireSparks);
	BFUI::SliderFloat(T(TKEY("spark_size"), "Spark Size"), &settings.SparkSize, 0.2f, 3.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("spark_lifetime"), "Spark Lifetime"), &settings.SparkLifetime, 0.15f, 3.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("brightness"), "Brightness"), &settings.Brightness, 0.0f, 4.0f, "%.2f");
	int sparkCount = static_cast<int>(settings.SparkCount);
	if (BFUI::SliderInt(T(TKEY("spark_count"), "Sparks per block"), &sparkCount, 4, 48))
		settings.SparkCount = static_cast<uint32_t>(sparkCount);
	int magicCount = static_cast<int>(settings.MagicCount);
	if (BFUI::SliderInt(T(TKEY("magic_count"), "Magic particles per hit"), &magicCount, 4, 48))
		settings.MagicCount = static_cast<uint32_t>(magicCount);
	BFUI::SliderFloat(T(TKEY("fire_rate"), "Fire spark rate"), &settings.FireSparkRate, 0.2f, 12.0f, "%.1f");
	BFUI::SliderFloat(T(TKEY("fire_radius"), "Fire scan radius"), &settings.FireSparkRadius, 400.0f, 6000.0f, "%.0f");
	BFUI::SliderFloat(T(TKEY("magic_distance"), "Magic max distance"), &settings.MagicMaxDistance, 500.0f, 8000.0f, "%.0f");
}
