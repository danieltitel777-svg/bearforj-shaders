#include "ScreenSpaceCollisionDepth.h"

#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "Hooks.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/GeometryCapture.h"
#include "Utils/VersionedRelocation.h"

#include <array>

// Implements pass replay into a private depth target. The depth hook captures the
// normal prepass; the immediate-render hook adds eligible forward-only statics while
// restoring SSPP bindings after Skyrim updates dirty render state.
namespace
{
	constexpr std::uint32_t kDebugWidth = 512;
	constexpr std::uint32_t kDebugHeight = 288;
	constexpr std::uint32_t kDebugThreads = 8;

	struct ReplayStateBackup
	{
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets;
		winrt::com_ptr<ID3D11DepthStencilView> depthTarget;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		std::uint32_t stencilReference{};

		// Summary: Saves output-merger and vertex-shader state mutated by pass replay.
		// Args:
		//   a_context: Immediate device context to query.
		// Returns: None.
		void Save(ID3D11DeviceContext* a_context)
		{
			ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* depth{};
			a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
			for (std::size_t index = 0; index < renderTargets.size(); ++index)
				renderTargets[index].attach(targets[index]);
			depthTarget.attach(depth);
			a_context->OMGetDepthStencilState(depthState.put(), &stencilReference);
			a_context->VSGetShader(vertexShader.put(), nullptr, nullptr);
		}

		// Summary: Restores output-merger and vertex-shader state saved before replay.
		// Args:
		//   a_context: Immediate device context to update.
		// Returns: None.
		void Restore(ID3D11DeviceContext* a_context) const
		{
			ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			for (std::size_t index = 0; index < renderTargets.size(); ++index)
				targets[index] = renderTargets[index].get();
			a_context->OMSetRenderTargets(
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depthTarget.get());
			a_context->OMSetDepthStencilState(depthState.get(), stencilReference);
			a_context->VSSetShader(vertexShader.get(), nullptr, 0);
		}
	};
}

ScreenSpaceCollisionDepth& ScreenSpaceCollisionDepth::GetSingleton() noexcept
{
	static ScreenSpaceCollisionDepth singleton;
	return singleton;
}

void ScreenSpaceCollisionDepth::InstallHooks()
{
	stl::write_thunk_call<MainRenderDepthHook>(
		REL::RelocationID(35560, 36559).address() +
		Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3));
	stl::write_thunk_call<RenderPassImmediatelyHook>(
		REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
}

// Summary: Wraps Skyrim's main depth render with SSPP capture phase transitions.
// Args:
//   a_first: First argument forwarded to Skyrim's renderer.
//   a_second: Second argument forwarded to Skyrim's renderer.
// Returns: None.
void ScreenSpaceCollisionDepth::MainRenderDepthHook::thunk(bool a_first, bool a_second)
{
	auto& capture = GetSingleton();
	capture.BeginCapture();
	func(a_first, a_second);
	capture.FinishDepthPrepass();
}

// Summary: Observes completed immediate passes and replays eligible geometry.
// Args:
//   a_pass: Render pass submitted by Skyrim.
//   a_technique: Technique selected for the pass.
//   a_alphaTest: Whether the pass uses alpha testing.
//   a_renderFlags: Immediate-render flags.
// Returns: None.
void ScreenSpaceCollisionDepth::RenderPassImmediatelyHook::thunk(
	RE::BSRenderPass* a_pass,
	std::uint32_t a_technique,
	bool a_alphaTest,
	std::uint32_t a_renderFlags)
{
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
	GetSingleton().CapturePassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void ScreenSpaceCollisionDepth::SetupResources()
{
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	if (!device || !context || !globals::game::renderer) {
		logger::error("[SSPP] Actor-free depth resource setup skipped because the renderer is unavailable");
		return;
	}

	auto& mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!mainDepth.texture || !mainDepth.depthSRV || !mainDepth.views[0]) {
		logger::error("[SSPP] Actor-free depth resource setup skipped because Skyrim's main depth target is incomplete");
		return;
	}
	if (sourceDepthTexture.get() == mainDepth.texture && depthTexture && depthDSV && depthSRV &&
		captureDepthState) {
		return;
	}

	sourceDepthTexture.copy_from(mainDepth.texture);
	depthSRV = nullptr;
	depthDSV = nullptr;
	depthTexture = nullptr;
	captureDepthState = nullptr;
	debugSRV = nullptr;
	debugUAV = nullptr;
	debugTexture = nullptr;

	D3D11_TEXTURE2D_DESC captureDesc{};
	mainDepth.texture->GetDesc(&captureDesc);
	DX::ThrowIfFailed(device->CreateTexture2D(&captureDesc, nullptr, depthTexture.put()));
	Util::SetResourceName(depthTexture.get(), "SSPP::ActorFreeDepth");

	D3D11_SHADER_RESOURCE_VIEW_DESC captureSRVDesc{};
	mainDepth.depthSRV->GetDesc(&captureSRVDesc);
	DX::ThrowIfFailed(device->CreateShaderResourceView(depthTexture.get(), &captureSRVDesc, depthSRV.put()));

	D3D11_DEPTH_STENCIL_VIEW_DESC captureDSVDesc{};
	mainDepth.views[0]->GetDesc(&captureDSVDesc);
	DX::ThrowIfFailed(device->CreateDepthStencilView(depthTexture.get(), &captureDSVDesc, depthDSV.put()));

	D3D11_DEPTH_STENCIL_DESC captureDepthDesc{};
	captureDepthDesc.DepthEnable = true;
	captureDepthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	captureDepthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&captureDepthDesc, captureDepthState.put()));
	Util::SetResourceName(captureDepthState.get(), "SSPP::ActorFreeCaptureDepthState");

	if (capturedGeometry.bucket_count() == 0)
		capturedGeometry.reserve(4096);
	Reset();
	logger::info(
		"[SSPP] Actor-free depth resources created: {}x{} format={} samples={}",
		captureDesc.Width,
		captureDesc.Height,
		magic_enum::enum_name(captureDesc.Format),
		captureDesc.SampleDesc.Count);
}

