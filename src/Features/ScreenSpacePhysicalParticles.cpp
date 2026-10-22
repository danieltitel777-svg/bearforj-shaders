#include "ScreenSpacePhysicalParticles.h"
#include "NativeParticlePhysics.h"
#include "SSPPSubFeatures.h"
#include "WetnessEffects.h"
#include "Bearforj/Context.h"
#include "Bearforj/TipUI.h"
#include "Bearforj/WeatherPresets.h"

#include "../I18n/I18n.h"
#include "../Utils/UI.h"
#include "Buffer.h"
#include "Globals.h"
#include "LinearLighting.h"
#include "Skylighting.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <numbers>
#include <unordered_map>

#define I18N_KEY_PREFIX "feature.sspp."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ScreenSpacePhysicalParticles::Extra, SuppressVanilla, ParticleBudget, GlobalDensity, LeafGroundMin, LeafGroundMax, LeafGlide, LeafGustLift, SparkRestTime, IceRestTime, MagicParticles, MagicCount, MagicLifetime, MagicMaxDistance, SizeVariation, FadeDistance, WindDrift, WindDriftDensity, FireSparks, FireSparkRate, FireSparkRadius, ReactMask, ReactToActors, Pollen, Insects, WaterDrips, GroundStains, SchoolMagic, ShoutParticles, WardImpact)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpacePhysicalParticles::Settings,
	Enabled, Rain, Snow, Leaves, Drizzle, Ash, LeafAutumnBoost, bf, Sparks, Blood, FootstepParticles, Collisions, SkyOcclusion,
	RainCount, SnowCount, LeafCount, DynamicCount, SpawnRadius, SpawnHeight, WindScale,
	RainWidth, RainLength, RainBrightness, RainSpeed, SplashChance, SplashSize, SplashBrightness, SplashLifetime,
	SnowSize, SnowBrightness, SnowSpeed, SnowSettleTime,
	LeafSize, LeafBrightness, LeafSpeed, LeafSettleTime, LeafAmount,
	SparkLifetime, SparkSize, SparkBrightness, SparkBounce, SparkCount,
	DustSize, DustBrightness, DustLifetime, DustCount,
	BloodSize, BloodBrightness, BloodStickTime, BloodCount,
	CollisionThickness, SoftDepth)

namespace
{
	int SchoolOf(const RE::MagicItem* a_magic)
	{
		if (!a_magic)
			return -1;
		for (auto* eff : a_magic->effects) {
			if (!eff || !eff->baseEffect)
				continue;
			switch (eff->baseEffect->GetMagickSkill()) {
			case RE::ActorValue::kRestoration: return 0;
			case RE::ActorValue::kAlteration: return 1;
			case RE::ActorValue::kIllusion: return 2;
			case RE::ActorValue::kConjuration: return 3;
			case RE::ActorValue::kDestruction: return 4;
			default: break;
			}
		}
		return -1;
	}

	bool IsEquippedShoutSpell(RE::Actor* a_actor, RE::FormID a_spell)
	{
		auto* shout = a_actor ? a_actor->GetCurrentShout() : nullptr;
		if (!shout)
			return false;
		for (const auto& variation : shout->variations)
			if (variation.spell && variation.spell->GetFormID() == a_spell)
				return true;
		return false;
	}

	constexpr uint32_t kMaxRain = 200000;
	constexpr uint32_t kMaxSnow = 120000;
	constexpr uint32_t kMaxLeaves = 10000;
	constexpr uint32_t kMaxDynamic = 65536;
}

void ScreenSpacePhysicalParticles::SetupResources()
{
	auto device = globals::d3d::device;
	frameCB = new ConstantBuffer(ConstantBufferDesc<FrameCB>(), "SSPP::Frame");
	emitCB = new ConstantBuffer(ConstantBufferDesc<CBUInt4>(), "SSPP::Emit");

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, counterBuffer.put()));
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_TYPELESS;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = 4;
	uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(counterBuffer.get(), &uav, counterUAV.put()));

	D3D11_BUFFER_DESC ev{};
	ev.ByteWidth = sizeof(EmitEvent) * kMaxEvents;
	ev.Usage = D3D11_USAGE_DYNAMIC;
	ev.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ev.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ev.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	ev.StructureByteStride = sizeof(EmitEvent);
	DX::ThrowIfFailed(device->CreateBuffer(&ev, nullptr, eventBuffer.put()));
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv.Buffer.NumElements = kMaxEvents;
	DX::ThrowIfFailed(device->CreateShaderResourceView(eventBuffer.get(), &srv, eventSRV.put()));

	// premultiplied alpha; alpha 0 = additive
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
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&ds, depthState.put()));

	D3D11_RASTERIZER_DESC rs{};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.DepthClipEnable = TRUE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rs, rasterState.put()));

	D3D11_SAMPLER_DESC samp{};
	samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samp, spriteSampler.put()));
}

void ScreenSpacePhysicalParticles::LoadSpriteAtlas()
{
	if (spriteLoadTried || spriteAtlas)
		return;
	ImVec2 size{};
	ID3D11ShaderResourceView* srv = nullptr;
	if (Util::LoadDDSTextureFromFile(globals::d3d::device, "Textures\\Bearforj\\Particles\\atlas.dds", &srv, size) && srv) {
		spriteAtlas.attach(srv);
		spriteLoadTried = true;
		logger::info("[SSPP] Loaded particle atlas {}x{}", size.x, size.y);
	} else if (frame > 300) {
		spriteLoadTried = true;
		logger::warn("[SSPP] Particle atlas missing, drawing procedural quads");
	}
}

void ScreenSpacePhysicalParticles::CreatePool()
{
	poolRain = std::min(settings.RainCount, kMaxRain);
	poolSnow = std::min(settings.SnowCount, kMaxSnow);
	poolLeaf = std::min(settings.LeafCount, kMaxLeaves);
	poolDynamic = std::clamp(settings.DynamicCount, 1024u, kMaxDynamic);
	{  // shared particle budget
		const uint64_t want = (uint64_t)poolRain + poolSnow + poolLeaf + poolDynamic;
		const uint64_t budget = std::max<uint64_t>(settings.bf.ParticleBudget, 8192u);
		if (want > budget) {
			const double k = (double)budget / (double)want;
			poolRain = (uint32_t)(poolRain * k);
			poolSnow = (uint32_t)(poolSnow * k);
			poolLeaf = (uint32_t)(poolLeaf * k);
			poolDynamic = std::max<uint32_t>((uint32_t)(poolDynamic * k), 1024u);
		}
	}
	const uint32_t total = poolRain + poolSnow + poolLeaf + poolDynamic;

	particleSRV = nullptr;
	particleUAV = nullptr;
	particleBuffer = nullptr;
	auto device = globals::d3d::device;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = sizeof(Particle) * total;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(Particle);
	std::vector<Particle> zero(total, Particle{});  // life 0 = dead
	D3D11_SUBRESOURCE_DATA init{ zero.data(), 0, 0 };
	DX::ThrowIfFailed(device->CreateBuffer(&desc, &init, particleBuffer.put()));
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv.Buffer.NumElements = total;
	DX::ThrowIfFailed(device->CreateShaderResourceView(particleBuffer.get(), &srv, particleSRV.put()));
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = total;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(particleBuffer.get(), &uav, particleUAV.put()));
	poolDirty = false;
	clearPending = false;
	logger::info("[SSPP] Particle pool: rain {}, snow {}, leaves {}, dynamic {} ({} KB)", poolRain, poolSnow, poolLeaf, poolDynamic, desc.ByteWidth / 1024);
}

bool ScreenSpacePhysicalParticles::CompileShaders()
{
	if (shadersFailed)
		return false;
	if (simulateCS && emitCS && drawVS && drawPS)
		return true;
	const bool sky = globals::features::skylighting.loaded;
	std::vector<std::pair<const char*, const char*>> simDefines;
	if (sky)
		simDefines.emplace_back("SSPP_SKYLIGHTING", "");
	simulateCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ScreenSpacePhysicalParticles\\Runtime\\SSPPSimulateCS.hlsl", simDefines, "cs_5_0")));
	emitCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ScreenSpacePhysicalParticles\\Runtime\\SSPPEmitCS.hlsl", {}, "cs_5_0")));
	drawVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ScreenSpacePhysicalParticles\\Runtime\\SSPPDraw.hlsl", { { "VSHADER", "" } }, "vs_5_0")));
	drawPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ScreenSpacePhysicalParticles\\Runtime\\SSPPDraw.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
	shadersFailed = !(simulateCS && emitCS && drawVS && drawPS);
	if (shadersFailed)
		logger::error("[SSPP] Failed to compile runtime shaders; particles disabled until the shader cache is cleared");
	return !shadersFailed;
}

