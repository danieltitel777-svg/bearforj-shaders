#pragma once

#include <cstdint>
#include <string_view>

namespace RE
{
	class TESForm;
	class TESObjectREFR;
	class MagicItem;
	class TESObjectCELL;
}

namespace BearforjParticles
{
	inline constexpr std::uint32_t kSpeciesCount = 5;
	inline constexpr std::uint32_t kRegionCount = 6;
	inline constexpr std::uint32_t kLeafFrameCount = 4;
	inline constexpr const char* kSpeciesNames[] = { "aspen", "birch", "oak", "maple", "pine" };
	inline constexpr const char* kRegionNames[] = {
		"falkreath_rift",
		"whiterun_tundra",
		"reach",
		"solstheim",
		"pine_hjaal",
		"neutral"
	};

	// Summary: Returns the species key for a texture-array row.
	// Args:
	//   a_species: Species index in kSpeciesNames.
	// Returns: Lowercase species name, or "pine" when the index is out of range.
	[[nodiscard]] std::string_view SpeciesName(std::uint32_t a_species) noexcept;

	// Summary: Returns the region key for a texture-array column.
	// Args:
	//   a_region: Region index in kRegionNames.
	// Returns: Lowercase region name, or "default" when the index is out of range.
	[[nodiscard]] std::string_view RegionName(std::uint32_t a_region) noexcept;

	// Summary: Classifies a tree form from its model path.
	// Args:
	//   a_base: Base form of a placed reference.
	// Returns: Species index. Unrecognized trees use pine.
	[[nodiscard]] std::uint32_t SpeciesIndex(const RE::TESForm* a_base) noexcept;

	// Summary: Resolves the player's hold, with Solstheim and Blackreach handled first.
	// Args: None.
	// Returns: Region index. Unknown locations use default.
	[[nodiscard]] std::uint32_t CurrentRegionIndex() noexcept;

	// Summary: Tests whether a reference should drop leaves.
	// Args:
	//   a_ref: Candidate world reference.
	// Returns: True for Tree forms and tree model paths.
	[[nodiscard]] bool IsTreeReference(const RE::TESObjectREFR* a_ref) noexcept;

	// Summary: Tests whether a reference is a glowing mushroom.
	// Args:
	//   a_ref: Candidate world reference.
	// Returns: True when the name or model identifies a glowing mushroom.
	[[nodiscard]] bool IsGlowingMushroom(const RE::TESObjectREFR* a_ref) noexcept;

	// Summary: Tests whether a reference is grass or a grass clump.
	// Args:
	//   a_ref: Candidate world reference.
	// Returns: True for grass forms and grass models that are not trees.
	[[nodiscard]] bool IsGrassReference(const RE::TESObjectREFR* a_ref) noexcept;

	// Summary: Tests whether a placed light should attract moths.
	// Args:
	//   a_ref: Candidate world reference.
	// Returns: True for lamp, lantern, torch, candle, sconce, and brazier lights.
	[[nodiscard]] bool IsLampReference(const RE::TESObjectREFR* a_ref) noexcept;

	// Summary: Reports evening hours, treating Blackreach as always dark.
	// Args: None.
	// Returns: True from 18:00 through 06:00, and in Blackreach.
	[[nodiscard]] bool IsEvening() noexcept;

	// Summary: Reports night hours, treating Blackreach as always dark.
	// Args: None.
	// Returns: True from 20:00 through 05:00, and in Blackreach.
	[[nodiscard]] bool IsNight() noexcept;

	// Summary: Tests whether the player's current cell has a water plane.
	// Args: None.
	// Returns: True when the current cell is flagged as having water.
	[[nodiscard]] bool CurrentCellHasWater() noexcept;

	// Summary: Packs a spell tint into the particle general-scalar channel.
	// Args:
	//   a_spell: Spell, scroll, or enchantment being cast.
	//   a_packed: Receives an RGB888 value stored in a float.
	// Returns: False for shouts, wards, or a missing spell.
	[[nodiscard]] bool TrySpellColor(const RE::MagicItem* a_spell, float& a_packed) noexcept;
}
