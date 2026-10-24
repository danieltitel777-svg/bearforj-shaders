#include "PhysicalLeaves.h"

#include "Deferred.h"
#include "Features/ScreenSpacePhysicalParticles/Collision/PlayerCollisionProvider.h"
#include "Features/ScreenSpacePhysicalParticles/OrthographicHeightField.h"
#include "Features/ScreenSpacePhysicalParticles/BearforjWorldQuery.h"
#include "Features/ScreenSpacePhysicalParticles/ScreenSpacePhysicalParticles.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <DDSTextureLoader.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

#ifndef PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING
#	define PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING 0
#endif

// Implements the Physical Leaves adapter over SSPP. Feature-owned shaders maintain
// ground anchors, wake leaves near the player capsule, and draw shared particle IDs.
namespace
{
	constexpr float kExteriorCellSize = 4096.0F;
	constexpr float kNeighborhoodTileSize = kExteriorCellSize * 0.5F;
	constexpr std::uint32_t kGrassRescanFrames = 15;
	constexpr float kSkyrimUnitsPerMeter = 70.0F;
	constexpr float kDebugLeafFieldHalfExtent = 10.0F * kSkyrimUnitsPerMeter;
	constexpr std::uint32_t kDebugLeafFieldCount = 1024;
	constexpr std::uint32_t kStandaloneFieldFlag = 1U << 0U;

	bool LandTextureHasGrass(const RE::TESLandTexture* a_texture) noexcept
	{
		return a_texture && !a_texture->textureGrassList.empty();
	}

	// Summary: Tests whether any painted layer in a LAND quadrant references grass.
	// Args:
	//   a_land: Loaded landscape data containing texture weights.
	//   a_quadrant: Landscape quadrant index in the range [0, 3].
	// Returns: True when at least one weighted texture can produce grass.
	bool QuadrantHasGrass(
		const RE::TESObjectLAND::LoadedLandData& a_land,
		std::uint32_t a_quadrant) noexcept
	{
		std::array<bool, 6> grassLayers{};
		bool anyGrassLayer = false;
		for (std::uint32_t layer = 0; layer < grassLayers.size(); ++layer) {
			grassLayers[layer] = LandTextureHasGrass(a_land.quadTextures[a_quadrant][layer]);
			anyGrassLayer |= grassLayers[layer];
		}
		const bool grassBase = LandTextureHasGrass(a_land.defQuadTextures[a_quadrant]);
		if (!grassBase && !anyGrassLayer)
			return false;

		// Percent weights are byte values; unclaimed weight belongs to the base texture.
		for (std::uint32_t vertex = 0; vertex < 289; ++vertex) {
			std::uint32_t paintedWeight{};
			for (std::uint32_t layer = 0; layer < grassLayers.size(); ++layer) {
				const auto weight = static_cast<std::uint8_t>(
					a_land.percents[a_quadrant][vertex][layer]);
				paintedWeight += weight;
				if (grassLayers[layer] && weight != 0)
					return true;
			}
			if (grassBase && paintedWeight < 255U)
				return true;
		}
		return false;
	}

	void HashCombine(std::uint64_t& a_hash, std::uint64_t a_value) noexcept
	{
		a_hash ^= a_value;
		a_hash *= 1099511628211ULL;
	}

}

class PhysicalLeaves::SharedRenderer final : public ScreenSpacePhysicalParticles::Core::IParticleRenderer
{
public:
	explicit SharedRenderer(PhysicalLeaves& a_owner) noexcept :
		owner(a_owner) {}

	void SetupResources() override { owner.SetupSharedGraphicsResources(); }
	void Reset() override { owner.ResetSharedClientState(); }
	void ClearShaderCache() override
	{
		owner.sharedVertexShader = nullptr;
		owner.sharedPixelShader = nullptr;
	}
	[[nodiscard]] bool IsReady() const noexcept override { return owner.SharedRendererReady(); }
	void Prepare(const ScreenSpacePhysicalParticles::Core::ParticleRenderContext&) override {}
	void Draw(const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context) override
	{
		owner.DrawSharedParticles(a_context);
	}

private:
	PhysicalLeaves& owner;
};

class PhysicalLeaves::SharedMaintenance final : public ScreenSpacePhysicalParticles::Core::IPersistentParticleMaintenance
{
public:
	explicit SharedMaintenance(PhysicalLeaves& a_owner) noexcept :
		owner(a_owner) {}

	[[nodiscard]] bool IsReady() const noexcept override { return owner.SharedMaintenanceReady(); }
	void Dispatch(const ScreenSpacePhysicalParticles::Core::ParticleMaintenanceContext& a_context) override
	{
		owner.DispatchSharedMaintenance(a_context);
	}

private:
	PhysicalLeaves& owner;
};

class PhysicalLeaves::SharedNewbornInitializer final : public ScreenSpacePhysicalParticles::Core::INewbornParticleInitializer
{
public:
	explicit SharedNewbornInitializer(PhysicalLeaves& a_owner) noexcept :
		owner(a_owner) {}

	void SetupResources() override { owner.SetupSharedGraphicsResources(); }
	void Reset() override {}
	void ClearShaderCache() override { owner.sharedNewbornCS = nullptr; }
	[[nodiscard]] bool IsReady() const noexcept override { return owner.SharedNewbornReady(); }
	void Dispatch(
		const ScreenSpacePhysicalParticles::Core::NewbornInitializationContext& a_context) override
	{
		owner.DispatchSharedNewborn(a_context);
	}

private:
	PhysicalLeaves& owner;
};

PhysicalLeaves::PhysicalLeaves() = default;
PhysicalLeaves::~PhysicalLeaves() = default;

std::pair<std::string, std::vector<std::string>> PhysicalLeaves::GetFeatureSummary()
{
	return {
		T("feature.physical_leaves.description", "Adds persistent GPU-simulated leaf fields for SSPP interaction testing."),
		{ T("feature.physical_leaves.key_feature_1", "Manual player-centered field seeding"),
			T("feature.physical_leaves.key_feature_2", "Player body interaction and GPU flight physics"),
			T("feature.physical_leaves.key_feature_3", "Screen-space collision with orthographic-height fallback") }
	};
}

// Summary: Hashes a scene and tile coordinate into a stable nonzero patch key.
// Args:
//   a_sceneKey: Worldspace or interior-scene identifier.
//   a_tileX: Horizontal tile coordinate.
//   a_tileY: Vertical tile coordinate.
// Returns: Stable nonzero key for the patch.
std::uint64_t PhysicalLeaves::MakePatchKey(
	std::uint32_t a_sceneKey,
	std::int32_t a_tileX,
	std::int32_t a_tileY) const noexcept
{
	std::uint64_t hash = 14695981039346656037ULL;
	HashCombine(hash, a_sceneKey);
	HashCombine(hash, static_cast<std::uint32_t>(a_tileX));
	HashCombine(hash, static_cast<std::uint32_t>(a_tileY));
	return hash == 0 ? 1 : hash;
}

// Summary: Splits a grass-bearing world bound into leaf coverage tiles.
// Args:
//   a_sceneKey: Worldspace or interior-scene identifier.
//   a_boundsMin: Inclusive minimum world bound.
//   a_boundsMax: Exclusive maximum world bound.
// Returns: Number of newly inserted coverage tiles.
std::uint32_t PhysicalLeaves::QueueGrassRegion(
	std::uint32_t a_sceneKey,
	const RE::NiPoint3& a_boundsMin,
	const RE::NiPoint3& a_boundsMax)
{
	if (!std::isfinite(a_boundsMin.x) || !std::isfinite(a_boundsMin.y) || !std::isfinite(a_boundsMin.z) ||
		!std::isfinite(a_boundsMax.x) || !std::isfinite(a_boundsMax.y) || !std::isfinite(a_boundsMax.z) ||
		a_boundsMax.x <= a_boundsMin.x || a_boundsMax.y <= a_boundsMin.y || a_boundsMax.z <= a_boundsMin.z) {
		return 0;
	}

	const auto firstTileX = static_cast<std::int32_t>(std::floor(a_boundsMin.x / kNeighborhoodTileSize));
	const auto lastTileX = static_cast<std::int32_t>(std::floor((a_boundsMax.x - 0.01F) / kNeighborhoodTileSize));
	const auto firstTileY = static_cast<std::int32_t>(std::floor(a_boundsMin.y / kNeighborhoodTileSize));
	const auto lastTileY = static_cast<std::int32_t>(std::floor((a_boundsMax.y - 0.01F) / kNeighborhoodTileSize));
	std::uint32_t insertedCount{};
	for (auto tileY = firstTileY; tileY <= lastTileY; ++tileY) {
		for (auto tileX = firstTileX; tileX <= lastTileX; ++tileX) {
			const RE::NiPoint3 tileMinimum{
				std::max(a_boundsMin.x, static_cast<float>(tileX) * kNeighborhoodTileSize),
				std::max(a_boundsMin.y, static_cast<float>(tileY) * kNeighborhoodTileSize),
				a_boundsMin.z
			};
			const RE::NiPoint3 tileMaximum{
				std::min(a_boundsMax.x, static_cast<float>(tileX + 1) * kNeighborhoodTileSize),
				std::min(a_boundsMax.y, static_cast<float>(tileY + 1) * kNeighborhoodTileSize),
				a_boundsMax.z
			};
			const auto patchKey = MakePatchKey(a_sceneKey, tileX, tileY);
			auto [entry, inserted] = observedPatches.try_emplace(
				patchKey,
				ObservedPatch{ tileMinimum, tileMaximum });
			if (inserted) {
				++insertedCount;
			} else {
				// Merge grass layers sharing a spatial tile.
				entry->second.boundsMin.x = std::min(entry->second.boundsMin.x, tileMinimum.x);
				entry->second.boundsMin.y = std::min(entry->second.boundsMin.y, tileMinimum.y);
				entry->second.boundsMin.z = std::min(entry->second.boundsMin.z, tileMinimum.z);
				entry->second.boundsMax.x = std::max(entry->second.boundsMax.x, tileMaximum.x);
				entry->second.boundsMax.y = std::max(entry->second.boundsMax.y, tileMaximum.y);
				entry->second.boundsMax.z = std::max(entry->second.boundsMax.z, tileMaximum.z);
			}
		}
	}
	return insertedCount;
}

