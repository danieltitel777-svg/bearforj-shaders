#include "SubmersionWetness.h"
#include "Bearforj/TipUI.h"

#include "../I18n/I18n.h"
#include "../Utils/UI.h"
#include "Buffer.h"
#include "Globals.h"
#include "State.h"
#include "Skylighting.h"
#include "WetnessEffects.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.submersion_wetness."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SubmersionWetness::Settings,
	Enabled,
	IncludeCreatures,
	RainSoak,
	Strength,
	BeadDensity,
	HairStrength,
	ArmorStrength,
	RainCoatStrength,
	DryTime,
	EmergeFlowTime,
	EdgeSoftness,
	Range,
	RespectShelter,
	RainSoakTime,
	SoakInTime)

namespace
{
	constexpr float kNoWater = -1.0e9f;

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::submersionWetness.OnLightingSetupGeometry(a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::submersionWetness.OnLightingRestoreGeometry();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	RE::Actor* FindActor(RE::NiAVObject* a_object)
	{
		for (int depth = 0; a_object && depth < 32; ++depth, a_object = a_object->parent) {
			if (auto* ref = a_object->GetUserData())
				return ref->As<RE::Actor>();
		}
		return nullptr;
	}
}

void SubmersionWetness::SetupResources()
{
	perGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometry>(), "SubmersionWetness::PerGeometry");
	queryCB = new ConstantBuffer(ConstantBufferDesc<CBUInt4>(), "SubmersionWetness::QueryCB");
	auto device = globals::d3d::device;

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = sizeof(float4) * kMaxQueries;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(float4);
	DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, positionBuffer.put()));
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.NumElements = kMaxQueries;
	DX::ThrowIfFailed(device->CreateShaderResourceView(positionBuffer.get(), &srvDesc, positionSRV.put()));

	desc.ByteWidth = sizeof(float) * kMaxQueries;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.CPUAccessFlags = 0;
	desc.StructureByteStride = sizeof(float);
	DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, visibilityBuffer.put()));
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = kMaxQueries;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(visibilityBuffer.get(), &uavDesc, visibilityUAV.put()));

	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.ByteWidth = sizeof(float) * kMaxQueries;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (auto& buffer : staging)
		DX::ThrowIfFailed(device->CreateBuffer(&stagingDesc, nullptr, buffer.put()));
}

void SubmersionWetness::ClearShaderCache()
{
	visibilityCS = nullptr;
	visibilityCSFailed = false;
}

void SubmersionWetness::UpdateSkyVisibility(const std::vector<RE::Actor*>& a_actors)
{
	auto context = globals::d3d::context;
	auto& skylighting = globals::features::skylighting;
	const bool skyAvailable = skylighting.loaded && skylighting.texProbeArray && skylighting.texProbeArray->srv;

	// 1) collect a finished readback (oldest slot), never stalling the render thread
	const uint32_t readSlot = (queryFrame + 1) % kReadbackLatency;
	if (stagingPending[readSlot]) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = context->Map(staging[readSlot].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (SUCCEEDED(hr)) {
			const float* vis = static_cast<const float*>(mapped.pData);
			const auto& ids = stagingActors[readSlot];
			for (size_t i = 0; i < ids.size(); ++i) {
				auto it = actors.find(ids[i]);
				if (it != actors.end() && std::isfinite(vis[i]))
					it->second.skyVisibility = std::clamp(vis[i], 0.0f, 1.0f);
			}
			context->Unmap(staging[readSlot].get(), 0);
			stagingPending[readSlot] = false;
		}
	}

	if (!skyAvailable || a_actors.empty()) {
		if (!skyAvailable)
			for (auto& [id, st] : actors)
				st.skyVisibility = 1.0f;  // no occlusion data: fall back to the open-sky behaviour
		return;
	}
	if (!visibilityCS && !visibilityCSFailed) {
		visibilityCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SubmersionWetness\\ActorSkyVisibilityCS.hlsl", {}, "cs_5_0")));
		visibilityCSFailed = !visibilityCS;
	}
	if (!visibilityCS)
		return;

	// 2) queue this frame's query
	const uint32_t writeSlot = queryFrame % kReadbackLatency;
	if (stagingPending[writeSlot])
		return;  // GPU still busy with that slot; skip a frame
	const RE::NiPoint3 eye = Util::GetEyePosition(0);
	auto& ids = stagingActors[writeSlot];
	ids.clear();
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(positionBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	auto* positions = static_cast<float4*>(mapped.pData);
	for (auto* actor : a_actors) {
		if (ids.size() >= kMaxQueries)
			break;
		const RE::NiPoint3 pos = actor->GetPosition();
		const float height = std::max(actor->GetHeight(), 16.0f);
		positions[ids.size()] = float4(pos.x - eye.x, pos.y - eye.y, pos.z + height - eye.z, 1.0f);
		ids.push_back(actor->GetFormID());
	}
	context->Unmap(positionBuffer.get(), 0);

	queryCB->Update(CBUInt4{ static_cast<uint32_t>(ids.size()), 0u, 0u, 0u });
	globals::state->BindSharedDataCS(context);
	ID3D11Buffer* cb = queryCB->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* srvs[2] = { positionSRV.get(), skylighting.texProbeArray->srv.get() };
	context->CSSetShaderResources(0, 2, srvs);
	ID3D11UnorderedAccessView* uav = visibilityUAV.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(visibilityCS.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);
	ID3D11ShaderResourceView* nullSRV[2] = {};
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 2, nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
	context->CopyResource(staging[writeSlot].get(), visibilityBuffer.get());
	stagingPending[writeSlot] = true;
}

void SubmersionWetness::PostPostLoad()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Submersion Wetness] Installed lighting geometry hooks");
}

