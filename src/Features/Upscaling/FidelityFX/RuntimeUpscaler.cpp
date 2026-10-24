// DX12 runtime FSR 4.1 path. The host DX11 FSR 3.1 dispatch in FidelityFX.cpp stays
// the fallback and the path used when the menu toggle is off. Frame generation is not touched.

#include "../FidelityFX.h"
#include "Fsr4AdapterClassify.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "../../../State.h"
#include "../../../Utils/D3D.h"
#include "../../../Utils/SehGuard.h"
#include "../../Upscaling.h"

extern ffxFunctions ffxModule;

namespace
{
	constexpr uint32_t kAmdVendorId = 0x1002u;

	std::string ToUpperAscii(std::string a_value)
	{
		std::transform(a_value.begin(), a_value.end(), a_value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return a_value;
	}

	uint32_t UpscalerMajor(uint32_t a_version)
	{
		return (a_version >> 22) & 0x3FFu;
	}

	std::string UpscalerVersionToString(uint32_t a_version)
	{
		const uint32_t major = UpscalerMajor(a_version);
		const uint32_t minor = (a_version >> 12) & 0x3FFu;
		const uint32_t patch = a_version & 0xFFFu;
		return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	}

	bool VersionIdMatches(uint64_t a_id, uint32_t a_version)
	{
		return a_id != 0 && static_cast<uint32_t>(a_id & 0xFFFFFFFFull) == a_version;
	}

	bool NameMatchesVersion(const std::string& a_name, uint32_t a_version)
	{
		return !a_name.empty() && a_name.find(UpscalerVersionToString(a_version)) != std::string::npos;
	}

	bool TryGetTexture2DDesc(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_outDesc)
	{
		if (!a_resource)
			return false;
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(a_resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;
		texture->GetDesc(&a_outDesc);
		return true;
	}

	D3D11_TEXTURE2D_DESC MakeSharedTextureDesc(const D3D11_TEXTURE2D_DESC& a_sourceDesc, uint32_t a_width, uint32_t a_height, UINT a_bindFlags)
	{
		D3D11_TEXTURE2D_DESC desc = a_sourceDesc;
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.BindFlags = a_bindFlags;
		desc.MiscFlags = 0;
		return desc;
	}

	bool SameTextureDesc(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
	{
		return a_left.Width == a_right.Width &&
		       a_left.Height == a_right.Height &&
		       a_left.Format == a_right.Format &&
		       a_left.BindFlags == a_right.BindFlags;
	}

	void DeleteWrapped(WrappedResource*& a_resource)
	{
		delete a_resource;
		a_resource = nullptr;
	}

	struct VersionCandidate
	{
		uint64_t id = 0;
		std::string name;
		uint32_t version = 0;
	};

	bool NameLooksLikeFsr4(const std::string& a_name)
	{
		return a_name.find("4.") != std::string::npos || a_name.find("MLFI") != std::string::npos;
	}

	bool QueryFsr4Version(ID3D12Device* a_device, uint64_t a_createDescType, uint32_t a_apiVersion, VersionCandidate& a_out)
	{
		a_out = {};
		if (!a_device || !ffxModule.Query)
			return false;

		uint64_t versionCount = 0;
		ffxQueryDescGetVersions countQuery{};
		countQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		countQuery.createDescType = a_createDescType;
		countQuery.device = a_device;
		countQuery.outputCount = &versionCount;

		if (ffxModule.Query(nullptr, &countQuery.header) != FFX_API_RETURN_OK || versionCount == 0)
			return false;

		std::vector<uint64_t> versionIds(static_cast<size_t>(versionCount), 0);
		std::vector<const char*> versionNames(static_cast<size_t>(versionCount), nullptr);
		uint64_t returnedCount = versionCount;
		ffxQueryDescGetVersions versionsQuery{};
		versionsQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
		versionsQuery.createDescType = a_createDescType;
		versionsQuery.device = a_device;
		versionsQuery.outputCount = &returnedCount;
		versionsQuery.versionIds = versionIds.data();
		versionsQuery.versionNames = versionNames.data();

		if (ffxModule.Query(nullptr, &versionsQuery.header) != FFX_API_RETURN_OK)
			return false;

		const size_t resultCount = std::min(versionIds.size(), static_cast<size_t>(returnedCount));
		VersionCandidate best{};
		bool found = false;
		for (size_t i = 0; i < resultCount; ++i) {
			const std::string name = versionNames[i] ? versionNames[i] : "";
			const uint32_t low = static_cast<uint32_t>(versionIds[i] & 0xFFFFFFFFull);
			const bool encodedFsr4 = UpscalerMajor(low) == 4;
			const bool namedFsr4 = NameLooksLikeFsr4(name);
			if (!encodedFsr4 && !namedFsr4)
				continue;

			VersionCandidate candidate{ versionIds[i], name, encodedFsr4 ? low : a_apiVersion };
			const bool exact = VersionIdMatches(versionIds[i], a_apiVersion) || NameMatchesVersion(name, a_apiVersion);
			if (!found || exact || candidate.version > best.version) {
				best = std::move(candidate);
				found = true;
				if (exact)
					break;
			}
		}

		if (!found)
			return false;
		a_out = std::move(best);
		return true;
	}

	enum class CreateAttempt : uint8_t
	{
		kOverride,
		kVersionDescriptor,
		kDefaultProvider
	};

	ffxReturnCode_t TryCreateContext(
		ffx::Context& a_context,
		CreateAttempt a_attempt,
		ffx::CreateContextDescUpscale& a_createDesc,
		ffx::CreateBackendDX12Desc& a_backendDesc,
		ffx::CreateContextDescUpscaleVersion& a_versionDesc,
		ffx::CreateContextDescOverrideVersion& a_overrideDesc)
	{
		a_context = nullptr;
		a_createDesc.header.pNext = nullptr;
		a_backendDesc.header.pNext = nullptr;
		a_versionDesc.header.pNext = nullptr;
		a_overrideDesc.header.pNext = nullptr;

		switch (a_attempt) {
		case CreateAttempt::kOverride:
			a_createDesc.header.pNext = &a_backendDesc.header;
			a_backendDesc.header.pNext = &a_versionDesc.header;
			a_versionDesc.header.pNext = &a_overrideDesc.header;
			break;
		case CreateAttempt::kVersionDescriptor:
			a_createDesc.header.pNext = &a_versionDesc.header;
			a_versionDesc.header.pNext = &a_backendDesc.header;
			break;
		case CreateAttempt::kDefaultProvider:
			a_createDesc.header.pNext = &a_backendDesc.header;
			break;
		}

		ffx::Context created = nullptr;
		const auto result = ffxModule.CreateContext(&created, &a_createDesc.header, nullptr);
		if (result == FFX_API_RETURN_OK && created) {
			a_context = created;
			return result;
		}
		if (created && ffxModule.DestroyContext)
			(void)ffxModule.DestroyContext(&created, nullptr);
		return result == FFX_API_RETURN_OK ? FFX_API_RETURN_ERROR : result;
	}
}

bool FidelityFX::QueryFsr4Provider(uint64_t a_createDescType, uint32_t a_apiVersion, uint64_t& a_versionId, uint32_t& a_version, std::string& a_name)
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	VersionCandidate selected{};
	if (!QueryFsr4Version(swapChain.d3d12Device.get(), a_createDescType, a_apiVersion, selected))
		return false;
	a_versionId = selected.id;
	a_version = selected.version;
	a_name = std::move(selected.name);
	return true;
}

FidelityFX::Fsr4AdapterSupport FidelityFX::GetFsr4AdapterSupport(const DXGI_ADAPTER_DESC& a_adapterDesc)
{
	std::wstring wide(a_adapterDesc.Description);
	const std::string description = ToUpperAscii(stl::utf16_to_utf8(wide).value_or(""));
	switch (BearforjFsr4::ClassifyUpper(a_adapterDesc.VendorId, description)) {
	case BearforjFsr4::Support::Rx6000:
		return Fsr4AdapterSupport::RadeonRx6000;
	case BearforjFsr4::Support::Rx7000:
		return Fsr4AdapterSupport::RadeonRx7000;
	case BearforjFsr4::Support::Rx9000:
		return Fsr4AdapterSupport::RadeonRx9000;
	default:
		return Fsr4AdapterSupport::Unsupported;
	}
}

void FidelityFX::NoteAdapter(const DXGI_ADAPTER_DESC& a_adapterDesc)
{
	adapterSupport = GetFsr4AdapterSupport(a_adapterDesc);
	adapterSupportKnown = true;
	if (a_adapterDesc.VendorId == kAmdVendorId) {
		logger::info("[FidelityFX] Adapter '{}' FSR 4.1 class {}",
			stl::utf16_to_utf8(std::wstring(a_adapterDesc.Description)).value_or(""),
			adapterSupport == Fsr4AdapterSupport::Unsupported ? "unsupported" : "eligible");
	}
}

FidelityFX::Fsr4AdapterSupport FidelityFX::GetFsr4AdapterSupport() const
{
	if (adapterSupportKnown)
		return adapterSupport;
	return Fsr4AdapterSupport::Unsupported;
}

bool FidelityFX::IsRuntimeFsr4AutoEligible() const
{
	return GetFsr4AdapterSupport() != Fsr4AdapterSupport::Unsupported;
}

void FidelityFX::LatchRuntimeFsr4Failure(const char* a_reason)
{
	if (runtimeFsr4FailureLatched)
		return;
	runtimeFsr4FailureLatched = true;
	driverReportsFsr4 = false;
	logger::warn("[FidelityFX] FSR 4.1 did not start ({}); falling back to FSR 3.1", a_reason ? a_reason : "unknown");
}

const char* FidelityFX::MenuMethodLabel()
{
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.fsr4RuntimeEnableBoot || runtimeFsr4FailureLatched || !driverReportsFsr4)
		return "AMD FSR 3.1";
	return "AMD FSR 4";
}

void FidelityFX::ProbeRuntimeFsr4()
{
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.fsr4RuntimeEnableBoot || runtimeFsr4FailureLatched || runtimeVersionProbeAttempted)
		return;
	if (!globals::d3d::device || !globals::d3d::context)
		return;
	if (!featureRuntimeUpscaler) {
		runtimeVersionProbeAttempted = true;
		LatchRuntimeFsr4Failure("amd_fidelityfx_upscaler_dx12.dll is not loaded");
		return;
	}
	if (!EnsureRuntimeUpscalerInterop()) {
		runtimeVersionProbeAttempted = true;
		LatchRuntimeFsr4Failure("DX12 interop device was not created");
		return;
	}