void ScreenSpacePhysicalParticles::ClearShaderCache()
{
	simulateCS = nullptr;
	emitCS = nullptr;
	drawVS = nullptr;
	drawPS = nullptr;
	shadersFailed = false;
}

void ScreenSpacePhysicalParticles::PostPostLoad() {}

void ScreenSpacePhysicalParticles::DataLoaded()
{
	if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
		events->AddEventSink<RE::TESHitEvent>(&hitSink);
		events->AddEventSink<RE::TESSpellCastEvent>(&spellSink);
		logger::info("[SSPP] Registered hit and spell-cast sinks");
	}
	if (auto* manager = RE::BGSFootstepManager::GetSingleton()) {
		manager->AddEventSink(&footstepSink);
		logger::info("[SSPP] Registered footstep event sink (dust / snow kick-up)");
	}
}

RE::BSEventNotifyControl ScreenSpacePhysicalParticles::FootstepSink::ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*)
{
	auto& f = globals::features::screenSpacePhysicalParticles;
	if (!a_event || !f.loaded || !f.settings.Enabled || !f.settings.FootstepParticles)
		return RE::BSEventNotifyControl::kContinue;
	auto actorPtr = a_event->actor.get();
	auto* actor = actorPtr ? actorPtr->As<RE::Actor>() : nullptr;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!actor || !player || !tes || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInWater())
		return RE::BSEventNotifyControl::kContinue;
	if (actor->GetPosition().GetDistance(player->GetPosition()) > 4000.0f)
		return RE::BSEventNotifyControl::kContinue;
	// same foot-node lookup as TerrainDeformation::OnFootstep
	const std::string_view tag = a_event->tag.c_str() ? std::string_view(a_event->tag.c_str()) : std::string_view{};
	const bool left = tag.find("Left") != std::string_view::npos || tag.find("FootL") != std::string_view::npos;
	const bool right = tag.find("Right") != std::string_view::npos || tag.find("FootR") != std::string_view::npos;
	RE::NiPoint3 pos = actor->GetPosition();
	if (auto* root = actor->Get3D(); root && (left || right)) {
		static const RE::BSFixedString l = "NPC L Foot [Lft ]", r = "NPC R Foot [Rft ]";
		if (auto* foot = root->GetObjectByName(left ? l : r))
			pos = foot->world.translate;
	}
	f.EmitFootstep(pos, tes->GetLandMaterialType(pos), std::clamp(actor->GetScale(), 0.5f, 3.0f));
	return RE::BSEventNotifyControl::kContinue;
}

void ScreenSpacePhysicalParticles::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening)
		clearPending = true;  // kill everything on cell/load transitions
	std::lock_guard lock(eventLock);
	pendingEvents.clear();
}

void ScreenSpacePhysicalParticles::Emit(const EmitEvent& a_event)
{
	if (!loaded || !settings.Enabled || a_event.count == 0)
		return;
	std::lock_guard lock(eventLock);
	if (pendingEvents.size() < kMaxEvents)
		pendingEvents.push_back(a_event);
}

void ScreenSpacePhysicalParticles::EmitFootstep(const RE::NiPoint3& a_position, RE::MATERIAL_ID a_material, float a_scale)
{
	if (!settings.FootstepParticles || !SSPPSub::Footsteps().Allowed())
		return;
	using M = RE::MATERIAL_ID;
	// plan step 3: leaves kicked up by steps on forest floor (tree context, no snow on the ground)
	if (settings.Leaves && SSPPSub::Leaves().Allowed() && a_material != M::kSnow && a_material != M::kSnowStairs && a_material != M::kStone && a_material != M::kWood) {
		const auto& cx = Bearforj::Context::Get();
		if (cx.trees > 0.25f && !(cx.loc & Bearforj::Context::kLocInterior) && cx.cold < 0.5f) {
			static uint32_t rng = 7u;
			rng = rng * 1664525u + 1013904223u;
			const float t = (rng >> 8) / 16777216.0f;
			EmitEvent l{};
			l.position = float3(a_position.x, a_position.y, a_position.z + 3.0f);
			const bool conifer = cx.conifers > cx.trees;
			const bool twig = (rng & 3u) == 0u;
			l.kind = conifer ? (twig ? kTwig : kNeedle) : WithSprite(kLeaf, (t < 0.55f) ? kSprLeaf : kSprLeafNarrow);
			l.direction = float3(0.0f, 0.0f, 1.0f);
			l.count = 2u + static_cast<uint32_t>(4.0f * cx.trees * a_scale);
			const float leafScale = !conifer ? 1.0f : (twig ? 0.5f : 0.32f);
			l.params = float4(110.0f * a_scale, 0.8f, settings.LeafSize * 0.8f * leafScale, 12.0f);
			l.color = t < 0.4f ? float4(0.55f, 0.32f, 0.08f, 1) : t < 0.75f ? float4(0.62f, 0.48f, 0.12f, 1) : float4(0.3f, 0.38f, 0.1f, 1);
			Emit(l);
		}
	}
	float4 color;
	float amount = 1.0f;
	switch (a_material) {
	case M::kSnow:
	case M::kSnowStairs:
		color = float4(0.95f, 0.97f, 1.0f, 1.0f);
		amount = 1.6f;
		break;
	case M::kSand:
		color = float4(0.72f, 0.62f, 0.45f, 1.0f);
		break;
	case M::kDirt:
		color = float4(0.45f, 0.38f, 0.3f, 1.0f);
		amount = 0.8f;
		break;
	case M::kGravel:
		color = float4(0.5f, 0.48f, 0.45f, 1.0f);
		amount = 0.6f;
		break;
	case M::kMud:
		color = float4(0.3f, 0.24f, 0.18f, 1.0f);
		amount = 0.5f;
		break;
	default:
		return;
	}
	EmitEvent e{};
	e.position = float3(a_position.x, a_position.y, a_position.z + 2.0f);
	e.kind = WithSprite(kDust, (a_material == M::kSnow || a_material == M::kSnowStairs) ? kSprSnow : kSprDust);
	e.direction = float3(0.0f, 0.0f, 0.0f);
	e.count = static_cast<uint32_t>(std::max(1.0f, settings.DustCount * amount * a_scale));
	e.params = float4(70.0f * a_scale, 1.0f, settings.DustSize * a_scale, settings.DustLifetime);
	e.color = color;
	Emit(e);
}

RE::BSEventNotifyControl ScreenSpacePhysicalParticles::HitSink::ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*)
{
	globals::features::screenSpacePhysicalParticles.OnHit(a_event);
	return RE::BSEventNotifyControl::kContinue;
}