// Summary: Scans the current exterior 3x3 cell neighborhood for grass-bearing LAND.
// Args: None.
// Returns: None.
void PhysicalLeaves::ScanLoadedGrassNeighborhood()
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!cell || !cell->IsExteriorCell()) {
		lastGrassScanCellKey = 0;
		grassScanCountdown = 0;
		return;
	}

	const auto cellKey = cell->GetFormID();
	const bool cellChanged = cellKey != lastGrassScanCellKey;
	if (cellChanged) {
		if (lastGrassScanCellKey != 0) {
			RetireSharedEmitters();
			if (sharedOwnerFeature != ScreenSpacePhysicalParticles::Core::kInvalidRoute) {
				(void)ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton()
					.RequestFeatureDrain(sharedOwnerFeature);
			}
		}
		lastGrassScanCellKey = cellKey;
		grassScanCountdown = 0;
	} else if (++grassScanCountdown < kGrassRescanFrames) {
		return;
	} else {
		grassScanCountdown = 0;
	}

	const auto* centerCoordinates = cell->GetCoordinates();
	const auto* centerWorldspace = cell->GetRuntimeData().worldSpace;
	auto* tes = RE::TES::GetSingleton();
	if (!centerCoordinates || !tes)
		return;

	std::array<RE::TESObjectCELL*, 9> loadedCells{};
	std::size_t loadedCellCount{};
	// Summary: Collects attached cells in the current 3x3 worldspace neighborhood.
	// Args:
	//   a_loadedCell: Loaded cell visited by TES.
	// Returns: None.
	tes->ForEachCell([&](RE::TESObjectCELL* a_loadedCell) {
		if (!a_loadedCell || !a_loadedCell->IsExteriorCell() ||
			a_loadedCell->GetRuntimeData().worldSpace != centerWorldspace)
			return;
		const auto* coordinates = a_loadedCell->GetCoordinates();
		if (coordinates && std::abs(coordinates->cellX - centerCoordinates->cellX) <= 1 &&
			std::abs(coordinates->cellY - centerCoordinates->cellY) <= 1 &&
			loadedCellCount < loadedCells.size())
			loadedCells[loadedCellCount++] = a_loadedCell;
	});

	std::uint32_t landscapeCellCount{};
	std::uint32_t grassQuadrantCount{};
	std::uint32_t insertedTiles{};
	const auto sceneKey = centerWorldspace ? centerWorldspace->GetFormID() : cellKey;
	for (std::size_t cellIndex = 0; cellIndex < loadedCellCount; ++cellIndex) {
		auto* loadedCell = loadedCells[cellIndex];
		if (!loadedCell->IsAttached())
			continue;
		const auto* coordinates = loadedCell->GetCoordinates();
		auto* land = loadedCell->GetRuntimeData().cellLand;
		const auto* loadedLand = land ? land->loadedData : nullptr;
		if (!coordinates || !loadedLand)
			continue;
		++landscapeCellCount;
		const auto cellBaseZ =
			(loadedLand->heightExtents.x + loadedLand->heightExtents.y) * 0.5F;
		for (std::uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
			const auto hiddenFlag = static_cast<RE::EXTERIOR_DATA::LandHideFlag>(1U << quadrant);
			if (coordinates->landHideFlags.any(hiddenFlag) ||
				!QuadrantHasGrass(*loadedLand, quadrant)) {
				continue;
			}

			float minimumZ = std::numeric_limits<float>::max();
			float maximumZ = std::numeric_limits<float>::lowest();
			for (const auto localHeight : loadedLand->heights[quadrant]) {
				const auto worldHeight = localHeight + cellBaseZ;
				if (!std::isfinite(worldHeight))
					continue;
				minimumZ = std::min(minimumZ, worldHeight);
				maximumZ = std::max(maximumZ, worldHeight);
			}
			if (minimumZ > maximumZ)
				continue;

			const auto quadrantX = quadrant & 1U;
			const auto quadrantY = quadrant >> 1U;
			const auto minimumX = coordinates->worldX +
			                      static_cast<float>(quadrantX) * kNeighborhoodTileSize;
			const auto minimumY = coordinates->worldY +
			                      static_cast<float>(quadrantY) * kNeighborhoodTileSize;
			constexpr float zBoundsPadding = 1.0F;
			insertedTiles += QueueGrassRegion(
				sceneKey,
				{ minimumX, minimumY, minimumZ - zBoundsPadding },
				{ minimumX + kNeighborhoodTileSize,
					minimumY + kNeighborhoodTileSize,
					maximumZ + zBoundsPadding });
			++grassQuadrantCount;
		}
	}

	if (cellChanged || insertedTiles > 0) {
		logger::debug(
			"[PhysicalLeaves] Scanned loaded 3x3 landscape grass coverage: cell={:08X}, loadedCells={}, landscapeCells={}, grassQuadrants={}, newTiles={}",
			cellKey,
			loadedCellCount,
			landscapeCellCount,
			grassQuadrantCount,
			insertedTiles);
	}
}

void PhysicalLeaves::ConfigureCanopyEmission()
{
	using namespace ScreenSpacePhysicalParticles::Core;
	if (sharedSimulationProfile == kInvalidRoute || sharedRenderClass == kInvalidRoute)
		return;
	if (!leafTextures.SRV())
		leafTextures.LoadGroup("leaves");

	auto& runtime = ParticleRuntime::GetSingleton();
	ParticleTemplateGPU canopy{};
	canopy.simulationProfile = sharedSimulationProfile;
	canopy.renderClass = sharedRenderClass;
	canopy.lifecyclePhase = static_cast<std::uint32_t>(LifecyclePhase::kDynamic);
	canopy.persistentListID = kInvalidRoute;
	canopy.customInitialization = kInvalidRoute;
	canopy.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
	canopyTemplate = runtime.RegisterParticleTemplate("PhysicalLeaves.Canopy", canopy);

	activeRegionIndex = BearforjParticles::CurrentRegionIndex();
	const auto region = BearforjParticles::RegionName(activeRegionIndex);
	for (std::uint32_t species = 0; species < BearforjParticles::kSpeciesCount; ++species) {
		EmissionProfileGPU emission{};
		emission.initialSpeed.range = { 40.0F, 120.0F };
		emission.initialSpeed.type = static_cast<std::uint32_t>(DistributionType::kUniform);
		emission.initialSpeed.shape = 1.0F;
		emission.horizontalAngleDegrees.range = { 0.0F, 360.0F };
		emission.horizontalAngleDegrees.type = static_cast<std::uint32_t>(DistributionType::kUniform);
		emission.horizontalAngleDegrees.shape = 1.0F;
		emission.verticalAngleDegrees.range = { 0.0F, 18.0F };
		emission.verticalAngleDegrees.type = static_cast<std::uint32_t>(DistributionType::kUniform);
		emission.verticalAngleDegrees.shape = 1.0F;
		emission.lifetime.range = {
			settings.particleLifetime * 0.80F,
			settings.particleLifetime * 1.20F
		};
		emission.lifetime.type = static_cast<std::uint32_t>(DistributionType::kUniform);
		emission.lifetime.shape = 1.0F;
		emission.radius.range = {
			settings.particleSize * 0.80F,
			settings.particleSize * 1.20F
		};
		emission.radius.type = static_cast<std::uint32_t>(DistributionType::kUniform);
		emission.radius.shape = 1.0F;
		emission.opacity.range = { 1.0F, 1.0F };
		emission.opacity.type = static_cast<std::uint32_t>(DistributionType::kConstant);
		emission.opacity.shape = 1.0F;
		emission.generalScalar = static_cast<float>(leafTextures.Find(
			BearforjParticles::SpeciesName(species),
			region,
			"01"));
		emission.spawnShape = static_cast<std::uint32_t>(EmitterShape::kBox);
		emission.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
		emission.trajectoryPolicy = static_cast<std::uint32_t>(
			EmissionTrajectoryPolicy::kBoundedIndependent);
		canopyEmissionProfiles[species] = runtime.RegisterEmissionProfile(
			std::format("PhysicalLeaves.Canopy.{}", BearforjParticles::SpeciesName(species)),
			emission);
	}
}

