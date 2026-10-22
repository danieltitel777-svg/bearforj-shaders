#pragma once

#include "I18n/I18n.h"

/** @brief BearForge: particles kicked up by footsteps on snow, mud, sand, dirt, puddles and shallow water. */
struct FootstepKickParticles : Feature, RE::BSTEventSink<RE::BGSFootstepEvent>
{
	virtual inline std::string GetName() override { return "Footstep Kick Particles"; }
	virtual inline std::string GetDisplayName() override { return T("feature.footstep_kick_particles.name", "Footstep Kick Particles"); }
	virtual inline std::string GetShortName() override { return "FootstepKickParticles"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual inline bool SupportsVR() override { return true; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.footstep_kick_particles.description", "Footsteps kick up snow, mud, sand and dirt, and splash in puddles and shallow water."),
			{ T("feature.footstep_kick_particles.key_1", "Surface picked from the ground material under the foot"),
				T("feature.footstep_kick_particles.key_2", "Dirt turns to mud in the rain"),
				T("feature.footstep_kick_particles.key_3", "Splashes scale with water depth, up to knee-deep water") } };
	}

	struct Settings
	{
		bool Enabled = false;  // Bearforj 0.6.4: off by default, SSPP Footstep Particles sub-feature already emits footsteps
		bool Snow = true;
		bool Mud = true;
		bool Sand = true;
		bool Dirt = false;
		bool Water = true;
		bool RainMakesMud = true;
		bool PlayerOnly = false;
		float Scale = 1.0f;
		float WaterScale = 1.0f;
		float MaxShallowDepth = 40.0f;
		float MaxDistance = 3000.0f;
		float Lifetime = 1.6f;
	} settings;

	enum class Kind : uint8_t
	{
		None,
		Snow,
		Mud,
		Sand,
		Dirt,
		Water
	};

	virtual void DataLoaded() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*) override;

	Kind Classify(RE::Actor* a_actor, const RE::NiPoint3& a_foot, float& a_waterDepth) const;
	static const char* ModelFor(Kind a_kind);
};
