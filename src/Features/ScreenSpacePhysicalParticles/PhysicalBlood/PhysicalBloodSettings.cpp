#include "PhysicalBlood.h"
#include "Features/ScreenSpacePhysicalParticles/NifPath.h"
#include <algorithm>
#include <cmath>
#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "feature.physical_blood."

namespace NifPath = ScreenSpacePhysicalParticles::NifPath;

// This translation unit renders the Physical Blood editor. Profile edits occur
// on a local copy, then one immutable snapshot is published at the end of the
// panel. A compact selector avoids duplicating the full emission editor for each
// NIF, while per-property controls expose bounded distributions and water tuning.

void PhysicalBlood::DrawSettings()
{
	// Summary: Draws a quantized floating-point slider.
	// Args:
	//   a_label: ImGui identifier or visible label.
	//   a_value: Setting updated in place.
	//   a_min: Inclusive lower bound.
	//   a_max: Inclusive upper bound.
	//   a_step: Quantization interval.
	//   a_format: ImGui display format.
	// Returns: Nothing.
	const auto steppedSlider = [](
		const char* a_label,
		float& a_value,
		float a_min,
		float a_max,
		float a_step,
		const char* a_format) {
		if (ImGui::SliderFloat(a_label, &a_value, a_min, a_max, a_format, ImGuiSliderFlags_AlwaysClamp)) {
			a_value = std::clamp(std::round(a_value / a_step) * a_step, a_min, a_max);
		}
	};
	// Summary: Shows wrapped help text while the preceding control is hovered.
	// Args:
	//   a_explanation: Localized help text.
	// Returns: Nothing.
	const auto explainLastItem = [](const char* a_explanation) {
		if (auto _tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", a_explanation);
	};
	// Summary: Advances a two-column settings table to its editable value column.
	// Args:
	//   a_label: Label rendered in the first column.
	// Returns: Nothing.
	const auto settingRow = [](const char* a_label) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextWrapped("%s", a_label);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-FLT_MIN);
	};
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable physical blood"), &settings.enabled)) {
		hookFeatureEnabled.store(settings.enabled, std::memory_order_release);
		if (!settings.enabled)
			runtimeResetPending.store(true, std::memory_order_release);
	}
	ImGui::SameLine();
	const auto sharedCapacity =
		ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton().Capacity();
	ImGui::TextDisabled(
		T(TKEY("gpu_pool"), "Shared GPU pool: %u particles"),
		sharedCapacity);
	if (ImGui::TreeNodeEx(T(TKEY("hijack_blood_mesh"), "Hijack blood mesh"), ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto currentProfiles = LoadBloodMeshProfiles();
		BloodMeshProfileList profiles = currentProfiles ? *currentProfiles : BloodMeshProfileList{};
		auto selectedIndex = selectedBloodMeshProfileIndex;
		auto hijackingEnabled = settings.hijackBloodMeshes;
		if (!profiles.empty())
			selectedIndex = std::min(selectedIndex, profiles.size() - 1);

		ImGui::Checkbox(T(TKEY("enable_nif_hijacking"), "Enable NIF hijacking"), &hijackingEnabled);
		ImGui::SameLine();
		const auto activeProfiles = std::ranges::count_if(profiles, [](const auto& a_profile) {
			return a_profile.matchMode != NifMatchMode::kOnHit && a_profile.enabled;
		});
		const auto nifProfiles = std::ranges::count_if(profiles, [](const auto& a_profile) {
			return a_profile.matchMode != NifMatchMode::kOnHit;
		});
		const auto suppressedProfiles = std::ranges::count_if(profiles, [](const auto& a_profile) {
			return a_profile.matchMode != NifMatchMode::kOnHit && a_profile.enabled && a_profile.suppressOriginal;
		});
		const auto customProfiles = std::ranges::count_if(profiles, [](const auto& a_profile) {
			return a_profile.matchMode != NifMatchMode::kOnHit && !a_profile.builtIn;
		});
		const auto onHit = std::ranges::find_if(profiles, [](const auto& a_profile) {
			return a_profile.matchMode == NifMatchMode::kOnHit;
		});
		ImGui::TextDisabled(
			T(TKEY("profile_summary"), "On Hit: %s | %zu/%zu NIF active | %zu hide original | %zu custom"),
			onHit != profiles.end() && onHit->enabled ? T(TKEY("status_on"), "On") : T(TKEY("status_off"), "Off"),
			hijackingEnabled ? static_cast<std::size_t>(activeProfiles) : 0,
			static_cast<std::size_t>(nifProfiles),
			hijackingEnabled ? static_cast<std::size_t>(suppressedProfiles) : 0,
			static_cast<std::size_t>(customProfiles));
		ImGui::TextDisabled("%s", T(TKEY("nif_hijacking_disabled_desc"), "Disabling NIF hijacking restores every original mesh; On Hit remains independent."));

		const auto selectedLabel = profiles.empty() ?
		                               std::string(T(TKEY("no_profiles"), "No profiles")) :
		                               BloodMeshProfileLabel(profiles[selectedIndex]);
		ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 105.0F, 180.0F));
		if (ImGui::BeginCombo("##BloodMeshProfile", selectedLabel.c_str())) {
			for (std::size_t index = 0; index < profiles.size(); ++index) {
				auto& profile = profiles[index];
				ImGui::PushID(static_cast<int>(index));
				const auto label = std::format(
					"[{}{}] {}##entry",
					profile.enabled ? T(TKEY("status_on"), "On") : T(TKEY("status_off"), "Off"),
					profile.matchMode != NifMatchMode::kOnHit && profile.suppressOriginal && profile.enabled ?
						T(TKEY("status_original_hidden_suffix"), ", original hidden") :
						"",
					BloodMeshProfileLabel(profile));
				const bool isSelected = index == selectedIndex;
				if (ImGui::Selectable(label.c_str(), isSelected))
					selectedIndex = index;
				if (isSelected)
					ImGui::SetItemDefaultFocus();
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button(
				addingBloodMeshProfile ? T(TKEY("cancel_add"), "Cancel add") : T(TKEY("add_nif"), "Add NIF"))) {
			addingBloodMeshProfile = !addingBloodMeshProfile;
			pendingBloodMeshPath.clear();
			bloodMeshProfileMessage.clear();
		}

		if (addingBloodMeshProfile) {
			ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 74.0F, 120.0F));
			const bool submitted = ImGui::InputTextWithHint(
				"##NewBloodMeshPath",
				T(TKEY("new_nif_path_hint"), "NIF filename or relative path (for example Effects\\MyBlood.nif)"),
				&pendingBloodMeshPath,
				ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			const auto addButtonLabel = std::format("{}##BloodMesh", T(TKEY("add"), "Add"));
			if (ImGui::Button(addButtonLabel.c_str()) || submitted) {
				auto normalized = NifPath::Normalize(pendingBloodMeshPath);
				const auto matchMode = normalized.contains('\\') ? NifMatchMode::kExactPath : NifMatchMode::kFilename;
				if (matchMode == NifMatchMode::kFilename)
					normalized = std::string(NifPath::Filename(normalized));

				const bool duplicate = std::ranges::any_of(profiles, [&](const auto& a_profile) {
					return a_profile.matchMode == matchMode && a_profile.modelPath == normalized;
				});
				if (!IsValidNifPath(normalized)) {
					bloodMeshProfileMessage = T(TKEY("error_invalid_nif_path"), "Enter a valid .nif filename or path (maximum 260 characters).");
					bloodMeshProfileMessageIsError = true;
				} else if (duplicate) {
					bloodMeshProfileMessage = T(TKEY("error_duplicate_nif"), "That NIF match already has a profile.");
					bloodMeshProfileMessageIsError = true;
				} else if (static_cast<std::size_t>(nifProfiles) >= kMaxBloodMeshProfiles) {
					bloodMeshProfileMessage = T(TKEY("error_profile_limit"), "The 64-profile safety limit has been reached.");
					bloodMeshProfileMessageIsError = true;
				} else {
					BloodMeshProfile profile{};
					profile.modelPath = std::move(normalized);
					profile.matchMode = matchMode;
					profile.emission = profiles.empty() ? settings.defaultEmission : profiles[selectedIndex].emission;
					profiles.push_back(std::move(profile));
					selectedIndex = profiles.size() - 1;
					pendingBloodMeshPath.clear();
					addingBloodMeshProfile = false;
					bloodMeshProfileMessage = T(TKEY("custom_profile_added"), "Custom NIF profile added.");
					bloodMeshProfileMessageIsError = false;
				}
			}
			ImGui::TextDisabled("%s", T(TKEY("nif_match_help"), "Filenames match in any folder; paths match that exact relative mesh path."));
		}
		if (!bloodMeshProfileMessage.empty()) {
			const auto color = bloodMeshProfileMessageIsError ?
			                       ImVec4(1.0F, 0.42F, 0.35F, 1.0F) :
			                       ImVec4(0.42F, 0.88F, 0.55F, 1.0F);
			ImGui::TextColored(color, "%s", bloodMeshProfileMessage.c_str());
		}

		if (!profiles.empty()) {
			selectedIndex = std::min(selectedIndex, profiles.size() - 1);
			auto& profile = profiles[selectedIndex];
			const bool isOnHit = profile.matchMode == NifMatchMode::kOnHit;
			ImGui::PushID(static_cast<int>(selectedIndex));
			ImGui::SeparatorText(BloodMeshProfileLabel(profile).c_str());
			if (isOnHit) {
				ImGui::TextDisabled("%s", T(TKEY("on_hit_desc"), "Accurate Actor::ProcessHit position and hit vector; player melee, unarmed, and bash hits."));
			} else {
				ImGui::TextDisabled("%s", profile.modelPath.c_str());
				ImGui::TextDisabled(
					"%s | %s",
					profile.builtIn ? T(TKEY("built_in"), "Built-in") : T(TKEY("custom"), "Custom"),
					profile.matchMode == NifMatchMode::kExactPath ?
						T(TKEY("exact_relative_path"), "Exact relative path") :
						T(TKEY("filename_any_folder"), "Filename in any folder"));
			}

			ImGui::Checkbox(T(TKEY("profile_enabled"), "Enabled"), &profile.enabled);
			if (isOnHit) {
				const bool anyNifEmitter = hijackingEnabled && std::ranges::any_of(profiles, [](const auto& a_profile) {
					return a_profile.matchMode != NifMatchMode::kOnHit && a_profile.enabled;
				});
				if (profile.enabled && anyNifEmitter) {
					ImGui::TextColored(
						{ 1.0F, 0.72F, 0.30F, 1.0F },
						"%s",
						T(TKEY("on_hit_additive_warning"), "Additive source: a matching NIF profile can emit a second physical-blood burst."));
				}
				ImGui::TextDisabled("%s", T(TKEY("original_suppression_nif_desc"), "Original-mesh suppression is configured by each NIF entry."));
			} else {
				ImGui::SameLine();
				ImGui::Checkbox(T(TKEY("do_not_spawn_original"), "Do not spawn original mesh"), &profile.suppressOriginal);
				if (!profile.enabled)
					ImGui::TextDisabled("%s", T(TKEY("profile_disabled_desc"), "This entry is bypassed; Skyrim will spawn the original mesh."));
				else if (!profile.suppressOriginal)
					ImGui::TextDisabled("%s", T(TKEY("original_and_physical_spawn_desc"), "Physical blood and the original NIF will both spawn."));
			}

			auto& emission = profile.emission;
			if (ImGui::BeginTable(
					"BloodMeshProfileParameters",
					2,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.42F);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58F);
				settingRow(T(TKEY("particles_per_hit"), "Particles per hit"));
				ImGui::SliderInt("##ParticlesPerHit", &emission.particlesPerHit, 1, 256);
				explainLastItem(T(TKEY("particles_per_hit_tooltip"), "Base number of droplets emitted by this source. On Hit power attacks scale the count up by 1.65x."));
				settingRow(T(TKEY("splat_multiplier"), "Blood splat multiplier"));
				steppedSlider("##BloodSplatMultiplier", emission.splatSizeMultiplier, 0.0F, 5.0F, 0.1F, T(TKEY("multiplier_format"), "%.1fx"));
				explainLastItem(T(TKEY("splat_multiplier_tooltip"), "Scales the settled ground splat relative to the droplet that created it. For example, 1.5x makes the splat 50% larger than its droplet."));
				settingRow(T(TKEY("opacity"), "Opacity"));
				ImGui::SliderFloat("##Opacity", &emission.opacity, 0.1F, 1.0F, "%.2f");
				explainLastItem(T(TKEY("opacity_tooltip"), "Visual opacity of this profile's airborne droplets and settled splats. It also scales how much dye a droplet deposits into water."));
				ImGui::EndTable();
			}

			if (ImGui::TreeNodeEx(
					T(TKEY("random_ranges_and_distributions"), "Random ranges and distributions"))) {
				struct DistributionUiSpec
				{
					DistributionRange* range;
					const char* name;
					const char* minimumLabel;
					const char* maximumLabel;
					const char* minimumTooltip;
					const char* maximumTooltip;
					float lowerBound;
					float upperBound;
					float step;
					const char* format;
				};
				std::array distributionSpecs{
					DistributionUiSpec{
						&emission.initialSpeed,
						T(TKEY("initial_speed"), "Initial speed"),
						T(TKEY("initial_speed_min"), "Initial speed low (5%)"),
						T(TKEY("initial_speed_max"), "Initial speed high (95%)"),
						T(TKEY("initial_speed_min_tooltip"), "Approximate lower percentile anchor before On Hit power-attack scaling. Rare tail droplets can launch more slowly."),
						T(TKEY("initial_speed_max_tooltip"), "Approximate upper percentile anchor before On Hit power-attack scaling. Rare tail droplets can launch faster."),
						0.0F,
						2500.0F,
						10.0F,
						"%.0f" },
					DistributionUiSpec{
						&emission.horizontalAngleDegrees,
						T(TKEY("horizontal_angle"), "Horizontal angle"),
						T(TKEY("horizontal_angle_min"), "Horizontal angle low (5%)"),
						T(TKEY("horizontal_angle_max"), "Horizontal angle high (95%)"),
						T(TKEY("horizontal_angle_min_tooltip"), "Approximate left-side percentile anchor relative to the hit vector. Rare tail droplets can travel farther left."),
						T(TKEY("horizontal_angle_max_tooltip"), "Approximate right-side percentile anchor relative to the hit vector. Rare tail droplets can travel farther right."),
						-45.0F,
						45.0F,
						0.5F,
						T(TKEY("signed_degrees_format"), "%+.1f deg") },
					DistributionUiSpec{
						&emission.verticalAngleDegrees,
						T(TKEY("vertical_angle"), "Vertical angle"),
						T(TKEY("vertical_angle_min"), "Vertical angle low (5%)"),
						T(TKEY("vertical_angle_max"), "Vertical angle high (95%)"),
						T(TKEY("vertical_angle_min_tooltip"), "Approximate lower percentile anchor relative to the hit vector. Rare tail droplets can launch farther downward."),
						T(TKEY("vertical_angle_max_tooltip"), "Approximate upper percentile anchor relative to the hit vector. Rare tail droplets can launch farther upward."),
						-45.0F,
						45.0F,
						0.5F,
						T(TKEY("signed_degrees_format"), "%+.1f deg") },
					DistributionUiSpec{
						&emission.particleLifetime,
						T(TKEY("droplet_lifetime"), "Droplet lifetime"),
						T(TKEY("droplet_lifetime_min"), "Droplet lifetime min"),
						T(TKEY("droplet_lifetime_max"), "Droplet lifetime max"),
						T(TKEY("droplet_lifetime_min_tooltip"), "Shortest airborne lifetime before a droplet expires. Settled splats use the global splat lifetime."),
						T(TKEY("droplet_lifetime_max_tooltip"), "Longest airborne lifetime before a droplet expires. Settled splats use the global splat lifetime."),
						0.10F,
						6.0F,
						0.05F,
						T(TKEY("seconds_2_format"), "%.2f s") },
					DistributionUiSpec{
						&emission.particleSize,
						T(TKEY("droplet_size"), "Droplet size"),
						T(TKEY("droplet_size_min"), "Droplet size min"),
						T(TKEY("droplet_size_max"), "Droplet size max"),
						T(TKEY("droplet_size_min_tooltip"), "Smallest visual radius that a newly emitted droplet can receive."),
						T(TKEY("droplet_size_max_tooltip"), "Largest visual radius that a newly emitted droplet can receive."),
						0.0F,
						5.0F,
						0.1F,
						"%.1f" }
				};
				selectedEmissionDistributionIndex =
					std::min(selectedEmissionDistributionIndex, distributionSpecs.size() - 1);
				if (ImGui::BeginTable(
						"DistributionRangeEditor",
						2,
						ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
					ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.42F);
					ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58F);
					settingRow(T(TKEY("distribution_property"), "Property"));
					if (ImGui::BeginCombo(
							"##DistributionProperty",
							distributionSpecs[selectedEmissionDistributionIndex].name)) {
						for (std::size_t index = 0; index < distributionSpecs.size(); ++index) {
							const bool selected = index == selectedEmissionDistributionIndex;
							if (ImGui::Selectable(distributionSpecs[index].name, selected))
								selectedEmissionDistributionIndex = index;
							if (selected)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				explainLastItem(T(TKEY("distribution_property_tooltip"), "Choose which randomized emission property to shape. Each property stores its own range and distribution."));

					auto& spec = distributionSpecs[selectedEmissionDistributionIndex];
					ImGui::PushID(static_cast<int>(selectedEmissionDistributionIndex));
					settingRow(spec.minimumLabel);
					steppedSlider(
						"##DistributionMinimum",
						spec.range->minimum,
						spec.lowerBound,
						spec.upperBound,
						spec.step,
						spec.format);
					explainLastItem(spec.minimumTooltip);
					settingRow(spec.maximumLabel);
					steppedSlider(
						"##DistributionMaximum",
						spec.range->maximum,
						spec.lowerBound,
						spec.upperBound,
						spec.step,
						spec.format);
					explainLastItem(spec.maximumTooltip);
					if (spec.range->minimum > spec.range->maximum)
						std::swap(spec.range->minimum, spec.range->maximum);

					const std::array distributionModes{
						DistributionMode::kUniform,
						DistributionMode::kNormal,
						DistributionMode::kBiasLow,
						DistributionMode::kBiasHigh
					};
					const std::array distributionLabels{
						T(TKEY("distribution_uniform"), "Uniform"),
						T(TKEY("distribution_normal"), "Normal"),
						T(TKEY("distribution_bias_low"), "Bias low"),
						T(TKEY("distribution_bias_high"), "Bias high")
					};
					auto selectedMode = static_cast<std::size_t>(spec.range->distribution);
					if (selectedMode >= distributionModes.size()) {
						selectedMode = 0;
						spec.range->distribution = DistributionMode::kUniform;
					}
					settingRow(T(TKEY("distribution_mode"), "Distribution"));
					if (ImGui::BeginCombo("##DistributionMode", distributionLabels[selectedMode])) {
						for (std::size_t index = 0; index < distributionModes.size(); ++index) {
							const bool selected = index == selectedMode;
							if (ImGui::Selectable(distributionLabels[index], selected)) {
								spec.range->distribution = distributionModes[index];
								selectedMode = index;
							}
							if (selected)
								ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					explainLastItem(T(
						TKEY("distribution_mode_tooltip"),
						"Uniform uses an even core, Normal favors the center, and Bias low/high shift the mean. For launch speed and angles, low/high are soft 5th/95th percentile anchors and rare tails can exceed them; lifetime and size remain bounded."));
					ImGui::PopID();
					ImGui::EndTable();
				}
				ImGui::TreePop();
			}

			if (ImGui::Button(T(TKEY("reset_profile"), "Reset profile"))) {
				profile.enabled = true;
				profile.suppressOriginal = !isOnHit;
				profile.emission = {};
				bloodMeshProfileMessage = T(TKEY("profile_reset_message"), "Profile restored to Physical Blood defaults.");
				bloodMeshProfileMessageIsError = false;
			}
			bool removeProfile = false;
			if (!profile.builtIn) {
				ImGui::SameLine();
				removeProfile = ImGui::Button(T(TKEY("remove_custom_profile"), "Remove custom profile"));
			}
			ImGui::PopID();
			if (removeProfile) {
				profiles.erase(profiles.begin() + static_cast<std::ptrdiff_t>(selectedIndex));
				selectedIndex = selectedIndex == 0 ? 0 : selectedIndex - 1;
				bloodMeshProfileMessage = T(TKEY("custom_profile_removed"), "Custom NIF profile removed.");
				bloodMeshProfileMessageIsError = false;
			}
		}
		selectedBloodMeshProfileIndex = selectedIndex;
		settings.hijackBloodMeshes = hijackingEnabled;
		if (!currentProfiles || profiles != *currentProfiles)
			PublishBloodMeshProfiles(std::move(profiles));
		// Publish the profile snapshot before enabling hook-side reads.
		hookNifHijackingEnabled.store(hijackingEnabled, std::memory_order_release);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("physics"), "Physics"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("gravity"), "Gravity"), &settings.gravity, 200.0F, 1800.0F, "%.0f");
		explainLastItem(T(TKEY("gravity_tooltip"), "Downward acceleration applied to airborne droplets in Skyrim world units per second squared."));
		ImGui::SliderFloat(T(TKEY("drag"), "Drag"), &settings.drag, 0.0F, 2.5F, "%.2f");
		explainLastItem(T(TKEY("drag_tooltip"), "Air resistance that reduces droplet velocity over time. Higher values shorten and soften the spray trajectory."));
		ImGui::SliderFloat(T(TKEY("splat_lifetime"), "Splat lifetime"), &settings.splatLifetime, 1.0F, 60.0F, T(TKEY("seconds_1_format"), "%.1f s"));
		explainLastItem(T(TKEY("splat_lifetime_tooltip"), "How long a ground or static-surface splat remains visible after a droplet collides."));
		ImGui::SliderInt(
			T(TKEY("maximum_splats"), "Maximum blood splats"),
			&maximumSplatsDraft,
			kMinimumOwnedSplatCapacity,
			kMaximumOwnedSplatCapacity,
			"%d",
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		const bool commitSplatCapacity = ImGui::IsItemDeactivatedAfterEdit();
		explainLastItem(T(
			TKEY("maximum_splats_tooltip"),
			"Maximum simultaneous surface splats and the size of their GPU buffer. Applying a new value clears current splats; larger values use more VRAM and can increase rendering cost when many splats are visible."));
		if (commitSplatCapacity) {
			constexpr std::int32_t capacityStep = 1024;
			maximumSplatsDraft = std::clamp(
				((maximumSplatsDraft + capacityStep / 2) / capacityStep) * capacityStep,
				kMinimumOwnedSplatCapacity,
				kMaximumOwnedSplatCapacity);
			settings.maximumSplats = maximumSplatsDraft;
		}
		const auto displayedCapacity = allocatedOwnedSplatCapacity != 0U ?
			                               allocatedOwnedSplatCapacity :
			                               static_cast<std::uint32_t>(settings.maximumSplats);
		ImGui::TextDisabled(
			T(TKEY("splat_storage_status"), "Allocated: %u splats | %.1f MiB"),
			displayedCapacity,
			static_cast<double>(displayedCapacity) * sizeof(OwnedSplatGPU) /
				(1024.0 * 1024.0));
		ImGui::TextDisabled("%s", T(TKEY("collision_desc"), "Collision: actor-free terrain/static depth (no synthetic floor)"));
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T(TKEY("water_dispersion"), "Water dispersion"), ImGuiTreeNodeFlags_DefaultOpen)) {
		steppedSlider(T(TKEY("water_update_rate"), "Simulation update rate"), settings.waterUpdateRate, 20.0F, 120.0F, 5.0F, T(TKEY("hz_format"), "%.0f Hz"));
		explainLastItem(T(TKEY("water_update_rate_tooltip"), "How often the water-blood advection compute pass runs. Lower values improve performance; rendering and droplet collision remain full-rate."));
		ImGui::TextDisabled("%s", T(TKEY("water_full_rate_desc"), "Rendering and particle collision remain full-rate"));
		ImGui::SliderFloat(T(TKEY("flow_speed"), "Flow speed"), &settings.waterFlowSpeed, 0.0F, 600.0F, "%.0f");
		explainLastItem(T(TKEY("flow_speed_tooltip"), "Scales movement from the authored water flowmap. Zero stops directional transport while diffusion can still spread the dye."));
		ImGui::SliderFloat(T(TKEY("diffusion"), "Diffusion"), &settings.waterDiffusion, 0.0F, 1.0F, "%.2f");
		explainLastItem(T(TKEY("diffusion_tooltip"), "Controls how quickly blood spreads outward into neighboring water-density samples."));
		ImGui::SliderFloat(T(TKEY("dissipation"), "Dissipation"), &settings.waterDissipation, 0.0F, 1.0F, "%.2f");
		explainLastItem(T(TKEY("dissipation_tooltip"), "Controls how quickly blood density fades from the water over time."));
		steppedSlider(T(TKEY("initial_water_size_multiplier"), "Initial size multiplier"), settings.waterImpactSizeMultiplier, 0.5F, 5.0F, 0.1F, T(TKEY("multiplier_format"), "%.1fx"));
		explainLastItem(T(TKEY("initial_water_size_multiplier_tooltip"), "Scales the initial water stain relative to the incoming droplet. At 1.0x, the stain begins at the droplet's own size and disperses afterward."));
		steppedSlider(T(TKEY("initial_cloud_strength"), "Initial cloud strength"), settings.waterImpactStrength, 0.10F, 3.00F, 0.05F, "%.2f");
		explainLastItem(T(TKEY("initial_cloud_strength_tooltip"), "Amount of dye injected per water impact. Higher values create a darker initial blood cloud."));
		ImGui::TextDisabled("%s", T(TKEY("flowmap_only_desc"), "Only authored/generated flowmap motion is used"));
		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX
