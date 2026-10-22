#include "TerrainDeformation.h"
#include "Bearforj/TipUI.h"

#include "../I18n/I18n.h"
#include "../Utils/UI.h"
#include "Globals.h"
#include "ScreenSpacePhysicalParticles.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include <filesystem>
#include <fstream>
#include <numbers>

#include "Utils/FileSystem.h"

#define I18N_KEY_PREFIX "feature.terrain_deformation."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainDeformation::Settings,
	Enabled,
	IncludeNPCs,
	EnableTessellation,
	StrideFallback,
	Radius,
	PrintRadius,
	Depth,
	Rim,
	Darkening,
	Lifetime,
	SnowStrength,
	MudStrength,
	SandStrength,
	DirtStrength,
	GravelStrength,
	GrassStrength,
	OtherStrength,
	MaxTessFactor,
	TessRange,
	DisplacementScale,
	NormalStrength)

namespace
{
	const RE::BSFixedString kLeftFootNode = "NPC L Foot [Lft ]";
	const RE::BSFixedString kRightFootNode = "NPC R Foot [Rft ]";
	constexpr float kStrideFallbackLength = 38.0f;   // game units between prints when no footstep events arrive
	constexpr float kEventSilenceSeconds = 1.5f;     // actor counts as "event-less" after this long without events
	constexpr float kFootSideOffset = 7.0f;

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::terrainDeformation.OnLightingSetupGeometry(a_shader, a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::terrainDeformation.OnLightingRestoreGeometry();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void TerrainDeformation::SetupResources()
{
	vr = globals::game::isVR;
	upload.resize(kMaxGPUPrints + 1);

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = static_cast<UINT>(sizeof(GPUPrint) * (kMaxGPUPrints + 1));
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(GPUPrint);
	DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, printBuffer.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = kMaxGPUPrints + 1;
	DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(printBuffer.get(), &srvDesc, printSRV.put()));
}

void TerrainDeformation::PostPostLoad()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Terrain Deformation] Installed hooks - BSLightingShader SetupGeometry/RestoreGeometry");
}

void TerrainDeformation::DataLoaded()
{
	LoadKnownDescriptors();
	if (auto* manager = RE::BGSFootstepManager::GetSingleton()) {
		manager->AddEventSink(&footstepSink);
		logger::info("[Terrain Deformation] Registered footstep event sink");
	} else {
		// 0.5 silently skipped this case; the stride fallback still produces prints without events.
		logger::warn("[Terrain Deformation] BGSFootstepManager unavailable, using stride fallback only");
	}
}

RE::BSEventNotifyControl TerrainDeformation::FootstepSink::ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*)
{
	auto& feature = globals::features::terrainDeformation;
	if (!a_event || !feature.loaded || !feature.settings.Enabled)
		return RE::BSEventNotifyControl::kContinue;
	auto actorPtr = a_event->actor.get();
	if (!actorPtr)
		return RE::BSEventNotifyControl::kContinue;
	feature.OnFootstep(actorPtr.get(), a_event->tag.c_str() ? std::string_view(a_event->tag.c_str()) : std::string_view{});
	return RE::BSEventNotifyControl::kContinue;
}

float TerrainDeformation::MaterialStrength(RE::MATERIAL_ID a_material) const
{
	using M = RE::MATERIAL_ID;
	switch (a_material) {
	case M::kSnow:
	case M::kSnowStairs:
		return settings.SnowStrength;
	case M::kMud:
		return settings.MudStrength;
	case M::kSand:
		return settings.SandStrength;
	case M::kDirt:
		return settings.DirtStrength;
	case M::kGravel:
		return settings.GravelStrength;
	case M::kGrass:
		return settings.GrassStrength;
	default:
		return settings.OtherStrength;
	}
}