void PhysicalLeaves::ScanLoadedTrees()
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell || canopyTemplate == ScreenSpacePhysicalParticles::Core::kInvalidRoute)
		return;
	if (grassScanCountdown > 0U) {
		--grassScanCountdown;
		return;
	}
	grassScanCountdown = 20U;
	observedPatches.clear();

	const auto playerPosition = player->GetPosition();
	constexpr float kCutoff = 3072.0F;
	auto visit = [&](RE::TESObjectCELL* a_cell) {
		if (!a_cell)
			return;
		a_cell->ForEachReferenceInRange(playerPosition, kCutoff, [&](RE::TESObjectREFR* a_ref) {
			if (!BearforjParticles::IsTreeReference(a_ref))
				return RE::BSContainer::ForEachResult::kContinue;
			const auto* base = a_ref->GetBaseObject();
			const auto position = a_ref->GetPosition();
			const auto scale = std::max(a_ref->GetScale(), 0.25F);
			float radius = 160.0F;
			float bottom = 220.0F;
			float top = 640.0F;
			if (const auto* bound = base ? base->As<RE::TESBoundObject>() : nullptr) {
				radius = std::max({
					std::abs(static_cast<float>(bound->boundData.boundMin.x)),
					std::abs(static_cast<float>(bound->boundData.boundMax.x)),
					std::abs(static_cast<float>(bound->boundData.boundMin.y)),
					std::abs(static_cast<float>(bound->boundData.boundMax.y)),
					96.0F });
				bottom = std::max(static_cast<float>(bound->boundData.boundMax.z) * 0.45F, 64.0F);
				top = std::max(static_cast<float>(bound->boundData.boundMax.z), bottom + 64.0F);
			}
			ObservedPatch patch{};
			patch.species = BearforjParticles::SpeciesIndex(base);
			patch.boundsMin = {
				position.x - radius * scale,
				position.y - radius * scale,
				position.z + bottom * scale
			};
			patch.boundsMax = {
				position.x + radius * scale,
				position.y + radius * scale,
				position.z + top * scale
			};
			observedPatches[a_ref->GetFormID()] = patch;
			return RE::BSContainer::ForEachResult::kContinue;
		});
	};
	visit(cell);
	if (auto* tes = RE::TES::GetSingleton(); tes && cell->IsExteriorCell()) {
		tes->ForEachCell([&](RE::TESObjectCELL* a_other) {
			if (a_other && a_other != cell && a_other->IsAttached())
				visit(a_other);
		});
	}
	if (observedPatches.empty())
		RetireSharedEmitters();
	else
		SynchronizeSharedEmitters();
}

void PhysicalLeaves::SetupResources()
{
	if (!globals::features::screenSpacePhysicalParticles.loaded) {
		loaded = false;
		failedLoadedMessage = "Physical Leaves requires Screen-Space Physical Particles";
		logger::error("[Physical Leaves] Screen-Space Physical Particles is not loaded");
		return;
	}
	SetupSharedRuntime();
}

// Summary: Creates feature sidecars, constants, textures, states, and shaders.
// Args: None.
// Returns: None.
void PhysicalLeaves::SetupSharedGraphicsResources()
{
	auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	auto* device = globals::d3d::device;
	if (!device || !globals::d3d::context || !runtime.Ready())
		return;

	const auto capacity = runtime.Capacity();
	if (!sharedSidecars || sharedSidecarCapacity != capacity) {
		const auto sidecarDesc = StructuredBufferDesc<LeafSidecarGPU>(capacity, true, false);
		sharedSidecars = std::make_unique<Buffer>(
			sidecarDesc, nullptr, "PhysicalLeaves::SharedSidecars");
		D3D11_SHADER_RESOURCE_VIEW_DESC sidecarSRV{};
		sidecarSRV.Format = DXGI_FORMAT_UNKNOWN;
		sidecarSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sidecarSRV.Buffer.NumElements = capacity;
		sharedSidecars->CreateSRV(sidecarSRV);
		D3D11_UNORDERED_ACCESS_VIEW_DESC sidecarUAV{};
		sidecarUAV.Format = DXGI_FORMAT_UNKNOWN;
		sidecarUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		sidecarUAV.Buffer.NumElements = capacity;
		sharedSidecars->CreateUAV(sidecarUAV);
		const std::uint32_t clear[4]{};
		globals::d3d::context->ClearUnorderedAccessViewUint(
			sharedSidecars->uav.get(), clear);
		sharedSidecarCapacity = capacity;
	}

	if (!sharedMaintenanceCB) {
		sharedMaintenanceCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<SharedMaintenanceCB>(),
			"PhysicalLeaves::SharedMaintenanceCB");
	}
	if (!sharedNewbornCB) {
		sharedNewbornCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<SharedNewbornCB>(),
			"PhysicalLeaves::SharedNewbornCB");
	}
	if (!renderCB) {
		renderCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<RenderCB>(), "PhysicalLeaves::SharedRenderCB");
	}

	if (!leafTextureSRV) {
		if (leafTextures.LoadGroup("leaves")) {
			if (auto* view = leafTextures.SRV())
				leafTextureSRV.copy_from(view);
		} else {
			logger::error("[PhysicalLeaves] Leaf textures were not loaded from particles.json");
		}
	}
	if (!leafSampler) {
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, leafSampler.put()));
		Util::SetResourceName(leafSampler.get(), "PhysicalLeaves::SharedLeafSampler");
	}
	if (!blendState) {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
		Util::SetResourceName(blendState.get(), "PhysicalLeaves::SharedBlendState");
	}
	if (!depthState) {
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = true;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
		Util::SetResourceName(depthState.get(), "PhysicalLeaves::SharedDepthState");
	}
	if (!rasterizerState) {
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = true;
		DX::ThrowIfFailed(device->CreateRasterizerState(
			&rasterDesc, rasterizerState.put()));
		Util::SetResourceName(
			rasterizerState.get(), "PhysicalLeaves::SharedRasterizerState");
	}
	EnsureSharedShaders();
}

// Summary: Lazily compiles the feature's newborn, maintenance, and render shaders.
// Args: None.
// Returns: None.
void PhysicalLeaves::EnsureSharedShaders()
{
	if (!sharedNewbornCS) {
		sharedNewbornCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalLeaves\\PhysicalLeavesNewbornCS.hlsl",
			{},
			"cs_5_0",
			"InitializeMain")));
	}
	if (!sharedMaintenanceCS) {
		sharedMaintenanceCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalLeaves\\PhysicalLeavesMaintenanceCS.hlsl",
			{},
			"cs_5_0",
			"MaintainMain")));
	}
	if (!sharedVertexShader) {
		sharedVertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalLeaves\\PhysicalLeavesShared.hlsl",
			{ { "VSHADER", "" } },
			"vs_5_0",
			"VSMain")));
	}
	if (!sharedPixelShader) {
		sharedPixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalLeaves\\PhysicalLeavesShared.hlsl",
			{ { "PSHADER", "" } },
			"ps_5_0",
			"PSMain")));
	}
}

bool PhysicalLeaves::SharedRendererReady() const noexcept
{
	const auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	return sharedSidecars && sharedSidecars->srv && sharedSidecarCapacity == runtime.Capacity() &&
	       renderCB && leafTextureSRV && leafSampler && sharedVertexShader && sharedPixelShader &&
	       blendState && depthState && rasterizerState;
}

bool PhysicalLeaves::SharedMaintenanceReady() const noexcept
{
	const auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	return sharedGroundedList.Valid() && sharedSidecars && sharedSidecars->uav &&
	       sharedSidecarCapacity == runtime.Capacity() && sharedMaintenanceCB &&
	       sharedMaintenanceCS;
}

bool PhysicalLeaves::SharedNewbornReady() const noexcept
{
	return sharedNewbornInitializerID != ScreenSpacePhysicalParticles::Core::kInvalidRoute &&
	       sharedNewbornCB && sharedNewbornCS;
}