void ScreenSpacePhysicalParticles::OnHit(const RE::TESHitEvent* a_event)
{
	if (!a_event || !a_event->target || !loaded || !settings.Enabled)
		return;
	auto* target = a_event->target.get();
	auto* actor = target->As<RE::Actor>();
	if (!actor || !actor->Is3DLoaded())
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (player && actor->GetPosition().GetDistance(player->GetPosition()) > 4096.0f)
		return;
	const RE::NiPoint3 pos = actor->GetPosition();
	const float height = std::max(actor->GetHeight(), 32.0f);
	float3 dir{ 0.0f, 0.0f, 0.3f };
	if (auto* cause = a_event->cause.get()) {
		const RE::NiPoint3 d = pos - cause->GetPosition();
		const float len = std::max(d.Length(), 1.0f);
		dir = float3(d.x / len, d.y / len, 0.35f);
	}
	const bool blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
	// Destruction stays on the resist variable. The other schools use their own type ids.
	RE::MagicItem* magic = a_event->source ? RE::TESForm::LookupByID<RE::MagicItem>(a_event->source) : nullptr;
	const float dist = player ? actor->GetPosition().GetDistance(player->GetPosition()) : 0.0f;
	const float lod = std::clamp(1.0f - dist / std::max(settings.bf.MagicMaxDistance, 1.0f), 0.2f, 1.0f);
	int element = -1;
	const int school = SchoolOf(magic);
	if (magic) {
		for (auto* eff : magic->effects) {
			if (!eff || !eff->baseEffect)
				continue;
			const auto rv = eff->baseEffect->data.resistVariable;
			element = rv == RE::ActorValue::kResistFire ? 0 : rv == RE::ActorValue::kResistFrost ? 1 : rv == RE::ActorValue::kResistShock ? 2 : element;
			if (element >= 0)
				break;
		}
	}
	const bool magicNear = dist < settings.bf.MagicMaxDistance;
	if (settings.bf.MagicParticles && magic && magicNear && element >= 0) {
		EmitEvent m{};
		m.position = float3(pos.x, pos.y, pos.z + height * 0.6f);
		m.direction = dir;
		m.count = std::min<uint32_t>(static_cast<uint32_t>(settings.bf.MagicCount * lod), 256u);
		if (element == 0) {
			m.kind = WithSprite(kEmber, kSprEmber);
			m.params = float4(220.0f, 0.9f, settings.SparkSize * 1.6f, settings.bf.MagicLifetime);
			m.color = float4(1.0f, 0.5f, 0.1f, 1.0f);
		} else if (element == 1) {
			m.kind = WithSprite(kIce, kSprIce);
			m.params = float4(320.0f, 0.7f, settings.SparkSize * 2.2f, settings.bf.MagicLifetime);
			m.color = float4(0.8f, 0.9f, 1.0f, 1.0f);
		} else {
			m.kind = WithSprite(kSpark, kSprSpark);
			m.params = float4(520.0f, 1.0f, settings.SparkSize, settings.bf.MagicLifetime * 0.5f);
			m.color = float4(0.5f, 0.7f, 1.0f, 1.0f);
		}
		Emit(m);
	} else if (settings.bf.SchoolMagic && magic && magicNear && school >= 0 && school <= 3) {
		EmitEvent m{};
		m.position = float3(pos.x, pos.y, pos.z + height * 0.7f);
		m.direction = school == 1 ? float3(0.0f, 0.0f, -0.4f) : float3(0.0f, 0.0f, 1.0f);
		m.count = std::min<uint32_t>(static_cast<uint32_t>(settings.bf.MagicCount * 0.55f * lod), 128u);
		if (school == 0) {
			m.kind = kRestoration;
			m.params = float4(40.0f, 0.9f, 1.3f, 1.8f);
			m.color = float4(1.0f, 0.86f, 0.4f, 0.0f);
		} else if (school == 1) {
			m.kind = kAlteration;
			m.params = float4(80.0f, 1.0f, 1.1f, settings.bf.MagicLifetime);
			m.color = float4(0.65f, 0.86f, 1.0f, 0.85f);
		} else if (school == 2) {
			m.kind = kIllusion;
			m.params = float4(50.0f, 1.0f, 1.2f, 1.6f);
			m.color = float4(0.78f, 0.4f, 1.0f, 0.0f);
		} else {
			m.kind = kConjuration;
			m.params = float4(35.0f, 0.8f, 2.6f, 2.2f);
			m.color = float4(0.4f, 0.18f, 0.62f, 0.55f);
		}
		Emit(m);
	}
	float ward = 0.0f;
	if (auto* values = actor->AsActorValueOwner())
		ward = values->GetActorValue(RE::ActorValue::kWardPower);
	if (blocked && settings.bf.WardImpact && ward > 0.0f && magicNear) {
		float4 tint{ 0.85f, 0.9f, 1.0f, 0.0f };
		if (element == 0)
			tint = float4(1.0f, 0.45f, 0.1f, 0.0f);
		else if (element == 1)
			tint = float4(0.7f, 0.9f, 1.0f, 0.0f);
		else if (element == 2)
			tint = float4(0.5f, 0.7f, 1.0f, 0.0f);
		else if (school == 0)
			tint = float4(1.0f, 0.86f, 0.4f, 0.0f);
		else if (school == 2)
			tint = float4(0.78f, 0.4f, 1.0f, 0.0f);
		else if (school == 3)
			tint = float4(0.45f, 0.2f, 0.7f, 0.0f);
		EmitEvent shard{};
		shard.position = float3(pos.x, pos.y, pos.z + height * 0.65f);
		shard.direction = dir;
		shard.kind = kWardShard;
		shard.count = 22;
		shard.params = float4(380.0f, 0.9f, settings.SparkSize * 1.5f, 0.7f);
		shard.color = tint;
		Emit(shard);
		EmitEvent ripple{};
		ripple.position = float3(pos.x, pos.y, pos.z + 4.0f);
		ripple.direction = float3(0.0f, 0.0f, 0.0f);
		ripple.kind = kWardRipple;
		ripple.count = 1;
		ripple.params = float4(0.01f, 0.0f, 8.0f, 0.65f);
		ripple.color = float4(tint.x, tint.y, tint.z, 0.55f);
		Emit(ripple);
		return;
	}
	EmitEvent e{};
	e.direction = dir;
	if (blocked) {
		if (!settings.Sparks || !SSPPSub::Effects().Allowed())
			return;
		e.position = float3(pos.x, pos.y, pos.z + height * 0.65f);
		e.kind = WithSprite(kSpark, kSprSpark);
		e.count = settings.SparkCount;
		e.params = float4(450.0f, 0.8f, settings.SparkSize, settings.SparkLifetime);
		e.color = float4(1.0f, 0.7f, 0.3f, 1.0f);
	} else {
		if (!settings.Blood || !SSPPSub::Blood().Allowed())
			return;
		e.position = float3(pos.x, pos.y, pos.z + height * 0.6f);
		e.kind = WithSprite(kBlood, kSprBlood);
		e.count = settings.BloodCount;
		e.params = float4(260.0f, 0.6f, settings.BloodSize, 30.0f);  // lifetime ends when it sticks
		e.color = float4(0.35f, 0.02f, 0.02f, 1.0f);
	}
	Emit(e);
}

RE::BSEventNotifyControl ScreenSpacePhysicalParticles::SpellSink::ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	globals::features::screenSpacePhysicalParticles.OnSpellCast(a_event);
	return RE::BSEventNotifyControl::kContinue;
}

void ScreenSpacePhysicalParticles::OnSpellCast(const RE::TESSpellCastEvent* a_event)
{
	if (!a_event || !a_event->object || !loaded || !settings.Enabled)
		return;
	auto* actor = a_event->object->As<RE::Actor>();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!actor || !actor->Is3DLoaded() || !player)
		return;
	if (actor->GetPosition().GetDistance(player->GetPosition()) > settings.bf.MagicMaxDistance)
		return;
	static std::unordered_map<RE::FormID, float> lastCast;
	const float now = time;
	const RE::FormID id = actor->GetFormID();
	if (const auto it = lastCast.find(id); it != lastCast.end() && now - it->second < 0.3f)
		return;
	lastCast[id] = now;
	const RE::NiPoint3 pos = actor->GetPosition();
	const float height = std::max(actor->GetHeight(), 32.0f);
	if (settings.bf.ShoutParticles && IsEquippedShoutSpell(actor, a_event->spell)) {
		EmitEvent wave{};
		wave.position = float3(pos.x, pos.y, pos.z + height * 0.45f);
		wave.direction = float3(0.0f, 0.0f, 0.0f);
		wave.kind = kShout;
		wave.count = 40;
		wave.params = float4(460.0f, 1.0f, 3.5f, 1.15f);
		wave.color = float4(0.86f, 0.9f, 1.0f, 0.4f);
		Emit(wave);
		return;
	}
	if (!settings.bf.SchoolMagic)
		return;
	auto* magic = RE::TESForm::LookupByID<RE::MagicItem>(a_event->spell);
	const int school = SchoolOf(magic);
	if (school < 0 || school > 3)
		return;
	EmitEvent m{};
	m.position = float3(pos.x, pos.y, pos.z + height * 0.7f);
	m.direction = school == 1 ? float3(0.0f, 0.0f, -0.3f) : float3(0.0f, 0.0f, 1.0f);
	m.count = std::min<uint32_t>(settings.bf.MagicCount / 2u, 64u);
	if (school == 0) {
		m.kind = kRestoration;
		m.params = float4(40.0f, 0.9f, 1.3f, 1.8f);
		m.color = float4(1.0f, 0.86f, 0.4f, 0.0f);
	} else if (school == 1) {
		m.kind = kAlteration;
		m.params = float4(70.0f, 1.0f, 1.1f, settings.bf.MagicLifetime);
		m.color = float4(0.65f, 0.86f, 1.0f, 0.85f);
	} else if (school == 2) {
		m.kind = kIllusion;
		m.params = float4(50.0f, 1.0f, 1.2f, 1.6f);
		m.color = float4(0.78f, 0.4f, 1.0f, 0.0f);
	} else {
		m.kind = kConjuration;
		m.params = float4(30.0f, 0.8f, 2.6f, 2.2f);
		m.color = float4(0.4f, 0.18f, 0.62f, 0.55f);
	}
	Emit(m);
}