void SubmersionWetness::UpdateActor(RE::Actor* a_actor, float a_dt, float a_rain)
{
	auto& st = actors[a_actor->GetFormID()];
	st.lastSeenFrame = frame;
	const RE::NiPoint3 pos = a_actor->GetPosition();
	st.feetZ = pos.z;
	st.height = std::max(a_actor->GetHeight(), 16.0f);

	float waterZ = WaterHeightAt(a_actor, pos);
	const float prevDepth = st.depth;
	st.depth = waterZ > kNoWater ? std::clamp(waterZ - st.feetZ, 0.0f, st.height + 16.0f) : 0.0f;
	st.waterZ = waterZ;

	const float dryScale = globals::features::wetnessEffects.GetDryingScale(a_actor);
	const float dryTime = std::max(settings.DryTime, 1.0f) / std::max(dryScale, 0.15f);
	const bool fullyUnder = st.depth >= st.height * 0.98f;
	if (fullyUnder) {
		// The whole body is under the surface: soak immediately. A 60% threshold is not used.
		st.soakLine = st.height;
		st.soak = 1.0f;
	} else if (st.depth > 1.0f) {
		// Wading in: the wet line climbs toward the water over SoakInTime (ankles, then knees, then
		// waist) instead of jumping to the current depth, and the soaked part fills in over the same time.
		const float desired = std::max(st.soakLine, st.depth);
		const float climb = st.height / std::max(settings.SoakInTime, 0.1f);
		st.soakLine = std::min(desired, st.soakLine + climb * a_dt);
		st.soak = std::min(1.0f, st.soak + a_dt / std::max(settings.SoakInTime, 0.1f));
	} else {
		st.soak = std::max(0.0f, st.soak - a_dt / dryTime);
		// The waterline creeps down as the actor dries, so only the still-soaked part stays wet.
		st.soakLine = std::max(0.0f, st.soakLine - st.height * a_dt / dryTime);
		if (st.soak <= 0.0f)
			st.soakLine = 0.0f;
	}

	const float headLine = st.height * 0.85f;
	if (prevDepth >= headLine && st.depth < headLine)
		st.flow = 1.0f;  // the head just left the water: rivulets run down
	else
		st.flow = std::max(0.0f, st.flow - a_dt / std::max(settings.EmergeFlowTime, 0.5f));

	auto* cell = a_actor->GetParentCell();
	const bool exterior = cell && !cell->IsInteriorCell();
	// Rain soak: rises while the actor stands under open sky, dries (like the water soak) under cover.
	const float shelter = settings.RespectShelter ? std::clamp((st.skyVisibility - 0.25f) / 0.5f, 0.0f, 1.0f) : 1.0f;
	const float target = (settings.RainSoak && exterior) ? a_rain * settings.RainCoatStrength * shelter : 0.0f;
	const float soakTime = std::max(settings.RainSoakTime, 0.5f);
	if (target > 0.02f) {
		// Droplets reach from the head downward first. Darkening follows that front.
		st.rainProgress = std::min(1.0f, st.rainProgress + a_dt / soakTime);
		const float darkTarget = std::clamp((st.rainProgress - 0.08f) / 0.92f, 0.0f, 1.0f) * std::min(target / std::max(settings.RainCoatStrength, 0.05f), 1.0f);
		st.rainDarken = std::min(darkTarget, st.rainDarken + a_dt / (soakTime * 1.35f));
	} else {
		st.rainDarken = std::max(0.0f, st.rainDarken - a_dt / dryTime);
		st.rainProgress = std::max(0.0f, st.rainProgress - a_dt / dryTime);
	}
	st.rain = st.rainDarken * std::max(settings.RainCoatStrength, 0.0f);

	bool snowing = false;
	if (auto* sky = RE::Sky::GetSingleton()) {
		if (auto* weather = sky->currentWeather)
			snowing = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
	}
	if (snowing && exterior && shelter > 0.2f)
		st.snow = std::min(1.0f, st.snow + a_dt / 45.0f);
	else
		st.snow = std::max(0.0f, st.snow - a_dt / 30.0f);
}

