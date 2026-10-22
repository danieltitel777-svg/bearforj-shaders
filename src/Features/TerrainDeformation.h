#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <unordered_map>

/**
 * @brief Bearforj Terrain Deformation: footprints and trails pressed into snow, mud, sand and dirt.
 *
 * CPU side keeps a list of oriented prints (from BGSFootstepEvent plus a stride-based fallback for actors
 * that do not emit footstep events). Each frame the prints inside the window are uploaded (t54 header + prints)
 * and stamped by compute into a 2048^2 heightmap (t56) plus a 64^2 activity map (t57). Those are read by:
 *  - the landscape hull shader (activity map gates tessellation),
 *  - the landscape domain and pixel shaders (displacement, normals, darkening), and
 *  - the utility depth and shadow vertex shader (so the depth prepass / AO and shadow maps see the offset).
 * The heightmap is stamped at the start of shadow rendering, before the z-prepass.
 * Grass is never displaced: prints on grass material are soft-disabled (GrassStrength, default 0).
 * Snow, mud and sand use different depth, radius, rim and shoulder profiles.
 */
struct TerrainDeformation : Feature
{
	virtual inline std::string GetName() override { return "Terrain Deformation"; }
	virtual std::string GetDisplayName() override { return T("feature.terrain_deformation.name", "Terrain Deformation"); }
	virtual inline std::string GetShortName() override { return "TerrainDeformation"; }
	virtual inline std::string_view GetShaderDefineName() override { return "TERRAIN_DEFORMATION"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override
	{
		return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Utility;
	}
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.terrain_deformation.description", "Footprints are pressed into snow, mud and sand and slowly fill back in."),
			{ T("feature.terrain_deformation.key_1", "Footprints from the player and NPCs"),
				T("feature.terrain_deformation.key_2", "Real displacement with landscape tessellation near the camera"),
				T("feature.terrain_deformation.key_3", "Depth depends on the ground material; grass is left untouched"),
				T("feature.terrain_deformation.key_4", "Prints slowly fill back in") } };
	}

	struct Settings
	{
		bool Enabled = true;
		bool IncludeNPCs = true;
		bool EnableTessellation = true;
		bool StrideFallback = true;  // prints for actors that emit no footstep events (creatures, some NPC skeletons)
		float Radius = 4096.0f;      // prints further than this from the player are not uploaded
		float PrintRadius = 9.0f;    // foot length in game units
		float Depth = 3.0f;          // base depth in game units
		float Rim = 0.25f;           // raised rim height as a fraction of depth
		float Darkening = 0.25f;
		float Lifetime = 240.0f;     // seconds until a print has fully filled back in
		float SnowStrength = 1.0f;
		float MudStrength = 0.85f;
		float SandStrength = 0.75f;
		float DirtStrength = 0.45f;
		float GravelStrength = 0.2f;
		float GrassStrength = 0.0f;   // soft-disabled on grass (decision 0.6)
		float OtherStrength = 0.0f;   // stone, wood, etc.
		float MaxTessFactor = 16.0f;
		float TessRange = 1536.0f;
		float DisplacementScale = 1.0f;
		float NormalStrength = 1.0f;
		// Raised layer in game units. Footprints press into it. 0 leaves the old dip-only height.
		float SnowRaise = 60.0f;
		float MudRaise = 18.0f;
		float SandRaise = 10.0f;
		float SinkFraction = 0.65f;  // how much of the layer a foot presses down
	} settings;

	struct GPUPrint
	{
		float4 A;
		float4 B;
	};
	static_assert(sizeof(GPUPrint) == 32);

	struct PrintRecord
	{
		float x, y, z, yaw, radius, depth, rim, dark, material, shoulder;
		float spawnTime;
	};

	static constexpr uint32_t kMaxGPUPrints = 511;  // + 1 header element
	static constexpr size_t kMaxStoredPrints = 4096;
	// Must match features/Terrain Deformation/Shaders/TerrainDeformation/HeightmapCommon.hlsli
	static constexpr uint32_t kHeightmapSize = 2048;
	static constexpr float kHeightmapTexel = 2.0f;
	static constexpr uint32_t kActivitySize = 64;
	// Coarse material grid over the same 4096-unit window. R = raise (units), G = sink fraction.
	static constexpr uint32_t kRaiseCells = 32;
	static constexpr float kRaiseCell = 128.0f;
	static constexpr uint32_t kRaiseRowsPerFrame = 8;

	virtual void SetupResources() override;
	/** @brief Uploads prints and stamps the heightmap. Called before shadow maps, not from Feature::Prepass (that runs after the z-prepass). */
	void UpdateHeightmap();
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;
	virtual void OnSceneTransitionReset(bool a_opening) override;

	/** @brief Adds a print at an absolute world position if the ground material allows it. */
	void AddPrint(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_yaw, bool a_leftFoot);
	void OnFootstep(RE::Actor* a_actor, std::string_view a_tag);
	void ClearPrints();
	size_t GetStoredPrintCount();

	/** @brief Lighting shader SetupGeometry/RestoreGeometry: binds HS/DS + patch topology for near landscape. */
	void OnLightingSetupGeometry(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);
	void OnLightingRestoreGeometry();
	/** @brief Utility shader SetupGeometry/RestoreGeometry: vertex-offsets near landscape in the depth and shadow passes. */
	void OnUtilitySetupGeometry(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);
	void OnUtilityRestoreGeometry();