void ScreenSpaceCollisionDepth::RequestCapture() noexcept
{
	captureRequested.store(true, std::memory_order_release);
}

void ScreenSpaceCollisionDepth::BeginCapture()
{
	ApplyPendingReset();
	terrainPasses = 0;
	staticPasses = 0;
	capturedGeometry.clear();
	const bool captureWasRequested = captureRequested.exchange(false, std::memory_order_acq_rel);
	captureActive = captureWasRequested && depthDSV;
	if (captureWasRequested) {
		// Never reuse camera-relative depth after a failed current-frame capture.
		ready = false;
		lastCompletedFrame = static_cast<std::uint32_t>(-1);
	}
	capturePhase = captureActive ? CapturePhase::kDepthPrepass : CapturePhase::kIdle;
	if (captureActive)
		globals::d3d::context->ClearDepthStencilView(depthDSV.get(), D3D11_CLEAR_DEPTH, 1.0F, 0U);
}

void ScreenSpaceCollisionDepth::FinishDepthPrepass() noexcept
{
	if (captureActive && capturePhase == CapturePhase::kDepthPrepass)
		capturePhase = CapturePhase::kForwardStatic;
}

// Summary: Applies shared solid-geometry policy and excludes grass-specific shader types.
// Args:
//   a_pass: Render pass to classify.
//   a_ownership: Ownership attributes for the pass geometry.
// Returns: True when the pass can contribute actor-free depth.
bool ScreenSpaceCollisionDepth::IsStaticDepthOccluder(
	const RE::BSRenderPass& a_pass,
	const ScreenSpacePhysicalParticles::Utils::GeometryOwnership& a_ownership)
{
	if (!a_pass.geometry || !a_pass.shaderProperty || !a_pass.shader)
		return false;
	if (a_pass.shader->shaderType.get() == RE::BSShader::Type::Grass ||
		a_pass.shader->shaderType.get() == RE::BSShader::Type::DistantTree) {
		return false;
	}
	return ScreenSpacePhysicalParticles::Utils::IsSolidSceneGeometry(
		a_pass.geometry, a_pass.shaderProperty, a_ownership);
}

void ScreenSpaceCollisionDepth::CapturePassImmediately(
	RE::BSRenderPass* a_pass,
	std::uint32_t a_technique,
	bool a_alphaTest,
	std::uint32_t a_renderFlags)
{
	if (!captureActive || !a_pass || !a_pass->geometry || !a_pass->shaderProperty || !a_pass->shader)
		return;
	if (a_pass->shader->shaderType.get() == RE::BSShader::Type::Water)
		return;
	if (capturedGeometry.contains(a_pass->geometry))
		return;

	const bool terrain = a_pass->shaderProperty->flags.all(
		RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
	const auto ownership = ScreenSpacePhysicalParticles::Utils::ClassifyGeometryOwnership(a_pass->geometry);
	if (ownership.dynamicReference || ownership.invalidReference)
		return;
	const bool staticReference = ownership.explicitStatic;
	if (capturePhase == CapturePhase::kDepthPrepass) {
		// Preserve authored STATIC/SCOL cutouts; reject anonymous foliage cards.
		if (!terrain && (!IsStaticDepthOccluder(*a_pass, ownership) ||
			(a_alphaTest && !staticReference)))
			return;
	} else if (capturePhase == CapturePhase::kForwardStatic) {
		if (!globals::state || !globals::state->inWorld ||
			(globals::state->permutationData.ExtraShaderDescriptor &
				static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsReflections)) != 0 ||
			!staticReference ||
			a_pass->shader->shaderType.get() != RE::BSShader::Type::Lighting ||
			!IsStaticDepthOccluder(*a_pass, ownership)) {
			return;
		}
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto& flags = a_pass->shaderProperty->flags;
		if (flags.none(Flag::kZBufferWrite) || flags.any(
				Flag::kDecal,
				Flag::kDynamicDecal,
				Flag::kTempRefraction,
				Flag::kRefraction,
				Flag::kMenuScreen)) {
			return;
		}
	} else {
		return;
	}

	ReplayStateBackup backup;
	backup.Save(globals::d3d::context);
	auto& shadowData = globals::game::shadowState->GetRuntimeData();
	const auto savedDepthMode = shadowData.depthStencilDepthMode;
	auto& terrainBlending = globals::features::terrainBlending;
	const bool savedRenderAltTerrain = terrainBlending.renderAltTerrain;
	shadowData.depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestWrite;
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	replayActive = true;
	Hooks::SetDirtyStatesObserver(
		[] { ScreenSpaceCollisionDepth::GetSingleton().RestoreReplayState(); });
	RestoreReplayState();
	RenderPassImmediatelyHook::func(
		a_pass,
		a_technique,
		a_alphaTest,
		a_renderFlags);
	Hooks::SetDirtyStatesObserver(nullptr);
	replayActive = false;
	terrainBlending.renderAltTerrain = savedRenderAltTerrain;
	shadowData.depthStencilDepthMode = savedDepthMode;
	backup.Restore(globals::d3d::context);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	capturedGeometry.insert(a_pass->geometry);
	terrain ? ++terrainPasses : ++staticPasses;
}

