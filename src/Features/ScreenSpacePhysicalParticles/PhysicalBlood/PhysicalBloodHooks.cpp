#include "PhysicalBlood.h"

#include "Features/ScreenSpacePhysicalParticles/NifPath.h"
#include "Features/ScreenSpacePhysicalParticles/ScreenSpacePhysicalParticles.h"
#include "Features/UnifiedWater.h"
#include "Globals.h"

#include <algorithm>
#include <cmath>

// This translation unit is the gameplay-thread boundary. One hook observes
// resolved player melee hits; the other observes Skyrim temporary particle NIF
// creation. Both convert eligible events into compact SSPP burst requests using
// immutable profile and routing snapshots, then chain to Skyrim unless a matched
// NIF profile explicitly suppresses the original effect after a successful queue.

namespace NifPath = ScreenSpacePhysicalParticles::NifPath;

void PhysicalBlood::PostPostLoad()
{
	if (!globals::features::screenSpacePhysicalParticles.loaded) {
		loaded = false;
		hookFeatureEnabled.store(false, std::memory_order_release);
		failedLoadedMessage = "Physical Blood requires Screen-Space Physical Particles";
		logger::error("[Physical Blood] Screen-Space Physical Particles is not loaded");
		return;
	}

	UnifiedWater::SetPostSetupGeometryObserver([] {
		globals::features::physicalBlood.BindWaterShaderResources();
	});
	UnifiedWater::InstallSetupGeometryHook();

	if (REL::Module::IsVR()) {
		logger::warn("[Physical Blood] Native emission hooks are unavailable on VR");
		return;
	}

	// The Euler entry point forwards to this matrix overload.
	stl::detour_thunk<ParticleEffectSpawnHook>(REL::RelocationID(29219, 30072));

	REL::Relocation<std::uintptr_t> processHit{ REL::RelocationID(37673, 38627) };
	const auto callSite = processHit.address() + REL::Relocate(0x3C0, 0x4A8);
	const auto opcode = *reinterpret_cast<const std::uint8_t*>(callSite);

	if (opcode != 0xE8) {
		logger::critical(
			"[Physical Blood] On Hit hook rejected at 0x{:X}: expected CALL, found 0x{:02X}",
			callSite,
			opcode);
		return;
	}

	stl::write_thunk_call<ActorProcessHitHook>(callSite);
	logger::info("[Physical Blood] Installed chained Actor::ProcessHit hook at 0x{:X}", callSite);
}

// Summary: Replaces matching blood-effect NIF spawns with SSPP bursts.
// Args:
//   a_cell: Cell that would own the temporary effect.
//   a_lifetime: Requested effect lifetime.
//   a_modelName: Relative NIF path selected by Skyrim.
//   a_rotation: Effect orientation; its Y axis supplies burst direction.
//   a_position: World-space effect origin.
//   a_scale: Requested effect scale.
//   a_flags: Temporary-effect creation flags.
//   a_target: Optional scene object receiving the effect.
// Returns: The original effect, or nullptr when its mesh is suppressed.
RE::BSTempEffectParticle* PhysicalBlood::ParticleEffectSpawnHook::thunk(
	RE::TESObjectCELL* a_cell,
	float a_lifetime,
	const char* a_modelName,
	const RE::NiMatrix3& a_rotation,
	const RE::NiPoint3& a_position,
	float a_scale,
	std::uint32_t a_flags,
	RE::NiAVObject* a_target)
{
	auto& feature = globals::features::physicalBlood;
	if (!globals::features::screenSpacePhysicalParticles.IsKindEnabled(
			ScreenSpacePhysicalParticlesFeature::ParticleKind::kBlood)) {
		return func(a_cell, a_lifetime, a_modelName, a_rotation, a_position, a_scale, a_flags, a_target);
	}
	const bool suppressOriginal =
		a_modelName && feature.HandleEffectSpawn(a_position, a_rotation.GetVectorY(), a_modelName);
	if (suppressOriginal)
		return nullptr;
	return func(a_cell, a_lifetime, a_modelName, a_rotation, a_position, a_scale, a_flags, a_target);
}

// Summary: Enqueues configured player melee hits before chaining to Skyrim.
// Args:
//   a_target: Actor receiving the resolved hit.
//   a_hitData: Resolved hit position, direction, weapon, and flags.
// Returns: Nothing.
void PhysicalBlood::ActorProcessHitHook::thunk(RE::Actor* a_target, RE::HitData& a_hitData)
{
	auto& feature = globals::features::physicalBlood;

	try {
		if (feature.hookFeatureEnabled.load(std::memory_order_acquire) &&
			globals::features::screenSpacePhysicalParticles.IsKindEnabled(
				ScreenSpacePhysicalParticlesFeature::ParticleKind::kBlood)) {
			const auto profiles = feature.LoadBloodMeshProfiles();
			if (profiles) {
				const auto onHit = std::ranges::find_if(*profiles, [](const auto& a_profile) {
					return a_profile.matchMode == NifMatchMode::kOnHit;
				});
				if (onHit != profiles->end() && onHit->enabled) {
					auto aggressor = a_hitData.aggressor.get();
					(void)feature.QueueResolvedHit(
						a_hitData, aggressor.get(), a_target, onHit->emission);
				}
			}
		}
	} catch (const std::exception& e) {
		logger::error("[Physical Blood] Hit capture failed: {}", e.what());
	} catch (...) {
		logger::error("[Physical Blood] Hit capture failed with an unknown exception");
	}

	func(a_target, a_hitData);
}