	runtimeVersionProbeAttempted = true;
	VersionCandidate selected{};
	auto& swapChain = upscaling.dx12SwapChain;
	if (!QueryFsr4Version(swapChain.d3d12Device.get(), FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, FFX_UPSCALER_VERSION, selected)) {
		LatchRuntimeFsr4Failure("driver did not report an FSR 4.x upscaler");
		return;
	}

	reportedFsr4VersionId = selected.id;
	reportedFsr4Version = selected.version != 0 ? selected.version : FFX_UPSCALER_VERSION;
	reportedFsr4VersionName = selected.name;
	driverReportsFsr4 = UpscalerMajor(reportedFsr4Version) == 4 || NameLooksLikeFsr4(selected.name);
	logger::info("[FidelityFX] Driver upscaler provider '{}' id 0x{:X} ({})",
		reportedFsr4VersionName.empty() ? "unnamed" : reportedFsr4VersionName,
		reportedFsr4VersionId,
		UpscalerVersionToString(reportedFsr4Version));
	if (!driverReportsFsr4)
		LatchRuntimeFsr4Failure("queried provider is not FSR 4.x");
}

bool FidelityFX::EnsureRuntimeUpscalerInterop()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!globals::d3d::device || !globals::d3d::context)
		return false;

	try {
		if (!swapChain.d3d11Device)
			swapChain.SetD3D11Device(globals::d3d::device);
		if (!swapChain.d3d11Context)
			swapChain.SetD3D11DeviceContext(globals::d3d::context);
		if (!swapChain.d3d12Device) {
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			DX::ThrowIfFailed(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
			winrt::com_ptr<IDXGIAdapter> adapter;
			DX::ThrowIfFailed(dxgiDevice->GetAdapter(adapter.put()));
			swapChain.CreateD3D12Device(adapter.get());
			logger::info("[FidelityFX] Created a D3D12 device for FSR 4.1 without frame generation");
		}
		if (!runtimeFence.fence12 || !runtimeFence.fence11)
			runtimeFence.Create(swapChain.d3d12Device.get(), swapChain.d3d11Device.get());

		for (auto& commandContext : runtimeCommandContexts) {
			if (commandContext.commandAllocator && commandContext.commandList)
				continue;
			commandContext.commandList = nullptr;
			commandContext.commandAllocator = nullptr;
			commandContext.fenceValue = 0;
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandContext.commandAllocator)));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandContext.commandAllocator.get(), nullptr, IID_PPV_ARGS(&commandContext.commandList)));
			DX::ThrowIfFailed(commandContext.commandList->Close());
		}
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] FSR 4.1 interop setup failed: {}", e.what());
		return false;
	} catch (...) {
		logger::error("[FidelityFX] FSR 4.1 interop setup failed");
		return false;
	}

	return swapChain.d3d11Device && swapChain.d3d11Context && swapChain.d3d12Device && swapChain.commandQueue && runtimeFence.fence11 && runtimeFence.fence12;
}

