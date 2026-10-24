#include "PhysicalBlood.h"

#include "Features/ScreenSpacePhysicalParticles/NifPath.h"

#include <algorithm>
#include <cmath>
#include <limits>

#define I18N_KEY_PREFIX "feature.physical_blood."

// This translation unit defines profile defaults, canonical NIF matching, and
// settings persistence. Settings are parsed into validated values, assembled as
// a complete profile list, and atomically published so gameplay hooks never see
// partially edited configuration. Exact paths and filename-wide matches remain
// distinct serialized keys; the built-in On Hit source has no NIF path.

namespace
{
	namespace NifPath = ScreenSpacePhysicalParticles::NifPath;

	constexpr std::size_t kMaxBloodMeshPathLength = 260;

	enum class DefaultBloodMeshLabel : std::uint8_t
	{
		kImpact,
		kLargeArrowLegacy,
		kLargeArrow,
		kLargeImpact,
		kLargeMist,
		kUnarmedImpact,
		kArrowImpact,
		kHammerImpact
	};

	struct DefaultBloodMeshProfile
	{
		DefaultBloodMeshLabel label;
		std::string_view modelPath;
	};

	constexpr std::array kDefaultBloodMeshProfiles{
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kImpact, "bloodsprayimpact01.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kLargeArrowLegacy, "bloodspraylargearrow01.nif.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kLargeArrow, "bloodspraylargearrow01.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kLargeImpact, "bloodspraylargeimpact01.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kLargeMist, "bloodspraylargemist01.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kUnarmedImpact, "bloodsprayunarmedimpact01.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kArrowImpact, "bloodsprayarrowimpact01.nif" },
		DefaultBloodMeshProfile{ DefaultBloodMeshLabel::kHammerImpact, "bloodsprayhammerimpact01.nif" }
	};

	// Summary: Converts a distribution mode to its stable JSON name.
	// Args:
	//   a_mode: Distribution mode to serialize.
	// Returns: The settings-file token for the mode.
	constexpr std::string_view DistributionModeName(PhysicalBlood::DistributionMode a_mode)
	{
		switch (a_mode) {
		case PhysicalBlood::DistributionMode::kNormal:
			return "Normal";
		case PhysicalBlood::DistributionMode::kBiasLow:
			return "BiasLow";
		case PhysicalBlood::DistributionMode::kBiasHigh:
			return "BiasHigh";
		default:
			return "Uniform";
		}
	}

	// Summary: Parses an exact persisted distribution-mode token.
	// Args:
	//   a_value: JSON value expected to contain a mode string.
	//   a_result: Destination updated only for a recognized mode.
	// Returns: True when parsing succeeds.
	bool TryParseDistributionMode(const json& a_value, PhysicalBlood::DistributionMode& a_result)
	{
		if (!a_value.is_string())
			return false;
		const auto name = a_value.get<std::string>();
		if (name == "Uniform")
			a_result = PhysicalBlood::DistributionMode::kUniform;
		else if (name == "Normal")
			a_result = PhysicalBlood::DistributionMode::kNormal;
		else if (name == "BiasLow")
			a_result = PhysicalBlood::DistributionMode::kBiasLow;
		else if (name == "BiasHigh")
			a_result = PhysicalBlood::DistributionMode::kBiasHigh;
		else
			return false;
		return true;
	}
}

void PhysicalBlood::RestoreDefaultSettings()
{
	settings = {};
	maximumSplatsDraft = settings.maximumSplats;
	hookFeatureEnabled.store(settings.enabled, std::memory_order_release);
	hookNifHijackingEnabled.store(settings.hijackBloodMeshes, std::memory_order_release);
	ResetBloodMeshProfiles(settings.defaultEmission);
	pendingBloodMeshPath.clear();
	bloodMeshProfileMessage.clear();
	addingBloodMeshProfile = false;
	selectedEmissionDistributionIndex = 0;
}

// Summary: Rebuilds the on-hit and built-in NIF profiles from one emission template.
// Args:
//   a_emission: Settings copied into every default profile.
// Returns: Nothing.
void PhysicalBlood::ResetBloodMeshProfiles(const EmissionSettings& a_emission)
{
	BloodMeshProfileList profiles;
	profiles.reserve(kDefaultBloodMeshProfiles.size() + 1);
	BloodMeshProfile onHit{};
	onHit.matchMode = NifMatchMode::kOnHit;
	onHit.enabled = true;
	onHit.suppressOriginal = false;
	onHit.builtIn = true;
	onHit.emission = a_emission;
	profiles.push_back(std::move(onHit));
	for (const auto& definition : kDefaultBloodMeshProfiles) {
		BloodMeshProfile profile{};
		profile.modelPath = std::string(definition.modelPath);
		profile.matchMode = NifMatchMode::kFilename;
		profile.builtIn = true;
		profile.emission = a_emission;
		profiles.push_back(std::move(profile));
	}
	selectedBloodMeshProfileIndex = 0;
	PublishBloodMeshProfiles(std::move(profiles));
}

