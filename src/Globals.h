#pragma once

#include <atomic>

struct CloudShadows;
struct ProceduralSun;
struct DynamicCubemaps;
struct VolumetricShadows;
struct ExtendedMaterials;
struct GrassCollision;
struct GrassLighting;
struct FoliageLighting;
struct GrassOptimizations;
struct HairSpecular;
struct HiZOcclusion;
struct HorizonFix;
struct IBL;
struct LightLimitFix;
struct LinearLighting;
struct LODBlending;
struct InteriorSun;
struct RainRefraction;
struct NRD;
struct ScreenSpaceGI;
struct ScreenSpaceReflections;
struct ScreenSpaceShadows;
struct Skylighting;
struct TerrainVariation;
struct SkySync;
struct SubsurfaceScattering;
struct TerrainBlending;
struct TerrainHelper;
struct TerrainShadows;
struct TreeLODWind;
struct ProceduralAurora;
struct UnifiedWater;
struct VanillaFresnel;
struct VolumetricLighting;
struct WaterEffects;
struct PerformanceOverlay;
struct RainRendering;
struct WetnessEffects;
struct ExtendedTranslucency;
struct ReverseZ;
struct Upscaling;
class Profiler;
struct CSEditor;
struct Effects11;
struct ExponentialHeightFog;
struct HDRDisplay;
struct PostProcessing;
struct ScreenshotFeature;
struct Skin;
struct SnowCover;
struct FootstepParticles;
struct HairBacklighting;
struct NightEye;
struct FurShells;
struct LandscapeSeams;
struct SnowDeformation;
struct WaterWaves;
struct ScreenSpacePhysicalParticlesFeature;
struct PhysicalBlood;
struct PhysicalLeaves;
struct PhysicalWorldParticles;
struct SceneManager;

class State;
class Deferred;
struct TruePBR;
class RenderDoc;
class RemoteControl;
class Menu;
class SceneSettingsManager;

namespace SIE
{
	class ShaderCache;
	class ShaderFileDependencyTracker;
}

/**
 * @brief Initializes core singletons (ShaderCache, State, Menu, Deferred).
 */
void OnInit();

/**
 * @brief Resolves runtime game pointers, RTTI relocations, and D3D device references.
 */
void ReInit();

/**
 * @brief Caches late-binding game singletons (player, sky, INI settings).
 */
void OnDataLoaded();

/**
 * @brief Signals shader compilation to stop.
 */
void OnGameWindowClose();

/**
 * @brief Installs Detours hooks on the device context's Map/Unmap vtable slots to capture per-frame constant buffer data.
 * @param a_context The D3D11 device context to hook.
 */
void InstallD3DHooks(ID3D11DeviceContext* a_context);
namespace globals
{
	namespace d3d
	{
		extern ID3D11Device* device;
		extern ID3D11DeviceContext* context;
		extern IDXGISwapChain* swapChain;
	}

	namespace features
	{
		extern CloudShadows cloudShadows;
		extern ProceduralSun proceduralSun;
		extern DynamicCubemaps dynamicCubemaps;
		extern VolumetricShadows volumetricShadows;
		extern ExtendedMaterials extendedMaterials;
		extern GrassCollision grassCollision;
		extern GrassLighting grassLighting;
		extern FoliageLighting foliageLighting;
		extern GrassOptimizations grassOptimizations;
		extern HairSpecular hairSpecular;
		extern HiZOcclusion hiZOcclusion;
		extern HorizonFix horizonFix;
		extern IBL ibl;
		extern LightLimitFix lightLimitFix;
		extern LinearLighting linearLighting;
		extern LODBlending lodBlending;
		extern InteriorSun interiorSun;
		extern RainRefraction rainRefraction;
		extern NRD nrd;
		extern ScreenSpaceGI screenSpaceGI;
		extern ScreenSpaceReflections screenSpaceReflections;
		extern ScreenSpaceShadows screenSpaceShadows;
		extern Skylighting skylighting;
		extern TerrainVariation terrainVariation;
		extern SkySync skySync;
		extern SubsurfaceScattering subsurfaceScattering;
		extern TerrainBlending terrainBlending;
		extern TerrainHelper terrainHelper;
		extern TerrainShadows terrainShadows;
		extern TreeLODWind treeLODWind;
		extern ProceduralAurora proceduralAurora;
		extern UnifiedWater unifiedWater;
		extern VanillaFresnel vanillaFresnel;
		extern VolumetricLighting volumetricLighting;
		extern WaterEffects waterEffects;
		extern PerformanceOverlay performanceOverlay;
		extern RainRendering rainRendering;
		extern WetnessEffects wetnessEffects;
		extern ExtendedTranslucency extendedTranslucency;
		extern ReverseZ reverseZ;
		extern Upscaling upscaling;
		extern HDRDisplay hdrDisplay;
		extern Effects11 effects11;
		extern RenderDoc renderDoc;
		extern RemoteControl remoteControl;
		extern ScreenshotFeature screenshotFeature;
		extern CSEditor csEditor;
		extern ExponentialHeightFog exponentialHeightFog;
		extern TruePBR truePBR;
	extern PostProcessing postProcessing;
		extern Skin skin;
		extern SnowCover snowCover;
		extern FootstepParticles footstepParticles;
		extern HairBacklighting hairBacklighting;
		extern NightEye nightEye;
		extern FurShells furShells;
		extern LandscapeSeams landscapeSeams;
		extern SnowDeformation snowDeformation;
		extern WaterWaves waterWaves;
		extern ScreenSpacePhysicalParticlesFeature screenSpacePhysicalParticles;
		extern PhysicalBlood physicalBlood;
		extern PhysicalLeaves physicalLeaves;
		extern PhysicalWorldParticles physicalWorldParticles;
		extern SceneManager sceneManager;

	}

