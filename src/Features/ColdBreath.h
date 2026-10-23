#pragma once

#include <unordered_map>

struct ColdBreath : Feature
{
	virtual inline std::string GetName() override { return "Cold Breath"; }
	virtual std::string GetDisplayName() override { return T("feature.cold_breath.name", "Cold Breath"); }
	virtual inline std::string GetShortName() override { return "ColdBreath"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsDisabledByDefault() const override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.cold_breath.description", "Visible breath in snow and cold weather, using Wet and Cold's breath meshes."),
			{ T("feature.cold_breath.key_1", "Plays on the head of the player, NPCs and a few creatures"),
				T("feature.cold_breath.key_2", "Only while it is snowing, out of doors") } };
	}

	struct Settings
	{
		bool Enabled = true;
		float Interval = 2.2f;
		float Lifetime = 1.5f;
		float Range = 4096.0f;
	} settings;

	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

private:
	struct Slot
	{
		RE::NiPointer<RE::NiAVObject> node;
		float age = 0.0f;
		float next = 0.0f;
	};
	std::unordered_map<RE::FormID, Slot> slots;
	bool demandFailed = false;
};