// Summary: Acquires the immutable profile snapshot used by hook threads.
// Args: None.
// Returns: The currently published profile list.
std::shared_ptr<const PhysicalBlood::BloodMeshProfileList>
PhysicalBlood::LoadBloodMeshProfiles() const noexcept
{
	return bloodMeshProfiles.load(std::memory_order_acquire);
}

// Summary: Atomically publishes a complete immutable profile list.
// Args:
//   a_profiles: Replacement profile list.
// Returns: Nothing.
void PhysicalBlood::PublishBloodMeshProfiles(BloodMeshProfileList a_profiles)
{
	bloodMeshProfiles.store(
		std::make_shared<const BloodMeshProfileList>(std::move(a_profiles)),
		std::memory_order_release);
}

// Summary: Validates a normalized NIF path and its configured length limit.
// Args:
//   a_normalizedPath: Canonical relative NIF path.
// Returns: True when the path is safe and names a NIF.
bool PhysicalBlood::IsValidNifPath(std::string_view a_normalizedPath)
{
	return NifPath::IsValid(a_normalizedPath, kMaxBloodMeshPathLength);
}

// Summary: Builds the localized selector label for a profile.
// Args:
//   a_profile: Profile to describe.
// Returns: A localized built-in label or the custom match text.
std::string PhysicalBlood::BloodMeshProfileLabel(const BloodMeshProfile& a_profile)
{
	if (a_profile.matchMode == NifMatchMode::kOnHit)
		return T(TKEY("profile_on_hit"), "On Hit");
	if (a_profile.builtIn) {
		for (const auto& definition : kDefaultBloodMeshProfiles) {
			if (definition.modelPath != a_profile.modelPath)
				continue;
			switch (definition.label) {
			case DefaultBloodMeshLabel::kImpact:
				return T(TKEY("profile_impact"), "Impact");
			case DefaultBloodMeshLabel::kLargeArrowLegacy:
				return T(TKEY("profile_large_arrow_legacy"), "Large arrow (legacy name)");
			case DefaultBloodMeshLabel::kLargeArrow:
				return T(TKEY("profile_large_arrow"), "Large arrow");
			case DefaultBloodMeshLabel::kLargeImpact:
				return T(TKEY("profile_large_impact"), "Large impact");
			case DefaultBloodMeshLabel::kLargeMist:
				return T(TKEY("profile_large_mist"), "Large mist");
			case DefaultBloodMeshLabel::kUnarmedImpact:
				return T(TKEY("profile_unarmed_impact"), "Unarmed impact");
			case DefaultBloodMeshLabel::kArrowImpact:
				return T(TKEY("profile_arrow_impact"), "Arrow impact");
			case DefaultBloodMeshLabel::kHammerImpact:
				return T(TKEY("profile_hammer_impact"), "Hammer impact");
			}
		}
	}
	if (a_profile.matchMode == NifMatchMode::kExactPath)
		return a_profile.modelPath;
	return std::string(NifPath::Filename(a_profile.modelPath));
}