// Summary: Advances the lock-free xorshift seed used for burst requests.
// Args: None.
// Returns: The next 32-bit pseudorandom value.
std::uint32_t PhysicalBlood::NextRandom() noexcept
{
	auto current = nextSeed.load(std::memory_order_relaxed);
	for (;;) {
		auto next = current;
		next ^= next << 13;
		next ^= next >> 17;
		next ^= next << 5;
		if (nextSeed.compare_exchange_weak(
				current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
			return next;
		}
	}
}

// Summary: Validates a resolved hit and queues its configured burst.
// Args:
//   a_hitData: Resolved Skyrim hit information.
//   a_aggressor: Actor responsible for the hit.
//   a_target: Actor receiving the hit.
//   a_emission: On-hit emission profile.
// Returns: True when the hit qualifies and SSPP accepts the burst.
bool PhysicalBlood::QueueResolvedHit(
	const RE::HitData& a_hitData,
	RE::Actor* a_aggressor,
	RE::Actor* a_target,
	const EmissionSettings& a_emission)
{
	if (!a_aggressor || !a_target)
		return false;

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player || a_aggressor != player || !a_target->Is3DLoaded() ||
		a_hitData.flags.any(RE::HitData::Flag::kBlocked))
		return false;

	const auto isBash = a_hitData.flags.any(RE::HitData::Flag::kBash);
	const auto isMeleeWeapon = a_hitData.weapon && a_hitData.weapon->IsMelee();
	const auto isUnarmedAttack = !a_hitData.weapon && a_hitData.attackData && !a_hitData.attackDataSpell;
	if (!isMeleeWeapon && !isUnarmedAttack && !isBash)
		return false;

	const auto targetBase = a_target->GetPosition();
	auto origin = a_hitData.hitPosition;
	// Summary: Tests whether all coordinates can safely enter a GPU burst request.
	// Args:
	//   a_point: Position or direction to inspect.
	// Returns: True when every component is finite.
	const auto finitePoint = [](const RE::NiPoint3& a_point) {
		return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
	};
	const auto targetHeight = a_target->GetHeight();
	const auto maxHitOffset = std::max(targetHeight + 192.0F, 320.0F);
	if (!finitePoint(origin) ||
		origin.GetSquaredDistance(targetBase) > maxHitOffset * maxHitOffset) {
		origin = targetBase;
		origin.z += std::clamp(targetHeight * 0.55F, 48.0F, 108.0F);
	}

	auto direction = a_hitData.hitDirection;
	if (!finitePoint(direction) || direction.SqrLength() < 1.0e-5F) {
		const auto attackerPosition = a_aggressor->GetPosition();
		direction = {
			origin.x - attackerPosition.x,
			origin.y - attackerPosition.y,
			(origin.z - attackerPosition.z) * 0.2F
		};
	}
	direction = NormalizeDirection(direction);

	const auto strength =
		a_hitData.flags.any(RE::HitData::Flag::kPowerAttack) ? 1.65F : 1.0F;
	if (!hookFeatureEnabled.load(std::memory_order_acquire))
		return false;
	return QueueSharedBurst(origin, direction, strength, NextRandom(), 0U, a_emission);
}

// Summary: Matches a temporary-effect NIF and queues its configured burst.
// Args:
//   a_position: World-space effect origin.
//   a_direction: Effect-local forward direction.
//   a_modelName: Relative NIF path requested by Skyrim.
// Returns: True only when the burst was queued and the original NIF should be suppressed.
bool PhysicalBlood::HandleEffectSpawn(
	const RE::NiPoint3& a_position,
	const RE::NiPoint3& a_direction,
	std::string_view a_modelName)
{
	if (!hookFeatureEnabled.load(std::memory_order_acquire) ||
		!hookNifHijackingEnabled.load(std::memory_order_acquire)) {
		return false;
	}
	const auto normalized = NifPath::Normalize(a_modelName);
	if (!IsValidNifPath(normalized))
		return false;
	const auto filename = NifPath::Filename(normalized);

	EmissionSettings emission{};
	bool suppressOriginal{};
	std::uint32_t profilePayloadID{};
	if (const auto profiles = LoadBloodMeshProfiles()) {
		auto matched = profiles->end();
		// Exact-path entries override filename matches, including disabled entries.
		for (auto it = profiles->begin(); it != profiles->end(); ++it) {
			if (it->matchMode == NifMatchMode::kExactPath && it->modelPath == normalized) {
				matched = it;
				break;
			}
		}
		if (matched == profiles->end()) {
			for (auto it = profiles->begin(); it != profiles->end(); ++it) {
				if (it->matchMode == NifMatchMode::kFilename && NifPath::Filename(it->modelPath) == filename) {
					matched = it;
					break;
				}
			}
		}
		if (matched == profiles->end())
			return false;

		if (!matched->enabled)
			return false;
		emission = matched->emission;
		suppressOriginal = matched->suppressOriginal;
		profilePayloadID = static_cast<std::uint32_t>(
			std::distance(profiles->begin(), matched) + 1);
	} else {
		return false;
	}

	const auto direction = NormalizeDirection(a_direction);
	if (!hookFeatureEnabled.load(std::memory_order_acquire) ||
		!hookNifHijackingEnabled.load(std::memory_order_acquire))
		return false;
	if (!QueueSharedBurst(
		a_position,
		direction,
		1.0F,
		NextRandom(),
		profilePayloadID,
		emission)) {
		return false;
	}
	return suppressOriginal;
}
