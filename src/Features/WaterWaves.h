#pragma once

#include "WaterWavesMath.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct WaterWaves : Feature
{
	virtual inline std::string GetName() override { return "Water Waves"; }
	virtual std::string GetDisplayName() override { return T("feature.water_waves.name", "Water Waves"); }
	virtual inline std::string GetShortName() override { return "WaterWaves"; }
	virtual inline std::string_view GetShaderDefineName() override { return "WATER_WAVES"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override
	{
		return a_type == RE::BSShader::Type::Water || a_type == RE::BSShader::Type::Lighting;
	}
	virtual bool IsDisabledByDefault() const override { return false; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.water_waves.description", "The water mesh is tessellated into swells. Wind, rain, depth and foam follow the weather. Flowing water keeps the swells, headed downstream. Far water stays flat."),
			{ T("feature.water_waves.key_1", "Tessellated swells on lakes, rivers and distant water, including when the shader cache is off"),
				T("feature.water_waves.key_2", "Calm is a ripple, wind builds swells, a folding crest lets the sun through and leaves foam"),
				T("feature.water_waves.key_3", "Foam on the shore, on rocks and on the crests. On a current it piles up in front of an obstacle. Actors use the same height") } };
	}

	struct Settings
	{
		bool Enabled = true;
		float Amplitude = 22.0f;
		float Chop = 0.6f;
		float ShoreLength = 160.0f;
		float SeaScale = 1.0f;
		float InteriorScale = 0.35f;
		float MaxTessFactor = 8.0f;
		float TessRange = 2048.0f;
		float Foam = 1.0f;
		float Detail = 1.0f;
		// Off restores the unshifted detail ripple. The height is the Gerstner jacobian, not a foam texture.
		bool FoamParallax = true;
		// Elemental Contact shore, river, rock and crest foam. Off leaves the original shading.
		bool ShoreContact = true;
		// Foam lace and wave-detail normal shift by the height maps. Off samples those textures flat.
		bool HeightParallax = true;
	} settings;

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;

	/** @brief Fills the shared-data wave fields and the camera-under-waves flag. */
	void ApplyShared(float& a_displacedHeight, float& a_cameraBelow, float& a_scale, float& a_amplitude, float& a_chop, float& a_shore, float& a_windSpeed, float& a_windAngle, float& a_shoreOriginX, float& a_shoreOriginY, float& a_shoreCellSize, float& a_shoreCellCount, float& a_foam, float& a_detail, float& a_murk, float& a_storm);
	/** @brief Flat water height plus the Gerstner offset. Flowing cells keep the swell. */
	float DisplacedHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos, float a_flatHeight) const;
	void OnSetup(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);
	void OnLightingSetup();
	void OnRestore();

	static constexpr int kShoreCells = 64;
	static constexpr int kShoreRowsPerFrame = 8;
	static constexpr float kShoreCell = 224.0f;

private:
	WaterWaveMath::Params MakeParams(bool a_flowmap) const;
	bool CellUsesFlowmap(const RE::TESObjectREFR* a_ref) const;
	void UpdateShoreGrid();
	void UploadShoreGrid();
	void FillShoreRow(int a_y);
	void FillShoreColumn(int a_x);
	void ShiftShore(int a_dx, int a_dy);
	void BindShore() const;
	void BindFlowMaps() const;
	void BindElementalContact();
	void UnbindElementalContact();
	void UpdateWaterfalls();

	struct TessShaders
	{
		winrt::com_ptr<ID3D11HullShader> hs;
		winrt::com_ptr<ID3D11DomainShader> ds;
		std::atomic<int> state{ 0 };
	};
	TessShaders* GetTessShaders(RE::BSShader* a_shader, uint32_t a_descriptor);
	void QueueTessCompile(RE::BSShader* a_shader, uint32_t a_descriptor);
	void CompileWorker(std::stop_token a_stop);

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
	std::unordered_map<uint32_t, std::shared_ptr<TessShaders>> tessShaders;
	winrt::com_ptr<ID3D11Buffer> tessCB;
	winrt::com_ptr<ID3D11Texture2D> shoreTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> shoreSRV;
	winrt::com_ptr<ID3D11Texture2D> neutralFlowTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> neutralFlowSRV;
	winrt::com_ptr<ID3D11Texture2D> neutralFlowNormalTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> neutralFlowNormalSRV;
	winrt::com_ptr<ID3D11SamplerState> flowSampler;
	std::vector<float> shoreCPU;
	float shoreOriginX = 0.0f;
	float shoreOriginY = 0.0f;
	int shoreCursor = 0;
	bool shoreReady = false;
	bool tessBound = false;
	bool vr = false;
	winrt::com_ptr<ID3D11Buffer> ecCB;
	winrt::com_ptr<ID3D11ShaderResourceView> ecFoamSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> ecNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> ecFoamHeightSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> ecWaveHeightSRV;
	bool ecFoamFile = false;
	bool ecNormalFile = false;
	bool ecFoamHeightFile = false;
	bool ecWaveHeightFile = false;
	bool ecBound = false;
	struct FallPoint
	{
		float x;
		float y;
		float z;
		float radius;
	};
	std::vector<FallPoint> fallPoints;
	float fallAccum = 1.0f;
	mutable std::atomic<float> smoothedWind{ 0.0f };
	mutable std::atomic<float> smoothedAngle{ 0.0f };
	mutable std::atomic<float> smoothedRain{ 0.0f };
	mutable std::atomic<bool> weatherInit{ false };

	void UpdateWeather();
	float RainTarget() const;
	bool ReadSharedWind(float& a_speed, float& a_angle) const;
};
