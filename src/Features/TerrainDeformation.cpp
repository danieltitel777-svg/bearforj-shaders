#include "TerrainDeformation.h"
#include "Bearforj/TipUI.h"
#include "SubmersionWetness.h"

#include "../I18n/I18n.h"
#include "../Utils/UI.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include <cmath>
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
	NormalStrength,
	SnowRaise,
	MudRaise,
	SandRaise,
	SinkFraction,
	RaiseFullField)

namespace
{
	const RE::BSFixedString kLeftFootNode = "NPC L Foot [Lft ]";
	const RE::BSFixedString kRightFootNode = "NPC R Foot [Rft ]";
	constexpr float kStrideFallbackLength = 38.0f;   // game units between prints when no footstep events arrive
	constexpr float kEventSilenceSeconds = 1.5f;     // actor counts as "event-less" after this long without events
	constexpr float kFootSideOffset = 7.0f;
	// Utility.hlsl TD_DepthDraw. b4 is PerShader, b5/b6 shared data, b12 the frame buffer.
	constexpr UINT kDepthDrawSlot = 7;

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

	// VTABLE_BSUtilityShader[0] is Address Library SE 305632 / AE 255302 (CommonLibSSE-NG Offsets_VTABLE.h).
	// SetupGeometry / RestoreGeometry are vfuncs 0x6 / 0x7, the same slots Skylighting and Terrain Blending hook.
	struct BSUtilityShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::terrainDeformation.OnUtilitySetupGeometry(a_shader, a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::terrainDeformation.OnUtilityRestoreGeometry();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool IsNearLandscape(RE::BSRenderPass* a_pass)
	{
		auto* property = a_pass ? a_pass->shaderProperty : nullptr;
		if (!property)
			return false;
		if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape))
			return true;
		// BSLightingShaderMaterialLandscape::GetFeature returns kMultiTexLandLODBlend (CommonLib, value 19).
		return property->material && property->material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
	}
}

void TerrainDeformation::CreateUintTexture(UINT a_size, winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, const char* a_name)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_size;
	desc.Height = a_size;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	DX::ThrowIfFailed(globals::d3d::device->CreateTexture2D(&desc, nullptr, a_tex.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R32_UINT;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(a_tex.get(), &srv, a_srv.put()));

	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_UINT;
	uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	DX::ThrowIfFailed(globals::d3d::device->CreateUnorderedAccessView(a_tex.get(), &uav, a_uav.put()));
	if (a_name) {
		Util::SetResourceName(a_tex.get(), "%s", a_name);
		Util::SetResourceName(a_srv.get(), "%s SRV", a_name);
		Util::SetResourceName(a_uav.get(), "%s UAV", a_name);
	}
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

	CreateUintTexture(kHeightmapSize, heightTexture, heightSRV, heightUAV, "TerrainDeformation Heightmap");
	CreateUintTexture(kActivitySize, activityTexture, activitySRV, activityUAV, "TerrainDeformation Activity");

	{
		D3D11_TEXTURE2D_DESC raiseDesc{};
		raiseDesc.Width = kRaiseCells;
		raiseDesc.Height = kRaiseCells;
		raiseDesc.MipLevels = 1;
		raiseDesc.ArraySize = 1;
		raiseDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		raiseDesc.SampleDesc.Count = 1;
		raiseDesc.Usage = D3D11_USAGE_DEFAULT;
		raiseDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		DX::ThrowIfFailed(globals::d3d::device->CreateTexture2D(&raiseDesc, nullptr, raiseTexture.put()));
		D3D11_SHADER_RESOURCE_VIEW_DESC raiseSrv{};
		raiseSrv.Format = DXGI_FORMAT_R32G32_FLOAT;
		raiseSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		raiseSrv.Texture2D.MipLevels = 1;
		DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(raiseTexture.get(), &raiseSrv, raiseSRV.put()));
		Util::SetResourceName(raiseTexture.get(), "TerrainDeformation Raise");
		Util::SetResourceName(raiseSRV.get(), "TerrainDeformation Raise SRV");
	}
	raiseCPU.assign(kRaiseCells * kRaiseCells, {});

	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = 16;
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	const float zeros[4]{};
	D3D11_SUBRESOURCE_DATA init{ zeros, 0, 0 };
	DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cb, &init, depthDrawCB.put()));

	clearCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainDeformation\\StampCS.hlsl", { { "CLEAR", "" } }, "cs_5_0")));
	stampCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainDeformation\\StampCS.hlsl", {}, "cs_5_0")));
	if (!clearCS || !stampCS)
		logger::error("[Terrain Deformation] Failed to compile StampCS; footprints will not displace until shaders compile");
}