void TerrainDeformation::OnFootstep(RE::Actor* a_actor, std::string_view a_tag)
{
	if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDead())
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (a_actor != player && !settings.IncludeNPCs)
		return;
	auto* cell = a_actor->GetParentCell();
	if (!cell || cell->IsInteriorCell())
		return;

	const bool left = a_tag.find("Left") != std::string_view::npos || a_tag.find("FootL") != std::string_view::npos;
	const bool right = a_tag.find("Right") != std::string_view::npos || a_tag.find("FootR") != std::string_view::npos;

	RE::NiPoint3 position = a_actor->GetPosition();
	if (auto* root = a_actor->Get3D()) {
		const auto& nodeName = left ? kLeftFootNode : kRightFootNode;
		if ((left || right))
			if (auto* foot = root->GetObjectByName(nodeName))
				position = foot->world.translate;
	}
	{
		std::lock_guard lock(printsLock);
		auto& track = tracks[a_actor->GetFormID()];
		track.lastEventTime = time;
		track.lastPrint = a_actor->GetPosition();
		track.valid = true;
	}
	AddPrint(a_actor, position, a_actor->GetAngleZ(), left);
}

void TerrainDeformation::AddPrint(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_yaw, bool)
{
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;
	// Feet below the water surface leave no print.
	if (a_actor && a_actor->IsInWater())
		return;
	const auto material = tes->GetLandMaterialType(a_position);
	// 0.6.3: footstep particles come from SSPP's own BGSFootstepEvent sink (not tied to prints any more)
	const float strength = MaterialStrength(material);
	if (strength <= 0.0f)
		return;

	float scale = 1.0f;
	if (a_actor)
		scale = std::clamp(a_actor->GetScale(), 0.5f, 3.0f);

	PrintRecord record{};
	record.x = a_position.x;
	record.y = a_position.y;
	record.z = a_position.z;
	record.yaw = a_yaw;
	record.radius = settings.PrintRadius * scale;
	record.depth = settings.Depth * scale;
	record.rim = settings.Rim;
	record.dark = settings.Darkening;
	record.material = strength;
	std::lock_guard lock(printsLock);
	record.spawnTime = time;
	prints.push_back(record);
	while (prints.size() > kMaxStoredPrints)
		prints.pop_front();
}

void TerrainDeformation::UpdateStrideFallback()
{
	if (!settings.StrideFallback)
		return;
	auto* lists = RE::ProcessLists::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!lists || !player)
		return;

	auto visit = [&](RE::Actor* a_actor) {
		if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDead() || a_actor->IsInMidair())
			return;
		auto* cell = a_actor->GetParentCell();
		if (!cell || cell->IsInteriorCell())
			return;
		const RE::NiPoint3 pos = a_actor->GetPosition();
		if (pos.GetDistance(player->GetPosition()) > settings.Radius)
			return;
		bool emit = false;
		bool left = true;
		RE::NiPoint3 last;
		{
			std::lock_guard lock(printsLock);
			auto& track = tracks[a_actor->GetFormID()];
			if (!track.valid) {
				track.lastPrint = pos;
				track.valid = true;
				return;
			}
			if (time - track.lastEventTime < kEventSilenceSeconds) {
				track.lastPrint = pos;
				return;
			}
			if (pos.GetDistance(track.lastPrint) >= kStrideFallbackLength) {
				emit = true;
				left = track.leftNext;
				track.leftNext = !track.leftNext;
				last = track.lastPrint;
				track.lastPrint = pos;
			}
		}
		if (!emit)
			return;
		const float yaw = a_actor->GetAngleZ();
		const float side = left ? -kFootSideOffset : kFootSideOffset;
		RE::NiPoint3 footPos = pos;
		footPos.x += std::cos(yaw) * side;
		footPos.y -= std::sin(yaw) * side;
		AddPrint(a_actor, footPos, yaw, left);
	};

	visit(player);
	if (settings.IncludeNPCs) {
		for (auto& handle : lists->highActorHandles) {
			if (auto actor = handle.get())
				visit(actor.get());
		}
	}
}