// Summary: Parses and clamps one emission profile from JSON.
// Args:
//   a_json: JSON object containing profile overrides.
//   a_defaults: Values retained for absent or invalid fields.
// Returns: The validated emission settings.
PhysicalBlood::EmissionSettings PhysicalBlood::LoadEmissionSettings(
	const json& a_json,
	const EmissionSettings& a_defaults)
{
	EmissionSettings emission = a_defaults;
	// Summary: Reads a finite JSON number representable as float.
	// Args:
	//   a_value: JSON value to inspect.
	//   a_result: Destination updated on success.
	// Returns: True when conversion succeeds.
	const auto tryNumber = [](const json& a_value, float& a_result) {
		if (!a_value.is_number())
			return false;
		const auto raw = a_value.get<double>();
		if (!std::isfinite(raw) || raw < std::numeric_limits<float>::lowest() ||
			raw > std::numeric_limits<float>::max()) {
			return false;
		}
		a_result = static_cast<float>(raw);
		return true;
	};
	// Summary: Reads a finite float property with a fallback.
	// Args:
	//   a_object: JSON object containing the property.
	//   a_name: Property name.
	//   a_fallback: Value returned for an absent or invalid property.
	// Returns: The parsed number or fallback.
	const auto numberFrom = [&](const json& a_object, std::string_view a_name, float a_fallback) {
		const auto value = a_object.find(a_name);
		float result{};
		return value != a_object.end() && tryNumber(*value, result) ? result : a_fallback;
	};
	const auto number = [&](std::string_view a_name, float a_fallback) {
		return numberFrom(a_json, a_name, a_fallback);
	};
	// Summary: Reads and clamps an integer-valued profile property.
	// Args:
	//   a_name: Property name in the profile object.
	//   a_fallback: Value returned for an absent or invalid property.
	// Returns: An integer in the supported particle-count range.
	const auto integer = [&](std::string_view a_name, int a_fallback) {
		const auto value = a_json.find(a_name);
		if (value == a_json.end() || !value->is_number())
			return a_fallback;
		const auto raw = value->get<double>();
		return std::isfinite(raw) ? static_cast<int>(std::lround(std::clamp(raw, 1.0, 256.0))) : a_fallback;
	};
	// Summary: Parses, orders, and clamps one distribution range.
	// Args:
	//   a_name: Property name in the profile object.
	//   a_fallback: Range retained when the property is absent or invalid.
	//   a_lowerBound: Inclusive minimum accepted value.
	//   a_upperBound: Inclusive maximum accepted value.
	// Returns: A validated range.
	const auto range = [&](std::string_view a_name,
	                     const DistributionRange& a_fallback,
	                     float a_lowerBound,
	                     float a_upperBound) {
		DistributionRange result = a_fallback;
		const auto value = a_json.find(a_name);
		if (value != a_json.end() && value->is_object()) {
			const auto minimum = value->find("Min");
			const auto maximum = value->find("Max");
			const auto distribution = value->find("Distribution");
			DistributionRange parsed{};
			if (minimum != value->end() && maximum != value->end() && distribution != value->end() &&
				tryNumber(*minimum, parsed.minimum) && tryNumber(*maximum, parsed.maximum) &&
				TryParseDistributionMode(*distribution, parsed.distribution)) {
				result = parsed;
			}
		}
		result.minimum = std::clamp(result.minimum, a_lowerBound, a_upperBound);
		result.maximum = std::clamp(result.maximum, a_lowerBound, a_upperBound);
		if (result.minimum > result.maximum)
			std::swap(result.minimum, result.maximum);
		return result;
	};

	emission.particlesPerHit = std::clamp(integer("ParticlesPerHit", emission.particlesPerHit), 1, 256);
	emission.initialSpeed = range("InitialSpeed", emission.initialSpeed, 0.0F, 2500.0F);
	emission.horizontalAngleDegrees = range(
		"HorizontalAngleDegrees",
		emission.horizontalAngleDegrees,
		-45.0F,
		45.0F);
	emission.verticalAngleDegrees = range(
		"VerticalAngleDegrees",
		emission.verticalAngleDegrees,
		-45.0F,
		45.0F);
	emission.particleLifetime = range("ParticleLifetime", emission.particleLifetime, 0.10F, 6.0F);
	emission.particleSize = range("ParticleSize", emission.particleSize, 0.0F, 5.0F);
	emission.splatSizeMultiplier =
		std::clamp(number("SplatSizeMultiplier", emission.splatSizeMultiplier), 0.0F, 5.0F);
	emission.opacity = std::clamp(number("Opacity", emission.opacity), 0.1F, 1.0F);
	return emission;
}

