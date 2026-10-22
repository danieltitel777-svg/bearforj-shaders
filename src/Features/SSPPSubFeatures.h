#pragma once

#include "Feature.h"

/**
 * @brief Bearforj 0.6.3: the five 0.5 particle features as menu entries backed by SSPP v2.
 *
 * In 0.5 these were separate features (Physical Blood, Physical Effect Particles, Physical Leaves,
 * GPU Footstep Particles, Precipitation Physics). In 0.6 their work moved into the shared SSPP v2
 * simulation, so they have no GPU work of their own: each proxy owns its INI (so the INI is not an
 * orphan and the feature shows up in the menu) and its on/off switch gates the matching SSPP flags.
 * Turning one off in the menu (boot toggle) turns the SSPP particle kind off; settings stay in SSPP.
 */
struct SSPPSubFeature : Feature
{
	enum class Kind
	{
		kBlood,
		kEffects,
		kLeaves,
		kFootsteps,
		kPrecipitation
	};

	SSPPSubFeature(Kind a_kind, const char* a_name, const char* a_short, const char* a_key) :
		kind(a_kind), name(a_name), shortName(a_short), key(a_key) {}

	virtual std::string GetName() override { return name; }
	virtual std::string GetShortName() override { return shortName; }
	virtual std::string GetDisplayName() override;
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	virtual bool SupportsVR() override { return false; }
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json&) override {}
	virtual void SaveSettings(json&) override {}

	/** @brief False only when the user switched this sub-feature off (menu switch / boot list). */
	bool Allowed();

	Kind kind;
	const char* name;
	const char* shortName;
	const char* key;
};

namespace SSPPSub
{
	SSPPSubFeature& Blood();
	SSPPSubFeature& Effects();
	SSPPSubFeature& Leaves();
	SSPPSubFeature& Footsteps();
	SSPPSubFeature& Precipitation();
}