void ScreenSpacePhysicalParticles::UpdateAmbient(float a_dt, float a_rain, float a_snow, bool a_paused, const float3& a_camera)
{
	if (a_paused || a_dt <= 0.0f)
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;
	const float precip = std::clamp(std::max(a_rain, a_snow), 0.0f, 1.0f);
	const float calm = 1.0f - precip * 0.85f;
	const bool interior = Util::IsInterior();
	const auto& ctx = Bearforj::Context::Get();
	static uint32_t rng = 19u;
	auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; };
	auto burst = [&](float& a_acc, float a_rate, int a_cap) {
		a_acc += a_dt * a_rate * calm;
		int n = 0;
		while (a_acc >= 1.0f && n < a_cap) {
			a_acc -= 1.0f;
			++n;
		}
		return n;
	};

	if (settings.bf.Pollen && !interior && a_rain < 0.45f && a_snow < 0.45f && ctx.cold < 0.65f) {
		static float acc = 0.0f;
		for (int i = 0, n = burst(acc, 5.0f, 3); i < n; ++i) {
			const float a = rnd() * 6.2831853f;
			const float r = 80.0f + rnd() * 900.0f;
			EmitEvent e{};
			e.position = float3(a_camera.x + std::cos(a) * r, a_camera.y + std::sin(a) * r, a_camera.z - 40.0f + rnd() * 80.0f);
			e.kind = kPollen;
			e.direction = float3(0.0f, 0.0f, 0.05f);
			e.count = 1;
			e.params = float4(18.0f, 1.0f, 0.55f, 8.0f);
			e.color = float4(0.95f, 0.82f, 0.28f, 0.85f);
			Emit(e);
		}
	}

	if (settings.bf.WaterDrips && globals::features::wetnessEffects.loaded && player->Is3DLoaded()) {
		const float wet = globals::features::wetnessEffects.GetCharacterRetainedWetness();
		if (wet > 0.2f) {
			static float acc = 0.0f;
			const float rate = 1.5f + wet * 8.0f;
			acc += a_dt * rate;
			const float height = std::max(player->GetHeight(), 64.0f);
			const RE::NiPoint3 pos = player->GetPosition();
			int n = 0;
			while (acc >= 1.0f && n < 4) {
				acc -= 1.0f;
				++n;
				EmitEvent e{};
				e.position = float3(pos.x + (rnd() - 0.5f) * 36.0f, pos.y + (rnd() - 0.5f) * 36.0f, pos.z + height * (0.35f + rnd() * 0.6f));
				e.kind = kWaterDrip;
				e.direction = float3((rnd() - 0.5f) * 0.15f, (rnd() - 0.5f) * 0.15f, -1.0f);
				e.count = 1;
				e.params = float4(28.0f, 0.2f, 0.35f + wet * 0.25f, 1.6f);
				e.color = float4(0.75f, 0.86f, 0.98f, 0.8f);
				Emit(e);
			}
		}
	}

	if (!settings.bf.Insects)
		return;
	static float nextScan = 0.0f;
	static bool nearWater = false;
	static std::vector<float3> corpses;
	static std::vector<float3> lamps;
	nextScan -= a_dt;
	if (nextScan <= 0.0f) {
		nextScan = 2.0f;
		corpses.clear();
		lamps.clear();
		const RE::NiPoint3 ppos = player->GetPosition();
		const float water = SubmersionWetness::WaterHeightAt(player, ppos);
		nearWater = water > -1.0e30f && std::abs(ppos.z - water) < 200.0f;
		if (auto* tes = RE::TES::GetSingleton()) {
			tes->ForEachReferenceInRange(player, 1600.0f, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled())
					return RE::BSContainer::ForEachResult::kContinue;
				if (auto* dead = a_ref->As<RE::Actor>(); dead && dead->IsDead() && dead->Is3DLoaded() && corpses.size() < 4) {
					const auto p = dead->GetPosition();
					corpses.push_back(float3(p.x, p.y, p.z + 20.0f));
				}
				if (lamps.size() < 8) {
					if (auto* light = a_ref->GetBaseObject() ? a_ref->GetBaseObject()->As<RE::TESObjectLIGH>() : nullptr) {
						const auto& c = light->data.color;
						if (!light->GetNoFlicker() && c.red > 140 && light->data.radius >= 200 && ctx.night) {
							const auto p = a_ref->GetPosition();
							lamps.push_back(float3(p.x, p.y, p.z - 30.0f));
						}
					}
				}
				return (corpses.size() >= 4 && lamps.size() >= 8) ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
			});
		}
	}
	auto mote = [&](Kind a_kind, const float3& a_pos, float a_size, float a_life, float4 a_color) {
		EmitEvent e{};
		e.position = a_pos;
		e.kind = a_kind;
		e.direction = float3(0.0f, 0.0f, 0.1f);
		e.count = 1;
		e.params = float4(16.0f, 1.0f, a_size, a_life);
		e.color = a_color;
		Emit(e);
	};
	if (!interior && ctx.night && a_rain < 0.5f) {
		static float acc = 0.0f;
		for (int i = 0, n = burst(acc, 3.0f, 2); i < n; ++i) {
			const float a = rnd() * 6.2831853f, r = 100.0f + rnd() * 700.0f;
			mote(kFirefly, float3(a_camera.x + std::cos(a) * r, a_camera.y + std::sin(a) * r, a_camera.z + 40.0f + rnd() * 180.0f), 1.1f, 6.0f, float4(1.0f, 0.9f, 0.35f, 0.0f));
		}
	}
	if (!interior && !ctx.night && ctx.cold < 0.55f && a_rain < 0.4f) {
		static float acc = 0.0f;
		for (int i = 0, n = burst(acc, 1.4f, 1); i < n; ++i) {
			const float a = rnd() * 6.2831853f, r = 150.0f + rnd() * 800.0f;
			mote(kButterfly, float3(a_camera.x + std::cos(a) * r, a_camera.y + std::sin(a) * r, a_camera.z + rnd() * 120.0f), 4.5f, 9.0f, float4(0.95f, 0.55f, 0.12f, 0.9f));
		}
	}
	if (nearWater) {
		static float acc = 0.0f;
		for (int i = 0, n = burst(acc, 4.0f, 2); i < n; ++i) {
			const float a = rnd() * 6.2831853f, r = 40.0f + rnd() * 250.0f;
			mote(kGnat, float3(a_camera.x + std::cos(a) * r, a_camera.y + std::sin(a) * r, a_camera.z + 10.0f + rnd() * 50.0f), 0.45f, 4.0f, float4(0.75f, 0.8f, 0.7f, 0.7f));
		}
	}
	if (!corpses.empty()) {
		static float acc = 0.0f;
		for (int i = 0, n = burst(acc, 3.0f, 2); i < n; ++i) {
			const float3 c = corpses[static_cast<size_t>(rnd() * corpses.size()) % corpses.size()];
			mote(kFly, float3(c.x + (rnd() - 0.5f) * 40.0f, c.y + (rnd() - 0.5f) * 40.0f, c.z + rnd() * 30.0f), 0.7f, 5.0f, float4(0.15f, 0.15f, 0.16f, 0.9f));
		}
	}
	if (!lamps.empty()) {
		static float acc = 0.0f;
		for (int i = 0, n = burst(acc, 2.5f, 2); i < n; ++i) {
			const float3 c = lamps[static_cast<size_t>(rnd() * lamps.size()) % lamps.size()];
			mote(kLampInsect, float3(c.x + (rnd() - 0.5f) * 30.0f, c.y + (rnd() - 0.5f) * 30.0f, c.z + rnd() * 25.0f), 0.55f, 4.0f, float4(0.9f, 0.85f, 0.45f, 0.75f));
		}
	}
}

void ScreenSpacePhysicalParticles::ComputeWeather(float& a_rain, float& a_snow, float3& a_wind, float& a_gust) const
{
	a_rain = 0.0f;
	a_snow = 0.0f;
	a_wind = float3(0.0f, 0.0f, 0.0f);
	a_gust = 0.0f;
	auto* sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || Util::IsInterior())
		return;
	auto weatherAmount = [&](RE::TESWeather* a_weather, float a_weight) {
		if (!a_weather || a_weight <= 0.0f)
			return;
		const auto flags = a_weather->data.flags;
		if (flags.any(RE::TESWeather::WeatherDataFlag::kRainy))
			a_rain += a_weight;
		if (flags.any(RE::TESWeather::WeatherDataFlag::kSnow))
			a_snow += a_weight;
	};
	const float pct = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	weatherAmount(sky->currentWeather, pct);
	weatherAmount(sky->lastWeather, 1.0f - pct);
	// fade precipitation in over the second half of the transition, like the game
	a_rain = std::clamp(a_rain, 0.0f, 1.0f);
	a_snow = std::clamp(a_snow, 0.0f, 1.0f);

	if (auto* weather = sky->currentWeather) {
		const float speed = static_cast<float>(weather->data.windSpeed) / 255.0f;
		const float angle = static_cast<float>(static_cast<uint8_t>(weather->data.windDirection)) / 256.0f * 2.0f * std::numbers::pi_v<float>;
		const float strength = speed * 420.0f * settings.WindScale;
		a_wind = float3(std::sin(angle) * strength, std::cos(angle) * strength, 0.0f);
		a_gust = std::clamp(static_cast<float>(static_cast<uint8_t>(weather->data.windDirectionRange)) / 255.0f + speed * 0.3f, 0.0f, 1.0f);
	}
}

