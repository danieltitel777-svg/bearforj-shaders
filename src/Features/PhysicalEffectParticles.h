#pragma once

#include "I18n/I18n.h"

#include <mutex>
#include <vector>

/**
 * @brief Sparks, magic motes and forge embers in a small GPU pool.
 * The old screen-space particle simulation is gone; this feature does not draw rain or snow.
 */
struct PhysicalEffectParticles : Feature
{
	virtual inline std::string GetName() override { return "Physical Effect Particles"; }
	virtual inline std::string GetDisplayName() override { return T("feature.physical_effect_particles.name", "Physical Effect Particles"); }
	virtual inline std::string GetShortName() override { return "PhysicalEffectParticles"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	virtual inline bool SupportsVR() override { return false; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.physical_effect_particles.description", "Sparks when a hit is blocked, elemental motes on magic hits, and optional embers over fires. No rain or snow."),
			{ T("feature.physical_effect_particles.key_1", "A few dozen sparks, not a screen-wide particle storm"),
				T("feature.physical_effect_particles.key_2", "Fire, frost and shock hits use their own colors"),
				T("feature.physical_effect_particles.key_3", "Campfire embers stay off until you turn them on") } };
	}

	struct Settings
	{
		bool Enabled = true;
		bool Sparks = true;
		bool MagicParticles = true;
		bool FireSparks = false;
		float SparkSize = 0.8f;
		float SparkLifetime = 0.7f;
		float Brightness = 1.0f;
		uint32_t SparkCount = 24;
		uint32_t MagicCount = 28;
		float FireSparkRate = 3.0f;
		float FireSparkRadius = 2500.0f;
		float MagicMaxDistance = 3000.0f;
	} settings;

	static constexpr uint32_t kMaxParticles = 4096;
	static constexpr uint32_t kSlotsPerEvent = 48;
	static constexpr uint32_t kMaxEventsPerFrame = 32;

	struct alignas(16) Particle
	{
		float pos[3];
		float age;
		float vel[3];
		float lifetime;
		float color[3];
		float size;
		uint32_t kind;
		float brightness;
		float pad0;
		float pad1;
	};
	static_assert(sizeof(Particle) == 64);

	struct alignas(16) Event
	{
		float position[3];
		uint32_t firstSlot;
		float direction[3];
		uint32_t kind;
		float speed;
		float spread;
		float size;
		float lifetime;
		float color[4];
		uint32_t count;
		uint32_t seed;
		float brightness;
		float pad;
	};
	static_assert(sizeof(Event) == 80);

	struct alignas(16) FrameCB
	{
		float4 cameraPosAdjust;
		float deltaTime;
		float gravity;
		uint32_t eventCount;
		uint32_t maxParticles;
		float sizeScale;
		float lifetimeScale;
		float brightness;
		float pad;
	};
	static_assert(sizeof(FrameCB) == 48);

	virtual void SetupResources() override;
	virtual void DataLoaded() override;
	virtual void PostComposite() override;
	virtual void ClearShaderCache() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

private:
	struct HitSink : RE::BSTEventSink<RE::TESHitEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override;
	};
	HitSink hitSink;

	void EnsureShaders();
	void Emit(Event a_event);
	void OnHit(const RE::TESHitEvent* a_event);
	void UpdateFireSparks(float a_dt);

	winrt::com_ptr<ID3D11Buffer> particleBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> particleSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> particleUAV;
	winrt::com_ptr<ID3D11Buffer> eventBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> eventSRV;
	winrt::com_ptr<ID3D11Buffer> frameCB;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterState;

	winrt::com_ptr<ID3D11ComputeShader> spawnCS;
	winrt::com_ptr<ID3D11ComputeShader> simulateCS;
	winrt::com_ptr<ID3D11VertexShader> particleVS;
	winrt::com_ptr<ID3D11PixelShader> particlePS;
	bool shadersFailed = false;

	std::mutex eventMutex;
	std::vector<Event> pendingEvents;
	uint32_t nextSlot = 0;
	uint32_t seedCounter = 1;
	float aliveTimer = 8.0f;
	std::chrono::steady_clock::time_point lastFrame{};

	float fireNextScan = 0.0f;
	std::vector<float3> fireSources;
	std::vector<float> fireTimers;
};
