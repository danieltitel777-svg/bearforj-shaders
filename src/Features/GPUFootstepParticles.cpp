#include "GPUFootstepParticles.h"

#include "Deferred.h"
#include "Features/SubmersionWetness.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/UI.h"

#include <algorithm>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GPUFootstepParticles::Settings,
	Enabled, IncludeNPCs, Water, RainWetness, DustIntensity, SnowIntensity, MudIntensity, SplashIntensity,
	ParticleSize, Lifetime, Opacity, MaxDistance, MaxShallowDepth)

namespace
{
	// must match GPUFootsteps/Common.hlsli
	enum Surface : uint32_t
	{
		kHard = 0,
		kDirt = 1,
		kGravel = 2,
		kSand = 3,
		kGrass = 4,
		kMud = 5,
		kSnow = 6,
		kIce = 7,
		kAsh = 8,
		kWater = 9,
		kNone = 0xFFFFFFFF
	};
	constexpr uint32_t kFlagWetness = 1u << 0;
	constexpr uint32_t kFlagInterior = 1u << 3;
	constexpr uint32_t kEventLanding = 1u << 0;

	constexpr const wchar_t* kShaderDir = L"Data\\Shaders\\GPUFootsteps\\";
}

void GPUFootstepParticles::LoadSettings(json& o_json) { settings = o_json; }
void GPUFootstepParticles::SaveSettings(json& o_json) { o_json = settings; }
void GPUFootstepParticles::RestoreDefaultSettings() { settings = {}; }

void GPUFootstepParticles::DataLoaded()
{
	if (auto* manager = RE::BGSFootstepManager::GetSingleton()) {
		manager->AddEventSink<RE::BGSFootstepEvent>(this);
		logger::info("[GPU Footstep Particles] Registered footstep event sink");
	}
}

void GPUFootstepParticles::SetupResources()
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

	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv.Buffer.NumElements = kMaxParticles;
	DX::ThrowIfFailed(device->CreateShaderResourceView(particleBuffer.get(), &srv, particleSRV.put()));
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = kMaxParticles;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(particleBuffer.get(), &uav, particleUAV.put()));

	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.StructureByteStride = sizeof(Event);
	desc.ByteWidth = sizeof(Event) * kMaxEventsPerFrame;
	DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, eventBuffer.put()));
	srv.Buffer.NumElements = kMaxEventsPerFrame;
	DX::ThrowIfFailed(device->CreateShaderResourceView(eventBuffer.get(), &srv, eventSRV.put()));

	D3D11_BUFFER_DESC cb{};
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cb.ByteWidth = (sizeof(FrameCB) + 15) & ~15u;
	DX::ThrowIfFailed(device->CreateBuffer(&cb, nullptr, frameCB.put()));

	// premultiplied alpha
	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	DX::ThrowIfFailed(device->CreateBlendState(&blend, blendState.put()));

	D3D11_DEPTH_STENCIL_DESC ds{};
	ds.DepthEnable = FALSE;
	ds.StencilEnable = FALSE;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&ds, depthState.put()));

	D3D11_RASTERIZER_DESC rs{};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.DepthClipEnable = TRUE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rs, rasterState.put()));

	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&sd, linearSampler.put()));

	pendingEvents.reserve(kMaxEventsPerFrame);
}

void GPUFootstepParticles::ClearShaderCache()
{
	spawnCS = nullptr;
	simulateCS = nullptr;
	particleVS = nullptr;
	particlePS = nullptr;
	shadersFailed = false;
}

void GPUFootstepParticles::EnsureShaders()
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
		logger::warn("[GPU Footstep Particles] Shader compilation failed; feature disabled for this session");
	}
}

uint32_t GPUFootstepParticles::Classify(RE::Actor* a_actor, const RE::NiPoint3& a_foot, float& a_waterDepth, float& a_surfaceZ) const
{
	a_waterDepth = 0.0f;
	a_surfaceZ = a_foot.z;
	const float waterZ = SubmersionWetness::WaterHeightAt(a_actor, a_foot);
	if (waterZ != -RE::NI_INFINITY && waterZ > -1.0e8f) {
		const float depth = waterZ - a_foot.z;
		if (depth > 0.5f) {
			if (!settings.Water || depth > settings.MaxShallowDepth)
				return kNone;
			a_waterDepth = depth;
			a_surfaceZ = waterZ;
			return kWater;
		}
	}

	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return kHard;
	switch (tes->GetLandMaterialType(a_foot)) {
	case RE::MATERIAL_ID::kWaterPuddle:
		if (!settings.Water)
			return kNone;
		a_waterDepth = 2.0f;
		return kWater;
	case RE::MATERIAL_ID::kSnow:
	case RE::MATERIAL_ID::kSnowStairs:
		return kSnow;
	case RE::MATERIAL_ID::kIce:
		return kIce;
	case RE::MATERIAL_ID::kMud:
		return kMud;
	case RE::MATERIAL_ID::kSand:
		return kSand;
	case RE::MATERIAL_ID::kDirt:
		return kDirt;
	case RE::MATERIAL_ID::kGravel:
		return kGravel;
	case RE::MATERIAL_ID::kGrass:
		return kGrass;
	default:
		return kNone;  // stone, wood, metal: nothing to kick up
	}
}