void SubmersionWetness::Prepass()
{
	++frame;
	if (!loaded || !settings.Enabled)
		return;
	auto* lists = RE::ProcessLists::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!lists || !player)
		return;
	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float dt = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.25f);
	const float rain = globals::features::wetnessEffects.loaded ? globals::features::wetnessEffects.GetCharacterRetainedWetness() : 0.0f;
	const RE::NiPoint3 center = player->GetPosition();

	std::vector<RE::Actor*> visited;
	auto visit = [&](RE::Actor* a_actor) {
		if (!a_actor || !a_actor->Is3DLoaded())
			return;
		if (a_actor != player && a_actor->GetPosition().GetDistance(center) > settings.Range)
			return;
		UpdateActor(a_actor, dt, rain);
		visited.push_back(a_actor);
	};
	visit(player);
	std::vector<RE::NiPointer<RE::Actor>> keepAlive;
	for (auto& handle : lists->highActorHandles) {
		if (auto actor = handle.get()) {
			visit(actor.get());
			keepAlive.push_back(actor);
		}
	}
	if (settings.RespectShelter)
		UpdateSkyVisibility(visited);
	++queryFrame;

	if ((frame & 255u) == 0u) {
		std::erase_if(actors, [&](const auto& kv) { return frame - kv.second.lastSeenFrame > 600u; });
	}
}

void SubmersionWetness::OnLightingSetupGeometry(RE::BSRenderPass* a_pass)
{
	boundThisDraw = false;
	if (!loaded || !settings.Enabled || !perGeometryCB || !a_pass || !a_pass->geometry)
		return;
	auto* actor = FindActor(a_pass->geometry);
	if (!actor)
		return;
	if (!settings.IncludeCreatures && !actor->HasKeywordString("ActorTypeNPC"))
		return;
	auto it = actors.find(actor->GetFormID());
	if (it == actors.end())
		return;
	const auto& st = it->second;
	const float waterCoat = st.soak;
	const float coat = std::max(waterCoat, st.rain);
	if (coat <= 0.001f && st.flow <= 0.0f && st.rainProgress <= 0.001f && st.snow <= 0.001f)
		return;

	const bool fullyUnder = st.depth >= st.height * 0.98f;
	PerGeometry data{};
	data.SubA = float4(st.feetZ, st.soakLine, st.waterZ, waterCoat);
	data.SubB = float4(std::clamp(std::max(waterCoat, st.rainDarken) * 1.5f, 0.0f, 1.0f), st.flow, settings.EdgeSoftness, 1.0f);
	data.SubC = float4(settings.Strength, settings.BeadDensity, settings.HairStrength, settings.ArmorStrength);
	data.SubD = float4(st.rainProgress, st.rainDarken, st.height, fullyUnder ? 1.0f : 0.0f);
	data.SubE = float4(st.snow, 0.0f, 0.0f, 0.0f);
	perGeometryCB->Update(data);
	ID3D11Buffer* cb = perGeometryCB->CB();
	globals::d3d::context->PSSetConstantBuffers(9, 1, &cb);
	boundThisDraw = true;
}