// Summary: Projects newly emitted leaf particles onto captured solid surfaces.
// Args:
//   a_context: Shared-runtime resources and indirect dispatch arguments.
// Returns: None.
void PhysicalLeaves::DispatchSharedNewborn(
	const ScreenSpacePhysicalParticles::Core::NewbornInitializationContext& a_context)
{
	if (!SharedNewbornReady() || !a_context.context || !a_context.dispatchArguments)
		return;
	SharedNewbornCB constants{};
	constants.expectedInitializer = sharedNewbornInitializerID;
	constants.minimumGroundNormalZ = settings.minimumGroundNormalZ;
	constants.groundSearchBelow = settings.groundSearchBelow;
	constants.groundSearchAbove = settings.groundSearchAbove;
	constants.screenXYTolerance = settings.screenXYTolerance;
	constants.surfaceOffset = settings.surfaceOffset;
	sharedNewbornCB->Update(constants);

	auto* context = a_context.context;
	ID3D11Buffer* featureConstants = sharedNewbornCB->CB();
	context->CSSetConstantBuffers(0, 1, &a_context.frameConstants);
	context->CSSetConstantBuffers(1, 1, &a_context.collisionConstants);
	context->CSSetConstantBuffers(2, 1, &a_context.emitterConstants);
	context->CSSetConstantBuffers(3, 1, &a_context.lifecycleConstants);
	context->CSSetConstantBuffers(4, 1, &featureConstants);
	context->CSSetConstantBuffers(12, 1, &a_context.gamePerFrameConstants);
	ID3D11ShaderResourceView* srvs[9]{
		a_context.frameRequests,
		a_context.emissionChunks,
		a_context.freeIDs,
		a_context.particleTemplates,
		a_context.sceneDepth,
		a_context.solidHeight,
		a_context.waterHeight,
		a_context.routeDescriptors,
		a_context.simulationProfiles,
	};
	context->CSSetShaderResources(0, 9, srvs);
	ID3D11UnorderedAccessView* uavs[8]{
		a_context.positionAge,
		a_context.velocityLifetime,
		a_context.appearance,
		a_context.metadata,
		a_context.contactData,
		a_context.routeArena,
		a_context.auxiliaryState,
		a_context.counters,
	};
	context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
	context->CSSetShader(sharedNewbornCS.get(), nullptr, 0);
	context->DispatchIndirect(
		a_context.dispatchArguments, a_context.dispatchArgumentOffset);
	ID3D11UnorderedAccessView* nullUAVs[8]{};
	ID3D11ShaderResourceView* nullSRVs[9]{};
	context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	context->CSSetShaderResources(0, 9, nullSRVs);
	context->CSSetShader(nullptr, nullptr, 0);
}

// Summary: Registers Physical Leaves routes, callbacks, profiles, and renderer with SSPP.
// Args: None.
// Returns: None.
void PhysicalLeaves::SetupSharedRuntime()
{
	auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	if (!sharedRenderer)
		sharedRenderer = std::make_unique<SharedRenderer>(*this);
	if (!sharedMaintenance)
		sharedMaintenance = std::make_unique<SharedMaintenance>(*this);
	if (!sharedNewbornInitializer) {
		sharedNewbornInitializer =
			std::make_unique<SharedNewbornInitializer>(*this);
	}
	sharedOwnerFeature = runtime.RegisterFeature("PhysicalLeaves");
	sharedNewbornInitializerID = runtime.RegisterNewbornInitializer(
		"PhysicalLeaves.GroundProjection",
		{
			"Physical leaves ground projection",
			"PhysicalLeaves::NewbornInitialize",
		},
		sharedNewbornInitializer.get());

	sharedGroundedList = runtime.RegisterPersistentList(
		{
			"PhysicalLeaves.Grounded",
			sharedOwnerFeature,
			0.0F,
			0U,
			ScreenSpacePhysicalParticles::Core::BuiltinMaintenancePolicy::kNone,
			static_cast<std::uint32_t>(
				ScreenSpacePhysicalParticles::Core::CollisionQueryPolicy::kPlayerCapsules),
		},
		sharedMaintenance.get());

	ScreenSpacePhysicalParticles::Core::RenderClassDesc rendererDesc{};
	rendererDesc.name = "Physical leaves";
	rendererDesc.drawOrder = 300U;
	rendererDesc.vertexCountPerInstance = 4U;
	rendererDesc.cullRadiusScale = 1.5F;
	rendererDesc.maximumDistance = 4096.0F;
	rendererDesc.cullingFlags = static_cast<std::uint32_t>(
								   ScreenSpacePhysicalParticles::Core::RenderCullFlags::kFrustum) |
	                           static_cast<std::uint32_t>(
								   ScreenSpacePhysicalParticles::Core::RenderCullFlags::kDistance);
	rendererDesc.drawProfilerName = "PhysicalLeaves::ParticleRender";
	sharedRenderClass = runtime.RegisterRenderer(
		"PhysicalLeaves.Leaf",
		rendererDesc,
		sharedRenderer.get());
	ConfigureSharedRuntime();
	SetupSharedGraphicsResources();
	logger::info(
		"[PhysicalLeaves] SSPP adapter registered: owner={:08X}, profile={}, template={}, standaloneTemplate={}, emission={}, initializer={}, persistentList={}, renderClass={}, capacity={}",
		sharedOwnerFeature,
		sharedSimulationProfile,
		sharedParticleTemplate,
		sharedDebugParticleTemplate,
		sharedEmissionProfile,
		sharedNewbornInitializerID,
		sharedGroundedList.index,
		sharedRenderClass,
		runtime.Capacity());
}

// Summary: Publishes current physics, collision, template, and emission settings to SSPP.
// Args: None.
// Returns: None.
void PhysicalLeaves::ConfigureSharedRuntime()
{
	sharedConfigured = false;
	if (!sharedGroundedList.Valid() ||
		sharedRenderClass == ScreenSpacePhysicalParticles::Core::kInvalidRoute ||
		sharedNewbornInitializerID == ScreenSpacePhysicalParticles::Core::kInvalidRoute)
		return;
	auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	using namespace ScreenSpacePhysicalParticles::Core;

	SimulationProfileGPU simulation{};
	simulation.acceleration = { 0.0F, 0.0F, -settings.gravity };
	simulation.drag = settings.drag;
	simulation.restitution = settings.bounce * 0.62F;
	simulation.friction = 0.38F;
	simulation.settleSpeed = settings.settleSpeed;
	simulation.surfaceOffset = settings.surfaceOffset;
	simulation.collisionMask = static_cast<std::uint32_t>(CollisionChannel::kWorldSolid);
	simulation.queryPolicy =
		static_cast<std::uint32_t>(CollisionQueryPolicy::kPerspectiveDepth) |
		static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField);
	if (settings.playerCollision) {
		simulation.collisionMask |= static_cast<std::uint32_t>(CollisionChannel::kPlayer);
		simulation.queryPolicy |=
			static_cast<std::uint32_t>(CollisionQueryPolicy::kPlayerCapsules);
	}
	simulation.simulationClass = static_cast<std::uint32_t>(
		settings.playerCollision ? SimulationClass::kWorldPlayer : SimulationClass::kWorld);
	sharedSimulationProfile = runtime.RegisterSimulationProfile(
		"PhysicalLeaves.Flight", simulation);
	if (sharedSimulationProfile == kInvalidRoute)
		return;

	CollisionRuleGPU worldRule{};
	worldRule.response = static_cast<std::uint32_t>(CollisionResponse::kSettle);
	worldRule.destinationListID = sharedGroundedList.index;
	worldRule.restitutionOverride = settings.bounce * 0.62F;
	worldRule.frictionOverride = 0.38F;
	worldRule.collisionRadiusScale = 0.0F;
	worldRule.minimumSettleNormalZ = settings.minimumGroundNormalZ;
	worldRule.flags = 0U;
	runtime.SetCollisionRule(
		sharedSimulationProfile, CollisionChannel::kWorldSolid, worldRule);

	CollisionRuleGPU playerRule{};
	playerRule.response = static_cast<std::uint32_t>(CollisionResponse::kBounce);
	playerRule.restitutionOverride = settings.playerCollisionRestitution;
	playerRule.frictionOverride = 0.0F;
	playerRule.collisionRadiusScale = 0.15F;
	playerRule.separationOffset =
		std::max(settings.capsulePadding - settings.surfaceOffset, 0.0F);
	playerRule.minimumSettleNormalZ = -1.0F;
	runtime.SetCollisionRule(
		sharedSimulationProfile, CollisionChannel::kPlayer, playerRule);

	ParticleTemplateGPU particleTemplate{};
	particleTemplate.simulationProfile = sharedSimulationProfile;
	particleTemplate.renderClass = sharedRenderClass;
	particleTemplate.lifecyclePhase = static_cast<std::uint32_t>(
		LifecyclePhase::kPersistent);
	particleTemplate.persistentListID = sharedGroundedList.index;
	particleTemplate.customInitialization = sharedNewbornInitializerID;
	particleTemplate.spawnPlacement = static_cast<std::uint32_t>(
		SpawnPlacement::kProjectToWorldSolid);
	sharedParticleTemplate = runtime.RegisterParticleTemplate(
		"PhysicalLeaves.Leaf", particleTemplate);
	if (sharedParticleTemplate == kInvalidRoute)
		return;

	auto debugParticleTemplate = particleTemplate;
	debugParticleTemplate.customInitialization = kInvalidRoute;
	debugParticleTemplate.spawnPlacement = static_cast<std::uint32_t>(
		SpawnPlacement::kDirect);
	sharedDebugParticleTemplate = runtime.RegisterParticleTemplate(
		"PhysicalLeaves.StandaloneField", debugParticleTemplate);
	if (sharedDebugParticleTemplate == kInvalidRoute)
		return;

	EmissionProfileGPU emission{};
	emission.initialSpeed.range = { 0.0F, 0.0F };
	emission.initialSpeed.type = static_cast<std::uint32_t>(
		DistributionType::kConstant);
	emission.horizontalAngleDegrees = emission.initialSpeed;
	emission.verticalAngleDegrees = emission.initialSpeed;
	emission.lifetime.range = {
		settings.particleLifetime * 0.80F,
		settings.particleLifetime * 1.20F
	};
	emission.lifetime.type = static_cast<std::uint32_t>(DistributionType::kUniform);
	emission.radius.range = {
		settings.particleSize * 0.80F,
		settings.particleSize * 1.20F
	};
	emission.radius.type = static_cast<std::uint32_t>(DistributionType::kUniform);
	emission.opacity.range = { 1.0F, 1.0F };
	emission.opacity.type = static_cast<std::uint32_t>(DistributionType::kConstant);
	emission.generalScalar = 1.0F;
	emission.spawnShape = static_cast<std::uint32_t>(EmitterShape::kBox);
	emission.spawnPlacement = static_cast<std::uint32_t>(
		SpawnPlacement::kProjectToWorldSolid);
	emission.trajectoryPolicy = static_cast<std::uint32_t>(
		EmissionTrajectoryPolicy::kBoundedIndependent);
	sharedEmissionProfile = runtime.RegisterEmissionProfile(
		"PhysicalLeaves.GroundCover", emission);
	ConfigureCanopyEmission();
	sharedConfigured = sharedEmissionProfile != kInvalidRoute &&
	                   canopyTemplate != kInvalidRoute;
}

