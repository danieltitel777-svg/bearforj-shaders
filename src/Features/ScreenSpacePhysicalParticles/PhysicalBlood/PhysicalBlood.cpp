#include "PhysicalBlood.h"

#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/UnifiedWater.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <exception>

// This translation unit owns feature lifecycle and the water-density simulation.
// Setup creates D3D state and a double-buffered dye field. EarlyPrepass schedules
// fixed-rate updates, while PostComposite performs resource mutation under a full
// state backup. SSPP terminal events append settled splats to Physical Blood's
// chronological ring or write water dye plus a one-word activity signal;
// asynchronous readback stops advection once the field becomes empty.
// Unified Water invokes BindWaterShaderResources after Skyrim's SetupGeometry,
// because that call overwrites the pixel-shader bindings consumed by Water.hlsl.

void PhysicalBlood::SetupResources()
{
	auto* device = globals::d3d::device;
	waterFieldCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<WaterFieldCB>(), "PhysicalBlood::WaterFieldCB");
	SetupSharedResources();

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].BlendEnable = true;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
	Util::SetResourceName(blendState.get(), "PhysicalBlood::BlendState");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = true;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
	Util::SetResourceName(depthState.get(), "PhysicalBlood::DepthState");

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = true;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, rasterizerState.put()));
	Util::SetResourceName(rasterizerState.get(), "PhysicalBlood::RasterizerState");

	D3D11_SAMPLER_DESC waterSamplerDesc{};
	waterSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	waterSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	waterSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	waterSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	waterSamplerDesc.MaxAnisotropy = 1;
	waterSamplerDesc.MinLOD = 0.0F;
	waterSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&waterSamplerDesc, waterLinearSampler.put()));
	Util::SetResourceName(waterLinearSampler.get(), "PhysicalBlood::WaterLinearSampler");
	// Mip 6 is the last level whose 5x5 cells occupy independent BC3 blocks.
	waterSamplerDesc.MaxLOD = 6.0F;
	DX::ThrowIfFailed(device->CreateSamplerState(&waterSamplerDesc, bloodAtlasSampler.put()));
	Util::SetResourceName(bloodAtlasSampler.get(), "PhysicalBlood::BloodAtlasSampler");

	// Summary: Loads and names a required blood texture and its SRV.
	// Args:
	//   a_path: Game-relative DDS path.
	//   a_texture: Destination texture owner.
	//   a_srv: Destination shader-resource view owner.
	//   a_resourceName: D3D debug name for the texture.
	// Returns: Nothing; throws when loading fails.
	const auto loadBloodTexture = [&](const std::filesystem::path& a_path,
									  winrt::com_ptr<ID3D11Texture2D>& a_texture,
									  winrt::com_ptr<ID3D11ShaderResourceView>& a_srv,
									  const char* a_resourceName) {
		const auto result = Util::LoadTextureFromFile(device, a_path, a_texture.put(), a_srv.put());
		if (FAILED(result)) {
			logger::critical(
				"Required blood texture could not be loaded: {} (HRESULT=0x{:08X})",
				a_path.string(),
				static_cast<std::uint32_t>(result));
			DX::ThrowIfFailed(result);
		}
		Util::SetResourceName(a_texture.get(), a_resourceName);
	};
	loadBloodTexture(
		L"Data\\Shaders\\PhysicalBlood\\blooddropatlas5x5.dds",
		bloodDiffuseTexture,
		bloodDiffuseSRV,
		"PhysicalBlood::BloodDropDiffuseAtlas");
	loadBloodTexture(
		L"Data\\Shaders\\PhysicalBlood\\blooddropatlas5x5_n.dds",
		bloodNormalTexture,
		bloodNormalSRV,
		"PhysicalBlood::BloodDropNormalAtlas");
	loadBloodTexture(
		L"Data\\Shaders\\PhysicalBlood\\blooddropatlas5x5_blend.dds",
		bloodBlendTexture,
		bloodBlendSRV,
		"PhysicalBlood::BloodDropBlendAtlas");
	loadBloodTexture(
		L"Data\\Shaders\\PhysicalBlood\\blooddropatlas5x5_blendaltalpha.dds",
		bloodBlendAltAlphaTexture,
		bloodBlendAltAlphaSRV,
		"PhysicalBlood::BloodDropBlendAltAlphaAtlas");

	D3D11_TEXTURE2D_DESC fieldDesc{};
	fieldDesc.Width = kWaterFieldSize;
	fieldDesc.Height = kWaterFieldSize;
	fieldDesc.MipLevels = 1;
	fieldDesc.ArraySize = 1;
	fieldDesc.Format = DXGI_FORMAT_R16_FLOAT;
	fieldDesc.SampleDesc.Count = 1;
	fieldDesc.Usage = D3D11_USAGE_DEFAULT;
	fieldDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	for (std::uint32_t i = 0; i < 2; ++i) {
		DX::ThrowIfFailed(device->CreateTexture2D(&fieldDesc, nullptr, waterDensityTexture[i].put()));
		DX::ThrowIfFailed(device->CreateShaderResourceView(waterDensityTexture[i].get(), nullptr, waterDensitySRV[i].put()));
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(waterDensityTexture[i].get(), nullptr, waterDensityUAV[i].put()));
		const float clearDensity[4]{};
		globals::d3d::context->ClearUnorderedAccessViewFloat(waterDensityUAV[i].get(), clearDensity);
	}
	fieldDesc.Format = DXGI_FORMAT_R32_UINT;
	DX::ThrowIfFailed(device->CreateTexture2D(&fieldDesc, nullptr, waterSourceTexture.put()));
	DX::ThrowIfFailed(device->CreateShaderResourceView(waterSourceTexture.get(), nullptr, waterSourceSRV.put()));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(waterSourceTexture.get(), nullptr, waterSourceUAV.put()));
	const std::uint32_t clearSource[4]{};
	globals::d3d::context->ClearUnorderedAccessViewUint(waterSourceUAV.get(), clearSource);

	D3D11_TEXTURE1D_DESC activityDesc{};
	activityDesc.Width = 1;
	activityDesc.MipLevels = 1;
	activityDesc.ArraySize = 1;
	activityDesc.Format = DXGI_FORMAT_R32_UINT;
	activityDesc.Usage = D3D11_USAGE_DEFAULT;
	activityDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	DX::ThrowIfFailed(device->CreateTexture1D(&activityDesc, nullptr, waterActivityTexture.put()));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(waterActivityTexture.get(), nullptr, waterActivityUAV.put()));
	Util::SetResourceName(waterActivityTexture.get(), "PhysicalBlood::WaterActivity");
	activityDesc.Usage = D3D11_USAGE_STAGING;
	activityDesc.BindFlags = 0;
	activityDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (std::size_t index = 0; index < waterActivityReadbacks.size(); ++index) {
		auto& readback = waterActivityReadbacks[index];
		readback = {};
		DX::ThrowIfFailed(device->CreateTexture1D(&activityDesc, nullptr, readback.texture.put()));
		Util::SetResourceName(
			readback.texture.get(), "PhysicalBlood::WaterActivityReadback%zu", index);
	}
	globals::d3d::context->ClearUnorderedAccessViewUint(waterActivityUAV.get(), clearSource);
	currentWaterDensity = 0;
	nextWaterActivityReadback = 0;
	nextWaterActivityReadbackToRead = 0;
	waterUpdateAccumulator = 0.0F;
	waterFieldCenterInitialized = false;
	waterDensityActive = false;
	WaterFieldCB inactiveWaterField{};
	inactiveWaterField.extent = kWaterFieldExtent;
	waterFieldCB->Update(inactiveWaterField);

	EnsureShaders();
	RegisterSharedRuntime();
}

