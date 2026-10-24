#include "FeatureBuffer.h"

#include "Feature.h"

#include <array>

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/FoliageLighting.h"
#include "Features/GrassLighting.h"
#include "Features/HairBacklighting.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/NightEye.h"
#include "Features/PostProcessing.h"
#include "Features/ProceduralAurora.h"
#include "Features/ProceduralSun.h"
#include "Features/RainRefraction.h"
#include "Features/RainRendering.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceReflections.h"
#include "Features/Skin.h"
#include "Features/SnowCover.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/VanillaFresnel.h"
#include "Features/VolumetricLighting.h"
#include "Features/WaterWaves.h"
#include "Features/WetnessEffects.h"
#include "TruePBR.h"

template <class... Ts>
std::pair<unsigned char*, size_t> _GetFeatureBufferData(Ts... feat_datas)
{
	// The packed size is a compile-time constant, so reuse one aligned, thread-local buffer
	// instead of allocating/freeing every UpdateSharedData call. The returned pointer is
	// non-owning and must NOT be deleted by the caller.
	constexpr size_t totalSize = (... + sizeof(Ts));
	alignas(16) static thread_local std::array<unsigned char, totalSize> storage;
	size_t offset = 0;

	([&] {
		*reinterpret_cast<decltype(feat_datas)*>(storage.data() + offset) = feat_datas;
		offset += sizeof(decltype(feat_datas));
	}(),
		...);

	return std::make_pair(storage.data(), storage.size());
}

std::pair<unsigned char*, size_t> GetFeatureBufferData(bool a_inWorld)
{
	std::array<uint32_t, 4> runtimeFeatureFlags{};
	static const auto runtimeFeatures = std::to_array<std::pair<Feature*, uint32_t>>({
#define RUNTIME_FEATURE(name, index) { Feature::FindRegisteredFeatureByShortName(#name), index },
#include "../package/Shaders/Common/RuntimeFeatureList.hlsli"
#undef RUNTIME_FEATURE
	});
	static_assert(runtimeFeatures.size() <= sizeof(runtimeFeatureFlags) * 8);
	for (const auto& [feature, index] : runtimeFeatures) {
		if (feature && feature->IsEnabled())
			runtimeFeatureFlags[index >> 5] |= 1u << (index & 31u);
	}
	return _GetFeatureBufferData(
		globals::features::grassLighting.settings,
		globals::features::extendedMaterials.settings,
		globals::features::dynamicCubemaps.GetCommonBufferData(),
		globals::features::terrainShadows.GetCommonBufferData(),
		globals::features::lightLimitFix.GetCommonBufferData(),
		globals::features::wetnessEffects.GetCommonBufferData(),
		globals::features::skylighting.GetCommonBufferData(a_inWorld),
		globals::features::cloudShadows.GetCommonBufferData(),
		globals::features::proceduralSun.GetCommonBufferData(),
		globals::features::lodBlending.settings,
		globals::features::hairSpecular.settings,
		globals::features::terrainVariation.settings,
		globals::features::ibl.GetCommonBufferData(),
		globals::features::extendedTranslucency.GetCommonBufferData(),
		globals::features::linearLighting.GetCommonBufferData(),
		globals::features::effects11.GetCommonBufferData(),
		globals::features::terrainBlending.settings,
		globals::features::exponentialHeightFog.GetCommonBufferData(),
		globals::features::truePBR.settings,
		globals::features::foliageLighting.loaded ? globals::features::foliageLighting.settings : FoliageLighting::Settings{ 0, 0, 0, 0.0f, 0, {} },
		globals::features::skin.GetCommonBufferData(),
		globals::features::vanillaFresnel.settings,
		globals::features::snowCover.GetCommonBufferData(),
		globals::features::volumetricLighting.GetCommonBufferData(),
		globals::features::horizonFix.GetCommonBufferData(),
		globals::features::hairBacklighting.settings,
		globals::features::rainRefraction.GetCommonBufferData(),
		globals::features::nightEye.GetCommonBufferData(),
		globals::features::proceduralAurora.GetCommonBufferData(),
		globals::features::screenSpaceGI.GetCommonBufferData(),
		globals::features::screenSpaceReflections.GetCommonBufferData(),
		globals::features::rainRendering.GetCommonBufferData(),
		globals::features::waterWaves.GetCommonBufferData(),
		runtimeFeatureFlags);
}