// Summary: Queues one flat 20-by-20-meter leaf field at the player's base height.
// Args: None.
// Returns: True when SSPP accepted the burst request.
bool PhysicalLeaves::QueueDebugLeafField() noexcept
{
	using namespace ScreenSpacePhysicalParticles::Core;
	auto& runtime = ParticleRuntime::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!runtime.Ready() || !sharedConfigured || !player ||
		sharedOwnerFeature == kInvalidRoute ||
		sharedDebugParticleTemplate == kInvalidRoute) {
		return false;
	}

	const auto playerPosition = player->GetPosition();
	const auto fieldZ = playerPosition.z + settings.surfaceOffset;
	BurstRequestGPU request{};
	request.origin = { playerPosition.x, playerPosition.y, fieldZ };
	request.forward = { 0.0F, 1.0F, 0.0F };
	request.right = { 1.0F, 0.0F, 0.0F };
	request.vertical = { 0.0F, 0.0F, 1.0F };
	request.seed = 0x1EAFC001U ^ (++debugLeafFieldSequence * 0x9E3779B9U);
	request.count = kDebugLeafFieldCount;
	request.particleTemplate = sharedDebugParticleTemplate;
	request.initialSpeed = {
		{ 0.0F, 0.0F }, static_cast<std::uint32_t>(DistributionType::kConstant), 1.0F
	};
	request.horizontalAngleDegrees = request.initialSpeed;
	request.verticalAngleDegrees = request.initialSpeed;
	request.lifetime = {
		{ settings.particleLifetime * 0.80F, settings.particleLifetime * 1.20F },
		static_cast<std::uint32_t>(DistributionType::kUniform),
		1.0F
	};
	request.radius = {
		{ settings.particleSize * 0.80F, settings.particleSize * 1.20F },
		static_cast<std::uint32_t>(DistributionType::kUniform),
		1.0F
	};
	request.opacity = {
		{ 1.0F, 1.0F }, static_cast<std::uint32_t>(DistributionType::kConstant), 1.0F
	};
	request.priority = 100U;
	request.ownerFeature = sharedOwnerFeature;
	request.flags = kStandaloneFieldFlag;
	request.spawnBoundsMin = {
		playerPosition.x - kDebugLeafFieldHalfExtent,
		playerPosition.y - kDebugLeafFieldHalfExtent,
		fieldZ
	};
	request.spawnBoundsMax = {
		playerPosition.x + kDebugLeafFieldHalfExtent,
		playerPosition.y + kDebugLeafFieldHalfExtent,
		fieldZ
	};
	request.spawnShape = static_cast<std::uint32_t>(EmitterShape::kBox);
	request.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
	request.trajectoryPolicy = static_cast<std::uint32_t>(
		EmissionTrajectoryPolicy::kBoundedIndependent);
	if (!runtime.QueueBurst(request))
		return false;

	++debugLeafFieldSpawnCount;
	logger::info(
		"[PhysicalLeaves] Queued standalone debug field: count={}, center=({:.1f}, {:.1f}, {:.1f}), extent={}m x {}m",
		request.count,
		request.origin.x,
		request.origin.y,
		request.origin.z,
		20,
		20);
	return true;
}

// Summary: Builds a density-maintaining SSPP emitter for one observed grass patch.
// Args:
//   a_patchKey: Stable identity and random seed for the patch.
//   a_patch: World bounds of the grass-bearing patch.
// Returns: Emitter descriptor clipped to the current orthographic capture.
ScreenSpacePhysicalParticles::Core::EmitterDesc PhysicalLeaves::BuildSharedEmitterDesc(
	std::uint64_t a_patchKey,
	const ObservedPatch& a_patch) const
{
	using namespace ScreenSpacePhysicalParticles::Core;
	const auto& heightField =
		ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton();
	EmitterDesc emitter{};
	emitter.origin = {
		(a_patch.boundsMin.x + a_patch.boundsMax.x) * 0.5F,
		(a_patch.boundsMin.y + a_patch.boundsMax.y) * 0.5F,
		(a_patch.boundsMin.z + a_patch.boundsMax.z) * 0.5F
	};
	emitter.forward = { 0.0F, 0.0F, -1.0F };
	emitter.right = { 1.0F, 0.0F, 0.0F };
	emitter.vertical = { 0.0F, 1.0F, 0.0F };
	emitter.seed = static_cast<std::uint32_t>(
		a_patchKey ^ (a_patchKey >> 32U) ^ 0x1EAFC001U);
	const auto species = std::min(a_patch.species, 4U);
	emitter.particleTemplate = canopyTemplate != kInvalidRoute ? canopyTemplate : sharedParticleTemplate;
	emitter.emissionProfile = canopyEmissionProfiles[species] != kInvalidRoute ?
	                              canopyEmissionProfiles[species] :
	                              sharedEmissionProfile;
	emitter.boundsMin = {
		a_patch.boundsMin.x, a_patch.boundsMin.y, a_patch.boundsMin.z
	};
	emitter.boundsMax = {
		a_patch.boundsMax.x, a_patch.boundsMax.y, a_patch.boundsMax.z
	};
	emitter.particlesPerSecond = 0.0F;
	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto playerPosition = player ? player->GetPosition() : RE::NiPoint3{};
	const auto dx = emitter.origin.x - playerPosition.x;
	const auto dy = emitter.origin.y - playerPosition.y;
	const auto distanceScale = std::clamp(1.0F - std::sqrt(dx * dx + dy * dy) / 3072.0F, 0.0F, 1.0F);
	const auto amount = globals::features::screenSpacePhysicalParticles.ParticleAmountScale();
	auto fullTarget = static_cast<std::uint32_t>(std::lround(
		static_cast<float>(std::clamp(settings.leavesPerTile, 1, 64)) * 0.25F * amount * distanceScale));
	fullTarget = std::min(fullTarget, 64U);
	emitter.targetAlive = fullTarget;
	emitter.maximumAlive = fullTarget;
	emitter.priority = 100U;
	emitter.ownerFeature = sharedOwnerFeature;
	emitter.ownerCell = static_cast<std::uint32_t>(a_patchKey);
	emitter.flags = static_cast<std::uint32_t>(EmitterFlags::kEnabled);
	if (fullTarget > 0U)
		emitter.flags |= static_cast<std::uint32_t>(EmitterFlags::kMaintainDensity);

	if (heightField.Ready()) {
		const auto fieldCenter = heightField.Center();
		const auto fieldHalfExtent = heightField.HalfExtent();
		const auto border = heightField.TexelSize();
		const auto fieldMinimumX = fieldCenter.x - fieldHalfExtent + border;
		const auto fieldMaximumX = fieldCenter.x + fieldHalfExtent - border;
		const auto fieldMinimumY = fieldCenter.y - fieldHalfExtent + border;
		const auto fieldMaximumY = fieldCenter.y + fieldHalfExtent - border;
		const auto overlapMinimumX = std::max(a_patch.boundsMin.x, fieldMinimumX);
		const auto overlapMaximumX = std::min(a_patch.boundsMax.x, fieldMaximumX);
		const auto overlapMinimumY = std::max(a_patch.boundsMin.y, fieldMinimumY);
		const auto overlapMaximumY = std::min(a_patch.boundsMax.y, fieldMaximumY);
		if (overlapMaximumX > overlapMinimumX &&
			overlapMaximumY > overlapMinimumY) {
			emitter.boundsMin.x = overlapMinimumX;
			emitter.boundsMax.x = overlapMaximumX;
			emitter.boundsMin.y = overlapMinimumY;
			emitter.boundsMax.y = overlapMaximumY;

			const auto patchArea =
				(a_patch.boundsMax.x - a_patch.boundsMin.x) *
				(a_patch.boundsMax.y - a_patch.boundsMin.y);
			const auto overlapArea =
				(overlapMaximumX - overlapMinimumX) *
				(overlapMaximumY - overlapMinimumY);
			const auto coverage = patchArea > 0.0F ?
				std::clamp(overlapArea / patchArea, 0.0F, 1.0F) :
				0.0F;
			emitter.targetAlive = std::clamp(
				static_cast<std::uint32_t>(std::lround(
					static_cast<float>(fullTarget) * coverage)),
				1U,
				fullTarget);
			emitter.maximumAlive = emitter.targetAlive;
			emitter.flags |=
				static_cast<std::uint32_t>(EmitterFlags::kMaintainDensity);
		}
	}
	return emitter;
}