void TerrainDeformation::Prepass()
{
	auto context = globals::d3d::context;
	if (!printBuffer)
		return;

	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	if (!paused)
		time += std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.25f);

	uint32_t count = 0;
	if (settings.Enabled) {
		if (!paused)
			UpdateStrideFallback();

		RE::NiPoint3 center{};
		if (auto* player = RE::PlayerCharacter::GetSingleton())
			center = player->GetPosition();
		const float radius2 = settings.Radius * settings.Radius;
		const float lifetime = std::max(settings.Lifetime, 1.0f);

		std::lock_guard lock(printsLock);
		while (!prints.empty() && time - prints.front().spawnTime > lifetime)
			prints.pop_front();
		// Newest first so the most recent prints win when the GPU budget is exceeded.
		for (auto it = prints.rbegin(); it != prints.rend() && count < kMaxGPUPrints; ++it) {
			const float dx = it->x - center.x;
			const float dy = it->y - center.y;
			if (dx * dx + dy * dy > radius2)
				continue;
			const float age = std::clamp((time - it->spawnTime) / lifetime, 0.0f, 1.0f);
			const float fill = 1.0f - age;
			// no popping: prints fill in smoothly over time and fade out over the last 20% of the upload radius
			const float edge = std::clamp((radius2 - (dx * dx + dy * dy)) / std::max(radius2 * 0.36f, 1.0f), 0.0f, 1.0f);
			const float fade = fill * fill * (3.0f - 2.0f * fill) * edge;
			auto& gpu = upload[++count];
			gpu.A = float4(it->x, it->y, it->yaw, it->radius);
			gpu.B = float4(it->depth * fade, it->rim, it->dark * fade, it->material);
		}
	}

	const bool tess = settings.Enabled && settings.EnableTessellation && !vr;
	upload[0].A = float4(static_cast<float>(count), std::clamp(settings.MaxTessFactor, 1.0f, 64.0f), settings.TessRange, settings.DisplacementScale);
	upload[0].B = float4(settings.NormalStrength, tess ? 1.0f : 0.0f, 0.0f, 0.0f);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(context->Map(printBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		std::memcpy(mapped.pData, upload.data(), sizeof(GPUPrint) * (count + 1));
		context->Unmap(printBuffer.get(), 0);
	}
	uploadedCount = count;

	ID3D11ShaderResourceView* srv = printSRV.get();
	context->PSSetShaderResources(54, 1, &srv);  // t54 (t123 = TreeWindSpring PreviousTransientFields)
}

void TerrainDeformation::QueueTessCompile(RE::BSShader* a_shader, uint32_t a_vertexDescriptor)
{
	// Called with tessLock held.
	auto& slot = tessShaders[a_vertexDescriptor];
	if (slot)
		return;
	slot = std::make_shared<TessShaders>();
	CompileJob job{ a_vertexDescriptor, SIE::GetShaderSourcePath(*a_shader), SIE::GetShaderDefinesForDescriptor(*a_shader, a_vertexDescriptor), slot };
	tessQueue.push_back(std::move(job));
	if (std::find(knownDescriptors.begin(), knownDescriptors.end(), a_vertexDescriptor) == knownDescriptors.end()) {
		knownDescriptors.push_back(a_vertexDescriptor);
		knownDirty = true;
	}
	if (!tessWorker.joinable())
		tessWorker = std::jthread([this](std::stop_token st) { CompileWorker(st); });
	tessCv.notify_one();
}

void TerrainDeformation::CompileWorker(std::stop_token a_stop)
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
				logger::warn("[Terrain Deformation] Failed to compile {} for landscape descriptor {:X}; tessellation disabled for it", stage, job.descriptor);
				ok = false;
				break;
			}
			if (hull)
				job.target->hs.attach(static_cast<ID3D11HullShader*>(shader));
			else
				job.target->ds.attach(static_cast<ID3D11DomainShader*>(shader));
		}
		job.target->state.store(ok ? 1 : 2, std::memory_order_release);
		if (ok)
			logger::info("[Terrain Deformation] Compiled landscape tessellation for descriptor {:X} (background)", job.descriptor);
	}
}

