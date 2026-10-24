#pragma once

#include <array>
#include <atomic>
#include <d3d12.h>
#include <string>
#include <winrt/base.h>

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>

#include <FidelityFX/api/include/ffx_api.hpp>
#include <FidelityFX/api/include/ffx_api_loader.h>
#include <FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp>
#include <FidelityFX/framegeneration/include/ffx_framegeneration.hpp>
#include <FidelityFX/upscalers/include/ffx_upscale.hpp>

#include "../../Buffer.h"
#include "../../State.h"
#include "DX12SwapChain.h"

/** @brief Manages AMD FidelityFX upscaling and frame generation.
 *  Upscaling is FSR 4.1 through the DX12 runtime DLL when the driver reports 4.x, otherwise the original DX11 FSR 3.1 path.
 *  Frame generation stays on the FSR 3 frame-generation DLL. */
class FidelityFX
{
public:
	static constexpr const wchar_t* PluginDir = L"Data\\Shaders\\Upscaling\\FidelityFX";
	static constexpr std::wstring_view RuntimeUpscalerDllName = L"amd_fidelityfx_upscaler_dx12.dll";

	HMODULE module = nullptr;

	ffx::Context swapChainContext{};
	ffx::Context frameGenContext;
	FfxFsr3Context fsrContext[2];

	bool featureFSR3FG = false;
	bool featureRuntimeUpscaler = false;

	// Track if FidelityFX is currently being used for frame generation
	bool isFrameGenActive = false;

	// Track HDR state for frame generation callback (needs to be accessible from static callback)
	// Using atomic for thread safety since async workloads may read this from different threads
	static inline std::atomic<bool> isHDRActive = false;
	static inline std::atomic<float> hdrPeakNits = 1000.0f;
	static inline std::atomic<bool> needsReset = false;

	// Track previous HDR parameters to detect changes that require FG reset
	bool prevHDRActive = false;
	float prevPeakNits = 1000.0f;

	// Cached DLL version info for FidelityFX plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	/** @brief Loads the FidelityFX loader and frame generation DLLs from the plugin directory. */
	void LoadFFX();
	/** @brief Creates the FidelityFX frame generation context for the current swap chain. */
	void SetupFrameGeneration();
	/**
	 * @brief Presents the current frame, optionally performing FidelityFX frame generation.
	 * @param a_useFrameGeneration Whether to enable frame generation for this present call.
	 * @param a_isHDR Whether HDR output is active, affecting color space configuration.
	 */
	void Present(bool a_useFrameGeneration, bool a_isHDR = false);

	/** @brief Creates FSR3 upscaling resources including scratch buffers and context. */
	void CreateFSRResources();

	/** @brief Destroys FSR3 upscaling resources and frees the scratch buffer. */
	void DestroyFSRResources();

	/**
	 * @brief Dispatches FSR3 upscaling for the current frame.
	 * @param a_upscalingTexture The input color texture to upscale.
	 * @param a_reactiveMask Reactive mask identifying areas needing temporal stability.
	 * @param a_transparencyCompositionMask Mask for transparency and composition handling.
	 * @param a_motionVectors Per-pixel motion vectors for temporal reprojection.
	 * @param a_sharpness RCAS sharpening strength applied after upscaling.
	 */
	void Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors, float a_sharpness);

	enum class Fsr4AdapterSupport
	{
		Unsupported,
		RadeonRx6000,
		RadeonRx7000,
		RadeonRx9000
	};

	static Fsr4AdapterSupport GetFsr4AdapterSupport(const DXGI_ADAPTER_DESC& a_adapterDesc);
	void NoteAdapter(const DXGI_ADAPTER_DESC& a_adapterDesc);
	Fsr4AdapterSupport GetFsr4AdapterSupport() const;
	bool IsRuntimeFsr4AutoEligible() const;
	bool IsRuntimeFsr4FailureLatched() const { return runtimeFsr4FailureLatched; }
	/** @brief "AMD FSR 4" after the driver reports a 4.x provider, otherwise "AMD FSR 3.1". */
	const char* MenuMethodLabel();
	/** @brief Queries the runtime DLL for a 4.x upscaler. Safe to call from the menu. */
	void ProbeRuntimeFsr4();

private:
	bool TryRuntimeFsr4Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors, float a_sharpness);
	void LatchRuntimeFsr4Failure(const char* a_reason);
	void DestroyRuntimeUpscaler();
	bool EnsureRuntimeUpscalerInterop();
	bool EnsureRuntimeFsr4Context(uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight);
	bool DispatchRuntimeFsr4(ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output, uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight, float a_sharpness);

	Fsr4AdapterSupport adapterSupport = Fsr4AdapterSupport::Unsupported;
	bool adapterSupportKnown = false;
	bool runtimeFsr4FailureLatched = false;
	bool runtimeFsr4Quarantined = false;
	bool driverReportsFsr4 = false;
	uint32_t reportedFsr4Version = 0;
	uint64_t reportedFsr4VersionId = 0;
	std::string reportedFsr4VersionName;
	bool runtimeVersionProbeAttempted = false;

	ffx::Context runtimeUpscalerContext{};
	uint32_t runtimeRenderWidth = 0;
	uint32_t runtimeRenderHeight = 0;
	uint32_t runtimeDisplayWidth = 0;
	uint32_t runtimeDisplayHeight = 0;
	SharedFence runtimeFence;
	HMODULE runtimeUpscalerModule = nullptr;

	static constexpr uint32_t kRuntimeCommandContextCount = 3;
	struct RuntimeCommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;
		uint64_t fenceValue = 0;
	};
	std::array<RuntimeCommandContext, kRuntimeCommandContextCount> runtimeCommandContexts{};
	uint32_t runtimeCommandContextCursor = 0;

	WrappedResource* runtimeColor = nullptr;
	WrappedResource* runtimeDepth = nullptr;
	WrappedResource* runtimeMotion = nullptr;
	WrappedResource* runtimeReactive = nullptr;
	WrappedResource* runtimeTransparency = nullptr;
	WrappedResource* runtimeOutput = nullptr;
	D3D11_TEXTURE2D_DESC runtimeColorDesc{};
	D3D11_TEXTURE2D_DESC runtimeDepthDesc{};
	D3D11_TEXTURE2D_DESC runtimeMotionDesc{};
	D3D11_TEXTURE2D_DESC runtimeReactiveDesc{};
	D3D11_TEXTURE2D_DESC runtimeTransparencyDesc{};
	D3D11_TEXTURE2D_DESC runtimeOutputDesc{};

	// FSR scratch buffer - needs to be freed in DestroyFSRResources
	void* fsrScratchBuffer = nullptr;

	// Flag to prevent spamming the log with FSR3 dispatch crash messages
	bool fsrDispatchCrashLogged = false;
};
