#include "SSPPSubFeatures.h"

#include <imgui.h>

#include "Features/ScreenSpacePhysicalParticles.h"
#include "Globals.h"
#include "State.h"

namespace SSPPSub
{
	using K = SSPPSubFeature::Kind;
	SSPPSubFeature& Blood() { static SSPPSubFeature f{ K::kBlood, "Physical Blood", "PhysicalBlood", "blood" }; return f; }
	SSPPSubFeature& Effects() { static SSPPSubFeature f{ K::kEffects, "Physical Effect Particles", "PhysicalEffectParticles", "effects" }; return f; }
	SSPPSubFeature& Leaves() { static SSPPSubFeature f{ K::kLeaves, "Physical Leaves", "PhysicalLeaves", "leaves" }; return f; }
	SSPPSubFeature& Footsteps() { static SSPPSubFeature f{ K::kFootsteps, "GPU Footstep Particles", "FootstepParticles", "footsteps" }; return f; }
	SSPPSubFeature& Precipitation() { static SSPPSubFeature f{ K::kPrecipitation, "Precipitation Physics", "PrecipitationPhysics", "precipitation" }; return f; }
}

bool SSPPSubFeature::Allowed()
{
	return !globals::state || !globals::state->IsFeatureDisabled(shortName);
}

std::string SSPPSubFeature::GetDisplayName()
{
	switch (kind) {
	case Kind::kBlood: return T("feature.sspp_sub.blood.name", "Physical Blood");
	case Kind::kEffects: return T("feature.sspp_sub.effects.name", "Physical Effect Particles (sparks, embers)");
	case Kind::kLeaves: return T("feature.sspp_sub.leaves.name", "Physical Leaves");
	case Kind::kFootsteps: return T("feature.sspp_sub.footsteps.name", "Footstep Particles");
	default: return T("feature.sspp_sub.precipitation.name", "Precipitation Physics (rain, snow)");
	}
}

std::pair<std::string, std::vector<std::string>> SSPPSubFeature::GetFeatureSummary()
{
	return { T("feature.sspp_sub.summary", "Part of Physical Particles (SSPP v2). The switch turns this particle kind on or off; detailed settings are in Physical Particles."), {} };
}

void SSPPSubFeature::DrawSettings()
{
	auto& s = globals::features::screenSpacePhysicalParticles.settings;
	ImGui::TextWrapped("%s", T("feature.sspp_sub.summary", "Part of Physical Particles (SSPP v2). The switch turns this particle kind on or off; detailed settings are in Physical Particles."));
	switch (kind) {
	case Kind::kBlood: ImGui::Checkbox(T("feature.sspp_sub.blood.flag", "Blood droplets on hits"), &s.Blood); break;
	case Kind::kEffects:
		ImGui::Checkbox(T("feature.sspp_sub.effects.sparks", "Sparks on blocks"), &s.Sparks);
		ImGui::Checkbox(T("feature.sspp_sub.effects.magic", "Magic particles"), &s.bf.MagicParticles);
		ImGui::Checkbox(T("feature.sspp_sub.effects.fire", "Falling sparks from fires and forges"), &s.bf.FireSparks);
		break;
	case Kind::kLeaves: ImGui::Checkbox(T("feature.sspp_sub.leaves.flag", "Falling leaves"), &s.Leaves); break;
	case Kind::kFootsteps: ImGui::Checkbox(T("feature.sspp_sub.footsteps.flag", "Dust / snow kicked up by steps"), &s.FootstepParticles); break;
	default:
		ImGui::Checkbox(T("feature.sspp_sub.precipitation.rain", "Rain"), &s.Rain);
		ImGui::SameLine();
		ImGui::Checkbox(T("feature.sspp_sub.precipitation.snow", "Snow"), &s.Snow);
		break;
	}
	if (!globals::features::screenSpacePhysicalParticles.loaded)
		ImGui::TextColored({ 1, 0.6f, 0.3f, 1 }, "%s", T("feature.sspp_sub.needs_sspp", "Physical Particles is off: this sub-feature has no effect."));
}