void FidelityFX::DestroyRuntimeUpscaler()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	try {
		if (swapChain.d3d11Context && runtimeFence.fence11 && runtimeFence.fence12) {
			const uint64_t d3d11Value = runtimeFence.Next();
			if (SUCCEEDED(swapChain.d3d11Context->Signal(runtimeFence.fence11.get(), d3d11Value))) {
				swapChain.d3d11Context->Flush();
				(void)runtimeFence.CpuWait(d3d11Value, SharedFence::kFenceTimeoutMs);
			}
		} else if (globals::d3d::context) {
			globals::d3d::context->Flush();
		}
		if (swapChain.commandQueue && runtimeFence.fence12) {
			const uint64_t value = runtimeFence.Next();
			if (SUCCEEDED(swapChain.commandQueue->Signal(runtimeFence.fence12.get(), value)))
				(void)runtimeFence.CpuWait(value, SharedFence::kFenceTimeoutMs);
		}
	} catch (...) {
	}

	if (runtimeUpscalerContext && ffxModule.DestroyContext)
		(void)ffxModule.DestroyContext(&runtimeUpscalerContext, nullptr);
	runtimeUpscalerContext = nullptr;
	runtimeRenderWidth = 0;
	runtimeRenderHeight = 0;
	runtimeDisplayWidth = 0;
	runtimeDisplayHeight = 0;

	DeleteWrapped(runtimeColor);
	DeleteWrapped(runtimeDepth);
	DeleteWrapped(runtimeMotion);
	DeleteWrapped(runtimeReactive);
	DeleteWrapped(runtimeTransparency);
	DeleteWrapped(runtimeOutput);
	runtimeColorDesc = {};
	runtimeDepthDesc = {};
	runtimeMotionDesc = {};
	runtimeReactiveDesc = {};
	runtimeTransparencyDesc = {};
	runtimeOutputDesc = {};

	for (auto& commandContext : runtimeCommandContexts) {
		commandContext.commandList = nullptr;
		commandContext.commandAllocator = nullptr;
		commandContext.fenceValue = 0;
	}
	runtimeCommandContextCursor = 0;
}