// Summary: Keeps emitters for the nearest observed patches and retires the remainder.
// Args: None.
// Returns: None.
void PhysicalLeaves::SynchronizeSharedEmitters()
{
	using namespace ScreenSpacePhysicalParticles::Core;
	if (sharedParticleTemplate == kInvalidRoute || sharedEmissionProfile == kInvalidRoute)
		return;
	auto& runtime = ParticleRuntime::GetSingleton();
	struct Candidate
	{
		std::uint64_t key{};
		ObservedPatch patch{};
		float distanceSquared{};
	};
	std::vector<Candidate> candidates;
	if (observedPatches.empty())
		return;
	candidates.reserve(observedPatches.size());
	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto playerPosition = player ? player->GetPosition() : RE::NiPoint3{};
	for (const auto& [key, patch] : observedPatches) {
		const auto dx = std::max({ patch.boundsMin.x - playerPosition.x,
			0.0F,
			playerPosition.x - patch.boundsMax.x });
		const auto dy = std::max({ patch.boundsMin.y - playerPosition.y,
			0.0F,
			playerPosition.y - patch.boundsMax.y });
		candidates.push_back({ key, patch, dx * dx + dy * dy });
	}
	observedPatches.clear();
	if (candidates.size() > kMaxResidentPatches) {
		std::ranges::nth_element(
			candidates,
			candidates.begin() + kMaxResidentPatches,
			{},
			&Candidate::distanceSquared);
		candidates.resize(kMaxResidentPatches);
	}
	const auto isSelected = [&](std::uint64_t a_key) {
		return std::ranges::find(candidates, a_key, &Candidate::key) != candidates.end();
	};
	for (auto resident = sharedEmitters.begin(); resident != sharedEmitters.end();) {
		if (isSelected(resident->first)) {
			++resident;
			continue;
		}
		if (resident->second.Valid())
			(void)runtime.DestroyEmitter(resident->second);
		resident = sharedEmitters.erase(resident);
	}

	for (const auto& candidate : candidates) {
		const auto desc = BuildSharedEmitterDesc(candidate.key, candidate.patch);
		auto found = sharedEmitters.find(candidate.key);
		if (found == sharedEmitters.end()) {
			auto handle = runtime.CreateEmitter(desc);
			if (!handle.Valid())
				continue;
			sharedEmitters.emplace(candidate.key, handle);
			++emittedGrassBatchCount;
			continue;
		}
		if (!runtime.UpdateEmitter(found->second, desc)) {
			auto replacement = runtime.CreateEmitter(desc);
			if (replacement.Valid())
				found->second = replacement;
		}
	}
}

// Summary: Destroys all resident leaf emitters and clears observed patch state.
// Args: None.
// Returns: None.
void PhysicalLeaves::RetireSharedEmitters()
{
	auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	for (const auto& [key, emitter] : sharedEmitters) {
		(void)key;
		if (emitter.Valid())
			(void)runtime.DestroyEmitter(emitter);
	}
	sharedEmitters.clear();
	observedPatches.clear();
}

// Summary: Retires emitters, requests feature drain, and clears client state.
// Args: None.
// Returns: None.
void PhysicalLeaves::ResetSharedRuntimeState()
{
	RetireSharedEmitters();
	auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	if (sharedOwnerFeature != ScreenSpacePhysicalParticles::Core::kInvalidRoute)
		(void)runtime.RequestFeatureDrain(sharedOwnerFeature);
	ResetSharedClientState();
}

// Summary: Clears feature-owned CPU state and GPU sidecars without resetting SSPP.
// Args: None.
// Returns: None.
void PhysicalLeaves::ResetSharedClientState()
{
	sharedEmitters.clear();
	observedPatches.clear();
	sharedConfigured = false;
	lastWorldspaceKey = 0U;
	lastGrassScanCellKey = 0U;
	grassScanCountdown = 0U;
	debugLeafFieldSpawnCount = 0U;
	debugLeafFieldSequence = 0U;
	if (sharedSidecars && sharedSidecars->uav && globals::d3d::context) {
		const std::uint32_t clear[4]{};
		globals::d3d::context->ClearUnorderedAccessViewUint(
			sharedSidecars->uav.get(), clear);
	}
}

// Summary: Wakes grounded leaves intersecting the player capsule into dynamic routes.
// Args:
//   a_context: Persistent-list resources and indirect dispatch arguments.
// Returns: None.
void PhysicalLeaves::DispatchSharedMaintenance(
	const ScreenSpacePhysicalParticles::Core::ParticleMaintenanceContext& a_context)
{
	if (!SharedMaintenanceReady() || !a_context.dispatchArguments)
		return;
	auto* context = globals::d3d::context;
	if (!context)
		return;

	SharedMaintenanceCB constants{};
	constants.inputBase = a_context.inputBase;
	constants.inputCounterOffset = a_context.inputCounterOffset;
	constants.keepRoute = a_context.keepRoute;
	constants.freeRoute = a_context.freeRoute;
	constants.dynamicRouteBase = a_context.dynamicRouteBase;
	constants.dynamicSimulationClass = static_cast<std::uint32_t>(
		settings.playerCollision ?
			ScreenSpacePhysicalParticles::Core::SimulationClass::kWorldPlayer :
			ScreenSpacePhysicalParticles::Core::SimulationClass::kWorld);
	constants.persistentListID = sharedGroundedList.index;
	constants.particleCapacity = a_context.particleCapacity;
	constants.playerCollisionEnabled = settings.playerCollision ? 1U : 0U;
	constants.capsulePadding = settings.capsulePadding;
	constants.elapsedSeconds = a_context.elapsedSeconds;
	constants.playerVelocityInfluence = settings.playerVelocityInfluence;
	constants.playerHorizontalImpulse = settings.playerHorizontalImpulse;
	constants.playerVerticalImpulse = settings.playerVerticalImpulse;
	if (settings.playerCollision) {
		auto& playerCollision =
			ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton();
		constants.playerCapsuleCount = playerCollision.Count();
		constants.playerVelocity = playerCollision.Velocity();
		std::ranges::copy(
			playerCollision.Capsules(), constants.playerCapsules.begin());
	}
	sharedMaintenanceCB->Update(constants);

	ID3D11Buffer* frame = a_context.frameConstants;
	ID3D11Buffer* lifecycle = a_context.lifecycleConstants;
	ID3D11Buffer* featureConstants = sharedMaintenanceCB->CB();
	context->CSSetConstantBuffers(0, 1, &frame);
	context->CSSetConstantBuffers(3, 1, &lifecycle);
	context->CSSetConstantBuffers(4, 1, &featureConstants);
	ID3D11ShaderResourceView* srvs[10]{};
	srvs[0] = a_context.inputReferences;
	srvs[7] = a_context.routeDescriptors;
	srvs[8] = a_context.auxiliaryState;
	srvs[9] = a_context.emitterStates;
	context->CSSetShaderResources(0, 10, srvs);
	ID3D11UnorderedAccessView* uavs[8]{
		a_context.positionAge,
		a_context.velocityLifetime,
		a_context.appearance,
		a_context.metadata,
		a_context.contactData,
		a_context.routeArena,
		// This pass uses u6 for sidecars and emits no exit events.
		sharedSidecars->uav.get(),
		a_context.counters
	};
	context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
	context->CSSetShader(sharedMaintenanceCS.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalLeaves::GroundedWake");
	context->DispatchIndirect(
		a_context.dispatchArguments, a_context.dispatchArgumentOffset);
	globals::profiler->EndPass();
	ID3D11UnorderedAccessView* nullUAVs[8]{};
	ID3D11ShaderResourceView* nullSRVs[10]{};
	context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	context->CSSetShaderResources(0, 10, nullSRVs);
	context->CSSetShader(nullptr, nullptr, 0);
}

// Summary: Draws visible leaf particles as textured or diagnostic quads.
// Args:
//   a_context: Shared particle buffers, visibility segment, and indirect draw command.
// Returns: None.
void PhysicalLeaves::DrawSharedParticles(
	const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context)
{
	if (!SharedRendererReady() || !a_context.context)
		return;
	auto* renderer = globals::game::renderer;
	if (!renderer || !globals::deferred)
		return;
	const auto mainTarget = globals::deferred->forwardRenderTargets[0];
	auto& main = renderer->GetRuntimeData().renderTargets[mainTarget];
	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ID3D11RenderTargetView* rtv = main.RTV;
	ID3D11DepthStencilView* dsv =
		depth.readOnlyViews[0] ? depth.readOnlyViews[0] : depth.views[0];
	if (!rtv)
		return;

	auto* context = a_context.context;
	context->OMSetRenderTargets(1, &rtv, dsv);
	auto resolution = float2{
		static_cast<float>(globals::game::graphicsState->screenWidth),
		static_cast<float>(globals::game::graphicsState->screenHeight)
	};
	resolution = Util::ConvertToDynamic(resolution, true);
	const D3D11_VIEWPORT viewport{
		0.0F, 0.0F, resolution.x, resolution.y, 0.0F, 1.0F
	};
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterizerState.get());
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFFU);
	context->OMSetDepthStencilState(depthState.get(), 0);
	ID3D11Buffer* nullVertexBuffer{};
	UINT stride{};
	UINT offset{};
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(
		0, 1, &nullVertexBuffer, &stride, &offset);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	RenderCB renderConstants{};
	renderConstants.debugWhiteQuads = settings.debugWhiteQuads ? 1U : 0U;
	renderConstants.visibleSegmentBase = a_context.visibleSegmentBase;
	renderConstants.particleCapacity = a_context.particleCapacity;
	renderCB->Update(renderConstants);
	ID3D11Buffer* renderConstantsBuffer = renderCB->CB();
	context->VSSetConstantBuffers(0, 1, &renderConstantsBuffer);
	context->PSSetConstantBuffers(0, 1, &renderConstantsBuffer);
	ID3D11ShaderResourceView* sidecars = sharedSidecars->srv.get();
	context->VSSetShaderResources(6, 1, &sidecars);
	ID3D11ShaderResourceView* leafTexture = leafTextureSRV.get();
	context->PSSetShaderResources(2, 1, &leafTexture);
	ID3D11SamplerState* sampler = leafSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->VSSetShader(sharedVertexShader.get(), nullptr, 0);
	context->PSSetShader(sharedPixelShader.get(), nullptr, 0);
	a_context.DrawInstancedIndirect();
	ID3D11ShaderResourceView* nullSidecar{};
	context->VSSetShaderResources(6, 1, &nullSidecar);
	ID3D11ShaderResourceView* nullTexture{};
	context->PSSetShaderResources(2, 1, &nullTexture);
}