private:
	float MaterialStrength(RE::MATERIAL_ID a_material) const;
	void UpdateStrideFallback();
	struct TessShaders
	{
		winrt::com_ptr<ID3D11HullShader> hs;
		winrt::com_ptr<ID3D11DomainShader> ds;
		std::atomic<int> state{ 0 };  // 0 queued/compiling, 1 ready, 2 failed
	};
	/** @brief Returns ready shaders, or nullptr (and queues a background compile) - never compiles on the render thread. */
	TessShaders* GetTessShaders(RE::BSShader* a_shader, uint32_t a_vertexDescriptor);
	void QueueTessCompile(RE::BSShader* a_shader, uint32_t a_vertexDescriptor);
	void WarmTessCache();
	void CompileWorker(std::stop_token a_stop);
	void LoadKnownDescriptors();
	void SaveKnownDescriptors();

	struct CompileJob
	{
		uint32_t descriptor;
		std::wstring path;
		std::vector<std::pair<std::string, std::string>> defines;
		std::shared_ptr<TessShaders> target;
	};
	std::mutex tessLock;
	std::condition_variable_any tessCv;
	std::deque<CompileJob> tessQueue;
	std::jthread tessWorker;
	RE::BSShader* lightingShader = nullptr;
	std::vector<uint32_t> knownDescriptors;
	bool knownDirty = false;

	winrt::com_ptr<ID3D11Buffer> printBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> printSRV;
	winrt::com_ptr<ID3D11Texture2D> heightTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> heightSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> heightUAV;
	winrt::com_ptr<ID3D11Texture2D> activityTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> activitySRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> activityUAV;
	winrt::com_ptr<ID3D11ComputeShader> clearCS;
	winrt::com_ptr<ID3D11ComputeShader> stampCS;
	struct RaiseTexel
	{
		float raise;
		float sink;
	};
	static_assert(sizeof(RaiseTexel) == 8);
	winrt::com_ptr<ID3D11Texture2D> raiseTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> raiseSRV;
	std::vector<RaiseTexel> raiseCPU;
	float raiseOriginX = 0.0f;
	float raiseOriginY = 0.0f;
	uint32_t raiseCursor = 0;
	bool raisePrimed = false;
	bool raiseActive = false;
	winrt::com_ptr<ID3D11Buffer> depthDrawCB;
	bool depthDisplace = false;
	float windowOriginX = 0.0f;
	float windowOriginY = 0.0f;
	std::vector<GPUPrint> upload;

	struct SurfaceProfile
	{
		float depth;
		float radius;
		float rim;
		float shoulder;
		float dark;
	};
	static SurfaceProfile ProfileFor(RE::MATERIAL_ID a_material);
	void CreateUintTexture(UINT a_size, winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, const char* a_name);
	enum class ShaderStage
	{
		VS,
		HS,
		DS,
		PS
	};
	void BindHeightViews(ShaderStage a_stage) const;
	void StampHeightmap(ID3D11DeviceContext* a_context);
	float LayerRaise(RE::MATERIAL_ID a_material) const;
	void UpdateRaiseMap(const RE::NiPoint3& a_center);
	void UploadRaiseMap();
	void SetDepthDisplace(bool a_on);
	std::deque<PrintRecord> prints;
	std::mutex printsLock;
	float time = 0.0f;
	uint32_t uploadedCount = 0;

	struct ActorTrack
	{
		RE::NiPoint3 lastPrint{};
		float lastEventTime = -1000.0f;
		bool leftNext = true;
		bool valid = false;
	};
	std::unordered_map<RE::FormID, ActorTrack> tracks;

	std::unordered_map<uint32_t, std::shared_ptr<TessShaders>> tessShaders;
	bool tessellationBound = false;
	bool vr = false;

	struct FootstepSink : public RE::BSTEventSink<RE::BGSFootstepEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>* a_source) override;
	} footstepSink;
};
