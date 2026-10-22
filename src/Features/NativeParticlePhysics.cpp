#include "NativeParticlePhysics.h"

#include "Features/SubmersionWetness.h"
#include "Globals.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cctype>
#include <cmath>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeParticlePhysics::CategorySettings,
	Enabled, GroundCollision, WaterCollision, DieInWater, Bounce, Friction, Gravity, Drag, Wind, ActorPush)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeParticlePhysics::Settings,
	Enabled, WindSpeedScale, PushRadius, PushStrength, MaxDistance, Categories)

std::array<NativeParticlePhysics::CategorySettings, NativeParticlePhysics::kCount> NativeParticlePhysics::DefaultCategories()
{
	std::array<CategorySettings, kCount> c{};
	//            En    Ground Water  Die    Bounce Fric  Grav    Drag  Wind  Push
	c[kMagic] = { true, true, true, false, 0.35f, 0.3f, 0.0f, 0.0f, 0.15f, 0.6f };
	c[kFire] = { true, false, true, true, 0.0f, 0.0f, 0.0f, 0.2f, 0.5f, 0.5f };
	c[kSparks] = { true, true, true, true, 0.45f, 0.5f, 400.0f, 0.3f, 0.2f, 0.3f };
	c[kSmoke] = { true, true, true, false, 0.0f, 0.6f, 0.0f, 0.4f, 0.8f, 1.0f };
	c[kSnow] = { true, true, true, true, 0.0f, 0.9f, 0.0f, 0.2f, 0.7f, 0.8f };
	c[kRain] = { true, true, true, true, 0.1f, 0.8f, 0.0f, 0.0f, 0.3f, 0.2f };
	c[kDust] = { true, true, true, true, 0.1f, 0.7f, 0.0f, 0.5f, 0.8f, 1.0f };
	c[kWater] = { true, true, false, false, 0.2f, 0.5f, 0.0f, 0.0f, 0.2f, 0.4f };
	c[kOther] = { false, true, true, false, 0.25f, 0.4f, 0.0f, 0.0f, 0.2f, 0.5f };
	return c;
}

const char* NativeParticlePhysics::CategoryName(uint32_t a_category)
{
	switch (a_category) {
	case kMagic: return T("feature.native_particle_physics.cat_magic", "Magic");
	case kFire: return T("feature.native_particle_physics.cat_fire", "Fire");
	case kSparks: return T("feature.native_particle_physics.cat_sparks", "Sparks and Embers");
	case kSmoke: return T("feature.native_particle_physics.cat_smoke", "Smoke, Steam and Fog");
	case kSnow: return T("feature.native_particle_physics.cat_snow", "Snow");
	case kRain: return T("feature.native_particle_physics.cat_rain", "Rain and Drips");
	case kDust: return T("feature.native_particle_physics.cat_dust", "Dust, Dirt and Debris");
	case kWater: return T("feature.native_particle_physics.cat_water", "Water Splashes and Mist");
	default: return T("feature.native_particle_physics.cat_other", "Other");
	}
}

void NativeParticlePhysics::LoadSettings(json& o_json) { settings = o_json; }
void NativeParticlePhysics::SaveSettings(json& o_json) { o_json = settings; }
void NativeParticlePhysics::RestoreDefaultSettings() { settings = {}; }

void NativeParticlePhysics::PostPostLoad()
{
	// NiParticleSystem::UpdateDownwardPass is the NiAVObject slot. CommonLibSSE-NG (8ffc2b74) marks it // 2C
	// on SE/AE. VR inserts ApplyLocalTransformToWorld after UpdateControllers (// 25), so the slot is 0x2D there.
	// VTABLE_NiParticleSystem[0] is Address Library SE 287258 / AE 237888 (Offsets_VTABLE.h). NiParticleSystem::AddModifier
	// is the sibling check RELOCATION_ID(72799, 74499).
	if (REL::Module::IsVR())
		stl::write_vfunc<0x2D, Hooks::NiParticleSystem_UpdateDownwardPass>(RE::VTABLE_NiParticleSystem[0]);
	else
		stl::write_vfunc<0x2C, Hooks::NiParticleSystem_UpdateDownwardPass>(RE::VTABLE_NiParticleSystem[0]);
	logger::info("[Native Particle Physics] Installed NiParticleSystem::UpdateDownwardPass (SE/AE slot 0x2C, vtable SE 287258 / AE 237888)");
}