	/** @brief GPU constant buffer layout matching Skyrim's per-frame camera data. */
	struct FrameBuffer
	{
		Matrix CameraView;
		Matrix CameraProj;
		Matrix CameraViewProj;
		Matrix CameraViewProjUnjittered;
		Matrix CameraPreviousViewProjUnjittered;
		Matrix CameraProjUnjittered;
		Matrix CameraProjUnjitteredInverse;
		Matrix CameraViewInverse;
		Matrix CameraViewProjInverse;
		Matrix CameraProjInverse;
		float4 CameraPosAdjust;
		float4 CameraPreviousPosAdjust;
		float4 FrameParams;
		float4 DynamicResolutionParams1;
		float4 DynamicResolutionParams2;
	};

	/** @brief VR per-frame camera buffer. Packoffsets c0 to c86, one pair of matrices per eye. */
	struct FrameBufferVR
	{
		Matrix CameraView[2];
		Matrix CameraProj[2];
		Matrix CameraViewProj[2];
		Matrix CameraViewProjUnjittered[2];
		Matrix CameraPreviousViewProjUnjittered[2];
		Matrix CameraProjUnjittered[2];
		Matrix CameraProjUnjitteredInverse[2];
		Matrix CameraViewInverse[2];
		Matrix CameraViewProjInverse[2];
		Matrix CameraProjInverse[2];
		float4 CameraPosAdjust[2];
		float4 CameraPreviousPosAdjust[2];
		float4 FrameParams;
		float4 DynamicResolutionParams1;
		float4 DynamicResolutionParams2;
	};

	/**
	 * @brief Cached snapshot of per-frame camera data, captured between Map/Unmap of the per-frame constant buffer.
	 * Flat and VR layouts are stored separately. Eye 0 is the only flat matrix; VR indexes the stereo pair.
	 */
	struct FrameBufferCache
	{
		FrameBuffer nonVR{};
		FrameBufferVR vr{};