// Summary: Serializes one emission profile into JSON.
// Args:
//   a_json: Destination JSON object.
//   a_emission: Emission settings to serialize.
// Returns: Nothing.
void PhysicalBlood::SaveEmissionSettings(json& a_json, const EmissionSettings& a_emission)
{
	// Summary: Clamps and serializes one distribution range.
	// Args:
	//   a_name: Destination property name.
	//   a_range: Range to validate and serialize.
	//   a_lowerBound: Inclusive serialized minimum.
	//   a_upperBound: Inclusive serialized maximum.
	// Returns: Nothing.
	const auto saveRange = [&](
		std::string_view a_name,
		DistributionRange a_range,
		float a_lowerBound,
		float a_upperBound) {
		a_range.minimum = std::isfinite(a_range.minimum) ?
		                      std::clamp(a_range.minimum, a_lowerBound, a_upperBound) :
		                      std::clamp(0.0F, a_lowerBound, a_upperBound);
		a_range.maximum = std::isfinite(a_range.maximum) ?
		                      std::clamp(a_range.maximum, a_lowerBound, a_upperBound) :
		                      a_range.minimum;
		if (a_range.minimum > a_range.maximum)
			std::swap(a_range.minimum, a_range.maximum);
		a_json[a_name] = {
			{ "Min", a_range.minimum },
			{ "Max", a_range.maximum },
			{ "Distribution", DistributionModeName(a_range.distribution) }
		};
	};
	a_json["ParticlesPerHit"] = a_emission.particlesPerHit;
	saveRange("InitialSpeed", a_emission.initialSpeed, 0.0F, 2500.0F);
	saveRange("HorizontalAngleDegrees", a_emission.horizontalAngleDegrees, -45.0F, 45.0F);
	saveRange("VerticalAngleDegrees", a_emission.verticalAngleDegrees, -45.0F, 45.0F);
	saveRange("ParticleLifetime", a_emission.particleLifetime, 0.10F, 6.0F);
	saveRange("ParticleSize", a_emission.particleSize, 0.0F, 5.0F);
	a_json["SplatSizeMultiplier"] = a_emission.splatSizeMultiplier;
	a_json["Opacity"] = a_emission.opacity;
}

void PhysicalBlood::LoadSettings(json& a_json)
{
	const bool wasEnabled = hookFeatureEnabled.load(std::memory_order_acquire);
	settings.enabled = a_json.value("Enabled", settings.enabled);
	settings.defaultEmission = LoadEmissionSettings(a_json, settings.defaultEmission);
	settings.gravity = std::clamp(a_json.value("Gravity", settings.gravity), 200.0F, 1800.0F);
	settings.drag = std::clamp(a_json.value("Drag", settings.drag), 0.0F, 2.5F);
	settings.splatLifetime = std::clamp(a_json.value("SplatLifetime", settings.splatLifetime), 1.0F, 60.0F);
	settings.maximumSplats = std::clamp(
		a_json.value("MaximumSplats", settings.maximumSplats),
		kMinimumOwnedSplatCapacity,
		kMaximumOwnedSplatCapacity);
	maximumSplatsDraft = settings.maximumSplats;
	settings.waterUpdateRate = std::clamp(a_json.value("WaterUpdateRate", settings.waterUpdateRate), 20.0F, 120.0F);
	settings.waterFlowSpeed = std::clamp(a_json.value("WaterFlowSpeed", settings.waterFlowSpeed), 0.0F, 600.0F);
	settings.waterDiffusion = std::clamp(a_json.value("WaterDiffusion", settings.waterDiffusion), 0.0F, 1.0F);
	settings.waterDissipation = std::clamp(a_json.value("WaterDissipation", settings.waterDissipation), 0.0F, 1.0F);
	settings.waterImpactSizeMultiplier =
		std::clamp(a_json.value("WaterImpactSizeMultiplier", settings.waterImpactSizeMultiplier), 0.5F, 5.0F);
	settings.waterImpactStrength =
		std::clamp(a_json.value("WaterImpactStrength", settings.waterImpactStrength), 0.10F, 3.00F);
	hookFeatureEnabled.store(settings.enabled, std::memory_order_release);
	if (wasEnabled && !settings.enabled)
		runtimeResetPending.store(true, std::memory_order_release);

	ResetBloodMeshProfiles(settings.defaultEmission);
	auto profilesSnapshot = LoadBloodMeshProfiles();
	BloodMeshProfileList profiles = profilesSnapshot ? *profilesSnapshot : BloodMeshProfileList{};
	// Summary: Reads a Boolean property without coercing other JSON types.
	// Args:
	//   a_object: JSON object containing the property.
	//   a_name: Property name.
	//   a_fallback: Value returned for an absent or invalid property.
	// Returns: The parsed Boolean or fallback.
	const auto readBool = [](const json& a_object, std::string_view a_name, bool a_fallback) {
		const auto value = a_object.find(a_name);
		return value != a_object.end() && value->is_boolean() ? value->get<bool>() : a_fallback;
	};
	if (auto onHitJson = a_json.find("OnHit"); onHitJson != a_json.end() && onHitJson->is_object()) {
		const auto onHit = std::ranges::find_if(profiles, [](const auto& a_profile) {
			return a_profile.matchMode == NifMatchMode::kOnHit;
		});
		if (onHit != profiles.end()) {
			onHit->enabled = readBool(*onHitJson, "Enabled", onHit->enabled);
			onHit->emission = LoadEmissionSettings(*onHitJson, onHit->emission);
		}
	}
	if (auto hijack = a_json.find("HijackBloodMesh"); hijack != a_json.end() && hijack->is_object()) {
		settings.hijackBloodMeshes = readBool(*hijack, "Enabled", settings.hijackBloodMeshes);
		if (auto profileEntries = hijack->find("Profiles");
			profileEntries != hijack->end() && profileEntries->is_object()) {
			for (const auto& [rawPath, profileJson] : profileEntries->items()) {
				if (!profileJson.is_object()) {
					logger::warn("[Physical Blood] Ignored non-object blood mesh profile '{}'", rawPath);
					continue;
				}

				auto normalized = NifPath::Normalize(rawPath);
				const auto matchMode = normalized.contains('\\') ? NifMatchMode::kExactPath : NifMatchMode::kFilename;
				if (matchMode == NifMatchMode::kFilename)
					normalized = std::string(NifPath::Filename(normalized));
				if (!IsValidNifPath(normalized)) {
					logger::warn("[Physical Blood] Ignored invalid blood mesh profile path '{}'", rawPath);
					continue;
				}

				auto existing = std::ranges::find_if(profiles, [&](const auto& a_profile) {
					return a_profile.matchMode == matchMode && a_profile.modelPath == normalized;
				});
				if (existing == profiles.end()) {
					if (profiles.size() >= kMaxBloodMeshProfiles + 1) {
						logger::warn(
							"[Physical Blood] Ignored blood mesh profile '{}': limit is {}",
							rawPath,
							kMaxBloodMeshProfiles);
						continue;
					}
					BloodMeshProfile custom{};
					custom.modelPath = std::move(normalized);
					custom.matchMode = matchMode;
					custom.emission = settings.defaultEmission;
					profiles.push_back(std::move(custom));
					existing = std::prev(profiles.end());
				}

				existing->enabled = readBool(profileJson, "Enabled", existing->enabled);
				existing->suppressOriginal =
					readBool(profileJson, "DoNotSpawnOriginalMesh", existing->suppressOriginal);
				existing->emission = LoadEmissionSettings(profileJson, existing->emission);
			}
		}
	}
	selectedBloodMeshProfileIndex = profiles.empty() ?
	                                    0 :
	                                    std::min(selectedBloodMeshProfileIndex, profiles.size() - 1);
	PublishBloodMeshProfiles(std::move(profiles));
	hookNifHijackingEnabled.store(settings.hijackBloodMeshes, std::memory_order_release);
}

