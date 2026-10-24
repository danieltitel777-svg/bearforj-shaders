#include "BearforjWorldQuery.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace BearforjParticles
{
	namespace
	{
		std::string LowerCopy(const char* a_text)
		{
			std::string value = a_text ? a_text : "";
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char a_char) {
				return static_cast<char>(std::tolower(a_char));
			});
			return value;
		}

		bool Contains(std::string_view a_haystack, std::string_view a_needle)
		{
			return a_haystack.find(a_needle) != std::string_view::npos;
		}

		std::string FormText(const RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			std::string text = LowerCopy(a_form->GetFormEditorID());
			text.push_back(' ');
			text += LowerCopy(a_form->GetName());
			if (const auto* model = a_form->As<RE::TESModel>()) {
				text.push_back(' ');
				text += LowerCopy(model->GetModel());
			}
			return text;
		}

		std::string ModelText(const RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			const auto* model = a_form->As<RE::TESModel>();
			return model ? LowerCopy(model->GetModel()) : std::string{};
		}

		float CurrentHour() noexcept
		{
			const auto* calendar = RE::Calendar::GetSingleton();
			return calendar ? calendar->GetHour() : 12.0F;
		}

		bool IsBlackreach() noexcept
		{
			const auto* player = RE::PlayerCharacter::GetSingleton();
			const auto* cell = player ? player->GetParentCell() : nullptr;
			const auto* worldspace = cell ? cell->GetRuntimeData().worldSpace : nullptr;
			return Contains(LowerCopy(worldspace ? worldspace->GetFormEditorID() : nullptr), "blackreach");
		}

		std::uint32_t RegionFromText(std::string_view a_text) noexcept
		{
			if (Contains(a_text, "solstheim"))
				return 3;
			if (Contains(a_text, "falkreath") || Contains(a_text, "rift"))
				return 0;
			if (Contains(a_text, "whiterun"))
				return 1;
			if (Contains(a_text, "hjaalmarch") || Contains(a_text, "morthal"))
				return 4;
			if (Contains(a_text, "markarth") || Contains(a_text, "reach"))
				return 2;
			return 5;
		}

		bool SpellTextHasWard(const RE::MagicItem* a_spell)
		{
			if (!a_spell)
				return false;
			const auto spellText = FormText(a_spell);
			if (Contains(spellText, "ward"))
				return true;
			for (const auto* effect : a_spell->effects) {
				if (!effect || !effect->baseEffect)
					continue;
				if (Contains(FormText(effect->baseEffect), "ward"))
					return true;
			}
			return false;
		}

		float PackColor(std::uint8_t a_red, std::uint8_t a_green, std::uint8_t a_blue) noexcept
		{
			if (a_red < 8 && a_green < 8 && a_blue < 8) {
				a_red = 255;
				a_green = 255;
				a_blue = 255;
			}
			const auto packed = (static_cast<std::uint32_t>(a_red) << 16U) |
			                    (static_cast<std::uint32_t>(a_green) << 8U) |
			                    static_cast<std::uint32_t>(a_blue);
			return static_cast<float>(packed);
		}
	}

	std::string_view SpeciesName(std::uint32_t a_species) noexcept
	{
		return a_species < kSpeciesCount ? kSpeciesNames[a_species] : kSpeciesNames[2];
	}

	std::string_view RegionName(std::uint32_t a_region) noexcept
	{
		return a_region < kRegionCount ? kRegionNames[a_region] : kRegionNames[kRegionCount - 1];
	}

	namespace
	{
		RegionTintColor g_regionTints[kRegionCount] = {
			{ 0.55F, 0.78F, 0.36F },
			{ 0.90F, 0.78F, 0.46F },
			{ 0.62F, 0.66F, 0.45F },
			{ 0.70F, 0.66F, 0.60F },
			{ 0.40F, 0.58F, 0.48F },
			{ 1.0F, 1.0F, 1.0F }
		};
	}

	RegionTintColor RegionTint(std::uint32_t a_region) noexcept
	{
		return a_region < kRegionCount ? g_regionTints[a_region] : g_regionTints[kRegionCount - 1];
	}

	void SetRegionTint(std::uint32_t a_region, float a_red, float a_green, float a_blue) noexcept
	{
		if (a_region >= kRegionCount)
			return;
		g_regionTints[a_region] = { a_red, a_green, a_blue };
	}

	std::uint32_t SpeciesIndex(const RE::TESForm* a_base) noexcept
	{
		const auto model = ModelText(a_base);
		if (Contains(model, "aspen"))
			return 0;
		if (Contains(model, "birch"))
			return 1;
		if (Contains(model, "oak"))
			return 2;
		if (Contains(model, "maple"))
			return 3;
		if (Contains(model, "pine") || Contains(model, "fir") || Contains(model, "spruce") ||
			Contains(model, "needle") || Contains(model, "snow"))
			return 4;
		return 2;
	}

	std::uint32_t CurrentRegionIndex() noexcept
	{
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		if (!player || !cell)
			return 5;

		std::string text;
		if (const auto* worldspace = cell->GetRuntimeData().worldSpace)
			text += LowerCopy(worldspace->GetFormEditorID());
		for (auto* location = player->GetCurrentLocation(); location; location = location->parentLoc) {
			text.push_back(' ');
			text += LowerCopy(location->GetFormEditorID());
			text.push_back(' ');
			text += LowerCopy(location->GetName());
		}
		const auto region = RegionFromText(text);
		return region;
	}

	bool IsTreeReference(const RE::TESObjectREFR* a_ref) noexcept
	{
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return false;
		const auto* base = a_ref->GetBaseObject();
		if (!base)
			return false;
		if (base->GetFormType() == RE::FormType::Tree)
			return true;
		const auto model = ModelText(base);
		return Contains(model, "trees") || Contains(model, "treepine") || Contains(model, "treeaspen");
	}

	bool IsGlowingMushroom(const RE::TESObjectREFR* a_ref) noexcept
	{
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return false;
		const auto text = FormText(a_ref->GetBaseObject());
		const bool mushroom = Contains(text, "mushroom") || Contains(text, "fung");
		const bool glowing = Contains(text, "glow") || Contains(text, "spore");
		return mushroom && glowing;
	}

	bool IsGrassReference(const RE::TESObjectREFR* a_ref) noexcept
	{
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return false;
		const auto* base = a_ref->GetBaseObject();
		if (!base || IsTreeReference(a_ref))
			return false;
		if (base->GetFormType() == RE::FormType::Grass)
			return true;
		const auto model = ModelText(base);
		return Contains(model, "grass");
	}

	bool IsLampReference(const RE::TESObjectREFR* a_ref) noexcept
	{
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return false;
		const auto* base = a_ref->GetBaseObject();
		if (!base || base->GetFormType() != RE::FormType::Light)
			return false;
		const auto text = FormText(base);
		return Contains(text, "lamp") || Contains(text, "lantern") || Contains(text, "torch") ||
		       Contains(text, "candle") || Contains(text, "sconce") || Contains(text, "brazier");
	}

	bool IsEvening() noexcept
	{
		if (IsBlackreach())
			return true;
		const auto hour = CurrentHour();
		return hour >= 18.0F || hour < 6.0F;
	}

	bool IsNight() noexcept
	{
		if (IsBlackreach())
			return true;
		const auto hour = CurrentHour();
		return hour >= 20.0F || hour < 5.0F;
	}

	bool CurrentCellHasWater() noexcept
	{
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		return cell && cell->cellFlags.any(RE::TESObjectCELL::Flag::kHasWater);
	}

	bool TrySpellColor(const RE::MagicItem* a_spell, float& a_packed) noexcept
	{
		a_packed = PackColor(255, 255, 255);
		if (!a_spell || a_spell->GetFormType() == RE::FormType::Shout)
			return false;
		if (SpellTextHasWard(a_spell))
			return false;

		for (const auto* effect : a_spell->effects) {
			const auto* base = effect ? effect->baseEffect : nullptr;
			if (!base)
				continue;
			if (const auto* shader = base->data.effectShader) {
				const auto& color = shader->data.colorKey1;
				a_packed = PackColor(color.red, color.green, color.blue);
				return true;
			}
		}
		for (const auto* effect : a_spell->effects) {
			const auto* base = effect ? effect->baseEffect : nullptr;
			if (!base || !base->data.light)
				continue;
			const auto& color = base->data.light->data.color;
			a_packed = PackColor(color.red, color.green, color.blue);
			return true;
		}
		return true;
	}
}