bool FidelityFX::EnsureRuntimeFsr4Context(uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (!a_renderWidth || !a_renderHeight || !a_displayWidth || !a_displayHeight)
		return false;
	if (!EnsureRuntimeUpscalerInterop() || !ffxModule.CreateContext || !ffxModule.DestroyContext || !ffxModule.Query)
		return false;

	ProbeRuntimeFsr4();
	if (!driverReportsFsr4 || runtimeFsr4FailureLatched)
		return false;

	if (runtimeUpscalerContext &&
		runtimeRenderWidth == a_renderWidth &&
		runtimeRenderHeight == a_renderHeight &&
		runtimeDisplayWidth == a_displayWidth &&
		runtimeDisplayHeight == a_displayHeight) {
		return true;
	}

	if (runtimeUpscalerContext && ffxModule.DestroyContext)
		(void)ffxModule.DestroyContext(&runtimeUpscalerContext, nullptr);
	runtimeUpscalerContext = nullptr;

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = swapChain.d3d12Device.get();

	ffx::CreateContextDescUpscale createDesc{};
	createDesc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	if (globals::features::hdrDisplay.loaded)
		createDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
	createDesc.maxRenderSize = { a_renderWidth, a_renderHeight };
	createDesc.maxUpscaleSize = { a_displayWidth, a_displayHeight };
	createDesc.fpMessage = nullptr;

	const uint32_t requestedVersion = reportedFsr4Version != 0 ? reportedFsr4Version : FFX_UPSCALER_VERSION;
	ffx::CreateContextDescUpscaleVersion versionDesc{};
	versionDesc.version = requestedVersion;

	ffx::CreateContextDescOverrideVersion overrideDesc{};
	overrideDesc.versionId = reportedFsr4VersionId;

	const CreateAttempt attempts[] = {
		CreateAttempt::kOverride,
		CreateAttempt::kVersionDescriptor,
		CreateAttempt::kDefaultProvider
	};

	bool created = false;
	for (CreateAttempt attempt : attempts) {
		if (attempt == CreateAttempt::kOverride && reportedFsr4VersionId == 0)
			continue;
		const auto result = TryCreateContext(runtimeUpscalerContext, attempt, createDesc, backendDesc, versionDesc, overrideDesc);
		if (result != FFX_API_RETURN_OK)
			continue;

		ffxQueryGetProviderVersion providerQuery{};
		providerQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		providerQuery.versionId = 0;
		providerQuery.versionName = nullptr;
		const auto queryResult = ffxModule.Query(&runtimeUpscalerContext, &providerQuery.header);
		const std::string providerName = providerQuery.versionName ? providerQuery.versionName : "";
		const uint32_t providerVersion = static_cast<uint32_t>(providerQuery.versionId & 0xFFFFFFFFull);
		const bool isFsr4 = queryResult == FFX_API_RETURN_OK &&
		                    (UpscalerMajor(providerVersion) == 4 || NameLooksLikeFsr4(providerName) ||
								VersionIdMatches(providerQuery.versionId, requestedVersion) || NameMatchesVersion(providerName, requestedVersion));
		if (!isFsr4) {
			logger::warn("[FidelityFX] Runtime provider '{}' is not FSR 4.x", providerName.empty() ? "unknown" : providerName);
			if (ffxModule.DestroyContext)
				(void)ffxModule.DestroyContext(&runtimeUpscalerContext, nullptr);
			runtimeUpscalerContext = nullptr;
			continue;
		}

		if (UpscalerMajor(providerVersion) == 4)
			reportedFsr4Version = providerVersion;
		if (!providerName.empty())
			reportedFsr4VersionName = providerName;
		driverReportsFsr4 = true;
		created = true;
		logger::info("[FidelityFX] FSR 4.1 context created ({}, render {}x{}, display {}x{})",
			reportedFsr4VersionName.empty() ? UpscalerVersionToString(reportedFsr4Version) : reportedFsr4VersionName,
			a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight);
		break;
	}

	if (!created)
		return false;

	runtimeRenderWidth = a_renderWidth;
	runtimeRenderHeight = a_renderHeight;
	runtimeDisplayWidth = a_displayWidth;
	runtimeDisplayHeight = a_displayHeight;
	return true;
}