bool SubmersionWetness::TryGetSoak(RE::FormID a_formID, float& a_line, float& a_amount) const
{
	auto it = actors.find(a_formID);
	if (it == actors.end())
		return false;
	a_line = it->second.soakLine;
	a_amount = it->second.soak;
	return true;
}

void SubmersionWetness::OnLightingRestoreGeometry()
{
	if (!boundThisDraw)
		return;
	ID3D11Buffer* none = nullptr;
	globals::d3d::context->PSSetConstantBuffers(9, 1, &none);
	boundThisDraw = false;
}

void SubmersionWetness::DrawSettings()
{
	BFUI::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	BFUI::Checkbox(T(TKEY("include_creatures"), "Include Creatures"), &settings.IncludeCreatures);
	BFUI::Checkbox(T(TKEY("rain_soak"), "Rain Soaks Characters"), &settings.RainSoak);
	BFUI::SliderFloat(T(TKEY("strength"), "Wetness Strength"), &settings.Strength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("bead_density"), "Bead Density"), &settings.BeadDensity, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("hair_strength"), "Hair Strength"), &settings.HairStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("armor_strength"), "Armor Strength"), &settings.ArmorStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("rain_coat"), "Rain Coat Strength"), &settings.RainCoatStrength, 0.0f, 1.0f, "%.2f");
	BFUI::Checkbox(T(TKEY("shelter"), "Roofs Keep Characters Dry"), &settings.RespectShelter);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shelter_tt"), "Uses Skylighting's sky occlusion above each actor's head. Needs Skylighting enabled."));
	BFUI::SliderFloat(T(TKEY("rain_soak_time"), "Rain Soak Time"), &settings.RainSoakTime, 0.5f, 120.0f, "%.1f s");
	BFUI::SliderFloat(T(TKEY("soak_in_time"), "Water Soak Time"), &settings.SoakInTime, 0.1f, 20.0f, "%.1f s");
	BFUI::SliderFloat(T(TKEY("dry_time"), "Dry Time"), &settings.DryTime, 10.0f, 900.0f, "%.0f s");
	BFUI::SliderFloat(T(TKEY("emerge_flow"), "Rivulet Time"), &settings.EmergeFlowTime, 0.5f, 30.0f, "%.1f s");
	BFUI::SliderFloat(T(TKEY("edge"), "Wet Line Softness"), &settings.EdgeSoftness, 0.5f, 32.0f, "%.1f units");
	BFUI::SliderFloat(T(TKEY("range"), "Range"), &settings.Range, 512.0f, 16384.0f, "%.0f units");
	ImGui::Text("%s: %zu", T(TKEY("tracked"), "Tracked actors"), actors.size());
}

void SubmersionWetness::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SubmersionWetness::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SubmersionWetness::RestoreDefaultSettings()
{
	settings = {};
	actors.clear();
}

// Ported from BearForge 0.5 (src/Features/SubmersionWetness.cpp)
float SubmersionWetness::WaterHeightAt(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos)
{
	if (!a_ref)
		return -RE::NI_INFINITY;
	float waterHeight = a_ref->GetWaterHeight();
	if (waterHeight != -RE::NI_INFINITY)
		return waterHeight;
	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		for (const auto& waterObject : waterManager->waterObjects) {
			if (!waterObject)
				continue;
			for (const auto& bound : waterObject->multiBounds) {
				if (bound && bound->size.z <= 10.0f) {
					const auto c = bound->center;
					const auto s = bound->size;
					if (a_pos.x >= c.x - s.x && a_pos.x <= c.x + s.x && a_pos.y >= c.y - s.y && a_pos.y <= c.y + s.y)
						return c.z;
				}
			}
		}
	}
	return -RE::NI_INFINITY;
}