void PhysicalLeaves::PostComposite()
{
	if (resetPending.exchange(false, std::memory_order_acq_rel))
		ResetSharedRuntimeState();
	auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	if (!runtime.Ready() || !sharedGroundedList.Valid() ||
		sharedRenderClass == ScreenSpacePhysicalParticles::Core::kInvalidRoute)
		return;

	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto* cell = player ? player->GetParentCell() : nullptr;
	const auto* worldspace = cell ? cell->GetRuntimeData().worldSpace : nullptr;
	const auto worldspaceKey = worldspace ?
	                               worldspace->GetFormID() :
	                               (cell ? cell->GetFormID() | 0x80000000U : 0U);
	if (lastWorldspaceKey == 0U) {
		lastWorldspaceKey = worldspaceKey;
	} else if (worldspaceKey != lastWorldspaceKey) {
		ResetSharedRuntimeState();
		lastWorldspaceKey = worldspaceKey;
	}

	if (!SharedRendererReady() || !SharedMaintenanceReady() || !SharedNewbornReady())
		SetupSharedGraphicsResources();
	if (!sharedConfigured)
		ConfigureSharedRuntime();
#if PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING
	ScanLoadedGrassNeighborhood();
	SynchronizeSharedEmitters();
#else
	if (!globals::features::screenSpacePhysicalParticles.IsKindEnabled(
			ScreenSpacePhysicalParticlesFeature::ParticleKind::kLeaves)) {
		if (!sharedEmitters.empty())
			RetireSharedEmitters();
	} else {
		if (sharedConfigured && BearforjParticles::CurrentRegionIndex() != activeRegionIndex)
			ConfigureCanopyEmission();
		ScanLoadedTrees();
	}
#endif
}

void PhysicalLeaves::ClearShaderCache()
{
	sharedMaintenanceCS = nullptr;
	sharedNewbornCS = nullptr;
	sharedVertexShader = nullptr;
	sharedPixelShader = nullptr;
}

void PhysicalLeaves::GameLoaded()
{
	resetPending.store(true, std::memory_order_release);
}

void PhysicalLeaves::DrawSettings()
{
	ImGui::TextWrapped("Use the diagnostic field below to seed persistent leaves around the player for SSPP collision testing.");
	const auto drawTooltip = [](std::string_view a_key, const char* a_fallback) {
		if (auto _tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(a_key, a_fallback));
	};
	ImGui::BeginDisabled(!sharedConfigured ||
		sharedDebugParticleTemplate == ScreenSpacePhysicalParticles::Core::kInvalidRoute);
	if (ImGui::Button(T(
			"feature.physical_leaves.spawn_debug_field",
			"Spawn 20 x 20 m leaf field"))) {
		if (!QueueDebugLeafField())
			logger::warn("[PhysicalLeaves] SSPP rejected the standalone debug field");
	}
	ImGui::EndDisabled();
	drawTooltip(
		"feature.physical_leaves.spawn_debug_field_tooltip",
		"Queues 1,024 leaves in a flat 20-by-20-meter square centered at the player's current base height.");
	ImGui::Separator();
	bool reseed = false;
	bool reconfigure = false;
	if (ImGui::TreeNodeEx(T("feature.physical_leaves.ground_cover", "Ground cover"), ImGuiTreeNodeFlags_DefaultOpen)) {
#if PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING
		reseed |= ImGui::SliderInt(T("feature.physical_leaves.leaves_per_tile", "Leaves per coverage tile"), &settings.leavesPerTile, 1, 64);
		drawTooltip(
			"feature.physical_leaves.leaves_per_tile_tooltip",
			"Number of leaves seeded into each 2048-unit landscape quadrant with a painted grass-bearing layer. Changing this rebuilds the current leaf population.");
#else
		reseed |= ImGui::SliderInt(
			T("feature.physical_leaves.leaves_per_tile", "Leaves per tree"),
			&settings.leavesPerTile,
			1,
			64);
		drawTooltip(
			"feature.physical_leaves.leaves_per_tile_tooltip",
			"Target leaves around each nearby tree canopy, before distance and the global amount scale. Leaves spawn from trees only.");
#endif
		reseed |= ImGui::SliderFloat(T("feature.physical_leaves.particle_size", "Leaf half-size"), &settings.particleSize, 2.0F, 32.0F, "%.1f");
		drawTooltip(
			"feature.physical_leaves.particle_size_tooltip",
			"Half the rendered leaf width and height, in Skyrim world units. Changing this rebuilds the current leaf population.");
		const bool surfaceOffsetChanged = ImGui::SliderFloat(T("feature.physical_leaves.surface_offset", "Surface offset"), &settings.surfaceOffset, 0.0F, 4.0F, "%.2f");
		reseed |= surfaceOffsetChanged;
		reconfigure |= surfaceOffsetChanged;
		drawTooltip(
			"feature.physical_leaves.surface_offset_tooltip",
			"Lifts grounded leaves along the reconstructed surface normal to prevent depth fighting. Changing this reseeds leaves.");
		const bool minimumNormalChanged = ImGui::SliderFloat(T("feature.physical_leaves.minimum_ground_normal", "Minimum upward slope"), &settings.minimumGroundNormalZ, 0.0F, 1.0F, "%.2f");
		reseed |= minimumNormalChanged;
		reconfigure |= minimumNormalChanged;
		drawTooltip(
			"feature.physical_leaves.minimum_ground_normal_tooltip",
			"Minimum upward component of a valid ground normal. Higher values reject steeper surfaces and walls.");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T("feature.physical_leaves.flight_physics", "Flight physics"), ImGuiTreeNodeFlags_DefaultOpen)) {
		reseed |= ImGui::SliderFloat(T("feature.physical_leaves.lifetime", "Maximum airborne lifetime"), &settings.particleLifetime, 1.0F, 120.0F, "%.1f s");
		drawTooltip(
			"feature.physical_leaves.lifetime_tooltip",
			"Maximum time a disturbed leaf remains airborne before returning to its last grounded anchor.");
		reconfigure |= ImGui::SliderFloat(T("feature.physical_leaves.gravity", "Gravity"), &settings.gravity, 0.0F, 980.0F, "%.0f");
		drawTooltip(
			"feature.physical_leaves.gravity_tooltip",
			"Downward acceleration applied to airborne leaves, in Skyrim units per second squared.");
		reconfigure |= ImGui::SliderFloat(T("feature.physical_leaves.drag", "Air drag"), &settings.drag, 0.0F, 4.0F, "%.2f");
		drawTooltip(
			"feature.physical_leaves.drag_tooltip",
			"Exponential velocity damping applied while leaves are airborne. Higher values stop motion sooner.");
		reconfigure |= ImGui::SliderFloat(T("feature.physical_leaves.bounce", "Ground bounce"), &settings.bounce, 0.0F, 1.0F, "%.2f");
		drawTooltip(
			"feature.physical_leaves.bounce_tooltip",
			"Fraction of normal velocity retained when an airborne leaf strikes reconstructed world geometry.");
		reconfigure |= ImGui::SliderFloat(T("feature.physical_leaves.settle_speed", "Settle speed"), &settings.settleSpeed, 0.0F, 100.0F, "%.1f");
		drawTooltip(
			"feature.physical_leaves.settle_speed_tooltip",
			"Airborne leaves settle when their speed falls below this value on an upward-facing surface.");
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T("feature.physical_leaves.player_collision", "Player collision"), ImGuiTreeNodeFlags_DefaultOpen)) {
		reconfigure |= ImGui::Checkbox(T("feature.physical_leaves.player_collision_enable", "Enable player body collision"), &settings.playerCollision);
		drawTooltip(
			"feature.physical_leaves.player_collision_enable_tooltip",
			"Lets the player's conservative collision capsule disturb grounded and airborne leaves.");
		ImGui::BeginDisabled(!settings.playerCollision);
		reconfigure |= ImGui::SliderFloat(T("feature.physical_leaves.capsule_padding", "Capsule padding"), &settings.capsulePadding, 0.0F, 32.0F, "%.1f");
		drawTooltip(
			"feature.physical_leaves.capsule_padding_tooltip",
			"Expands the player collision capsule by this many Skyrim units so nearby leaves react before clipping into the body.");
		ImGui::SliderFloat(T("feature.physical_leaves.horizontal_impulse", "Horizontal kick"), &settings.playerHorizontalImpulse, 0.0F, 800.0F, "%.0f");
		drawTooltip(
			"feature.physical_leaves.horizontal_impulse_tooltip",
			"Base sideways speed given to a grounded leaf when the player touches it.");
		ImGui::SliderFloat(T("feature.physical_leaves.vertical_impulse", "Upward kick"), &settings.playerVerticalImpulse, 0.0F, 800.0F, "%.0f");
		drawTooltip(
			"feature.physical_leaves.vertical_impulse_tooltip",
			"Base upward speed given to a grounded leaf when the player touches it.");
		ImGui::SliderFloat(T("feature.physical_leaves.velocity_influence", "Player velocity influence"), &settings.playerVelocityInfluence, 0.0F, 2.0F, "%.2f");
		drawTooltip(
			"feature.physical_leaves.velocity_influence_tooltip",
			"Scales how much of the player's current movement velocity is added to a disturbed leaf.");
		reconfigure |= ImGui::SliderFloat(T("feature.physical_leaves.player_restitution", "Airborne collision bounce"), &settings.playerCollisionRestitution, 0.0F, 1.0F, "%.2f");
		drawTooltip(
			"feature.physical_leaves.player_restitution_tooltip",
			"Controls how strongly airborne leaves rebound from the player's collision volumes.");
		ImGui::EndDisabled();
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(T("feature.physical_leaves.advanced", "Placement and diagnostics"))) {
		ImGui::Checkbox(T("feature.physical_leaves.debug_white_quads", "Render white debug quads"), &settings.debugWhiteQuads);
		drawTooltip(
			"feature.physical_leaves.debug_white_quads_tooltip",
			"Replaces the leaf atlas with solid white squares so placement and visibility are easy to inspect.");
#if PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING
		reseed |= ImGui::SliderFloat(T("feature.physical_leaves.search_below", "Ground search below grass"), &settings.groundSearchBelow, 0.0F, 512.0F, "%.0f");
		drawTooltip(
			"feature.physical_leaves.search_below_tooltip",
			"Maximum distance below an observed grass bound that may be accepted as its supporting ground.");
		reseed |= ImGui::SliderFloat(T("feature.physical_leaves.search_above", "Ground search above grass"), &settings.groundSearchAbove, 0.0F, 256.0F, "%.0f");
		drawTooltip(
			"feature.physical_leaves.search_above_tooltip",
			"Maximum distance above an observed grass bound that may be accepted as its supporting ground.");
		reseed |= ImGui::SliderFloat(T("feature.physical_leaves.screen_tolerance", "Screen reconstruction XY tolerance"), &settings.screenXYTolerance, 8.0F, 256.0F, "%.0f");
		drawTooltip(
			"feature.physical_leaves.screen_tolerance_tooltip",
			"Maximum horizontal mismatch allowed when screen depth is reprojected to seed a requested world position.");
#endif
		ImGui::TextDisabled(
			"%s",
			T("feature.physical_leaves.shared_diagnostics", "Collision previews are available on the Screen-Space Physical Particles page."));
		ImGui::TreePop();
	}
	if (reseed)
		resetPending.store(true, std::memory_order_release);
	if (reconfigure || reseed)
		sharedConfigured = false;

	ImGui::TextDisabled("Standalone fields queued: %llu", debugLeafFieldSpawnCount);
