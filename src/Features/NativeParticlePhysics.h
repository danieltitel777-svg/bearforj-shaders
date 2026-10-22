#pragma once

#include "I18n/I18n.h"

#include <array>
#include <mutex>
#include <unordered_map>

/**
 * @brief BearForge: physics for the game's own particle systems (NiParticleSystem):
 * ground and water collision, wind, extra gravity/drag and actor push, with per-category settings.
 * Companion of Screen Space Physical Particles (which simulates its own blood and leaves on the GPU).
 */
struct NativeParticlePhysics : Feature
{
	virtual inline std::string GetName() override { return "Native Particle Physics"; }
	virtual inline std::string GetDisplayName() override { return T("feature.native_particle_physics.name", "Native Particle Physics"); }
	virtual inline std::string GetShortName() override { return "NativeParticlePhysics"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kOther; }
	virtual inline bool SupportsVR() override { return true; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.native_particle_physics.description", "Gives the game's particle effects physics: magic, fire, sparks, smoke, snow, rain, dust and water."),
			{ T("feature.native_particle_physics.key_1", "Particles hit the ground and water, bounce and slide"),
				T("feature.native_particle_physics.key_2", "Wind carries particles, actors push them aside"),
				T("feature.native_particle_physics.key_3", "Separate settings for every particle category") } };
	}

	enum Category : uint32_t
	{
		kMagic,
		kFire,
		kSparks,
		kSmoke,
		kSnow,
		kRain,
		kDust,
		kWater,
		kOther,
		kCount
	};

	struct CategorySettings
	{
		bool Enabled = true;
		bool GroundCollision = true;
		bool WaterCollision = true;
		bool DieInWater = false;
		float Bounce = 0.3f;
		float Friction = 0.4f;
		float Gravity = 0.0f;  // extra, game units/s^2 (down)
		float Drag = 0.0f;     // 1/s
		float Wind = 0.3f;     // 0..1 follow wind
		float ActorPush = 1.0f;
	};

	struct Settings
	{
		bool Enabled = true;
		float WindSpeedScale = 1.0f;
		float PushRadius = 60.0f;
		float PushStrength = 250.0f;
		float MaxDistance = 6000.0f;
		std::array<CategorySettings, kCount> Categories = DefaultCategories();
	} settings;

	static std::array<CategorySettings, kCount> DefaultCategories();
	static const char* CategoryName(uint32_t a_category);

	struct SystemInfo
	{
		uint32_t category = kOther;
		uint32_t frame = 0;
		float groundZ = -1.0e9f;
		float waterZ = -1.0e9f;
	};
	std::unordered_map<RE::NiParticleSystem*, SystemInfo> systems;
	std::mutex lock;

	RE::NiPoint3 windVelocity{};
	RE::NiPoint3 playerPos{};
	float playerHeight = 128.0f;
	bool playerValid = false;

	virtual void PostPostLoad() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	uint32_t Classify(RE::NiParticleSystem* a_system) const;
	void Simulate(RE::NiParticleSystem* a_system);

	struct Hooks
	{
		struct NiParticleSystem_UpdateDownwardPass
		{
			static void thunk(RE::NiParticleSystem* a_this, RE::NiUpdateData& a_data, std::uint32_t a_arg2);
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};
};
