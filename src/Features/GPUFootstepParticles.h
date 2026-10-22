#pragma once

#include "I18n/I18n.h"

#include <mutex>

/**
 * @brief Bearforj: GPU-simulated footstep particles (dust, snow, clods, mud, droplets, mist),
 * recreated from Bottled Shaders' Footstep Particles with our own C++ and extended to mud,
 * puddles and shallow water. Off by default (Beta).
 */
struct GPUFootstepParticles : Feature, RE::BSTEventSink<RE::BGSFootstepEvent>
{
	virtual inline std::string GetName() override { return "GPU Footstep Particles"; }
	virtual inline std::string GetDisplayName() override { return T("feature.gpu_footstep_particles.name", "GPU Footstep Particles"); }
	virtual inline std::string GetShortName() override { return "GPUFootstepParticles"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual inline bool SupportsVR() override { return false; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.gpu_footstep_particles.description", "GPU-simulated dust, snow, mud clods and water splashes kicked up by footsteps."),
			{ T("feature.gpu_footstep_particles.key_1", "Dust and clods on dirt, gravel, sand and ash; snow powder on snow"),
				T("feature.gpu_footstep_particles.key_2", "Wet mud clods in rain and on mud"),
				T("feature.gpu_footstep_particles.key_3", "Splashes scale with puddle and shallow water depth"),
				T("feature.gpu_footstep_particles.key_4", "Lit by the sun and ambient light, soft against the ground") } };
	}

	struct Settings
	{
		bool Enabled = true;  // GPU footstep collision; SSPP also scatters resting particles in the same step
		bool IncludeNPCs = true;
		bool Water = true;
		bool RainWetness = true;
		float DustIntensity = 1.0f;
		float SnowIntensity = 1.0f;
		float MudIntensity = 1.0f;
		float SplashIntensity = 1.0f;
		float ParticleSize = 1.0f;
		float Lifetime = 1.0f;
		float Opacity = 1.0f;
		float MaxDistance = 3000.0f;
		float MaxShallowDepth = 40.0f;
	} settings;

	static constexpr uint32_t kMaxParticles = 16384;
	static constexpr uint32_t kMaxEventsPerFrame = 64;
	static constexpr uint32_t kSlotsPerEvent = 64;

	struct alignas(16) Particle
	{
		float pos[3];
		float age;
		float vel[3];
		float lifetime;
		float color[3];
		float size;
		uint32_t type;
		float groundZ;
		float growth;
		float opacity;
	};
	static_assert(sizeof(Particle) == 64);

	struct Event
	{
		float position[3];
		uint32_t firstSlot;
		float velocity[3];
		uint32_t surface;
		float forward[2];
		float strength;
		float scale;
		uint32_t seed;
		uint32_t flags;
		uint32_t slotCount;
		float waterDepth;
	};
	static_assert(sizeof(Event) == 64);

	struct alignas(16) FrameCB
	{
		float4 fogNear;
		float4 fogFar;
		float4 fogParams;
		float4 cameraPosAdjust;
		float deltaTime;
		float gravity;
		uint32_t eventCount;
		uint32_t maxParticles;
		float dust;
		float snow;
		float splash;
		float sizeScale;
		float lifetimeScale;
		float opacity;
		float softDistance;
		uint32_t frameFlags;
		float wind[2];
		float wetness;
		float mud;
	};

	virtual void SetupResources() override;
	virtual void DataLoaded() override;
	virtual void PostComposite() override;
	virtual void ClearShaderCache() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*) override;

private:
	void EnsureShaders();
	uint32_t Classify(RE::Actor* a_actor, const RE::NiPoint3& a_foot, float& a_waterDepth, float& a_surfaceZ) const;

	winrt::com_ptr<ID3D11Buffer> particleBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> particleSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> particleUAV;
	winrt::com_ptr<ID3D11Buffer> eventBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> eventSRV;
	winrt::com_ptr<ID3D11Buffer> frameCB;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11RasterizerState> rasterState;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;

	winrt::com_ptr<ID3D11ComputeShader> spawnCS;
	winrt::com_ptr<ID3D11ComputeShader> simulateCS;
	winrt::com_ptr<ID3D11VertexShader> particleVS;
	winrt::com_ptr<ID3D11PixelShader> particlePS;
	bool shadersFailed = false;

	std::mutex eventMutex;
	std::vector<Event> pendingEvents;
	uint32_t nextSlot = 0;
	uint32_t seedCounter = 1;
	float aliveTimer = 0.0f;  // seconds since the last spawn; skips simulation once everything has died
	std::chrono::steady_clock::time_point lastFrame{};
};
