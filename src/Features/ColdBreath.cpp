#include "ColdBreath.h"

#include "Bearforj/TipUI.h"
#include "Globals.h"

#define I18N_KEY_PREFIX "feature.cold_breath."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ColdBreath::Settings,
	Enabled, Interval, Lifetime, Range)

namespace
{
	struct Choice
	{
		const char* node;
		const char* file;
		const char* firstPersonFile;
	};

	const Choice kHuman{ "NPC Head [Head]", "meshes\\Bearforj\\FX\\coldbreath.nif", "meshes\\Bearforj\\FX\\coldbreath1st.nif" };
	const Choice kCanine{ "Canine_Head", "meshes\\Bearforj\\FX\\coldbreathcanine.nif", nullptr };
	const Choice kBear{ "NPC UpperLip", "meshes\\Bearforj\\FX\\coldbreathbear.nif", nullptr };

	bool Snowing()
	{
		auto* sky = RE::Sky::GetSingleton();
		auto* weather = sky ? sky->currentWeather : nullptr;
		return weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
	}

	RE::NiAVObject* LoadBreath(const char* a_path)
	{
		RE::NiPointer<RE::NiNode> model;
		RE::BSModelDB::Demand(a_path, model, RE::BSModelDB::DBTraits::ArgsType{});
		if (!model)
			return nullptr;
		RE::NiCloningProcess process{};
		return netimmerse_cast<RE::NiAVObject*>(model->CreateClone(process));
	}
}

void ColdBreath::Prepass()
{
	if (!loaded || !settings.Enabled)
		return;
	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float dt = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.25f);
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;
	const bool snowing = Snowing();
	const RE::NiPoint3 center = player->GetPosition();
	const bool firstPerson = RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson();

	std::unordered_map<RE::FormID, bool> seen;
	auto visit = [&](RE::Actor* actor) {
		if (!actor || !actor->Is3DLoaded())
			return;
		if (actor != player && actor->GetPosition().GetDistance(center) > settings.Range)
			return;
		seen[actor->GetFormID()] = true;
		auto& slot = slots[actor->GetFormID()];
		slot.age += dt;
		slot.next -= dt;
		if (slot.node && slot.age >= settings.Lifetime) {
			if (auto* parent = slot.node->parent)
				parent->DetachChild(slot.node.get());
			slot.node.reset();
		}
		auto* cell = actor->GetParentCell();
		if (!snowing || !cell || cell->IsInteriorCell() || actor->IsDead() || slot.node || slot.next > 0.0f)
			return;
		const bool first = actor == player && firstPerson;
		auto* root = actor->Get3D(first);
		if (!root)
			return;
		const Choice* choice = nullptr;
		if (root->GetObjectByName(kHuman.node))
			choice = &kHuman;
		else if (root->GetObjectByName(kCanine.node))
			choice = &kCanine;
		else if (root->GetObjectByName(kBear.node))
			choice = &kBear;
		if (!choice)
			return;
		auto* parentNode = root->GetObjectByName(choice->node);
		auto* parent = parentNode ? parentNode->AsNode() : nullptr;
		if (!parent)
			return;
		const char* file = (first && choice->firstPersonFile) ? choice->firstPersonFile : choice->file;
		auto* breath = LoadBreath(file);
		if (!breath) {
			if (!demandFailed) {
				demandFailed = true;
				logger::warn("[Cold Breath] Could not load {}", file);
			}
			return;
		}
		parent->AttachChild(breath, true);
		slot.node.reset(breath);
		slot.age = 0.0f;
		slot.next = settings.Interval;
	};

	visit(player);
	if (auto* lists = RE::ProcessLists::GetSingleton()) {
		for (auto& handle : lists->highActorHandles) {
			if (auto actor = handle.get())
				visit(actor.get());
		}
	}
	std::erase_if(slots, [&](const auto& kv) { return !seen.contains(kv.first); });
}

void ColdBreath::DrawSettings()
{
	BFUI::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	BFUI::SliderFloat(T(TKEY("interval"), "Breath Interval"), &settings.Interval, 0.6f, 6.0f, "%.1f s");
	BFUI::SliderFloat(T(TKEY("lifetime"), "Breath Length"), &settings.Lifetime, 0.4f, 4.0f, "%.1f s");
	BFUI::SliderFloat(T(TKEY("range"), "Range"), &settings.Range, 512.0f, 8192.0f, "%.0f");
	ImGui::TextWrapped("%s", T(TKEY("credit"), "Breath meshes and textures are from Wet and Cold by isparks, CC BY-NC-SA."));
}

void ColdBreath::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ColdBreath::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ColdBreath::RestoreDefaultSettings()
{
	settings = {};
	slots.clear();
}