// Summary: Advects, diffuses, dissipates, and recenters the water-density field.
// Args:
//   a_deltaTime: Simulation interval in seconds.
// Returns: Nothing.
void PhysicalBlood::UpdateWaterField(float a_deltaTime)
{
	if (!waterAdvectCS || !waterDensitySRV[currentWaterDensity] || !waterSourceSRV ||
		!waterActivityUAV)
		return;
	const auto activityEpoch = waterActivityEpoch.load(std::memory_order_acquire);

	WaterFieldCB constants{};
	constants.center = waterFieldCenter;
	constants.previousCenter = previousWaterFieldCenter;
	constants.extent = kWaterFieldExtent;
	const auto deltaTime = std::clamp(a_deltaTime, 0.0F, 1.0F / 20.0F);
	constants.flowStep = settings.waterFlowSpeed * deltaTime;
	constants.diffusionBlend = std::clamp(settings.waterDiffusion * deltaTime * 10.0F, 0.0F, 1.0F);
	constants.decayMultiplier = std::exp(-settings.waterDissipation * deltaTime);
	constants.densityActive = 1U;
	constants.inverseDimensions = {
		1.0F / static_cast<float>(kWaterFieldSize),
		1.0F / static_cast<float>(kWaterFieldSize)
	};
	globals::features::unifiedWater.GetFlowmapMapping(constants.flowOffsetX, constants.flowOffsetY, constants.flowWidth, constants.flowHeight);
	waterFieldCB->Update(constants);

	auto* context = globals::d3d::context;
	const auto next = 1U - currentWaterDensity;
	ID3D11ShaderResourceView* srvs[] = {
		waterDensitySRV[currentWaterDensity].get(),
		globals::features::unifiedWater.GetFlowmapSRV(),
		waterSourceSRV.get()
	};
	ID3D11UnorderedAccessView* outputs[]{
		waterDensityUAV[next].get(), waterActivityUAV.get()
	};
	ID3D11Buffer* cb = waterFieldCB->CB();
	ID3D11SamplerState* linearSampler = waterLinearSampler.get();
	context->CSSetShaderResources(0, 3, srvs);
	const std::uint32_t clearActivity[4]{};
	context->ClearUnorderedAccessViewUint(waterActivityUAV.get(), clearActivity);
	context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(0, 1, &linearSampler);
	context->CSSetShader(waterAdvectCS.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalBlood::WaterAdvection");
	// TODO(PhysicalBlood): Track active 16x16 tiles and dispatch only those tiles
	// plus the halo needed by transport, diffusion, and field recentering.
	context->Dispatch(kWaterFieldSize / 8, kWaterFieldSize / 8, 1);
	globals::profiler->EndPass();
	ID3D11ShaderResourceView* nullSRVs[3]{};
	ID3D11UnorderedAccessView* nullUAVs[2]{};
	ID3D11SamplerState* nullSampler{};
	context->CSSetShaderResources(0, 3, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetShader(nullptr, nullptr, 0);
	const std::uint32_t clear[4]{};
	context->ClearUnorderedAccessViewUint(waterSourceUAV.get(), clear);
	currentWaterDensity = next;
	SetWaterDensityActive(true);
	QueueWaterActivityReadback(activityEpoch);
}

// Summary: Polls the oldest asynchronous activity readback without blocking.
// Args: None.
// Returns: Nothing.
void PhysicalBlood::PollWaterActivityReadbacks()
{
	auto& readback = waterActivityReadbacks[nextWaterActivityReadbackToRead];
	if (!readback.pending || !readback.texture)
		return;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	const auto result = globals::d3d::context->Map(
		readback.texture.get(),
		0,
		D3D11_MAP_READ,
		D3D11_MAP_FLAG_DO_NOT_WAIT,
		&mapped);
	if (result == DXGI_ERROR_WAS_STILL_DRAWING)
		return;
	readback.pending = false;
	nextWaterActivityReadbackToRead = static_cast<std::uint32_t>(
		(nextWaterActivityReadbackToRead + 1) % waterActivityReadbacks.size());
	if (FAILED(result))
		return;
	const auto active = *static_cast<const std::uint32_t*>(mapped.pData) != 0;
	globals::d3d::context->Unmap(readback.texture.get(), 0);
	if (active) {
		SetWaterDensityActive(true);
	} else if (readback.epoch == waterActivityEpoch.load(std::memory_order_acquire)) {
		SetWaterDensityActive(false);
	}
}

// Summary: Copies the GPU activity flag into the next available staging slot.
// Args:
//   a_epoch: Burst epoch associated with the submitted activity test.
// Returns: Nothing; skips submission when the ring slot is still pending.
void PhysicalBlood::QueueWaterActivityReadback(std::uint64_t a_epoch)
{
	auto& readback = waterActivityReadbacks[nextWaterActivityReadback];
	if (readback.pending || !readback.texture)
		return;
	globals::d3d::context->CopyResource(readback.texture.get(), waterActivityTexture.get());
	readback.epoch = a_epoch;
	readback.pending = true;
	nextWaterActivityReadback = static_cast<std::uint32_t>(
		(nextWaterActivityReadback + 1) % waterActivityReadbacks.size());
}

// Summary: Changes whether water density participates in simulation and rendering.
// Args:
//   a_active: True while any water-density sample remains nonzero.
// Returns: Nothing.
void PhysicalBlood::SetWaterDensityActive(bool a_active)
{
	if (waterDensityActive == a_active)
		return;
	waterDensityActive = a_active;
	if (!a_active && waterFieldCB) {
		WaterFieldCB inactiveWaterField{};
		inactiveWaterField.extent = kWaterFieldExtent;
		waterFieldCB->Update(inactiveWaterField);
	}
}

void PhysicalBlood::BindWaterShaderResources()
{
	if (!waterFieldCB || !waterLinearSampler)
		return;
	ID3D11ShaderResourceView* density = settings.enabled && waterDensityActive ?
	                                     waterDensitySRV[currentWaterDensity].get() : nullptr;
	ID3D11Buffer* cb = waterFieldCB->CB();
	ID3D11SamplerState* linearSampler = waterLinearSampler.get();
	globals::d3d::context->PSSetShaderResources(66, 1, &density);
	globals::d3d::context->PSSetConstantBuffers(10, 1, &cb);
	globals::d3d::context->PSSetSamplers(12, 1, &linearSampler);
}

void PhysicalBlood::GameLoaded()
{
	// PostComposite performs the requested render-thread resource reset.
	runtimeResetPending.store(true, std::memory_order_release);
}

void PhysicalBlood::EarlyPrepass()
{
	if (!settings.enabled)
		return;

	InitializeWaterFieldCenter();
	EnsureShaders();
	RefreshSharedRuntimeConfiguration();
	if (sharedWaterUpdatePending)
		return;

	const auto deltaTime = globals::game::ui->GameIsPaused() ? 0.0F : *globals::game::deltaTime;
	const auto activityEpoch = waterActivityEpoch.load(std::memory_order_acquire);
	const bool newWaterProducer = activityEpoch != observedWaterActivityEpoch;
	observedWaterActivityEpoch = activityEpoch;
	PollWaterActivityReadbacks();
	if (!waterDensityActive) {
		waterUpdateAccumulator = 0.0F;
		return;
	}

	const auto updateInterval = 1.0F / std::max(settings.waterUpdateRate, 1.0F);
	waterUpdateAccumulator = std::min(waterUpdateAccumulator + deltaTime, 0.25F);
	if (newWaterProducer)
		waterUpdateAccumulator = std::max(waterUpdateAccumulator, updateInterval);
	if (waterUpdateAccumulator < updateInterval)
		return;

	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	constexpr float texelSize = kWaterFieldExtent / static_cast<float>(kWaterFieldSize);
	previousWaterFieldCenter = waterFieldCenter;
	waterFieldCenter = {
		std::round(camera.x / texelSize) * texelSize,
		std::round(camera.y / texelSize) * texelSize
	};
	sharedWaterUpdateDelta = std::min(waterUpdateAccumulator, 1.0F / 20.0F);
	waterUpdateAccumulator = std::fmod(waterUpdateAccumulator, updateInterval);
	sharedWaterUpdatePending = true;
}

// Summary: Drains airborne particles and clears owned terminal render state.
// Args: None.
// Returns: Nothing.
void PhysicalBlood::ResetRuntimeState()
{
	sharedEmissionReady.store(false, std::memory_order_release);
	if (sharedRegistrationReady) {
		(void)ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton().RequestFeatureDrain(
			sharedOwnerFeature);
	}

	const std::uint32_t clearUint[4]{};
	const float clearFloat[4]{};
	auto* context = globals::d3d::context;
	if (context) {
		for (const auto& density : waterDensityUAV) {
			if (density)
				context->ClearUnorderedAccessViewFloat(density.get(), clearFloat);
		}
		if (waterSourceUAV)
			context->ClearUnorderedAccessViewUint(waterSourceUAV.get(), clearUint);
		if (waterActivityUAV)
			context->ClearUnorderedAccessViewUint(waterActivityUAV.get(), clearUint);
		if (ownedSplatState)
			context->ClearUnorderedAccessViewUint(ownedSplatState->uav.get(), clearUint);
		if (ownedSplatDrawArguments) {
			const D3D11_DRAW_INSTANCED_INDIRECT_ARGS emptyDraws[2]{
				{ 4U, 0U, 0U, 0U }, { 4U, 0U, 0U, 0U }
			};
			context->UpdateSubresource(
				ownedSplatDrawArguments.get(), 0, nullptr, emptyDraws, 0, 0);
		}
	}

	currentWaterDensity = 0;
	waterFieldCenter = {};
	previousWaterFieldCenter = {};
	waterUpdateAccumulator = 0.0F;
	sharedWaterUpdateDelta = 0.0F;
	waterFieldCenterInitialized = false;
	sharedWaterUpdatePending = false;
	for (auto& readback : waterActivityReadbacks)
		readback.pending = false;
	nextWaterActivityReadback = 0;
	nextWaterActivityReadbackToRead = 0;
	SetWaterDensityActive(false);
}

void PhysicalBlood::PostComposite()
{
	const auto requestedSplatCapacity = static_cast<std::uint32_t>(std::clamp(
		settings.maximumSplats,
		kMinimumOwnedSplatCapacity,
		kMaximumOwnedSplatCapacity));
	if (requestedSplatCapacity != allocatedOwnedSplatCapacity) {
		try {
			RecreateOwnedSplatResources(requestedSplatCapacity);
			logger::info(
				"[Physical Blood] Resized splat storage to {} records ({:.1f} MiB)",
				allocatedOwnedSplatCapacity,
				static_cast<double>(allocatedOwnedSplatCapacity) * sizeof(OwnedSplatGPU) /
					(1024.0 * 1024.0));
		} catch (const std::exception& e) {
			logger::error(
				"[Physical Blood] Could not resize splat storage to {} records: {}",
				requestedSplatCapacity,
				e.what());
			settings.maximumSplats = static_cast<std::int32_t>(allocatedOwnedSplatCapacity);
			maximumSplatsDraft = settings.maximumSplats;
		}
	}
	if (!settings.enabled && runtimeEnabledLastFrame)
		runtimeResetPending.store(true, std::memory_order_release);
	if (runtimeResetPending.exchange(false, std::memory_order_acq_rel))
		ResetRuntimeState();
	if (!settings.enabled) {
		sharedEmissionReady.store(false, std::memory_order_release);
		if (sharedConfiguredEnabled) {
			auto& runtime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
			if (sharedSplatRenderClass != ScreenSpacePhysicalParticles::Core::kInvalidRoute)
				(void)runtime.SetRendererEnabled(sharedSplatRenderClass, false);
			if (sharedAirborneRenderClass != ScreenSpacePhysicalParticles::Core::kInvalidRoute)
				(void)runtime.SetRendererEnabled(sharedAirborneRenderClass, false);
			sharedConfiguredEnabled = false;
		}
		runtimeEnabledLastFrame = false;
		return;
	}
	runtimeEnabledLastFrame = true;
	InitializeWaterFieldCenter();
	EnsureShaders();
	const bool runtimeReady = sharedRegistrationReady && SharedRendererReady(false) &&
	                          SharedRendererReady(true) && SharedTerminalEventConsumerReady() &&
	                          waterAdvectCS && waterFieldCB && waterSourceUAV && waterActivityUAV;
	sharedEmissionReady.store(runtimeReady, std::memory_order_release);
	if (!runtimeReady) {
		if (!shaderFailureLogged) {
			logger::critical(
				"[Physical Blood] Shared runtime unavailable: registration={} waterAdvectCS={} terminalEventCS={} waterFieldCB={} waterSource={} waterActivity={}",
				sharedRegistrationReady,
				static_cast<bool>(waterAdvectCS),
				static_cast<bool>(sharedTerminalEventCS),
				waterFieldCB != nullptr,
				static_cast<bool>(waterSourceUAV),
				static_cast<bool>(waterActivityUAV));
			shaderFailureLogged = true;
		}
		return;
	}

	if (!sharedWaterUpdatePending)
		return;

	Effects11Util::D3D11FullStateBackup backup;
	backup.Save(globals::d3d::context);
	UpdateWaterField(sharedWaterUpdateDelta);
	sharedWaterUpdateDelta = 0.0F;
	sharedWaterUpdatePending = false;
	backup.Restore(globals::d3d::context);
	backup.Release();
}

// Summary: Lazily compiles every blood shader required by the current feature set.
// Args: None.
// Returns: Nothing; shader compilation failures propagate.
void PhysicalBlood::EnsureShaders()
{
	if (ownedSplatDrawArgsCS && sharedTerminalEventCS && sharedAirborneVS &&
		sharedAirbornePS && sharedSplatVS && sharedSplatPS && waterAdvectCS)
		return;
	EnsureSharedShaders();
	if (!waterAdvectCS)
		waterAdvectCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalBlood\\WaterBloodCS.hlsl", {}, "cs_5_0", "AdvectMain")));
}

void PhysicalBlood::ClearShaderCache()
{
	sharedEmissionReady.store(false, std::memory_order_release);
	waterAdvectCS = nullptr;
	ownedSplatDrawArgsCS = nullptr;
	sharedTerminalEventCS = nullptr;
	sharedAirborneVS = nullptr;
	sharedAirbornePS = nullptr;
	sharedSplatVS = nullptr;
	sharedSplatPS = nullptr;
	shaderFailureLogged = false;
}