		/** Gets the camera view matrix. */
		const Matrix& GetCameraView(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraView[eyeIndex] : nonVR.CameraView;
		}
		/** Gets the camera projection matrix. */
		const Matrix& GetCameraProj(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProj[eyeIndex] : nonVR.CameraProj;
		}
		/** Gets the combined camera view-projection matrix. */
		const Matrix& GetCameraViewProj(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewProj[eyeIndex] : nonVR.CameraViewProj;
		}
		/** Gets the unjittered camera view-projection matrix. */
		const Matrix& GetCameraViewProjUnjittered(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewProjUnjittered[eyeIndex] : nonVR.CameraViewProjUnjittered;
		}
		/** Gets the previous frame's unjittered view-projection matrix. */
		const Matrix& GetCameraPreviousViewProjUnjittered(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraPreviousViewProjUnjittered[eyeIndex] : nonVR.CameraPreviousViewProjUnjittered;
		}
		/** Gets the unjittered camera projection matrix. */
		const Matrix& GetCameraProjUnjittered(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProjUnjittered[eyeIndex] : nonVR.CameraProjUnjittered;
		}
		/** Gets the inverse of the unjittered camera projection matrix. */
		const Matrix& GetCameraProjUnjitteredInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProjUnjitteredInverse[eyeIndex] : nonVR.CameraProjUnjitteredInverse;
		}
		/** Gets the inverse camera view matrix. */
		const Matrix& GetCameraViewInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewInverse[eyeIndex] : nonVR.CameraViewInverse;
		}
		/** Gets the inverse camera view-projection matrix. */
		const Matrix& GetCameraViewProjInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewProjInverse[eyeIndex] : nonVR.CameraViewProjInverse;
		}
		/** Gets the inverse camera projection matrix. */
		const Matrix& GetCameraProjInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProjInverse[eyeIndex] : nonVR.CameraProjInverse;
		}
		/** Gets the camera position adjustment vector. */
		const float4& GetCameraPosAdjust(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraPosAdjust[eyeIndex] : nonVR.CameraPosAdjust;
		}
		/** Gets the previous frame's camera position adjustment vector. */
		const float4& GetCameraPreviousPosAdjust(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraPreviousPosAdjust[eyeIndex] : nonVR.CameraPreviousPosAdjust;
		}
		/** Gets the frame parameters (timer, frame count, etc.). */
		const float4& GetFrameParams() const
		{
			return REL::Module::IsVR() ? vr.FrameParams : nonVR.FrameParams;
		}
		/** Gets the first set of dynamic resolution parameters. */
		const float4& GetDynamicResolutionParams1() const
		{
			return REL::Module::IsVR() ? vr.DynamicResolutionParams1 : nonVR.DynamicResolutionParams1;
		}
		/** Gets the second set of dynamic resolution parameters. */
		const float4& GetDynamicResolutionParams2() const
		{
			return REL::Module::IsVR() ? vr.DynamicResolutionParams2 : nonVR.DynamicResolutionParams2;
		}
	};

	namespace game
	{
		extern RE::BSGraphics::RendererShadowState* shadowState;
		extern RE::BSGraphics::State* graphicsState;
		extern RE::BSGraphics::Renderer* renderer;
		extern RE::BSShaderManager::State* smState;
		extern RE::TES* tes;
		extern RE::MemoryManager* memoryManager;
		extern RE::INISettingCollection* iniSettingCollection;
		extern RE::INIPrefSettingCollection* iniPrefSettingCollection;
		extern RE::GameSettingCollection* gameSettingCollection;
		extern float* cameraNear;
		extern float* cameraFar;
		extern float* deltaTime;
		extern RE::BSUtilityShader* utilityShader;
		extern RE::PlayerCharacter* player;
		extern RE::Sky* sky;
		extern RE::UI* ui;
		extern RE::Calendar* calendar;
		extern RE::ImageSpaceManager* imageSpaceManager;
		extern bool* bEnableVolumetricLighting;
		extern std::atomic<bool> quitGame;

		extern bool isVR;

		extern RE::BSGraphics::PixelShader** currentPixelShader;
		extern RE::BSGraphics::VertexShader** currentVertexShader;
		extern REX::EnumSet<RE::BSGraphics::ShaderFlags, uint32_t>* stateUpdateFlags;

		extern RE::Setting* bEnableLandFade;
		extern RE::Setting* bShadowsOnGrass;
		extern RE::Setting* shadowMaskQuarter;
		extern REL::Relocation<ID3D11Buffer**> perFrame;
		extern REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> currentAccumulator;

		extern D3D11_MAPPED_SUBRESOURCE* mappedFrameBuffer;
		extern FrameBufferCache frameBufferCached;
	}

	namespace rtti
	{
		extern REL::Relocation<const RE::NiRTTI*> NiIntegerExtraDataRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSLightingShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSEffectShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSWaterShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiParticleSystemRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiBillboardNodeRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiAlphaPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiSourceTextureRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSGrassShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSMultiStreamInstanceTriShapeRTTI;
	}

	extern State* state;
	extern Deferred* deferred;
	extern Menu* menu;
	extern SIE::ShaderCache* shaderCache;
	extern Profiler* profiler;
	extern SceneSettingsManager* sceneSettingsManager;

	/** @brief Initializes core singletons (ShaderCache, State, Menu, Deferred). Called once at plugin load. */
	void OnInit();
	/** @brief Resolves runtime game pointers, RTTI relocations, and D3D device references. Called when the renderer is ready. */
	void ReInit();
	/** @brief Caches late-binding game singletons (player, sky, INI settings) after Skyrim's data files are loaded. */
	void OnDataLoaded();
	/** @brief Signals shader compilation to stop when the game window is closing. */
	void OnGameWindowClose();
	/**
	 * @brief Installs Detours hooks on the device context's Map/Unmap vtable slots to capture per-frame constant buffer data.
	 * @param a_context The D3D11 device context to hook.
	 */
	void InstallD3DHooks(ID3D11DeviceContext* a_context);
}