TerrainDeformation::TessShaders* TerrainDeformation::GetTessShaders(RE::BSShader* a_shader, uint32_t a_vertexDescriptor)
{
	std::lock_guard lock(tessLock);
	if (!lightingShader) {
		lightingShader = a_shader;
		for (auto d : knownDescriptors)  // first landscape draw of the session: warm everything seen before
			QueueTessCompile(a_shader, d);
	}
	auto it = tessShaders.find(a_vertexDescriptor);
	if (it == tessShaders.end() || !it->second) {
		QueueTessCompile(a_shader, a_vertexDescriptor);
		return nullptr;
	}
	return it->second->state.load(std::memory_order_acquire) == 1 ? it->second.get() : nullptr;
}

void TerrainDeformation::WarmTessCache()
{
	std::lock_guard lock(tessLock);
	if (!lightingShader)
		return;
	for (auto d : knownDescriptors)
		QueueTessCompile(lightingShader, d);
}

static std::filesystem::path TessListPath()
{
	return Util::PathHelpers::GetCommunityShaderPath() / "TerrainDeformationTess.txt";
}

void TerrainDeformation::LoadKnownDescriptors()
{
	std::lock_guard lock(tessLock);
	std::ifstream in(TessListPath());
	std::string line;
	while (std::getline(in, line)) {
		try {
			const uint32_t d = static_cast<uint32_t>(std::stoul(line, nullptr, 16));
			if (std::find(knownDescriptors.begin(), knownDescriptors.end(), d) == knownDescriptors.end())
				knownDescriptors.push_back(d);
		} catch (...) {
		}
	}
}

void TerrainDeformation::SaveKnownDescriptors()
{
	std::lock_guard lock(tessLock);
	if (!knownDirty)
		return;
	std::error_code ec;
	std::filesystem::create_directories(TessListPath().parent_path(), ec);
	std::ofstream out(TessListPath(), std::ios::trunc);
	for (auto d : knownDescriptors)
		out << std::format("{:08X}\n", d);
	knownDirty = false;
}

void TerrainDeformation::OnSceneTransitionReset(bool a_opening)
{
	// Loading screen: precompile every landscape variant seen so far in the background.
	if (!settings.Enabled || !settings.EnableTessellation || vr)
		return;
	if (a_opening)
		WarmTessCache();
	else
		SaveKnownDescriptors();
}

