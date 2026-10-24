#pragma once

#include "PostProcessFeature.h"

#include "Buffer.h"

/// Compute bloom (bloom.cs). Separate from pixel COD Bloom and from ENB Bloom.
/// Off by default; the pixel path is unchanged while this stays disabled.
struct NativeBloom : public PostProcessFeature
{
	bool outputReady = false;
	virtual inline std::string GetType() const override { return "Native Bloom"; }
	virtual inline std::string GetDisplayName() const override { return T("feature.post_processing.nativebloom.name", "Native Bloom"); }
	virtual inline std::string GetDesc() const override
	{
		return T("feature.post_processing.nativebloom.description",
			"Compute bloom (bloom.cs). Separate from COD Bloom (pixel) and ENB Bloom. Off leaves those paths unchanged. When on and the compute result is ready, Composite uses it instead of COD Bloom.");
	}
	virtual bool WritesToMainTexture() const override { return false; }
	virtual inline bool DisableInMainLoadingMenu() const override { return true; }

	TextureInfo GetBloomOutput() const { return outputReady && texBloom ? TextureInfo{ texBloom->resource.get(), texBloomMipSRVs[0].get() } : TextureInfo{}; }

	constexpr static size_t s_BloomMips = 9;

	struct Settings
	{
		// Defaults from the compute bloom feature, not the pixel COD Bloom defaults.
		float Threshold = 3.f;  // EV100 (0 EV100 = 0.125 linear luminance)
		float UpsampleRadius = 2.f;
		float BlendFactor = .01f;
		std::array<float, s_BloomMips - 1> MipBlendFactor = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
	} settings;

	struct alignas(16) BloomCB
	{
		float Threshold;
		float UpsampleRadius;
		float UpsampleMult;
		float CurrentMipMult;
	};
	std::unique_ptr<ConstantBuffer> bloomCB = nullptr;

	winrt::com_ptr<ID3D11SamplerState> colorSampler = nullptr;

	std::unique_ptr<Texture2D> texBloom = nullptr;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_BloomMips> texBloomMipSRVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_BloomMips> texBloomMipUAVs = { nullptr };

	winrt::com_ptr<ID3D11ComputeShader> thresholdCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> downsampleCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> downsampleFirstMipCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> compositeCS = nullptr;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};
