#pragma once

#include "RE/A/Actor.h"
#include "RE/B/BSGeometry.h"
#include "RE/B/BSXFlags.h"
#include "RE/P/Projectile.h"
#include "RE/T/TESObjectREFR.h"

#include <cstdint>

// Shared geometry classification for SSPP perspective and orthographic capture.
// The policy excludes animated, transient, skinned, and unsupported geometry while
// retaining terrain, explicit static references, and anonymous static scene nodes.
namespace ScreenSpacePhysicalParticles::Utils
{
	struct GeometryOwnership
	{
		bool hasReference{};
		bool explicitStatic{};
		bool invalidReference{};
		bool dynamicReference{};
		bool dynamicSceneFlags{};
	};

	// Summary: Classifies reference ownership and dynamic scene flags along an object's ancestry.
	// Args:
	//   a_object: Geometry or ancestor from which to begin the parent walk.
	// Returns: Ownership attributes used by the shared solid-capture policy.
	[[nodiscard]] inline GeometryOwnership ClassifyGeometryOwnership(RE::NiAVObject* a_object) noexcept
	{
		GeometryOwnership result{};
		using BSXFlag = RE::BSXFlags::Flag;
		constexpr auto rejectedBSX =
			static_cast<std::int32_t>(BSXFlag::kRagdoll) |
			static_cast<std::int32_t>(BSXFlag::kEditorMarker) |
			static_cast<std::int32_t>(BSXFlag::kDynamic) |
			static_cast<std::int32_t>(BSXFlag::kAddon) |
			static_cast<std::int32_t>(BSXFlag::kNeedsTransformUpdate) |
			static_cast<std::int32_t>(BSXFlag::kMagicShaderParticles) |
			static_cast<std::int32_t>(BSXFlag::kLights) |
			static_cast<std::int32_t>(BSXFlag::kBreakable) |
			static_cast<std::int32_t>(BSXFlag::kSearchedBreakable);
		for (auto* object = a_object; object; object = object->parent) {
			if (auto* fadeNode = object->AsFadeNode()) {
				if (auto* extraData = fadeNode->GetExtraData("BSX")) {
					const auto value = static_cast<const RE::BSXFlags*>(extraData)->value;
					result.dynamicSceneFlags |=
						(static_cast<std::int32_t>(value) & rejectedBSX) != 0;
				}
			}
		}
		if (auto* owner = a_object ? a_object->GetUserData() : nullptr) {
			result.hasReference = true;
			if (owner->As<RE::Actor>() || owner->As<RE::Projectile>()) {
				result.dynamicReference = true;
			} else if (owner->IsDeleted() || owner->IsDisabled()) {
				result.invalidReference = true;
			} else if (const auto* base = owner->GetBaseObject()) {
				result.explicitStatic =
					base->Is(RE::FormType::Static, RE::FormType::StaticCollection);
			}
		}
		return result;
	}

	namespace detail
	{
		// Summary: Tests whether a geometry subtype is unsupported by solid capture.
		// Args:
		//   a_geometry: Geometry to classify.
		// Returns: True for null, particle, dynamic, line, or instanced geometry.
		[[nodiscard]] inline bool IsUnsupportedGeometryType(RE::BSGeometry* a_geometry) noexcept
		{
			if (!a_geometry)
				return true;
			using Type = RE::BSGeometry::Type;
			const auto type = a_geometry->GetType().get();
			return type == Type::kParticles || type == Type::kStripParticles ||
				type == Type::kDynamicTriShape || type == Type::kMultiStreamInstanceTriShape ||
				type == Type::kParticleShaderDynamicTriShape || type == Type::kLines ||
				type == Type::kDynamicLines || type == Type::kInstanceGroup;
		}
	}

	// Summary: Applies renderer-format and ownership exclusions common to solid captures.
	// Args:
	//   a_geometry: Geometry to test.
	//   a_shaderProperty: Shader property used by the geometry's draw.
	//   a_ownership: Previously classified ownership attributes.
	// Returns: True when the geometry can safely represent a solid surface.
	[[nodiscard]] inline bool IsSolidCaptureCandidate(
		RE::BSGeometry* a_geometry,
		const RE::BSShaderProperty* a_shaderProperty,
		const GeometryOwnership& a_ownership) noexcept
	{
		if (!a_geometry)
			return false;
		if (!a_shaderProperty)
			return false;

		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto& flags = a_shaderProperty->flags;
		if (flags.any(
				Flag::kSkinned,
				Flag::kCharacterLighting,
				Flag::kFace,
				Flag::kFaceGenRGBTint,
				Flag::kWeaponBlood,
				Flag::kTreeAnim,
				Flag::kBillboard))
			return false;

		const auto& geometryData = a_geometry->GetGeometryRuntimeData();
		if (geometryData.skinInstance)
			return false;
		if (geometryData.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
			return false;
		if (detail::IsUnsupportedGeometryType(a_geometry))
			return false;

		if (a_ownership.dynamicReference)
			return false;
		if (a_ownership.invalidReference)
			return false;
		return true;
	}

	// Summary: Restricts solid candidates to terrain, explicit statics, or anonymous static nodes.
	// Args:
	//   a_geometry: Geometry to test.
	//   a_shaderProperty: Shader property used by the geometry's draw.
	//   a_ownership: Previously classified ownership attributes.
	// Returns: True when the geometry belongs in SSPP's solid capture set.
	[[nodiscard]] inline bool IsSolidSceneGeometry(
		RE::BSGeometry* a_geometry,
		const RE::BSShaderProperty* a_shaderProperty,
		const GeometryOwnership& a_ownership) noexcept
	{
		if (!IsSolidCaptureCandidate(a_geometry, a_shaderProperty, a_ownership))
			return false;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (a_shaderProperty->flags.all(Flag::kMultiTextureLandscape) ||
			a_ownership.explicitStatic)
			return true;
		return !a_ownership.hasReference && !a_ownership.dynamicSceneFlags;
	}

}