void ScreenSpaceCollisionDepth::EndCapture()
{
	if (!captureActive)
		return;
	captureActive = false;
	replayActive = false;
	capturePhase = CapturePhase::kIdle;
	ready = terrainPasses + staticPasses > 0;
	lastCompletedFrame = ready && globals::state ?
	                         globals::state->frameCount :
	                         static_cast<std::uint32_t>(-1);
}

void ScreenSpaceCollisionDepth::RestoreReplayState()
{
	if (!replayActive || !depthDSV)
		return;
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, depthDSV.get());
	context->OMSetDepthStencilState(captureDepthState.get(), 0U);
	auto* vertexShader = *globals::game::currentVertexShader;
	context->VSSetShader(
		vertexShader ? reinterpret_cast<ID3D11VertexShader*>(vertexShader->shader) : nullptr,
		nullptr,
		0);
}

// Summary: Lazily creates the depth-preview texture and conversion shader.
// Args: None.
// Returns: None.
void ScreenSpaceCollisionDepth::EnsureDebugResources()
{
	if (!debugTexture) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kDebugWidth;
		desc.Height = kDebugHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		auto* device = globals::d3d::device;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, debugTexture.put()));
		DX::ThrowIfFailed(device->CreateShaderResourceView(debugTexture.get(), nullptr, debugSRV.put()));
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(debugTexture.get(), nullptr, debugUAV.put()));
		Util::SetResourceName(debugTexture.get(), "SSPP::ActorFreeDepthDebug");
	}
	if (!debugCS) {
		debugCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\ScreenSpacePhysicalParticles\\ActorFreeDepthDebugCS.hlsl",
			{},
			"cs_5_0",
			"Main")));
	}
}

void ScreenSpaceCollisionDepth::UpdateDebug(bool a_enabled)
{
	if (!a_enabled || !ready || !depthSRV)
		return;
	EnsureDebugResources();
	if (!debugCS || !debugUAV)
		return;

	auto* context = globals::d3d::context;
	ID3D11ShaderResourceView* source = depthSRV.get();
	ID3D11UnorderedAccessView* output = debugUAV.get();
	ID3D11Buffer* perFrame = *globals::game::perFrame;
	ID3D11Buffer* sharedData = globals::state->sharedDataCB ? globals::state->sharedDataCB->CB() : nullptr;
	context->CSSetShaderResources(0, 1, &source);
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetConstantBuffers(5, 1, &sharedData);
	context->CSSetConstantBuffers(12, 1, &perFrame);
	context->CSSetShader(debugCS.get(), nullptr, 0);
	globals::profiler->BeginPass("SSPP::SceneDepthDebug");
	context->Dispatch(kDebugWidth / kDebugThreads, kDebugHeight / kDebugThreads, 1);
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSRV{};
	ID3D11UnorderedAccessView* nullUAV{};
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
}

void ScreenSpaceCollisionDepth::ClearShaderCache()
{
	debugCS = nullptr;
	logger::info("[SSPP] Released actor-free depth debug shader for cache refresh");
}

void ScreenSpaceCollisionDepth::Reset() noexcept
{
	captureRequested.store(false, std::memory_order_release);
	resetPending.store(true, std::memory_order_release);
}

// Summary: Clears render-owned state after a deferred reset request.
// Args: None.
// Returns: None.
void ScreenSpaceCollisionDepth::ApplyPendingReset() noexcept
{
	if (!resetPending.exchange(false, std::memory_order_acq_rel))
		return;
	captureActive = false;
	replayActive = false;
	capturePhase = CapturePhase::kIdle;
	ready = false;
	lastCompletedFrame = static_cast<std::uint32_t>(-1);
	capturedGeometry.clear();
	terrainPasses = 0;
	staticPasses = 0;
}
