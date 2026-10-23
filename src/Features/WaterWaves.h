#pragma once

#include "WaterWavesMath.h"

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
		return { T("feature.water_waves.description", "The existing water mesh is pushed into waves. The far water stays flat."),
			{ T("feature.water_waves.key_1", "Nine waves on the vanilla water mesh"),
				T("feature.water_waves.key_2", "The shoreline stays on the flat plane"),
				T("feature.water_waves.key_3", "Actors and the camera use the same height") } };
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
	} settings;

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;

	/** @brief Fills the shared-data wave fields and the camera-under-waves flag. */
	void ApplyShared(float& a_displacedHeight, float& a_cameraBelow, float& a_scale, float& a_amplitude, float& a_chop, float& a_shore, float& a_windSpeed, float& a_windAngle, float& a_shoreOriginX, float& a_shoreOriginY, float& a_shoreCellSize, float& a_shoreCellCount);
	/** @brief Flat water height plus the Gerstner offset, or the flat height on a flowmap. */
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
	std::vector<float> shoreCPU;
	float shoreOriginX = 0.0f;
	float shoreOriginY = 0.0f;
	int shoreCursor = 0;
	bool shoreReady = false;
	bool tessBound = false;
	bool vr = false;
};