namespace
{
	// Hides the game's precipitation geometry (whatever weather mod supplies it) while SSPP draws rain/snow.
	bool g_vanillaCulled = false;
	void SetVanillaPrecipCulled(bool a_cull)
	{
		auto* sky = globals::game::sky;
		if (!sky || !sky->precip || (a_cull == g_vanillaCulled && !a_cull))
			return;
		for (auto* g : { sky->precip->currentPrecip.get(), sky->precip->lastPrecip.get() })
			if (g)
				g->SetAppCulled(a_cull);
		g_vanillaCulled = a_cull;
	}
}

void ScreenSpacePhysicalParticles::UpdateFireSparks()
{
	if (!settings.bf.FireSparks || !settings.Sparks || !SSPPSub::Effects().Allowed())
		return;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!player || !tes)
		return;
	static auto last = std::chrono::steady_clock::now();
	const auto nowTp = std::chrono::steady_clock::now();
	const float dt = std::clamp(std::chrono::duration<float>(nowTp - last).count(), 0.0f, 0.25f);
	last = nowTp;
	fireNextScan -= dt;
	if (fireNextScan <= 0.0f) {
		fireNextScan = 2.0f;
		fireSources.clear();
		static RE::BGSKeyword* forgeKw[2] = {
			RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingForge"),
			RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmelter")
		};
		tes->ForEachReferenceInRange(player, std::max(settings.bf.FireSparkRadius, 300.0f), [&](RE::TESObjectREFR* a_ref) {
			if (fireSources.size() >= 16)
				return RE::BSContainer::ForEachResult::kStop;
			auto* base = a_ref && !a_ref->IsDisabled() ? a_ref->GetBaseObject() : nullptr;
			if (!base)
				return RE::BSContainer::ForEachResult::kContinue;
			bool fire = false;
			float lift = 20.0f;
			if (auto* light = base->As<RE::TESObjectLIGH>()) {
				// fire lights flicker and are warm (red > green > blue); torches/candles are excluded by radius
				const auto& c = light->data.color;
				fire = !light->GetNoFlicker() && c.red > c.green && c.green > c.blue && c.red > 150 && light->data.radius >= 300;
				lift = -40.0f;  // fire lights hang above the flames
			} else if (auto* furn = base->As<RE::TESFurniture>()) {
				for (auto* kw : forgeKw)
					fire |= kw && furn->HasKeyword(kw);
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
	const float interval = 1.0f / std::clamp(settings.bf.FireSparkRate, 0.1f, 20.0f);
	static uint32_t rng = 911u;
	for (size_t i = 0; i < fireSources.size(); ++i) {
		fireTimers[i] -= dt;
		if (fireTimers[i] > 0.0f)
			continue;
		rng = rng * 1664525u + 1013904223u;
		fireTimers[i] = interval * (0.5f + (rng >> 8) / 16777216.0f);
		EmitEvent e{};
		e.position = fireSources[i];
		e.direction = float3(0.0f, 0.0f, 1.0f);
		e.kind = WithSprite(kEmber, kSprEmber);
		e.count = 3 + (rng & 3u);
		e.params = float4(140.0f, 0.6f, settings.SparkSize * 1.2f, 2.5f);  // speed, spread, size, lifetime (same layout as magic embers)
		e.color = float4(1.0f, 0.55f, 0.15f, 1.0f);
		Emit(e);
	}
}

void ScreenSpacePhysicalParticles::Prepass()
{
	simulatedThisFrame = false;
	if (!loaded || !settings.Enabled || globals::game::isVR || !settings.bf.SuppressVanilla || !shadersReadyForCull)
		SetVanillaPrecipCulled(false);  // SSPP off / failed: the weather mod's precipitation is back
}

void ScreenSpacePhysicalParticles::OnBeforePostProcessing(RE::RENDER_TARGET a_renderTarget)
{
	// inWorld is set only for the body of Main::RenderWorld and cleared when that call returns.
	// ImageSpaceManager::PostProcessing (the HDR hook this runs from) is after that, so an
	// inWorld test here skips every frame. 0.6 drew in this hook and the particles were on
	// screen; 0.6.3's guard is why rain, snow, leaves, sparks, blood and dust all vanished.
	// worldRenderedThisFrame stays true until Present, which is what post-time code has to use.
	if (!loaded || !settings.Enabled || globals::game::isVR || !globals::state->worldRenderedThisFrame)
		return;
	if (simulatedThisFrame)
		return;  // HDR blend can be invoked more than once per frame
	UpdateFireSparks();
	shadersReadyForCull = CompileShaders();
	if (!shadersReadyForCull)
		return;
	if (poolDirty || clearPending || !particleBuffer)
		CreatePool();

	auto* renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	const auto targetIndex = static_cast<size_t>(a_renderTarget);
	if (!renderer || targetIndex >= static_cast<size_t>(Util::GetRenderTargetCount()))
		return;
	auto* rtv = Util::AsReal(renderer->GetRuntimeData().renderTargets[targetIndex].RTV);
	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto* depthSRV = Util::AsReal(depth.depthSRV);
	if (!rtv || !depthSRV)
		return;
	simulatedThisFrame = true;

	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float dt = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	time += dt;
	++frame;

	float rain = 0.0f, snow = 0.0f, gust = 0.0f;
	float3 wind{};
	ComputeWeather(rain, snow, wind, gust);

	const auto dynSize = Util::ConvertToDynamic(globals::state->screenSize, true);
	const auto& proj = globals::game::frameBufferCached.GetCameraProj(0);
	const auto& camAdjust = REL::Module::IsVR() ? globals::game::frameBufferCached.vr.CameraPosAdjust[0] : globals::game::frameBufferCached.nonVR.CameraPosAdjust;

	FrameCB cb{};
	cb.ViewProj = globals::game::frameBufferCached.GetCameraViewProj(0);
	cb.CameraPos = float4(camAdjust.x, camAdjust.y, camAdjust.z, Util::IsInterior() ? 1.0f : 0.0f);
	// row_major mul(P, v): clip.z = P[2][2] * z + P[2][3], clip.w = z
	cb.ProjParams = float4(proj.m[2][2], proj.m[2][3], dynSize.x, dynSize.y);
	cb.TimeParams = float4(time, dt, static_cast<float>(frame), static_cast<float>(poolDynamic));
	cb.Wind = float4(wind.x, wind.y, wind.z, gust);
	const float windStrength = std::sqrt(wind.x * wind.x + wind.y * wind.y) / 420.0f;
	// Bearforj: weather-driven variants on the same particle kinds, so wind, shelter check and LL apply unchanged.
	float rainW = 1.0f, rainL = 1.0f, rainB = 1.0f, rainS = 1.0f, snowB = 1.0f, snowS = 1.0f, leafBoost = 1.0f;
	if (!Util::IsInterior()) {
		const auto slot = Bearforj::WeatherPresets::CurrentSlot();
		if (settings.Drizzle && slot == Bearforj::WeatherPresets::kFog && rain < 0.05f) {
			rain = 0.3f;  // drizzle / mist
			rainW = 0.5f, rainL = 0.35f, rainB = 0.6f, rainS = 0.45f;
		}
		if (settings.Ash && slot == Bearforj::WeatherPresets::kAsh) {
			snow = std::max(snow, 0.7f);  // ash fall: slow, dark grey flakes
			snowB = 0.3f, snowS = 0.6f;
		}
		if (Bearforj::WeatherPresets::IsAutumnOrForest())
			leafBoost = settings.LeafAutumnBoost;
	}
	Bearforj::Context::Update();
	const auto& ctx = Bearforj::Context::Get();
	// Context (plan step 2): leaves follow the real tree density around the player (needles for conifers)
	leafBoost *= 0.35f + 1.25f * ctx.trees + 0.5f * ctx.conifers;
	// Never double precipitation: either SSPP draws rain/snow and the vanilla/weather-mod emitter is culled,
	// or (SuppressVanilla off) the weather mod's own precipitation stays and SSPP rain/snow stay off.
	{
		const bool precipOn = SSPPSub::Precipitation().Allowed();
		const bool ours = precipOn && settings.bf.SuppressVanilla && (settings.Rain || settings.Snow);
		if (!precipOn)
			rain = snow = 0.0f;
		if (!settings.bf.SuppressVanilla)
			rain = snow = 0.0f;
		SetVanillaPrecipCulled(ours);
	}
	const float leaves = settings.Leaves && SSPPSub::Leaves().Allowed() ? std::clamp(settings.LeafAmount * leafBoost * (0.3f + windStrength * 1.5f) * (1.0f - snow), 0.0f, 1.0f) : 0.0f;
	{
		const float gd = std::clamp(settings.bf.GlobalDensity, 0.0f, 2.0f);
		cb.Amounts = float4(settings.Rain ? std::min(rain * gd, 1.0f) : 0.0f, settings.Snow ? std::min(snow * gd, 1.0f) : 0.0f, Util::IsInterior() ? 0.0f : leaves * gd, settings.SpawnRadius);
	}
	cb.Ranges[0] = poolRain;
	cb.Ranges[1] = poolRain + poolSnow;
	cb.Ranges[2] = poolRain + poolSnow + poolLeaf;
	cb.Ranges[3] = cb.Ranges[2] + poolDynamic;
	cb.RainP = float4(settings.RainWidth * rainW, settings.RainLength * rainL, settings.RainBrightness * rainB, settings.RainSpeed * rainS);
	cb.SnowP = float4(settings.SnowSize, settings.SnowBrightness * snowB, settings.SnowSpeed * snowS, settings.SnowSettleTime);
	cb.LeafP = float4(settings.LeafSize, settings.LeafBrightness, settings.LeafSpeed, settings.LeafSettleTime);
	cb.SparkP = float4(settings.SparkLifetime, settings.SparkSize, settings.SparkBrightness, settings.SparkBounce);
	const auto llData = globals::features::linearLighting.loaded ? globals::features::linearLighting.GetCommonBufferData() : LinearLighting::PerFrameData{};
	cb.DustP = float4(settings.DustSize, settings.DustBrightness, settings.DustLifetime, llData.enableLinearLighting ? 1.0f : 0.0f);
	cb.BloodP = float4(settings.BloodSize, settings.BloodBrightness, settings.BloodStickTime, llData.enableACEScg ? 1.0f : 0.0f);
	cb.SplashP = float4(settings.SplashSize, settings.SplashBrightness, settings.SplashChance, settings.SplashLifetime);
	float3 light{ 0.6f, 0.6f, 0.65f };
	if (auto* sky = globals::game::sky) {
		const auto& amb = sky->skyColor[RE::TESWeather::ColorTypes::kAmbient];
		const auto& sun = sky->skyColor[RE::TESWeather::ColorTypes::kSunlight];
		light = float3(amb.red + sun.red * 0.5f, amb.green + sun.green * 0.5f, amb.blue + sun.blue * 0.5f);
	}
	cb.LightColor = float4(light.x, light.y, light.z, settings.CollisionThickness);
	cb.Flags = float4(settings.Collisions ? 1.0f : 0.0f, settings.SkyOcclusion ? 1.0f : 0.0f, settings.SoftDepth, settings.SpawnHeight);
	cb.ExtraP = float4(settings.bf.SparkRestTime, settings.bf.IceRestTime, std::min(settings.bf.LeafGroundMin, settings.bf.LeafGroundMax), std::max(settings.bf.LeafGroundMin, settings.bf.LeafGroundMax));
	{
		const auto& cx = Bearforj::Context::Get();
		cb.ContextA = float4((cx.loc & Bearforj::Context::kLocInterior) ? 1.0f : 0.0f, cx.trees, cx.conifers, cx.cold);
		cb.ContextB = float4(cx.hour, static_cast<float>(cx.loc), static_cast<float>(cx.weatherSlot), cx.night ? 1.0f : 0.0f);
	}
	cb.ExtraP2 = float4(std::clamp(settings.bf.SizeVariation, 0.0f, 0.95f), settings.bf.LeafGlide, settings.bf.LeafGustLift ? 1.0f : 0.0f, std::max(settings.bf.FadeDistance, 500.0f));
	globals::features::nativeParticlePhysics.RefreshInfluencers();
	for (uint32_t i = 0; i < NativeParticlePhysics::kInfluencerCount; ++i) {
		const auto& inf = globals::features::nativeParticlePhysics.influencers[i];
		cb.InfluencerPos[i] = float4(inf.pos[0], inf.pos[1], inf.pos[2], settings.bf.ReactToActors ? inf.pos[3] : 0.0f);
		cb.InfluencerVel[i] = float4(inf.vel[0], inf.vel[1], inf.vel[2], settings.bf.ReactToActors ? inf.vel[3] : 0.0f);
	}
	cb.ReactMask = settings.bf.ReactMask;
	LoadSpriteAtlas();
	cb.ReactPad[0] = spriteAtlas ? 1u : 0u;
	cb.ReactPad[1] = settings.bf.GroundStains ? 1u : 0u;
	UpdateAmbient(dt, rain, snow, paused, float3(camAdjust.x, camAdjust.y, camAdjust.z));

	// Wind drift (CPU-cheap: a few dust events per frame near the camera, GPU does the rest).
	if (settings.bf.WindDrift && !paused && !Util::IsInterior() && windStrength > 0.25f && rain < 0.5f) {
		static float accum = 0.0f;
		accum += dt * settings.bf.WindDriftDensity * 12.0f * std::clamp((windStrength - 0.25f) * 2.0f, 0.0f, 1.0f);
		const bool snowy = snow > 0.05f || Bearforj::WeatherPresets::CurrentSlot() == Bearforj::WeatherPresets::kSnow || Bearforj::WeatherPresets::CurrentSlot() == Bearforj::WeatherPresets::kBlizzard;
		while (accum >= 1.0f) {
			accum -= 1.0f;
			static uint32_t rng = 1u;
			auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; };
			const float a = rnd() * 6.2831853f, r = 300.0f + rnd() * 1200.0f;
			const bool high = snowy && gust > 0.4f && rnd() < 0.3f;  // gust: snow dust blown off roofs / cliffs / branches
			EmitEvent e{};
			e.position = float3(camAdjust.x + std::cos(a) * r, camAdjust.y + std::sin(a) * r, camAdjust.z - 110.0f + (high ? 250.0f + rnd() * 400.0f : rnd() * 20.0f));
			const auto slot = Bearforj::WeatherPresets::CurrentSlot();
			const uint32_t driftSprite = snowy ? (slot == Bearforj::WeatherPresets::kAsh ? kSprAsh : kSprSnow) : kSprDust;
			e.kind = WithSprite(kDust, driftSprite);
			const float wl = std::max(std::sqrt(wind.x * wind.x + wind.y * wind.y), 1.0f);
			e.direction = float3(wind.x / wl, wind.y / wl, high ? -0.3f : 0.05f);
			e.count = high ? 10u : 6u;
			e.params = float4(wl * 0.8f, 0.5f, settings.DustSize * (snowy ? 1.4f : 1.0f), settings.DustLifetime * 1.6f);
			e.color = snowy ? float4(0.95f, 0.97f, 1.0f, 1.0f) : float4(0.55f, 0.47f, 0.36f, 1.0f);
			Emit(e);
		}
	}
	frameCB->Update(cb);

	std::vector<EmitEvent> events;
	{
		std::lock_guard lock(eventLock);
		events.swap(pendingEvents);
	}

	// Save the state we touch.
	winrt::com_ptr<ID3D11RenderTargetView> oldRTV;
	winrt::com_ptr<ID3D11DepthStencilView> oldDSV;
	context->OMGetRenderTargets(1, oldRTV.put(), oldDSV.put());
	context->OMSetRenderTargets(0, nullptr, nullptr);

	ID3D11Buffer* frameBuffer = frameCB->CB();
	ID3D11UnorderedAccessView* uavs[2] = { particleUAV.get(), counterUAV.get() };

	if (!events.empty() && !paused) {
		CS_GPU_PASS("SSPP::Emit");
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(eventBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, events.data(), sizeof(EmitEvent) * events.size());
			context->Unmap(eventBuffer.get(), 0);
			emitCB->Update(CBUInt4{ static_cast<uint32_t>(events.size()), frame, 0u, 0u });
			ID3D11Buffer* cbs[2] = { frameBuffer, emitCB->CB() };
			context->CSSetConstantBuffers(0, 2, cbs);
			ID3D11ShaderResourceView* srv = eventSRV.get();
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			context->CSSetShader(emitCS.get(), nullptr, 0);
			context->Dispatch(static_cast<UINT>(events.size()), 1, 1);
		}
	}
	{
		CS_GPU_PASS("SSPP::Simulate");
		globals::state->BindSharedDataCS(context);
		context->CSSetConstantBuffers(0, 1, &frameBuffer);
		auto& skylighting = globals::features::skylighting;
		ID3D11ShaderResourceView* srvs[3] = { nullptr, depthSRV,
			(skylighting.loaded && skylighting.texProbeArray) ? skylighting.texProbeArray->srv.get() : nullptr };
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		context->CSSetShader(simulateCS.get(), nullptr, 0);
		context->Dispatch((cb.Ranges[3] + 255) / 256, 1, 1);
		ID3D11ShaderResourceView* nullSRV[3] = {};
		ID3D11UnorderedAccessView* nullUAV[2] = {};
		context->CSSetShaderResources(0, 3, nullSRV);
		context->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	{
		CS_GPU_PASS("SSPP::Draw");
		// Linear Lighting's scene decode is a full-target UAV write. It is registered later in
		// the feature list, so it used to run after this draw and erase every particle. Decode
		// first (no-op when the scene is already linear), then composite. The LL colour decode
		// in SSPPDraw.hlsl stays: these sprites are authored in gamma and the target is linear.
		if (globals::features::linearLighting.loaded)
			globals::features::linearLighting.OnBeforePostProcessing(a_renderTarget);
		D3D11_VIEWPORT vp{ 0.0f, 0.0f, dynSize.x, dynSize.y, 0.0f, 1.0f };
		context->RSSetViewports(1, &vp);
		context->RSSetState(rasterState.get());
		context->OMSetRenderTargets(1, &rtv, nullptr);
		const float blendFactor[4] = { 0, 0, 0, 0 };
		context->OMSetBlendState(blendState.get(), blendFactor, 0xFFFFFFFF);
		context->OMSetDepthStencilState(depthState.get(), 0);
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(drawVS.get(), nullptr, 0);
		context->PSSetShader(drawPS.get(), nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->VSSetConstantBuffers(0, 1, &frameBuffer);
		context->PSSetConstantBuffers(0, 1, &frameBuffer);
		ID3D11ShaderResourceView* vsSrvs[2] = { particleSRV.get(), depthSRV };
		ID3D11ShaderResourceView* psSrvs[3] = { particleSRV.get(), depthSRV, spriteAtlas.get() };
		context->VSSetShaderResources(0, 2, vsSrvs);
		context->PSSetShaderResources(0, 3, psSrvs);
		ID3D11SamplerState* samp = spriteSampler.get();
		ID3D11SamplerState* oldSamp = nullptr;
		context->PSGetSamplers(0, 1, &oldSamp);
		context->PSSetSamplers(0, 1, &samp);
		context->Draw(cb.Ranges[3] * 6, 0);
		ID3D11ShaderResourceView* nullVS[2] = {};
		ID3D11ShaderResourceView* nullPS[3] = {};
		context->VSSetShaderResources(0, 2, nullVS);
		context->PSSetShaderResources(0, 3, nullPS);
		context->PSSetSamplers(0, 1, &oldSamp);
		if (oldSamp)
			oldSamp->Release();
	}

	ID3D11RenderTargetView* restoreRTV = oldRTV.get();
	context->OMSetRenderTargets(1, &restoreRTV, oldDSV.get());
	// The game tracks its own pipeline state; tell it everything we touched is dirty.
	auto& flags = globals::game::shadowState->GetRuntimeData().stateUpdateFlags;
	flags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET, RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT,
		RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE, RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE,
		RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND, RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO,
		RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void ScreenSpacePhysicalParticles::DrawSettings()
{
	BFUI::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	BFUI::Checkbox(T(TKEY("rain"), "Rain"), &settings.Rain);
	ImGui::SameLine();
	BFUI::Checkbox(T(TKEY("snow"), "Snow"), &settings.Snow);
	ImGui::SameLine();
	BFUI::Checkbox(T(TKEY("leaves"), "Leaves"), &settings.Leaves);
	BFUI::Checkbox(T(TKEY("drizzle"), "Drizzle / mist in fog"), &settings.Drizzle);
	BFUI::Checkbox(T(TKEY("ash"), "Ash fall in ash weathers"), &settings.Ash);
	BFUI::Checkbox(T(TKEY("magic"), "Magic particles (fire embers, frost shards, shock sparks)"), &settings.bf.MagicParticles);
	BFUI::Checkbox(T(TKEY("school_magic"), "Restoration, Alteration, Illusion, Conjuration"), &settings.bf.SchoolMagic);
	BFUI::Checkbox(T(TKEY("shout_particles"), "Shout wave"), &settings.bf.ShoutParticles);
	BFUI::Checkbox(T(TKEY("ward_impact"), "Ward shatter and ripple"), &settings.bf.WardImpact);
	BFUI::Checkbox(T(TKEY("pollen"), "Pollen from grass"), &settings.bf.Pollen);
	BFUI::Checkbox(T(TKEY("insects"), "Insects (fireflies, butterflies, gnats, flies, lamps)"), &settings.bf.Insects);
	BFUI::Checkbox(T(TKEY("water_drips"), "Water drips off a wet character"), &settings.bf.WaterDrips);
	BFUI::Checkbox(T(TKEY("ground_stains"), "Blood and water floor stains"), &settings.bf.GroundStains);
	BFUI::Checkbox(T(TKEY("fire_sparks"), "Sparks from fires and forges"), &settings.bf.FireSparks);
	if (settings.bf.FireSparks)
		BFUI::SliderFloat(T(TKEY("fire_spark_rate"), "Fire spark bursts per second"), &settings.bf.FireSparkRate, 0.1f, 20.0f, "%.1f");
	BFUI::Checkbox(T(TKEY("wind_drift"), "Wind drift (blowing snow / dust)"), &settings.bf.WindDrift);
	BFUI::Checkbox(T(TKEY("leaf_gust"), "Gusts lift resting particles"), &settings.bf.LeafGustLift);
	BFUI::Checkbox(T(TKEY("react_actors"), "Actors, creatures, spells and shouts push particles"), &settings.bf.ReactToActors);
	if (ImGui::TreeNodeEx(T(TKEY("react_section"), "Which particles react"))) {
		auto bits = [&](const char* a_label, uint32_t a_mask) {
			bool on = (settings.bf.ReactMask & a_mask) == a_mask;
			if (BFUI::Checkbox(a_label, &on)) {
				if (on)
					settings.bf.ReactMask |= a_mask;
				else
					settings.bf.ReactMask &= ~a_mask;
			}
		};
		auto bit = [&](const char* a_label, Kind a_kind) { bits(a_label, 1u << a_kind); };
		const uint32_t insectBits = (1u << kFirefly) | (1u << kButterfly) | (1u << kGnat) | (1u << kFly) | (1u << kLampInsect);
		const uint32_t schoolBits = (1u << kRestoration) | (1u << kAlteration) | (1u << kIllusion) | (1u << kConjuration);
		const uint32_t wardBits = (1u << kWardShard) | (1u << kWardRipple);
		const uint32_t stainBits = (1u << kBloodStain) | (1u << kWaterStain);
		bit(T(TKEY("react_rain"), "Rain"), kRain);
		ImGui::SameLine();
		bit(T(TKEY("react_snow"), "Snow and ash"), kSnow);
		ImGui::SameLine();
		bit(T(TKEY("react_leaf"), "Leaves"), kLeaf);
		bit(T(TKEY("react_needle"), "Needles"), kNeedle);
		ImGui::SameLine();
		bit(T(TKEY("react_twig"), "Twigs"), kTwig);
		bit(T(TKEY("react_pollen"), "Pollen"), kPollen);
		ImGui::SameLine();
		bits(T(TKEY("react_insect"), "Insects"), insectBits);
		bit(T(TKEY("react_dust"), "Dust, sand and debris"), kDust);
		ImGui::SameLine();
		bit(T(TKEY("react_spark"), "Sparks"), kSpark);
		ImGui::SameLine();
		bit(T(TKEY("react_ember"), "Embers"), kEmber);
		bit(T(TKEY("react_ice"), "Ice crumbs"), kIce);
		ImGui::SameLine();
		bit(T(TKEY("react_blood"), "Blood"), kBlood);
		ImGui::SameLine();
		bit(T(TKEY("react_splash"), "Splashes"), kSplash);
		bits(T(TKEY("react_school"), "School magic"), schoolBits);
		ImGui::SameLine();
		bit(T(TKEY("react_shout"), "Shouts"), kShout);
		ImGui::SameLine();
		bits(T(TKEY("react_ward"), "Ward shatter and ripple"), wardBits);
		bits(T(TKEY("react_stain"), "Floor stains"), stainBits);
		ImGui::SameLine();
		bit(T(TKEY("react_drip"), "Water drips"), kWaterDrip);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("bearforj_section"), "Bearforj: lifetimes, density, fade"))) {
		BFUI::SliderFloat(T(TKEY("leaf_ground_min"), "Leaf rest on ground min"), &settings.bf.LeafGroundMin, 0.0f, 60.0f, "%.0f s");
		BFUI::SliderFloat(T(TKEY("leaf_ground_max"), "Leaf rest on ground max"), &settings.bf.LeafGroundMax, 0.0f, 90.0f, "%.0f s");
		BFUI::SliderFloat(T(TKEY("leaf_glide"), "Leaf glide / flutter"), &settings.bf.LeafGlide, 0.25f, 3.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("spark_rest"), "Spark / ember cooling on ground"), &settings.bf.SparkRestTime, 0.0f, 15.0f, "%.1f s");
		BFUI::SliderFloat(T(TKEY("ice_rest"), "Ice shard melt time"), &settings.bf.IceRestTime, 0.0f, 20.0f, "%.1f s");
		BFUI::SliderInt(T(TKEY("magic_count"), "Magic particles per hit"), reinterpret_cast<int*>(&settings.bf.MagicCount), 0, 256);
		BFUI::SliderFloat(T(TKEY("magic_life"), "Magic particle lifetime"), &settings.bf.MagicLifetime, 0.5f, 10.0f, "%.1f s");
		BFUI::SliderFloat(T(TKEY("magic_dist"), "Magic particles max distance"), &settings.bf.MagicMaxDistance, 500.0f, 8000.0f, "%.0f");
		BFUI::SliderFloat(T(TKEY("wind_drift_density"), "Wind drift density"), &settings.bf.WindDriftDensity, 0.0f, 2.0f, "%.2f");
		BFUI::Checkbox(T(TKEY("suppress_vanilla"), "Replace weather-mod rain/snow (off = keep theirs, SSPP rain/snow off)"), &settings.bf.SuppressVanilla);
		BFUI::SliderFloat(T(TKEY("global_density"), "Global density"), &settings.bf.GlobalDensity, 0.0f, 2.0f, "%.2f");
		if (BFUI::SliderScalar(T(TKEY("budget"), "Particle budget (all pools)"), ImGuiDataType_U32, &settings.bf.ParticleBudget, &(const uint32_t&)16384u, &(const uint32_t&)400000u))
			poolDirty = true;
		BFUI::SliderFloat(T(TKEY("size_var"), "Size variation"), &settings.bf.SizeVariation, 0.0f, 0.95f, "%.2f");
		BFUI::SliderFloat(T(TKEY("fade_dist"), "Fade-out distance"), &settings.bf.FadeDistance, 500.0f, 20000.0f, "%.0f");
		ImGui::TreePop();
	}
	BFUI::Checkbox(T(TKEY("sparks"), "Sparks"), &settings.Sparks);
	ImGui::SameLine();
	BFUI::Checkbox(T(TKEY("blood"), "Blood"), &settings.Blood);
	ImGui::SameLine();
	BFUI::Checkbox(T(TKEY("footsteps"), "Footstep Dust/Snow"), &settings.FootstepParticles);
	BFUI::Checkbox(T(TKEY("collisions"), "Collisions"), &settings.Collisions);
	ImGui::SameLine();
	BFUI::Checkbox(T(TKEY("sky_occlusion"), "No Rain Under Roofs"), &settings.SkyOcclusion);

	if (ImGui::TreeNodeEx(T(TKEY("pool"), "Particle Budget"))) {
		bool changed = false;
		changed |= BFUI::SliderScalar(T(TKEY("rain_count"), "Rain Drops"), ImGuiDataType_U32, &settings.RainCount, &(const uint32_t&)0u, &kMaxRain);
		changed |= BFUI::SliderScalar(T(TKEY("snow_count"), "Snow Flakes"), ImGuiDataType_U32, &settings.SnowCount, &(const uint32_t&)0u, &kMaxSnow);
		changed |= BFUI::SliderScalar(T(TKEY("leaf_count"), "Leaves"), ImGuiDataType_U32, &settings.LeafCount, &(const uint32_t&)0u, &kMaxLeaves);
		changed |= BFUI::SliderScalar(T(TKEY("dynamic_count"), "Effect Particles"), ImGuiDataType_U32, &settings.DynamicCount, &(const uint32_t&)1024u, &kMaxDynamic);
		poolDirty |= changed;
		BFUI::SliderFloat(T(TKEY("spawn_radius"), "Spawn Radius"), &settings.SpawnRadius, 300.0f, 5000.0f, "%.0f");
		BFUI::SliderFloat(T(TKEY("spawn_height"), "Spawn Height"), &settings.SpawnHeight, 300.0f, 4000.0f, "%.0f");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("rain_section"), "Rain"))) {
		BFUI::SliderFloat(T(TKEY("rain_width"), "Width"), &settings.RainWidth, 0.1f, 2.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("rain_length"), "Streak Length"), &settings.RainLength, 0.2f, 3.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("rain_brightness"), "Brightness"), &settings.RainBrightness, 0.0f, 8.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("rain_speed"), "Fall Speed"), &settings.RainSpeed, 200.0f, 2000.0f, "%.0f");
		BFUI::SliderFloat(T(TKEY("splash_chance"), "Splash Chance"), &settings.SplashChance, 0.0f, 1.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("splash_size"), "Splash Size"), &settings.SplashSize, 0.2f, 5.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("splash_brightness"), "Splash Brightness"), &settings.SplashBrightness, 0.0f, 8.0f, "%.2f");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("snow_section"), "Snow"))) {
		BFUI::SliderFloat(T(TKEY("snow_size"), "Size"), &settings.SnowSize, 0.5f, 8.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("snow_brightness"), "Brightness"), &settings.SnowBrightness, 0.0f, 8.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("snow_speed"), "Fall Speed"), &settings.SnowSpeed, 20.0f, 400.0f, "%.0f");
		BFUI::SliderFloat(T(TKEY("snow_settle"), "Settled Time"), &settings.SnowSettleTime, 0.0f, 60.0f, "%.1f s");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("leaf_section"), "Leaves"))) {
		BFUI::SliderFloat(T(TKEY("leaf_amount"), "Amount"), &settings.LeafAmount, 0.0f, 1.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("leaf_autumn"), "Autumn/forest boost"), &settings.LeafAutumnBoost, 1.0f, 4.0f, "%.1fx");
		BFUI::SliderFloat(T(TKEY("leaf_size"), "Size"), &settings.LeafSize, 1.0f, 15.0f, "%.1f");
		BFUI::SliderFloat(T(TKEY("leaf_brightness"), "Brightness"), &settings.LeafBrightness, 0.0f, 4.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("leaf_settle"), "Settled Time"), &settings.LeafSettleTime, 0.0f, 120.0f, "%.0f s");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("effects_section"), "Sparks, Blood, Footsteps"))) {
		BFUI::SliderFloat(T(TKEY("spark_life"), "Spark Lifetime"), &settings.SparkLifetime, 0.2f, 8.0f, "%.1f s");
		BFUI::SliderFloat(T(TKEY("spark_brightness"), "Spark Brightness"), &settings.SparkBrightness, 0.0f, 40.0f, "%.1f");
		BFUI::SliderFloat(T(TKEY("spark_bounce"), "Spark Bounce"), &settings.SparkBounce, 0.0f, 1.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("blood_size"), "Blood Drop Size"), &settings.BloodSize, 0.2f, 5.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("blood_stick"), "Blood Stick Time"), &settings.BloodStickTime, 0.0f, 60.0f, "%.0f s");
		BFUI::SliderFloat(T(TKEY("dust_size"), "Footstep Puff Size"), &settings.DustSize, 1.0f, 20.0f, "%.1f");
		BFUI::SliderFloat(T(TKEY("dust_life"), "Footstep Puff Lifetime"), &settings.DustLifetime, 0.2f, 5.0f, "%.1f s");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("advanced"), "Advanced"))) {
		BFUI::SliderFloat(T(TKEY("wind_scale"), "Wind Scale"), &settings.WindScale, 0.0f, 3.0f, "%.2f");
		BFUI::SliderFloat(T(TKEY("thickness"), "Collision Thickness"), &settings.CollisionThickness, 5.0f, 300.0f, "%.0f");
		BFUI::SliderFloat(T(TKEY("soft_depth"), "Soft Depth"), &settings.SoftDepth, 0.1f, 50.0f, "%.1f");
		if (ImGui::Button(T(TKEY("clear"), "Clear Particles")))
			clearPending = true;
		ImGui::TreePop();
	}
}

void ScreenSpacePhysicalParticles::LoadSettings(json& o_json)
{
	settings = o_json;
	constexpr uint32_t kOldReact = (1u << 9) - 1u;
	if (settings.bf.ReactMask == kOldReact)
		settings.bf.ReactMask = (1u << kKindCount) - 1u;
	poolDirty = true;
}

void ScreenSpacePhysicalParticles::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpacePhysicalParticles::RestoreDefaultSettings()
{
	settings = {};
	poolDirty = true;
}