void PhysicalBlood::SaveSettings(json& a_json)
{
	a_json = {
		{ "Enabled", settings.enabled },
		{ "Gravity", settings.gravity },
		{ "Drag", settings.drag },
		{ "SplatLifetime", settings.splatLifetime },
		{ "MaximumSplats", settings.maximumSplats },
		{ "WaterUpdateRate", settings.waterUpdateRate },
		{ "WaterFlowSpeed", settings.waterFlowSpeed },
		{ "WaterDiffusion", settings.waterDiffusion },
		{ "WaterDissipation", settings.waterDissipation },
		{ "WaterImpactSizeMultiplier", settings.waterImpactSizeMultiplier },
		{ "WaterImpactStrength", settings.waterImpactStrength }
	};
	SaveEmissionSettings(a_json, settings.defaultEmission);
	json onHitJson = json::object();
	json profileJson = json::object();
	const auto profiles = LoadBloodMeshProfiles();
	const bool hijackingEnabled = settings.hijackBloodMeshes;
	if (profiles) {
		for (const auto& profile : *profiles) {
			if (profile.matchMode == NifMatchMode::kOnHit) {
				onHitJson["Enabled"] = profile.enabled;
				SaveEmissionSettings(onHitJson, profile.emission);
				continue;
			}
			json entry{
				{ "Enabled", profile.enabled },
				{ "DoNotSpawnOriginalMesh", profile.suppressOriginal }
			};
			SaveEmissionSettings(entry, profile.emission);
			profileJson[profile.modelPath] = std::move(entry);
		}
	}
	a_json["OnHit"] = std::move(onHitJson);
	a_json["HijackBloodMesh"] = {
		{ "Enabled", hijackingEnabled },
		{ "Profiles", std::move(profileJson) }
	};
}

#undef I18N_KEY_PREFIX