void TerrainDeformation::PostPostLoad()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::write_vfunc<0x7, BSUtilityShader_RestoreGeometry>(RE::VTABLE_BSUtilityShader[0]);
	logger::info("[Terrain Deformation] Installed hooks - BSLightingShader and BSUtilityShader SetupGeometry/RestoreGeometry (utility vtable SE 305632 / AE 255302)");
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

TerrainDeformation::SurfaceProfile TerrainDeformation::ProfileFor(RE::MATERIAL_ID a_material)
{
	// Shape only. The material slider (MaterialStrength) still scales depth and darkening.
	// Snow keeps a raised rim, mud a steep wall, sand a wide soft bowl. Studied from Dynamic Terrain
	// Deformation's per-surface Depth/Radius/Shoulder/Rim (GPL-3.0, NearMidnightNow); these numbers are ours.
	using M = RE::MATERIAL_ID;
	switch (a_material) {
	case M::kSnow:
	case M::kSnowStairs:
		return { 1.15f, 1.05f, 1.25f, 0.40f, 1.00f };
	case M::kMud:
		return { 1.05f, 0.95f, 0.55f, 0.30f, 1.20f };
	case M::kSand:
		return { 0.65f, 1.25f, 0.30f, 0.62f, 0.65f };
	case M::kDirt:
		return { 0.50f, 1.00f, 0.45f, 0.50f, 0.85f };
	case M::kGravel:
		return { 0.30f, 0.90f, 0.20f, 0.70f, 0.45f };
	case M::kGrass:
		return { 1.00f, 1.00f, 0.20f, 0.55f, 0.30f };
	default:
		return { 0.35f, 1.00f, 0.25f, 0.55f, 0.50f };
	}
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

float TerrainDeformation::LayerRaise(RE::MATERIAL_ID a_material) const
{
	using M = RE::MATERIAL_ID;
	switch (a_material) {
	case M::kSnow:
	case M::kSnowStairs:
		return std::max(settings.SnowRaise, 0.0f);
	case M::kMud:
		return std::max(settings.MudRaise, 0.0f);
	case M::kSand:
		return std::max(settings.SandRaise, 0.0f);
	default:
		return 0.0f;
	}
}

void TerrainDeformation::UploadRaiseMap()
{
	auto context = globals::d3d::context;
	if (!context || !raiseTexture || raiseCPU.size() != kRaiseCells * kRaiseCells)
		return;
	context->UpdateSubresource(raiseTexture.get(), 0, nullptr, raiseCPU.data(), kRaiseCells * sizeof(RaiseTexel), 0);
}

void TerrainDeformation::UpdateRaiseMap(const RE::NiPoint3& a_center)
{
	if (!raiseTexture)
		return;
	if (raiseCPU.size() != kRaiseCells * kRaiseCells)
		raiseCPU.assign(kRaiseCells * kRaiseCells, {});

	const bool want = settings.Enabled && (settings.SnowRaise > 0.5f || settings.MudRaise > 0.5f || settings.SandRaise > 0.5f);
	if (!want) {
		if (raiseActive || raisePrimed) {
			std::fill(raiseCPU.begin(), raiseCPU.end(), RaiseTexel{});
			UploadRaiseMap();
		}
		raiseActive = false;
		raisePrimed = false;
		return;
	}

	const float originX = std::floor(windowOriginX / kRaiseCell) * kRaiseCell;
	const float originY = std::floor(windowOriginY / kRaiseCell) * kRaiseCell;
	int dx = 0;
	int dy = 0;
	if (raisePrimed) {
		dx = static_cast<int>(std::lround((originX - raiseOriginX) / kRaiseCell));
		dy = static_cast<int>(std::lround((originY - raiseOriginY) / kRaiseCell));
		if (std::abs(dx) >= static_cast<int>(kRaiseCells) || std::abs(dy) >= static_cast<int>(kRaiseCells)) {
			raisePrimed = false;
			dx = 0;
			dy = 0;
		} else if (dx != 0 || dy != 0) {
			std::vector<RaiseTexel> shifted(raiseCPU.size());
			for (int y = 0; y < static_cast<int>(kRaiseCells); ++y) {
				const int sy = y + dy;
				if (sy < 0 || sy >= static_cast<int>(kRaiseCells))
					continue;
				for (int x = 0; x < static_cast<int>(kRaiseCells); ++x) {
					const int sx = x + dx;
					if (sx < 0 || sx >= static_cast<int>(kRaiseCells))
						continue;
					shifted[static_cast<size_t>(y) * kRaiseCells + x] = raiseCPU[static_cast<size_t>(sy) * kRaiseCells + sx];
				}
			}
			raiseCPU.swap(shifted);
		}
	}
	raiseOriginX = originX;
	raiseOriginY = originY;

	auto* tes = RE::TES::GetSingleton();
	if (!tes) {
		raiseActive = false;
		return;
	}

	float waterZ = -1.0e8f;
	if (globals::features::submersionWetness.loaded) {
		if (auto* player = RE::PlayerCharacter::GetSingleton())
			waterZ = SubmersionWetness::WaterHeightAt(player, a_center);
	}
	const float sink = std::clamp(settings.SinkFraction, 0.0f, 1.0f);

	auto fill = [&](uint32_t x, uint32_t y) {
		const float wx = originX + (static_cast<float>(x) + 0.5f) * kRaiseCell;
		const float wy = originY + (static_cast<float>(y) + 0.5f) * kRaiseCell;
		RE::NiPoint3 probe{ wx, wy, a_center.z };
		float landZ = a_center.z;
		tes->GetLandHeight(probe, landZ);
		probe.z = landZ;
		const auto material = tes->GetLandMaterialType(probe);
		float raise = LayerRaise(material);
		// A shallow hollow in sand, with the water close above it, is washed down so shore run-up can take it.
		if (material == RE::MATERIAL_ID::kSand && raise > 0.5f && waterZ > landZ - 5.0f && waterZ < landZ + 30.0f) {
			float sum = 0.0f;
			int n = 0;
			const float step = kRaiseCell;
			const float offsets[4][2] = { { step, 0.0f }, { -step, 0.0f }, { 0.0f, step }, { 0.0f, -step } };
			for (const auto& offset : offsets) {
				RE::NiPoint3 neighbor{ wx + offset[0], wy + offset[1], a_center.z };
				float height = landZ;
				if (tes->GetLandHeight(neighbor, height)) {
					sum += height;
					++n;
				}
			}
			if (n > 0 && landZ + 8.0f < sum / static_cast<float>(n))
				raise *= 0.25f;
		}
		raiseCPU[static_cast<size_t>(y) * kRaiseCells + x] = { raise, sink };
	};

	if (!raisePrimed) {
		for (uint32_t y = 0; y < kRaiseCells; ++y)
			for (uint32_t x = 0; x < kRaiseCells; ++x)
				fill(x, y);
		raisePrimed = true;
	} else if (dx != 0 || dy != 0) {
		for (uint32_t y = 0; y < kRaiseCells; ++y) {
			for (uint32_t x = 0; x < kRaiseCells; ++x) {
				const bool freshX = dx > 0 ? x >= kRaiseCells - static_cast<uint32_t>(dx) : (dx < 0 ? x < static_cast<uint32_t>(-dx) : false);
				const bool freshY = dy > 0 ? y >= kRaiseCells - static_cast<uint32_t>(dy) : (dy < 0 ? y < static_cast<uint32_t>(-dy) : false);
				if (freshX || freshY)
					fill(x, y);
			}
		}
	} else {
		const uint32_t y0 = raiseCursor;
		raiseCursor = (raiseCursor + kRaiseRowsPerFrame) % kRaiseCells;
		for (uint32_t row = 0; row < kRaiseRowsPerFrame; ++row) {
			const uint32_t y = (y0 + row) % kRaiseCells;
			for (uint32_t x = 0; x < kRaiseCells; ++x)
				fill(x, y);
		}
	}

	raiseActive = false;
	for (const auto& texel : raiseCPU) {
		if (texel.raise > 0.5f) {
			raiseActive = true;
			break;
		}
	}
	UploadRaiseMap();
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
	// footstep particles come from the GPU Footstep Particles feature (not tied to prints)
	const float strength = MaterialStrength(material);
	if (strength <= 0.0f)
		return;
	const SurfaceProfile profile = ProfileFor(material);

	float scale = 1.0f;
	if (a_actor)
		scale = std::clamp(a_actor->GetScale(), 0.5f, 3.0f);

	PrintRecord record{};
	record.x = a_position.x;
	record.y = a_position.y;
	record.z = a_position.z;
	record.yaw = a_yaw;
	record.radius = settings.PrintRadius * scale * profile.radius;
	record.depth = settings.Depth * scale * profile.depth;
	record.rim = settings.Rim * profile.rim;
	record.dark = settings.Darkening * profile.dark;
	record.material = strength;
	record.shoulder = profile.shoulder;
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

void TerrainDeformation::UpdateHeightmap()
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
		const float lifetime = std::max(settings.Lifetime, 1.0f);

		const float halfWindow = static_cast<float>(kHeightmapSize) * kHeightmapTexel * 0.5f;
		windowOriginX = std::floor((center.x - halfWindow) / kHeightmapTexel) * kHeightmapTexel;
		windowOriginY = std::floor((center.y - halfWindow) / kHeightmapTexel) * kHeightmapTexel;
		const float limit = std::min(settings.Radius, halfWindow);
		const float limit2 = limit * limit;

		std::lock_guard lock(printsLock);
		while (!prints.empty() && time - prints.front().spawnTime > lifetime)
			prints.pop_front();
		// Newest first so the most recent prints win when the GPU budget is exceeded.
		for (auto it = prints.rbegin(); it != prints.rend() && count < kMaxGPUPrints; ++it) {
			const float dx = it->x - center.x;
			const float dy = it->y - center.y;
			const float dist2 = dx * dx + dy * dy;
			if (dist2 > limit2)
				continue;
			const float age = std::clamp((time - it->spawnTime) / lifetime, 0.0f, 1.0f);
			const float fill = 1.0f - age;
			// Fade across the outer 12% of the heightmap window so a scrolling origin does not pop prints.
			const float edgeDist = halfWindow - std::sqrt(dist2);
			const float edge = std::clamp(edgeDist / std::max(halfWindow * 0.12f, 1.0f), 0.0f, 1.0f);
			const float fade = fill * fill * (3.0f - 2.0f * fill) * edge;
			const float strength = it->material;
			auto& gpu = upload[++count];
			gpu.A = float4(it->x, it->y, it->yaw, it->radius);
			gpu.B = float4(it->depth * fade * strength, it->rim, it->dark * fade * strength, it->shoulder);
		}
	} else {
		windowOriginX = 0.0f;
		windowOriginY = 0.0f;
	}

	const bool tess = settings.Enabled && settings.EnableTessellation && !vr;
	uploadedCount = count;
	RE::NiPoint3 raiseCenter{};
	if (auto* player = RE::PlayerCharacter::GetSingleton())
		raiseCenter = player->GetPosition();
	UpdateRaiseMap(raiseCenter);
	upload[0].A = float4(static_cast<float>(count), std::clamp(settings.MaxTessFactor, 1.0f, 64.0f), settings.TessRange, settings.DisplacementScale);
	// 2 = tessellate the whole near field. 1.25 = footprints plus a bubble under the player. 1 = footprints only. 0 = off.
	float tessMode = 0.0f;
	if (tess)
		tessMode = raiseActive ? (settings.RaiseFullField ? 2.0f : 1.25f) : 1.0f;
	upload[0].B = float4(settings.NormalStrength, tessMode, windowOriginX, windowOriginY);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(context->Map(printBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		std::memcpy(mapped.pData, upload.data(), sizeof(GPUPrint) * (count + 1));
		context->Unmap(printBuffer.get(), 0);
	}
	StampHeightmap(context);
	BindHeightViews(ShaderStage::PS);
}

void TerrainDeformation::BindHeightViews(ShaderStage a_stage) const
{
	auto context = globals::d3d::context;
	if (!context)
		return;
	ID3D11ShaderResourceView* print = printSRV.get();
	ID3D11ShaderResourceView* height = heightSRV.get();
	ID3D11ShaderResourceView* activity = activitySRV.get();
	ID3D11ShaderResourceView* raise = raiseSRV.get();
	// t54 prints, t56 heightmap, t57 activity, t58 raise. t55 is Terrain Blending and is not touched.
	switch (a_stage) {
	case ShaderStage::VS:
		context->VSSetShaderResources(54, 1, &print);
		context->VSSetShaderResources(56, 1, &height);
		context->VSSetShaderResources(57, 1, &activity);
		context->VSSetShaderResources(58, 1, &raise);
		break;
	case ShaderStage::HS:
		context->HSSetShaderResources(54, 1, &print);
		context->HSSetShaderResources(56, 1, &height);
		context->HSSetShaderResources(57, 1, &activity);
		context->HSSetShaderResources(58, 1, &raise);
		break;
	case ShaderStage::DS:
		context->DSSetShaderResources(54, 1, &print);
		context->DSSetShaderResources(56, 1, &height);
		context->DSSetShaderResources(57, 1, &activity);
		context->DSSetShaderResources(58, 1, &raise);
		break;
	case ShaderStage::PS:
		context->PSSetShaderResources(54, 1, &print);
		context->PSSetShaderResources(56, 1, &height);
		context->PSSetShaderResources(57, 1, &activity);
		context->PSSetShaderResources(58, 1, &raise);
		break;
	}
}

void TerrainDeformation::StampHeightmap(ID3D11DeviceContext* a_context)
{
	if (!a_context || !clearCS || !stampCS || !heightUAV || !activityUAV || !printSRV)
		return;

	ID3D11ShaderResourceView* unbound = nullptr;
	a_context->VSSetShaderResources(56, 1, &unbound);
	a_context->VSSetShaderResources(57, 1, &unbound);
	a_context->HSSetShaderResources(56, 1, &unbound);
	a_context->HSSetShaderResources(57, 1, &unbound);
	a_context->DSSetShaderResources(56, 1, &unbound);
	a_context->DSSetShaderResources(57, 1, &unbound);
	a_context->PSSetShaderResources(56, 1, &unbound);
	a_context->PSSetShaderResources(57, 1, &unbound);

	ID3D11ShaderResourceView* printView = printSRV.get();
	a_context->CSSetShaderResources(0, 1, &printView);
	ID3D11UnorderedAccessView* uavs[2]{ heightUAV.get(), activityUAV.get() };
	UINT counts[2]{ 0u, 0u };
	a_context->CSSetUnorderedAccessViews(0, 2, uavs, counts);
	a_context->CSSetShader(clearCS.get(), nullptr, 0);
	const UINT groups = kHeightmapSize / 8u;
	a_context->Dispatch(groups, groups, 1);
	if (uploadedCount > 0) {
		a_context->CSSetShader(stampCS.get(), nullptr, 0);
		a_context->Dispatch(uploadedCount, 1, 1);
	}
	ID3D11UnorderedAccessView* none[2]{};
	a_context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
	ID3D11ShaderResourceView* noPrint = nullptr;
	a_context->CSSetShaderResources(0, 1, &noPrint);
	a_context->CSSetShader(nullptr, nullptr, 0);
}

void TerrainDeformation::SetDepthDisplace(bool a_on)
{
	if (!depthDrawCB)
		return;
	auto context = globals::d3d::context;
	if (!context)
		return;
	// Always rebind. RunGrass keeps its own buffer on b7, and that binding survives into the utility pass.
	if (depthDisplace != a_on) {
		depthDisplace = a_on;
		const float data[4]{ a_on ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(depthDrawCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, data, sizeof(data));
			context->Unmap(depthDrawCB.get(), 0);
		}
	}
	ID3D11Buffer* cb = depthDrawCB.get();
	context->VSSetConstantBuffers(kDepthDrawSlot, 1, &cb);
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
	if (loaded && settings.Enabled && heightSRV)
		BindHeightViews(ShaderStage::PS);
	if (!loaded || !settings.Enabled || !settings.EnableTessellation || vr || (uploadedCount == 0 && !raiseActive) || !a_shader || !a_pass)
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
	BindHeightViews(ShaderStage::PS);
	BindHeightViews(ShaderStage::HS);
	BindHeightViews(ShaderStage::DS);
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

void TerrainDeformation::OnUtilitySetupGeometry(RE::BSShader*, RE::BSRenderPass* a_pass)
{
	SetDepthDisplace(false);
	if (!loaded || !settings.Enabled || (uploadedCount == 0 && !raiseActive) || !heightSRV || !a_pass)
		return;
	if (!IsNearLandscape(a_pass))
		return;

	using F = SIE::ShaderCache::UtilityShaderFlags;
	const uint32_t desc = globals::state->modifiedVertexDescriptor;
	const uint32_t mask = static_cast<uint32_t>(F::RenderShadowmask) | static_cast<uint32_t>(F::RenderShadowmaskSpot) |
	                      static_cast<uint32_t>(F::RenderShadowmaskPb) | static_cast<uint32_t>(F::RenderShadowmaskDpb);
	const bool depthOrShadow = (desc & (static_cast<uint32_t>(F::RenderDepth) | static_cast<uint32_t>(F::RenderShadowmap))) != 0;
	if (!depthOrShadow || (desc & mask) != 0 || (desc & static_cast<uint32_t>(F::LodLandscape)) != 0 || (desc & static_cast<uint32_t>(F::Skinned)) != 0)
		return;
	if (auto* geometry = a_pass->geometry) {
		const auto& bound = geometry->worldBound;
		const float half = static_cast<float>(kHeightmapSize) * kHeightmapTexel * 0.5f;
		const float cx = windowOriginX + half;
		const float cy = windowOriginY + half;
		const float dx = std::max(0.0f, std::fabs(bound.center.x - cx) - half);
		const float dy = std::max(0.0f, std::fabs(bound.center.y - cy) - half);
		if (dx * dx + dy * dy > bound.radius * bound.radius)
			return;
	}

	BindHeightViews(ShaderStage::VS);
	SetDepthDisplace(true);
}

void TerrainDeformation::OnUtilityRestoreGeometry()
{
	if (!depthDisplace)
		return;
	SetDepthDisplace(false);
}

void TerrainDeformation::ClearShaderCache()
{
	{
		std::lock_guard lock(tessLock);
		tessQueue.clear();
		tessShaders.clear();
	}
	clearCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainDeformation\\StampCS.hlsl", { { "CLEAR", "" } }, "cs_5_0")));
	stampCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainDeformation\\StampCS.hlsl", {}, "cs_5_0")));
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

	ImGui::SeparatorText(T(TKEY("raise_section"), "Raised Ground"));
	BFUI::Checkbox(T(TKEY("raise_full"), "Raise the whole near field"), &settings.RaiseFullField);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("raise_full_tt"), "Off: snow, mud and sand stand only under your feet and in footprints. On: the raised layer covers the whole tessellation range."));
	BFUI::SliderFloat(T(TKEY("snow_raise"), "Snow Height"), &settings.SnowRaise, 0.0f, 120.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("snow_raise_tt"), "How far snow stands above the ground, in game units. Under the player it covers about 2.5 metres. The whole near field is only raised when that option is on."));
	BFUI::SliderFloat(T(TKEY("mud_raise"), "Mud Height"), &settings.MudRaise, 0.0f, 60.0f, "%.0f");
	BFUI::SliderFloat(T(TKEY("sand_raise"), "Sand Height"), &settings.SandRaise, 0.0f, 40.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("sand_raise_tt"), "Shallow hollows in sand near the water are lowered, so small shore waves can wash them out."));
	BFUI::SliderFloat(T(TKEY("sink_fraction"), "Leg Sink"), &settings.SinkFraction, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("sink_tt"), "How much of the raised layer a foot presses down. 0.65 sinks about two thirds of the drift."));

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
