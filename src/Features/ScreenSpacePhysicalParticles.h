#pragma once

#include "SubmersionWetness.h"

#include <mutex>
#include <vector>

class ConstantBuffer;

/**
 * @brief Bearforj SSPP runtime v2 - one shared GPU particle simulation for every particle profile.
 *
 * Profiles (all in one pool, one simulate dispatch, one draw):
 *  - Rain: wind-driven streaks with gravity/drag, depth-buffer collision and splash spawning.
 *  - Snow: flakes with drift, flutter and tumble that settle on surfaces and melt away.
 *  - Leaves: ambient falling leaves that tumble in the wind and settle.
 *  - Splashes, sparks (long-lived, bouncing), footstep dust/snow kick-up, blood droplets: event bursts
 *    written into a dynamic ring by EmitCS.
 * Precipitation is occluded by Skylighting's sky visibility, so it does not fall under roofs.
 * Rendered before post-processing into the HDR scene target with soft depth fading.
 */
struct ScreenSpacePhysicalParticles : Feature
{
	virtual inline std::string GetName() override { return "Screen Space Physical Particles"; }
	virtual std::string GetDisplayName() override { return T("feature.sspp.name", "Physical Particles"); }
	virtual inline std::string GetShortName() override { return "ScreenSpacePhysicalParticles"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.sspp.description", "Physical rain, snow, leaves, sparks, blood and footstep dust in one GPU simulation."),
			{ T("feature.sspp.key_1", "Rain streaks blown by the wind, splashing on whatever they hit"),
				T("feature.sspp.key_2", "Snow flakes that drift, tumble and settle"),
				T("feature.sspp.key_3", "No rain or snow under roofs"),
				T("feature.sspp.key_4", "Sparks on blocked hits, blood droplets, dust and snow kicked up by feet") } };
	}

	enum Kind : uint32_t
	{
		kRain = 0,
		kSnow,
		kLeaf,
		kSplash,
		kSpark,
		kDust,
		kBlood,
		kEmber,  // fire magic
		kIce     // frost magic
	};

	struct Extra  // Bearforj 0.6 additions (separate struct: nlohmann macro arg limit)
	{
		float LeafGroundMin = 10.0f;  // s a fallen leaf rests (random between min and max), then fades
		float LeafGroundMax = 20.0f;
		float LeafGlide = 1.0f;       // flutter/glide strength of falling leaves
		bool LeafGustLift = true;     // gusts can lift resting leaves again
		float SparkRestTime = 5.0f;   // s sparks/embers glow on the ground while cooling
		float IceRestTime = 6.0f;     // s ice shards lie before melting
		bool FireSparks = false;      // 0.6.3 (off by default, untested in game): embers/sparks rising and falling from campfires, braziers and forges
		float FireSparkRate = 3.0f;   // bursts per second per fire source
		float FireSparkRadius = 2500.0f;
		bool MagicParticles = true;   // fire embers / frost shards / shock sparks on magic hits
		uint32_t MagicCount = 40;
		float MagicLifetime = 3.0f;
		float MagicMaxDistance = 3000.0f;  // LOD: no magic particles beyond, fewer towards it
		float SizeVariation = 0.45f;  // 0 = uniform, 1 = very mixed sizes
		float FadeDistance = 6000.0f; // particles fade out smoothly towards this distance (no popping)
		bool WindDrift = true;        // blowing snow / dust / debris near the ground in wind
		float WindDriftDensity = 0.5f;
		bool SuppressVanilla = true;        // never double precipitation (see cpp)
		uint32_t ParticleBudget = 160000;  // shared limit for all pools; pools shrink proportionally above it
		float GlobalDensity = 1.0f;        // multiplies weather precipitation and leaf amounts
	};

	struct Settings
	{
		Extra bf;
		bool Enabled = true;
		bool Rain = true;
		bool Snow = true;
		bool Leaves = true;
		bool Drizzle = true;          // Bearforj: fine mist-rain in fog weathers (any weather mod)
		bool Ash = true;              // Bearforj: grey ash fall in ash/volcanic weathers (Solstheim, True Storms ash)
		float LeafAutumnBoost = 2.0f; // Bearforj: more leaves in autumn/forest weathers
		bool Sparks = true;
		bool Blood = true;
		bool FootstepParticles = true;
		bool Collisions = true;
		bool SkyOcclusion = true;

		uint32_t RainCount = 40000;   // 0.5: ~12k, too sparse to read as rain
		uint32_t SnowCount = 24000;
		uint32_t LeafCount = 1500;
		uint32_t DynamicCount = 16384;
		float SpawnRadius = 1800.0f;
		float SpawnHeight = 1400.0f;
		float WindScale = 1.0f;

		float RainWidth = 0.45f;
		float RainLength = 1.0f;
		float RainBrightness = 2.2f;
		float RainSpeed = 900.0f;
		float SplashChance = 0.35f;
		float SplashSize = 1.6f;
		float SplashBrightness = 2.0f;
		float SplashLifetime = 0.35f;

		float SnowSize = 2.4f;
		float SnowBrightness = 2.0f;
		float SnowSpeed = 110.0f;
		float SnowSettleTime = 12.0f;

		float LeafSize = 5.0f;
		float LeafBrightness = 1.0f;
		float LeafSpeed = 70.0f;
		float LeafSettleTime = 20.0f;
		float LeafAmount = 0.35f;     // base amount, scaled up by wind

		float SparkLifetime = 2.4f;   // 0.5 default ~0.8 s -> 3x
		float SparkSize = 0.8f;
		float SparkBrightness = 12.0f;
		float SparkBounce = 0.45f;
		uint32_t SparkCount = 48;

		float DustSize = 6.0f;
		float DustBrightness = 1.0f;
		float DustLifetime = 1.4f;
		uint32_t DustCount = 10;

		float BloodSize = 1.2f;
		float BloodBrightness = 0.6f;
		float BloodStickTime = 6.0f;
		uint32_t BloodCount = 40;

		float CollisionThickness = 60.0f;
		float SoftDepth = 8.0f;
	} settings;

	struct alignas(16) Particle
	{
		float3 position;
		float age;
		float3 velocity;
		float life;
		float size;
		float spin;
		uint32_t color;
		uint32_t kind;
	};
	static_assert(sizeof(Particle) == 48);

	struct EmitEvent
	{
		float3 position;
		uint32_t kind;
		float3 direction;
		uint32_t count;
		float4 params;
		float4 color;
	};
	static_assert(sizeof(EmitEvent) == 64);

	struct alignas(16) FrameCB
	{
		Matrix ViewProj;
		float4 CameraPos;
		float4 ProjParams;
		float4 TimeParams;
		float4 Wind;
		float4 Amounts;
		uint32_t Ranges[4];
		float4 RainP;
		float4 SnowP;
		float4 LeafP;
		float4 SparkP;
		float4 DustP;
		float4 BloodP;
		float4 SplashP;
		float4 LightColor;
		float4 Flags;
		float4 ExtraP;   // x spark/ember rest, y ice rest, z leaf ground min, w leaf ground max
		float4 ExtraP2;  // x size variation, y leaf glide, z gust lift, w fade distance
		float4 ContextA; // x interior, y trees, z conifers, w cold
		float4 ContextB; // x hour, y location flags, z weather slot, w night
	};

	virtual void SetupResources() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void Prepass() override;
	virtual void OnBeforePostProcessing(RE::RENDER_TARGET a_renderTarget) override;
	virtual void OnSceneTransitionReset(bool a_opening) override;
	virtual void ClearShaderCache() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Queues a burst of event particles (thread-safe). */
	void Emit(const EmitEvent& a_event);
	/** @brief Footstep kick-up (called by Terrain Deformation for every footstep / stride print). */
	void EmitFootstep(const RE::NiPoint3& a_position, RE::MATERIAL_ID a_material, float a_scale);
	/** @brief 0.6.3: periodic ember bursts from nearby fires/forges (scan every 2 s, max 16 sources). */
	void UpdateFireSparks();
	std::vector<float3> fireSources;
	std::vector<float> fireTimers;
	float fireNextScan = 0.0f;
	void OnHit(const RE::TESHitEvent* a_event);