void NativeParticlePhysics::DrawSettings()
{
	ImGui::Checkbox(T("feature.native_particle_physics.enabled", "Enable"), &settings.Enabled);
	ImGui::SliderFloat(T("feature.native_particle_physics.wind_scale", "Wind Strength"), &settings.WindSpeedScale, 0.0f, 3.0f, "%.2f");
	ImGui::SliderFloat(T("feature.native_particle_physics.push_radius", "Actor Push Radius"), &settings.PushRadius, 10.0f, 200.0f, "%.0f");
	ImGui::SliderFloat(T("feature.native_particle_physics.push_strength", "Actor Push Strength"), &settings.PushStrength, 0.0f, 1000.0f, "%.0f");
	ImGui::SliderFloat(T("feature.native_particle_physics.distance", "Max Distance"), &settings.MaxDistance, 1000.0f, 20000.0f, "%.0f");
	for (uint32_t i = 0; i < kCount; i++) {
		auto& c = settings.Categories[i];
		ImGui::PushID((int)i);
		if (ImGui::TreeNode(CategoryName(i))) {
			ImGui::Checkbox(T("feature.native_particle_physics.cat_enabled", "Enable"), &c.Enabled);
			ImGui::Checkbox(T("feature.native_particle_physics.ground", "Ground Collision"), &c.GroundCollision);
			ImGui::SameLine();
			ImGui::Checkbox(T("feature.native_particle_physics.water", "Water Collision"), &c.WaterCollision);
			ImGui::Checkbox(T("feature.native_particle_physics.die_water", "Dies in Water"), &c.DieInWater);
			ImGui::SliderFloat(T("feature.native_particle_physics.bounce", "Bounce"), &c.Bounce, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat(T("feature.native_particle_physics.friction", "Friction"), &c.Friction, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat(T("feature.native_particle_physics.gravity", "Extra Gravity"), &c.Gravity, -400.0f, 1200.0f, "%.0f");
			ImGui::SliderFloat(T("feature.native_particle_physics.drag", "Air Drag"), &c.Drag, 0.0f, 3.0f, "%.2f");
			ImGui::SliderFloat(T("feature.native_particle_physics.wind", "Wind Influence"), &c.Wind, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat(T("feature.native_particle_physics.push", "Actor Push"), &c.ActorPush, 0.0f, 2.0f, "%.2f");
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

static bool HasAny(const std::string& s, std::initializer_list<const char*> keys)
{
	for (auto k : keys)
		if (s.find(k) != std::string::npos)
			return true;
	return false;
}

uint32_t NativeParticlePhysics::Classify(RE::NiParticleSystem* a_system) const
{
	std::string key;
	if (a_system->name.c_str())
		key += a_system->name.c_str();
	key += '|';
	auto& geom = a_system->GetGeometryRuntimeData();
	if (auto* prop = geom.shaderProperty.get()) {
		if (auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(prop)) {
			if (auto* mat = static_cast<RE::BSEffectShaderMaterial*>(effect->material)) {
				if (mat->sourceTexturePath.c_str())
					key += mat->sourceTexturePath.c_str();
				key += '|';
				if (mat->greyscaleTexturePath.c_str())
					key += mat->greyscaleTexturePath.c_str();
			}
		}
	}
	for (auto* p = a_system->parent; p && key.size() < 512; p = p->parent) {
		if (p->name.c_str())
			(key += '|') += p->name.c_str();
	}
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });

	if (HasAny(key, { "spark", "ember", "cinder" }))
		return kSparks;
	if (HasAny(key, { "smoke", "steam", "fog", "vapor", "mist" }))
		return HasAny(key, { "water", "splash" }) ? kWater : kSmoke;
	if (HasAny(key, { "fire", "flame", "torch", "candle", "brazier" }))
		return kFire;
	if (HasAny(key, { "snow", "blizzard", "ash" }))
		return kSnow;
	if (HasAny(key, { "rain", "drip", "droplet" }))
		return kRain;
	if (HasAny(key, { "dust", "dirt", "sand", "debris", "rock", "gravel", "mud" }))
		return kDust;
	if (HasAny(key, { "water", "splash", "waterfall", "foam" }))
		return kWater;
	if (HasAny(key, { "magic", "spell", "frost", "shock", "lightning", "ward", "rune", "aura", "soul", "dragonsoul", "shout", "enchant" }))
		return kMagic;
	return kOther;
}

void NativeParticlePhysics::Prepass()
{
	const auto* sky = RE::Sky::GetSingleton();
	windVelocity = {};
	if (sky && sky->currentWeather) {
		const auto& d = sky->currentWeather->data;
		const float speed = (d.windSpeed / 255.0f) * 600.0f * settings.WindSpeedScale;
		const float dir = (float(d.windDirection) / 256.0f) * 2.0f * RE::NI_PI;
		windVelocity = { std::sin(dir) * speed, std::cos(dir) * speed, 0.0f };
	}
	if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->Is3DLoaded()) {
		playerPos = player->GetPosition();
		playerHeight = std::max(player->GetHeight(), 64.0f);
		playerValid = true;
	} else {
		playerValid = false;
	}
	std::scoped_lock l(lock);
	const uint32_t frame = globals::state->frameCount;
	if (systems.size() > 4096)
		std::erase_if(systems, [frame](const auto& e) { return frame - e.second.frame > 300; });
}

void NativeParticlePhysics::Simulate(RE::NiParticleSystem* a_system)
{
	auto& prt = a_system->GetParticlesRuntimeData();
	auto* data = prt.particleData.get();
	if (!data)
		return;
	auto& pd = data->GetParticlesRuntimeData();
	auto& sd = data->GetPSysRuntimeData();
	const uint16_t n = pd.numVertices;
	if (!n || !pd.positions || !sd.particleInfo)
		return;

	const auto& world = a_system->world;
	if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
		if (camera->cameraRoot->world.translate.GetDistance(world.translate) > settings.MaxDistance)
			return;
	}

	SystemInfo info;
	{
		const uint32_t frame = globals::state->frameCount;
		std::scoped_lock l(lock);
		auto [it, inserted] = systems.try_emplace(a_system);
		if (inserted || frame - it->second.frame > 60) {
			it->second.category = Classify(a_system);
			float h = -1.0e9f;
			if (auto* tes = RE::TES::GetSingleton()) {
				RE::NiPoint3 probe = world.translate;
				probe.z += 200.0f;
				if (!tes->GetLandHeight(probe, h))
					h = -1.0e9f;
			}
			it->second.groundZ = h;
			it->second.waterZ = -1.0e9f;
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const float w = SubmersionWetness::WaterHeightAt(player, world.translate);
				if (w != -RE::NI_INFINITY && w > -1.0e8f)
					it->second.waterZ = w;
			}
			it->second.frame = frame;
		}
		info = it->second;
	}

	const auto& c = settings.Categories[info.category];
	if (!c.Enabled)
		return;

	const float dt = std::clamp(*globals::game::deltaTime, 0.0f, 0.1f);
	if (dt <= 0.0f)
		return;

	const bool worldSpace = a_system->GetParticleSystemRuntimeData().isWorldspace;
	const RE::NiMatrix3 R = world.rotate;
	const RE::NiMatrix3 Rt = R.Transpose();
	const float s = world.scale > 1.0e-4f ? world.scale : 1.0f;

	const RE::NiPoint3 wind = windVelocity * c.Wind;
	const float drag = std::clamp(c.Drag * dt, 0.0f, 1.0f);
	const float windBlend = std::clamp(c.Wind * dt * 2.0f, 0.0f, 1.0f);
	const float pushR = settings.PushRadius;
	const float pushK = settings.PushStrength * c.ActorPush;

	for (uint16_t i = 0; i < n; i++) {
		auto& pInfo = sd.particleInfo[i];
		RE::NiPoint3 pos = pd.positions[i];
		RE::NiPoint3 vel = pInfo.velocity;
		if (!worldSpace) {
			pos = R * (pos * s) + world.translate;
			vel = R * (vel * s);
		}
		const float radius = pd.radii ? pd.radii[i] * (pd.sizes ? pd.sizes[i] : 1.0f) * s * 0.5f : 2.0f;

		// forces
		vel.z -= c.Gravity * dt;
		vel = vel * (1.0f - drag);
		if (windBlend > 0.0f) {
			vel.x += (wind.x - vel.x) * windBlend;
			vel.y += (wind.y - vel.y) * windBlend;
		}

		// player push (vertical capsule)
		if (playerValid && pushK > 0.0f && pos.z > playerPos.z - 10.0f && pos.z < playerPos.z + playerHeight) {
			const float dx = pos.x - playerPos.x, dy = pos.y - playerPos.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 < pushR * pushR && d2 > 1.0e-4f) {
				const float d = std::sqrt(d2);
				const float k = pushK * (1.0f - d / pushR) * dt;
				vel.x += dx / d * k;
				vel.y += dy / d * k;
			}
		}

		// water
		if (info.waterZ > -1.0e8f && pos.z < info.waterZ) {
			if (c.DieInWater) {
				pInfo.age = std::max(pInfo.age, pInfo.lifespan);
			} else if (c.WaterCollision) {
				pos.z = info.waterZ + radius * 0.25f;
				if (vel.z < 0.0f)
					vel.z = -vel.z * c.Bounce * 0.5f;
				vel.x *= 1.0f - c.Friction * 0.5f;
				vel.y *= 1.0f - c.Friction * 0.5f;
			}
		}

		// ground (land height under the effect, treated as a plane)
		if (c.GroundCollision && info.groundZ > -1.0e8f && pos.z < info.groundZ + radius) {
			pos.z = info.groundZ + radius;
			if (vel.z < 0.0f)
				vel.z = -vel.z * c.Bounce;
			vel.x *= 1.0f - c.Friction;
			vel.y *= 1.0f - c.Friction;
		}

		if (!worldSpace) {
			pos = Rt * (pos - world.translate) / s;
			vel = Rt * vel / s;
		}
		pd.positions[i] = pos;
		pInfo.velocity = vel;
	}
}

void NativeParticlePhysics::Hooks::NiParticleSystem_UpdateDownwardPass::thunk(RE::NiParticleSystem* a_this, RE::NiUpdateData& a_data, std::uint32_t a_arg2)
{
	func(a_this, a_data, a_arg2);
	auto& f = globals::features::nativeParticlePhysics;
	if (a_this && f.loaded && f.settings.Enabled)
		f.Simulate(a_this);
}