void TerrainDeformation::OnLightingSetupGeometry(RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
{
	if (tessellationBound)
		OnLightingRestoreGeometry();
	if (!loaded || !settings.Enabled || !settings.EnableTessellation || vr || uploadedCount == 0 || !a_shader || !a_pass)
		return;

	auto* state = globals::state;
	const uint32_t vsDescriptor = state->modifiedVertexDescriptor;
	const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((vsDescriptor >> 24) & 0x3F);
	if (technique != SIE::ShaderCache::LightingShaderTechniques::MTLand)
		return;
	// Only near landscape: the patch-constant function also fades tessellation out with distance.
	if (auto* geometry = a_pass->geometry) {
		const auto& bound = geometry->worldBound;
		RE::NiPoint3 camera{};
		if (auto* player = RE::PlayerCharacter::GetSingleton())
			camera = player->GetPosition();
		if (bound.center.GetDistance(camera) - bound.radius > settings.TessRange)
			return;
	}
	// HS/DS must match the custom (Community Shaders) landscape vertex shader signature.
	if (!globals::shaderCache->IsEnabled() || !globals::shaderCache->GetVertexShader(*a_shader, vsDescriptor))
		return;
	auto* shaders = GetTessShaders(a_shader, vsDescriptor);
	if (!shaders)
		return;

	auto context = globals::d3d::context;
	ID3D11Buffer* perFrame = nullptr;
	context->VSGetConstantBuffers(12, 1, &perFrame);
	context->DSSetConstantBuffers(12, 1, &perFrame);
	context->HSSetConstantBuffers(12, 1, &perFrame);
	if (perFrame)
		perFrame->Release();
	ID3D11ShaderResourceView* srv = printSRV.get();
	context->HSSetShaderResources(54, 1, &srv);
	context->DSSetShaderResources(54, 1, &srv);
	context->HSSetShader(shaders->hs.get(), nullptr, 0);
	context->DSSetShader(shaders->ds.get(), nullptr, 0);

	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	shadowState.topology = REX::W32::D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
	shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	tessellationBound = true;
}

void TerrainDeformation::OnLightingRestoreGeometry()
{
	if (!tessellationBound)
		return;
	auto context = globals::d3d::context;
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	shadowState.topology = REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	tessellationBound = false;
}

void TerrainDeformation::ClearShaderCache()
{
	{
		std::lock_guard lock(tessLock);
		tessQueue.clear();
		tessShaders.clear();
	}
	WarmTessCache();
}

void TerrainDeformation::ClearPrints()
{
	std::lock_guard lock(printsLock);
	prints.clear();
	tracks.clear();
}

size_t TerrainDeformation::GetStoredPrintCount()
{
	std::lock_guard lock(printsLock);
	return prints.size();
}

void TerrainDeformation::DrawSettings()
{
	BFUI::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	BFUI::Checkbox(T(TKEY("npcs"), "Prints from NPCs and creatures"), &settings.IncludeNPCs);
	BFUI::Checkbox(T(TKEY("stride_fallback"), "Prints without footstep events"), &settings.StrideFallback);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("stride_fallback_tt"), "Creates prints from movement for actors whose animations send no footstep events."));
	BFUI::Checkbox(T(TKEY("tessellation"), "Landscape tessellation"), &settings.EnableTessellation);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("tessellation_tt"), "Really pushes the ground down near the camera. Without it the prints are only shaded."));

	ImGui::SeparatorText(T(TKEY("prints"), "Prints"));
	BFUI::SliderFloat(T(TKEY("print_radius"), "Print Size"), &settings.PrintRadius, 2.0f, 30.0f, "%.1f");
	BFUI::SliderFloat(T(TKEY("depth"), "Depth"), &settings.Depth, 0.0f, 12.0f, "%.1f");
	BFUI::SliderFloat(T(TKEY("rim"), "Rim"), &settings.Rim, 0.0f, 1.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("darkening"), "Darkening"), &settings.Darkening, 0.0f, 1.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("lifetime"), "Fill-in Time (s)"), &settings.Lifetime, 5.0f, 1200.0f, "%.0f");
	BFUI::SliderFloat(T(TKEY("radius"), "Visible Radius"), &settings.Radius, 512.0f, 16384.0f, "%.0f");

	ImGui::SeparatorText(T(TKEY("materials"), "Ground Materials"));
	BFUI::SliderFloat(T(TKEY("snow"), "Snow"), &settings.SnowStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("mud"), "Mud"), &settings.MudStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("sand"), "Sand"), &settings.SandStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("dirt"), "Dirt"), &settings.DirtStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("gravel"), "Gravel"), &settings.GravelStrength, 0.0f, 2.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("grass"), "Grass"), &settings.GrassStrength, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("grass_tt"), "Off by default: grass is not displaced, so pressing the ground under it would make it float."));
	BFUI::SliderFloat(T(TKEY("other"), "Other (stone, wood...)"), &settings.OtherStrength, 0.0f, 2.0f, "%.2f");

	ImGui::SeparatorText(T(TKEY("tessellation_section"), "Tessellation"));
	BFUI::SliderFloat(T(TKEY("max_tess"), "Max Tessellation"), &settings.MaxTessFactor, 1.0f, 64.0f, "%.0f");
	BFUI::SliderFloat(T(TKEY("tess_range"), "Tessellation Range"), &settings.TessRange, 256.0f, 4096.0f, "%.0f");
	BFUI::SliderFloat(T(TKEY("displacement"), "Displacement Scale"), &settings.DisplacementScale, 0.0f, 3.0f, "%.2f");
	BFUI::SliderFloat(T(TKEY("normal_strength"), "Normal Strength"), &settings.NormalStrength, 0.0f, 3.0f, "%.2f");

	ImGui::Spacing();
	ImGui::Text("%s: %zu (%u %s)", T(TKEY("stored"), "Stored footprints"), GetStoredPrintCount(), uploadedCount, T(TKEY("visible"), "visible"));
	if (ImGui::Button(T(TKEY("clear"), "Clear Footprints")))
		ClearPrints();
}

void TerrainDeformation::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainDeformation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainDeformation::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
