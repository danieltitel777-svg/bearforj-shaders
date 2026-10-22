#include "FootstepKickParticles.h"

#include "Features/SubmersionWetness.h"
#include "Globals.h"
#include "Utils/UI.h"

#include <algorithm>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FootstepKickParticles::Settings,
	Enabled, Snow, Mud, Sand, Dirt, Water, RainMakesMud, PlayerOnly, Scale, WaterScale, MaxShallowDepth, MaxDistance, Lifetime)

void FootstepKickParticles::LoadSettings(json& o_json) { settings = o_json; }
void FootstepKickParticles::SaveSettings(json& o_json) { o_json = settings; }
void FootstepKickParticles::RestoreDefaultSettings() { settings = {}; }

void FootstepKickParticles::DataLoaded()
{
	if (auto* manager = RE::BGSFootstepManager::GetSingleton()) {
		manager->AddEventSink<RE::BGSFootstepEvent>(this);
		logger::info("[Footstep Kick Particles] Registered footstep event sink");
	}
}

void FootstepKickParticles::DrawSettings()
{
	ImGui::Checkbox(T("feature.footstep_kick_particles.enabled", "Enable"), &settings.Enabled);
	ImGui::Checkbox(T("feature.footstep_kick_particles.snow", "Snow"), &settings.Snow);
	ImGui::SameLine();
	ImGui::Checkbox(T("feature.footstep_kick_particles.mud", "Mud"), &settings.Mud);
	ImGui::SameLine();
	ImGui::Checkbox(T("feature.footstep_kick_particles.sand", "Sand"), &settings.Sand);
	ImGui::SameLine();
	ImGui::Checkbox(T("feature.footstep_kick_particles.dirt", "Dry Dirt"), &settings.Dirt);
	ImGui::Checkbox(T("feature.footstep_kick_particles.water", "Puddles and Shallow Water"), &settings.Water);
	ImGui::Checkbox(T("feature.footstep_kick_particles.rain_mud", "Dirt Becomes Mud in Rain"), &settings.RainMakesMud);
	ImGui::Checkbox(T("feature.footstep_kick_particles.player_only", "Player Only"), &settings.PlayerOnly);
	ImGui::SliderFloat(T("feature.footstep_kick_particles.scale", "Particle Size"), &settings.Scale, 0.2f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.footstep_kick_particles.water_scale", "Splash Size"), &settings.WaterScale, 0.2f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.footstep_kick_particles.depth", "Max Shallow Water Depth"), &settings.MaxShallowDepth, 5.0f, 80.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T("feature.footstep_kick_particles.depth_tt", "Deeper than this, the actor is wading or swimming and no footstep splash is made."));
	ImGui::SliderFloat(T("feature.footstep_kick_particles.distance", "Max Distance"), &settings.MaxDistance, 500.0f, 8000.0f, "%.0f");
	ImGui::SliderFloat(T("feature.footstep_kick_particles.lifetime", "Lifetime (s)"), &settings.Lifetime, 0.5f, 4.0f, "%.1f");
}

const char* FootstepKickParticles::ModelFor(Kind a_kind)
{
	switch (a_kind) {
	case Kind::Snow:
		return "BearForge\\FX\\snowkick.nif";
	case Kind::Mud:
		return "BearForge\\FX\\mudkick.nif";
	case Kind::Sand:
		return "BearForge\\FX\\sandkick.nif";
	case Kind::Dirt:
		return "BearForge\\FX\\dirtkick.nif";
	case Kind::Water:
		return "BearForge\\FX\\splash.nif";
	default:
		return nullptr;
	}
}

FootstepKickParticles::Kind FootstepKickParticles::Classify(RE::Actor* a_actor, const RE::NiPoint3& a_foot, float& a_waterDepth) const
{
	a_waterDepth = 0.0f;
	const float waterZ = SubmersionWetness::WaterHeightAt(a_actor, a_foot);
	if (waterZ != -RE::NI_INFINITY && waterZ > -1.0e8f) {
		const float depth = waterZ - a_actor->GetPositionZ();
		if (depth > 0.5f) {
			a_waterDepth = depth;
			return (settings.Water && depth <= settings.MaxShallowDepth) ? Kind::Water : Kind::None;
		}
	}

	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return Kind::None;
	const auto material = tes->GetLandMaterialType(a_foot);
	const auto* sky = RE::Sky::GetSingleton();
	const bool raining = sky && sky->IsRaining() && a_actor->GetParentCell() && !a_actor->GetParentCell()->IsInteriorCell();

	switch (material) {
	case RE::MATERIAL_ID::kWaterPuddle:
		a_waterDepth = 2.0f;
		return settings.Water ? Kind::Water : Kind::None;
	case RE::MATERIAL_ID::kSnow:
	case RE::MATERIAL_ID::kSnowStairs:
		return settings.Snow ? Kind::Snow : Kind::None;
	case RE::MATERIAL_ID::kMud:
		return settings.Mud ? Kind::Mud : Kind::None;
	case RE::MATERIAL_ID::kSand:
		return settings.Sand ? Kind::Sand : Kind::None;
	case RE::MATERIAL_ID::kDirt:
	case RE::MATERIAL_ID::kGravel:
		if (raining && settings.RainMakesMud)
			return settings.Mud ? Kind::Mud : Kind::None;
		return settings.Dirt ? Kind::Dirt : Kind::None;
	default:
		return Kind::None;
	}
}

RE::BSEventNotifyControl FootstepKickParticles::ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*)
{
	if (!a_event || !loaded || !settings.Enabled)
		return RE::BSEventNotifyControl::kContinue;

	auto actorPtr = a_event->actor.get();
	auto* actor = actorPtr.get();
	if (!actor || !actor->Is3DLoaded() || actor->IsInMidair())
		return RE::BSEventNotifyControl::kContinue;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (settings.PlayerOnly && actor != player)
		return RE::BSEventNotifyControl::kContinue;

	auto* cell = actor->GetParentCell();
	if (!cell)
		return RE::BSEventNotifyControl::kContinue;

	if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
		if (camera->cameraRoot->world.translate.GetDistance(actor->GetPosition()) > settings.MaxDistance)
			return RE::BSEventNotifyControl::kContinue;
	}

	// foot bone when the tag names a side, otherwise the actor position
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
	const Kind kind = Classify(actor, foot, waterDepth);
	const char* model = ModelFor(kind);
	if (!model)
		return RE::BSEventNotifyControl::kContinue;

	const float actorScale = std::clamp(actor->GetHeight() / 128.0f, 0.4f, 3.0f);
	float scale = settings.Scale * actorScale;
	RE::NiPoint3 position = foot;
	if (kind == Kind::Water) {
		scale = settings.WaterScale * actorScale * std::clamp(0.6f + waterDepth / 20.0f, 0.6f, 2.0f);
		position.z += waterDepth;  // splash at the surface, not at the feet
	} else if (actor->IsRunning() || actor->IsSprinting()) {
		scale *= 1.3f;
	}

	RE::BSTempEffectParticle::Spawn(cell, settings.Lifetime, model, RE::NiPoint3{}, position, scale, 7, nullptr);
	return RE::BSEventNotifyControl::kContinue;
}