#if PHYSICAL_LEAVES_ENABLE_DEPRECATED_GRASS_SEEDING
	ImGui::TextDisabled(
		"Deprecated grass patches: %llu | resident: %zu/%u",
		emittedGrassBatchCount,
		sharedEmitters.size(),
		kMaxResidentPatches);
#endif
}

void PhysicalLeaves::LoadSettings(json& a_json)
{
	settings.leavesPerTile = std::clamp(a_json.value("LeavesPerTile", settings.leavesPerTile), 1, 64);
	settings.particleSize = std::clamp(a_json.value("ParticleSize", settings.particleSize), 2.0F, 32.0F);
	settings.particleLifetime = std::clamp(a_json.value("ParticleLifetime", settings.particleLifetime), 1.0F, 120.0F);
	settings.gravity = std::clamp(a_json.value("Gravity", settings.gravity), 0.0F, 980.0F);
	settings.drag = std::clamp(a_json.value("Drag", settings.drag), 0.0F, 4.0F);
	settings.bounce = std::clamp(a_json.value("Bounce", settings.bounce), 0.0F, 1.0F);
	settings.settleSpeed = std::clamp(a_json.value("SettleSpeed", settings.settleSpeed), 0.0F, 100.0F);
	settings.surfaceOffset = std::clamp(a_json.value("SurfaceOffset", settings.surfaceOffset), 0.0F, 4.0F);
	settings.minimumGroundNormalZ = std::clamp(a_json.value("MinimumGroundNormalZ", settings.minimumGroundNormalZ), 0.0F, 1.0F);
	settings.groundSearchBelow = std::clamp(a_json.value("GroundSearchBelow", settings.groundSearchBelow), 0.0F, 512.0F);
	settings.groundSearchAbove = std::clamp(a_json.value("GroundSearchAbove", settings.groundSearchAbove), 0.0F, 256.0F);
	settings.screenXYTolerance = std::clamp(a_json.value("ScreenXYTolerance", settings.screenXYTolerance), 8.0F, 256.0F);
	settings.playerCollision = a_json.value("PlayerCollision", settings.playerCollision);
	settings.capsulePadding = std::clamp(a_json.value("CapsulePadding", settings.capsulePadding), 0.0F, 32.0F);
	settings.playerHorizontalImpulse = std::clamp(a_json.value("PlayerHorizontalImpulse", settings.playerHorizontalImpulse), 0.0F, 800.0F);
	settings.playerVerticalImpulse = std::clamp(a_json.value("PlayerVerticalImpulse", settings.playerVerticalImpulse), 0.0F, 800.0F);
	settings.playerVelocityInfluence = std::clamp(a_json.value("PlayerVelocityInfluence", settings.playerVelocityInfluence), 0.0F, 2.0F);
	settings.playerCollisionRestitution = std::clamp(a_json.value("PlayerCollisionRestitution", settings.playerCollisionRestitution), 0.0F, 1.0F);
	settings.debugWhiteQuads = a_json.value("DebugWhiteQuads", settings.debugWhiteQuads);
}

void PhysicalLeaves::SaveSettings(json& a_json)
{
	a_json = {
		{ "LeavesPerTile", settings.leavesPerTile },
		{ "ParticleSize", settings.particleSize },
		{ "ParticleLifetime", settings.particleLifetime },
		{ "Gravity", settings.gravity },
		{ "Drag", settings.drag },
		{ "Bounce", settings.bounce },
		{ "SettleSpeed", settings.settleSpeed },
		{ "SurfaceOffset", settings.surfaceOffset },
		{ "MinimumGroundNormalZ", settings.minimumGroundNormalZ },
		{ "GroundSearchBelow", settings.groundSearchBelow },
		{ "GroundSearchAbove", settings.groundSearchAbove },
		{ "ScreenXYTolerance", settings.screenXYTolerance },
		{ "PlayerCollision", settings.playerCollision },
		{ "CapsulePadding", settings.capsulePadding },
		{ "PlayerHorizontalImpulse", settings.playerHorizontalImpulse },
		{ "PlayerVerticalImpulse", settings.playerVerticalImpulse },
		{ "PlayerVelocityInfluence", settings.playerVelocityInfluence },
		{ "PlayerCollisionRestitution", settings.playerCollisionRestitution },
		{ "DebugWhiteQuads", settings.debugWhiteQuads }
	};
}

void PhysicalLeaves::RestoreDefaultSettings()
{
	settings = {};
	resetPending.store(true, std::memory_order_release);
}
