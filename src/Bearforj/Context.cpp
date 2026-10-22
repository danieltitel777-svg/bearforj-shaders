#include "Bearforj/Context.h"
#include "Bearforj/WeatherPresets.h"

namespace Bearforj::Context
{
	namespace
	{
		State g_state;
		float g_nextScan = 0.0f;
		float g_nextTrees = 0.0f;

		bool HasLocKeyword(RE::BGSLocation* a_loc, const char* a_kw)
		{
			for (auto* l = a_loc; l; l = l->parentLoc)
				if (l->HasKeywordString(a_kw))
					return true;
			return false;
		}

		bool IsConiferPath(std::string a_path)
		{
			std::ranges::transform(a_path, a_path.begin(), ::tolower);
			for (const char* k : { "pine", "spruce", "conifer", "fir", "snowtree" })
				if (a_path.find(k) != std::string::npos)
					return true;
			return false;
		}
	}

	const State& Get() { return g_state; }

	void Update()
	{
		const float now = RE::GetDurationOfApplicationRunTime() / 1000.0f;
		if (now < g_nextScan)
			return;
		g_nextScan = now + 0.5f;

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cal = RE::Calendar::GetSingleton();
		State s = g_state;
		s.hour = cal ? cal->GetHour() : 12.0f;
		s.night = s.hour < 5.0f || s.hour > 20.0f;
		s.weatherSlot = WeatherPresets::CurrentSlot();  // flags/data classification, not FormIDs
		s.loc = 0;
		const bool interior = player && player->GetParentCell() && player->GetParentCell()->IsInteriorCell();
		if (interior)
			s.loc |= kLocInterior;
		if (auto* loc = player ? player->GetCurrentLocation() : nullptr) {
			if (HasLocKeyword(loc, "LocTypeCave"))
				s.loc |= kLocCave;
			if (HasLocKeyword(loc, "LocTypeDungeon") || HasLocKeyword(loc, "LocTypeDraugrCrypt") || HasLocKeyword(loc, "LocTypeNordicRuin"))
				s.loc |= kLocDungeon;
			if (HasLocKeyword(loc, "LocTypeMine"))
				s.loc |= kLocMine;
			if (HasLocKeyword(loc, "LocTypeCity") || HasLocKeyword(loc, "LocTypeTown") || HasLocKeyword(loc, "LocTypeSettlement") || HasLocKeyword(loc, "LocTypeHabitation"))
				s.loc |= kLocCity;
			if (HasLocKeyword(loc, "LocTypeDwarvenAutomatons"))
				s.loc |= kLocDwemer;
		}
		const float coldTarget = (s.weatherSlot == WeatherPresets::kSnow || s.weatherSlot == WeatherPresets::kBlizzard) ? 1.0f : 0.0f;
		s.cold += (coldTarget - s.cold) * 0.25f;  // eased: no hard switch between weathers

		// Trees: scan loaded refs every 2 s (exterior only), counts smoothed over time.
		if (!interior && player && now >= g_nextTrees) {
			g_nextTrees = now + 2.0f;
			int deciduous = 0, conifer = 0;
			if (auto* tes = RE::TES::GetSingleton()) {
				tes->ForEachReferenceInRange(player, 3000.0f, [&](RE::TESObjectREFR* a_ref) {
					auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (base && base->Is(RE::FormType::Tree)) {
						auto* model = base->As<RE::TESModel>();
						if (model && model->GetModel() && IsConiferPath(model->GetModel()))
							++conifer;
						else
							++deciduous;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			s.trees += (std::clamp(deciduous / 25.0f, 0.0f, 1.0f) - s.trees) * 0.5f;
			s.conifers += (std::clamp(conifer / 25.0f, 0.0f, 1.0f) - s.conifers) * 0.5f;
		} else if (interior) {
			s.trees *= 0.5f;
			s.conifers *= 0.5f;
		}
		g_state = s;
	}
}