RE::BSEventNotifyControl GPUFootstepParticles::ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*)
{
	if (!a_event || !loaded || !settings.Enabled)
		return RE::BSEventNotifyControl::kContinue;

	auto actorPtr = a_event->actor.get();
	auto* actor = actorPtr.get();
	if (!actor || !actor->Is3DLoaded() || actor->IsInMidair())
		return RE::BSEventNotifyControl::kContinue;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!settings.IncludeNPCs && actor != player)
		return RE::BSEventNotifyControl::kContinue;
	if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
		if (camera->cameraRoot->world.translate.GetDistance(actor->GetPosition()) > settings.MaxDistance)
			return RE::BSEventNotifyControl::kContinue;
	}

	RE::NiPoint3 foot = actor->GetPosition();
	const std::string_view tag = a_event->tag.c_str() ? a_event->tag.c_str() : "";
	if (auto* root = actor->Get3D()) {
		const char* bone = nullptr;
		if (tag.find("Left") != std::string_view::npos || tag.find("FootL") != std::string_view::npos)
			bone = "NPC L Foot [Lft ]";
		else if (tag.find("Right") != std::string_view::npos || tag.find("FootR") != std::string_view::npos)
			bone = "NPC R Foot [Rft ]";
		if (bone) {
			if (auto* node = root->GetObjectByName(bone)) {
				foot.x = node->world.translate.x;
				foot.y = node->world.translate.y;
			}
		}
	}
	foot.z = actor->GetPositionZ();

	float waterDepth = 0.0f;
	float surfaceZ = foot.z;
	const uint32_t surface = Classify(actor, foot, waterDepth, surfaceZ);
	if (surface == kNone)
		return RE::BSEventNotifyControl::kContinue;

	RE::NiPoint3 velocity{};
	actor->GetLinearVelocity(velocity);
	const float heading = actor->GetAngleZ();

	Event e{};
	e.position[0] = foot.x;
	e.position[1] = foot.y;
	e.position[2] = surfaceZ;
	e.velocity[0] = velocity.x;
	e.velocity[1] = velocity.y;
	e.velocity[2] = velocity.z;
	e.surface = surface;
	e.forward[0] = std::sin(heading);
	e.forward[1] = std::cos(heading);
	e.strength = (actor->IsSprinting() ? 1.5f : actor->IsRunning() ? 1.2f : 1.0f);
	e.scale = std::clamp(actor->GetHeight() / 128.0f, 0.4f, 3.0f);
	e.flags = tag.find("Land") != std::string_view::npos ? kEventLanding : 0u;
	e.slotCount = kSlotsPerEvent;
	e.waterDepth = waterDepth;

	std::scoped_lock lock(eventMutex);
	if (pendingEvents.size() < kMaxEventsPerFrame) {
		e.seed = seedCounter++ * 2654435761u;
		pendingEvents.push_back(e);
	}
	return RE::BSEventNotifyControl::kContinue;
}