bool FidelityFX::DispatchRuntimeFsr4(
	ID3D11Resource* a_color,
	ID3D11Resource* a_depth,
	ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_reactiveMask,
	ID3D11Resource* a_transparencyCompositionMask,
	ID3D11Resource* a_output,
	uint32_t a_renderWidth,
	uint32_t a_renderHeight,
	uint32_t a_displayWidth,
	uint32_t a_displayHeight,
	float a_sharpness)
{
	if (!runtimeUpscalerContext || !a_color || !a_depth || !a_motionVectors || !a_reactiveMask || !a_transparencyCompositionMask || !a_output)
		return false;

	D3D11_TEXTURE2D_DESC colorDesc{};
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};
	D3D11_TEXTURE2D_DESC reactiveDesc{};
	D3D11_TEXTURE2D_DESC transparencyDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	if (!TryGetTexture2DDesc(a_color, colorDesc) ||
		!TryGetTexture2DDesc(a_depth, depthDesc) ||
		!TryGetTexture2DDesc(a_motionVectors, motionDesc) ||
		!TryGetTexture2DDesc(a_reactiveMask, reactiveDesc) ||
		!TryGetTexture2DDesc(a_transparencyCompositionMask, transparencyDesc) ||
		!TryGetTexture2DDesc(a_output, outputDesc)) {
		return false;
	}

	const D3D11_TEXTURE2D_DESC desiredColor = MakeSharedTextureDesc(colorDesc, colorDesc.Width, colorDesc.Height, 0);
	const D3D11_TEXTURE2D_DESC desiredMotion = MakeSharedTextureDesc(motionDesc, motionDesc.Width, motionDesc.Height, 0);
	const D3D11_TEXTURE2D_DESC desiredReactive = MakeSharedTextureDesc(reactiveDesc, reactiveDesc.Width, reactiveDesc.Height, 0);
	const D3D11_TEXTURE2D_DESC desiredTransparency = MakeSharedTextureDesc(transparencyDesc, transparencyDesc.Width, transparencyDesc.Height, 0);
	D3D11_TEXTURE2D_DESC desiredDepth = MakeSharedTextureDesc(depthDesc, depthDesc.Width, depthDesc.Height, D3D11_BIND_RENDER_TARGET);
	desiredDepth.Format = DXGI_FORMAT_R32_FLOAT;
	D3D11_TEXTURE2D_DESC desiredOutput = MakeSharedTextureDesc(outputDesc, outputDesc.Width, outputDesc.Height, D3D11_BIND_UNORDERED_ACCESS);

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	auto& upscaling = globals::features::upscaling;
	const bool needsResources =
		!runtimeColor || !runtimeDepth || !runtimeMotion || !runtimeReactive || !runtimeTransparency || !runtimeOutput ||
		!SameTextureDesc(runtimeColorDesc, desiredColor) ||
		!SameTextureDesc(runtimeDepthDesc, desiredDepth) ||
		!SameTextureDesc(runtimeMotionDesc, desiredMotion) ||
		!SameTextureDesc(runtimeReactiveDesc, desiredReactive) ||
		!SameTextureDesc(runtimeTransparencyDesc, desiredTransparency) ||
		!SameTextureDesc(runtimeOutputDesc, desiredOutput);

	if (needsResources) {
		uint64_t busyFence = 0;
		for (const auto& commandContext : runtimeCommandContexts)
			busyFence = std::max(busyFence, commandContext.fenceValue);
		if (busyFence != 0 && !runtimeFence.CpuWait(busyFence, SharedFence::kFenceTimeoutMs))
			return false;

		DeleteWrapped(runtimeColor);
		DeleteWrapped(runtimeDepth);
		DeleteWrapped(runtimeMotion);
		DeleteWrapped(runtimeReactive);
		DeleteWrapped(runtimeTransparency);
		DeleteWrapped(runtimeOutput);
		try {
			runtimeColor = new WrappedResource(desiredColor, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeDepth = new WrappedResource(desiredDepth, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeMotion = new WrappedResource(desiredMotion, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeReactive = new WrappedResource(desiredReactive, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeTransparency = new WrappedResource(desiredTransparency, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeOutput = new WrappedResource(desiredOutput, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
		} catch (const std::exception& e) {
			logger::error("[FidelityFX] FSR 4.1 shared textures failed: {}", e.what());
			DeleteWrapped(runtimeColor);
			DeleteWrapped(runtimeDepth);
			DeleteWrapped(runtimeMotion);
			DeleteWrapped(runtimeReactive);
			DeleteWrapped(runtimeTransparency);
			DeleteWrapped(runtimeOutput);
			return false;
		}
		runtimeColorDesc = desiredColor;
		runtimeDepthDesc = desiredDepth;
		runtimeMotionDesc = desiredMotion;
		runtimeReactiveDesc = desiredReactive;
		runtimeTransparencyDesc = desiredTransparency;
		runtimeOutputDesc = desiredOutput;
	}

	if (!runtimeDepth->rtv || !upscaling.copyDepthToSharedBufferPS || !upscaling.GetUpscaleVS())
		return false;

	const uint64_t completed = runtimeFence.fence12->GetCompletedValue();
	RuntimeCommandContext* commandContext = nullptr;
	for (uint32_t i = 0; i < kRuntimeCommandContextCount; ++i) {
		const uint32_t index = (runtimeCommandContextCursor + i) % kRuntimeCommandContextCount;
		auto& candidate = runtimeCommandContexts[index];
		if (candidate.fenceValue != 0 && completed < candidate.fenceValue)
			continue;
		candidate.fenceValue = 0;
		runtimeCommandContextCursor = (index + 1) % kRuntimeCommandContextCount;
		commandContext = &candidate;
		break;
	}
	if (!commandContext) {
		if (!runtimeFence.CpuWait(runtimeCommandContexts[0].fenceValue, SharedFence::kFenceTimeoutMs))
			return false;
		commandContext = &runtimeCommandContexts[0];
		commandContext->fenceValue = 0;
	}

	auto copyBox = [&](ID3D11Resource* a_source, WrappedResource* a_destination, uint32_t a_width, uint32_t a_height) {
		D3D11_BOX box{};
		box.right = a_width;
		box.bottom = a_height;
		box.back = 1;
		swapChain.d3d11Context->CopySubresourceRegion(a_destination->resource11, 0, 0, 0, 0, a_source, 0, &box);
	};

	copyBox(a_color, runtimeColor, std::min(colorDesc.Width, desiredColor.Width), std::min(colorDesc.Height, desiredColor.Height));
	copyBox(a_motionVectors, runtimeMotion, std::min(motionDesc.Width, desiredMotion.Width), std::min(motionDesc.Height, desiredMotion.Height));
	copyBox(a_reactiveMask, runtimeReactive, std::min(reactiveDesc.Width, desiredReactive.Width), std::min(reactiveDesc.Height, desiredReactive.Height));
	copyBox(a_transparencyCompositionMask, runtimeTransparency, std::min(transparencyDesc.Width, desiredTransparency.Width), std::min(transparencyDesc.Height, desiredTransparency.Height));

	auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ID3D11ShaderResourceView* depthView = depth.depthSRV;
	if (!depthView)
		return false;

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(desiredDepth.Width);
	viewport.Height = static_cast<float>(desiredDepth.Height);
	viewport.MaxDepth = 1.0f;
	swapChain.d3d11Context->RSSetViewports(1, &viewport);
	swapChain.d3d11Context->IASetInputLayout(nullptr);
	swapChain.d3d11Context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	swapChain.d3d11Context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	swapChain.d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	swapChain.d3d11Context->VSSetShader(upscaling.GetUpscaleVS(), nullptr, 0);
	if (upscaling.upscaleRasterizerState)
		swapChain.d3d11Context->RSSetState(upscaling.upscaleRasterizerState.get());
	if (upscaling.upscaleBlendState)
		swapChain.d3d11Context->OMSetBlendState(upscaling.upscaleBlendState.get(), nullptr, 0xffffffff);
	swapChain.d3d11Context->PSSetShaderResources(0, 1, &depthView);
	swapChain.d3d11Context->OMSetRenderTargets(1, &runtimeDepth->rtv, nullptr);
	swapChain.d3d11Context->PSSetShader(upscaling.copyDepthToSharedBufferPS.get(), nullptr, 0);
	swapChain.d3d11Context->Draw(3, 0);
	ID3D11ShaderResourceView* nullView = nullptr;
	swapChain.d3d11Context->PSSetShaderResources(0, 1, &nullView);
	swapChain.d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
	swapChain.d3d11Context->PSSetShader(nullptr, nullptr, 0);
	swapChain.d3d11Context->VSSetShader(nullptr, nullptr, 0);

	auto* commandAllocator = commandContext->commandAllocator.get();
	auto* commandList = commandContext->commandList.get();
	try {
		const uint64_t d3d11Submit = runtimeFence.Next();
		DX::ThrowIfFailed(swapChain.d3d11Context->Signal(runtimeFence.fence11.get(), d3d11Submit));
		DX::ThrowIfFailed(swapChain.commandQueue->Wait(runtimeFence.fence12.get(), d3d11Submit));

		DX::ThrowIfFailed(commandAllocator->Reset());
		DX::ThrowIfFailed(commandList->Reset(commandAllocator, nullptr));

		std::array<D3D12_RESOURCE_BARRIER, 6> beginBarriers = {
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeColor->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeDepth->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeMotion->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeReactive->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeTransparency->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeOutput->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		commandList->ResourceBarrier(static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

		ffx::DispatchDescUpscale dispatchParameters{};
		dispatchParameters.commandList = commandList;
		dispatchParameters.color = ffxApiGetResourceDX12(runtimeColor->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatchParameters.depth = ffxApiGetResourceDX12(runtimeDepth->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatchParameters.motionVectors = ffxApiGetResourceDX12(runtimeMotion->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatchParameters.exposure = FfxApiResource({});
		dispatchParameters.reactive = ffxApiGetResourceDX12(runtimeReactive->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatchParameters.transparencyAndComposition = ffxApiGetResourceDX12(runtimeTransparency->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		dispatchParameters.output = ffxApiGetResourceDX12(runtimeOutput->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
		dispatchParameters.jitterOffset = { -upscaling.jitter.x, -upscaling.jitter.y };
		dispatchParameters.motionVectorScale = { static_cast<float>(a_renderWidth), static_cast<float>(a_renderHeight) };
		dispatchParameters.renderSize = { a_renderWidth, a_renderHeight };
		dispatchParameters.upscaleSize = { a_displayWidth, a_displayHeight };
		dispatchParameters.enableSharpening = true;
		dispatchParameters.sharpness = a_sharpness;
		dispatchParameters.frameTimeDelta = *globals::game::deltaTime * 1000.f;
		dispatchParameters.preExposure = 1.0f;
		dispatchParameters.reset = false;
		dispatchParameters.cameraNear = *globals::game::cameraNear;
		dispatchParameters.cameraFar = *globals::game::cameraFar;
		dispatchParameters.cameraFovAngleVertical = Util::GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
		dispatchParameters.flags = 0;

		bool dispatchOk = false;
		DWORD faultCode = 0;
		const bool dispatchCompleted = Util::SehGuarded(
			[&] { dispatchOk = ffx::Dispatch(runtimeUpscalerContext, dispatchParameters) == ffx::ReturnCode::Ok; },
			&faultCode);
		if (!dispatchCompleted || !dispatchOk) {
			(void)Util::SehGuarded([&] { (void)commandList->Close(); });
			if (!dispatchCompleted) {
				runtimeFsr4Quarantined = true;
				logger::critical("[FidelityFX] FSR 4.1 dispatch faulted (exception 0x{:08X})", static_cast<uint32_t>(faultCode));
			}
			return false;
		}

		std::array<D3D12_RESOURCE_BARRIER, 6> endBarriers = {
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeColor->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeDepth->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeMotion->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeReactive->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeTransparency->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(runtimeOutput->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
		};
		commandList->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());
		DX::ThrowIfFailed(commandList->Close());

		ID3D12CommandList* lists[] = { commandList };
		const uint64_t d3d12Submit = runtimeFence.Next();
		swapChain.commandQueue->ExecuteCommandLists(1, lists);
		DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeFence.fence12.get(), d3d12Submit));
		commandContext->fenceValue = d3d12Submit;
		DX::ThrowIfFailed(swapChain.d3d11Context->Wait(runtimeFence.fence11.get(), d3d12Submit));

		D3D11_BOX outputBox{};
		outputBox.right = std::min(a_displayWidth, outputDesc.Width);
		outputBox.bottom = std::min(a_displayHeight, outputDesc.Height);
		outputBox.back = 1;
		if (!outputBox.right || !outputBox.bottom)
			return false;
		swapChain.d3d11Context->CopySubresourceRegion(a_output, 0, 0, 0, 0, runtimeOutput->resource11, 0, &outputBox);
		return true;
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] FSR 4.1 dispatch failed: {}", e.what());
		runtimeFsr4Quarantined = true;
		return false;
	} catch (...) {
		logger::error("[FidelityFX] FSR 4.1 dispatch failed");
		runtimeFsr4Quarantined = true;
		return false;
	}
}

bool FidelityFX::TryRuntimeFsr4Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors, float a_sharpness)
{
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.fsr4RuntimeEnableBoot || runtimeFsr4FailureLatched || runtimeFsr4Quarantined)
		return false;

	auto renderer = globals::game::renderer;
	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSize);
	const uint32_t displayWidth = static_cast<uint32_t>(screenSize.x);
	const uint32_t displayHeight = static_cast<uint32_t>(screenSize.y);
	const uint32_t renderWidth = static_cast<uint32_t>(renderSize.x);
	const uint32_t renderHeight = static_cast<uint32_t>(renderSize.y);

	if (!EnsureRuntimeFsr4Context(renderWidth, renderHeight, displayWidth, displayHeight)) {
		LatchRuntimeFsr4Failure("runtime context was not created");
		DestroyRuntimeUpscaler();
		return false;
	}

	if (!DispatchRuntimeFsr4(a_upscalingTexture, depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask, a_upscalingTexture, renderWidth, renderHeight, displayWidth, displayHeight, a_sharpness)) {
		LatchRuntimeFsr4Failure("runtime dispatch failed");
		DestroyRuntimeUpscaler();
		return false;
	}

	driverReportsFsr4 = true;
	return true;
}