private:
	void CreatePool();
	void ComputeWeather(float& a_rain, float& a_snow, float3& a_wind, float& a_gust) const;
	bool CompileShaders();

	winrt::com_ptr<ID3D11Buffer> particleBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> particleSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> particleUAV;
	winrt::com_ptr<ID3D11Buffer> counterBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> counterUAV;
	winrt::com_ptr<ID3D11Buffer> eventBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> eventSRV;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterState;
	ConstantBuffer* frameCB = nullptr;
	ConstantBuffer* emitCB = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> simulateCS;
	winrt::com_ptr<ID3D11ComputeShader> emitCS;
	winrt::com_ptr<ID3D11VertexShader> drawVS;
	winrt::com_ptr<ID3D11PixelShader> drawPS;
	bool shadersFailed = false;

	uint32_t poolRain = 0, poolSnow = 0, poolLeaf = 0, poolDynamic = 0;
	bool poolDirty = true;
	bool shadersReadyForCull = false;
	bool clearPending = true;
	bool simulatedThisFrame = false;
	float time = 0.0f;
	uint32_t frame = 0;

	static constexpr uint32_t kMaxEvents = 256;
	std::mutex eventLock;
	std::vector<EmitEvent> pendingEvents;

	struct HitSink : public RE::BSTEventSink<RE::TESHitEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override;
	} hitSink;
	/** @brief 0.6.3: footstep dust/snow straight from BGSFootstepEvent (no longer depends on Terrain Deformation prints). */
	struct FootstepSink : public RE::BSTEventSink<RE::BGSFootstepEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*) override;
	} footstepSink;
};