void GPUFootstepParticles::PostComposite()
{
	if (!settings.Enabled || !particleBuffer || !globals::deferred || globals::game::isVR)
		return;

	const auto now = std::chrono::steady_clock::now();
	float dt = lastFrame.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - lastFrame).count();
	lastFrame = now;
	if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused())
		dt = 0.0f;
	dt = std::clamp(dt, 0.0f, 0.1f);

	std::vector<Event> events;
	{
		std::scoped_lock lock(eventMutex);
		events.swap(pendingEvents);
		pendingEvents.reserve(kMaxEventsPerFrame);
	}
	if (!events.empty())
		aliveTimer = 0.0f;
	else
		aliveTimer += dt;
	// longest particle (dust) lives ~2 s times the lifetime scale; nothing left to simulate or draw after that
	if (events.empty() && aliveTimer > 2.5f * std::max(settings.Lifetime, 0.1f) + 0.5f)
		return;

	EnsureShaders();
	if (shadersFailed)
		return;

	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	for (auto& e : events) {
		e.firstSlot = nextSlot;
		nextSlot = (nextSlot + kSlotsPerEvent) % kMaxParticles;
	}

	// frame constants
	FrameCB frame{};
	const auto& posAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	frame.cameraPosAdjust = float4(posAdjust.x, posAdjust.y, posAdjust.z, 0.0f);
	frame.deltaTime = dt;
	frame.gravity = 980.0f;
	frame.eventCount = static_cast<uint32_t>(events.size());
	frame.maxParticles = kMaxParticles;
	frame.dust = settings.DustIntensity;
	frame.snow = settings.SnowIntensity;
	frame.splash = settings.SplashIntensity;
	frame.mud = settings.MudIntensity;
	frame.sizeScale = settings.ParticleSize;
	frame.lifetimeScale = settings.Lifetime;
	frame.opacity = settings.Opacity;
	frame.softDistance = 12.0f;
	const auto* sky = RE::Sky::GetSingleton();
	auto* playerRef = RE::PlayerCharacter::GetSingleton();
	auto* playerCell = playerRef ? playerRef->GetParentCell() : nullptr;
	const bool interior = !sky || !playerCell || playerCell->IsInteriorCell();
	if (interior)
		frame.frameFlags |= kFlagInterior;
	if (settings.RainWetness && !interior && sky->IsRaining()) {
		frame.frameFlags |= kFlagWetness;
		frame.wetness = 0.8f;
	}
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(frameCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, &frame, sizeof(frame));
			context->Unmap(frameCB.get(), 0);
		}
	}

	ID3D11Buffer* cbs[1]{ frameCB.get() };
	ID3D11UnorderedAccessView* uavs[1]{ particleUAV.get() };
	ID3D11UnorderedAccessView* nullUAV[1]{};
	ID3D11ShaderResourceView* nullSRV[1]{};

	// spawn
	if (!events.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(eventBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, events.data(), events.size() * sizeof(Event));
			context->Unmap(eventBuffer.get(), 0);
			ID3D11ShaderResourceView* srvs[1]{ eventSRV.get() };
			context->CSSetConstantBuffers(0, 1, cbs);
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(spawnCS.get(), nullptr, 0);
			context->Dispatch(frame.eventCount, 1, 1);
			context->CSSetShaderResources(0, 1, nullSRV);
		}
	}

	// simulate
	context->CSSetConstantBuffers(0, 1, cbs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(simulateCS.get(), nullptr, 0);
	context->Dispatch((kMaxParticles + 63) / 64, 1, 1);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	// draw into the main forward target
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
	ID3D11Buffer* nullVB{};
	UINT stride = 0, offset = 0;
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 1, &nullVB, &stride, &offset);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	ID3D11Buffer* perFrame = *globals::game::perFrame;
	ID3D11Buffer* shared[3]{ globals::state->permutationCB->CB(), globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->VSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(4, 3, shared);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	ID3D11ShaderResourceView* vsSRV[1]{ particleSRV.get() };
	context->VSSetShaderResources(0, 1, vsSRV);
	ID3D11ShaderResourceView* psSRV[1]{ depthSRV };
	context->PSSetShaderResources(0, 1, psSRV);
	ID3D11SamplerState* samp = linearSampler.get();
	context->PSSetSamplers(0, 1, &samp);
	context->VSSetShader(particleVS.get(), nullptr, 0);
	context->PSSetShader(particlePS.get(), nullptr, 0);
	context->DrawInstanced(4, kMaxParticles, 0, 0);
	context->VSSetShaderResources(0, 1, nullSRV);
}

void GPUFootstepParticles::DrawSettings()
{
	auto tip = [](const char* a_key, const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(a_key, a_text));
	};
	ImGui::Checkbox(T("feature.gpu_footstep_particles.enabled", "Enabled"), &settings.Enabled);
	ImGui::Checkbox(T("feature.gpu_footstep_particles.include_npcs", "Include NPCs and Creatures"), &settings.IncludeNPCs);
	ImGui::Checkbox(T("feature.gpu_footstep_particles.water", "Puddles and Shallow Water"), &settings.Water);
	ImGui::Checkbox(T("feature.gpu_footstep_particles.rain", "Rain Makes Ground Wet"), &settings.RainWetness);
	tip("feature.gpu_footstep_particles.rain_tooltip", "In rain, dust stops and loose ground throws wet clods and droplets.");
	ImGui::SeparatorText(T("feature.gpu_footstep_particles.surfaces", "Surfaces"));
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.dust", "Dust and Dirt"), &settings.DustIntensity, 0.0f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.snow", "Snow"), &settings.SnowIntensity, 0.0f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.mud", "Mud"), &settings.MudIntensity, 0.0f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.splash", "Splashes"), &settings.SplashIntensity, 0.0f, 3.0f, "%.2f");
	ImGui::SeparatorText(T("feature.gpu_footstep_particles.appearance", "Appearance"));
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.size", "Particle Size"), &settings.ParticleSize, 0.25f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.lifetime", "Lifetime"), &settings.Lifetime, 0.25f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.opacity", "Opacity"), &settings.Opacity, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.max_distance", "Max Distance"), &settings.MaxDistance, 500.0f, 8000.0f, "%.0f");
	tip("feature.gpu_footstep_particles.max_distance_tooltip", "Footsteps further than this from the camera (in game units) spawn nothing.");
	ImGui::SliderFloat(T("feature.gpu_footstep_particles.depth", "Max Shallow Water Depth"), &settings.MaxShallowDepth, 5.0f, 80.0f, "%.0f");
	tip("feature.gpu_footstep_particles.depth_tooltip", "Deeper than this, the actor is wading or swimming and no splash is made.");
}
