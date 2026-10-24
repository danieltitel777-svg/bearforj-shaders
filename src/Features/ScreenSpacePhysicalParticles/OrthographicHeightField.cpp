#include "OrthographicHeightField.h"

#include "ScreenSpacePhysicalParticles.h"
#include "Utils/GeometryCapture.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include "RE/M/Main.h"
#include "RE/S/SceneGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <limits>
#include <unordered_set>
#include <vector>

// Implements two independent top-down captures. Solid geometry is rebuilt at a
// configurable frame cadence around the player. Water is rebuilt when its
// cell context or asynchronously sampled TESWaterSystem data changes.
namespace
{
	constexpr std::uint32_t kDebugThreads = 8;
	constexpr std::size_t kMaximumCapturedGeometry = 65536;
	constexpr std::size_t kMaximumSnapshottedWaterBounds = 8192;
	constexpr std::size_t kMaximumWaterSurfaces = 512;
	constexpr float kMaximumValidWaterHeight = 1000000.0F;

	// Summary: Tests geometry against the format, material, and ownership requirements of the
	// replacement height-capture shader.
	// Args:
	//   a_geometry: Candidate scene geometry.
	// Returns: True when the geometry can be drawn as a solid height source.
	bool IsEligibleStaticGeometry(RE::BSGeometry* a_geometry) noexcept
	{
		if (!a_geometry || !a_geometry->AsTriShape())
			return false;
		const auto& data = a_geometry->GetGeometryRuntimeData();
		if (!data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer ||
			data.skinInstance || !data.shaderProperty ||
			!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
			return false;
		}

		const auto ownership =
			ScreenSpacePhysicalParticles::Utils::ClassifyGeometryOwnership(a_geometry);
		if (!ScreenSpacePhysicalParticles::Utils::IsSolidSceneGeometry(
				a_geometry, data.shaderProperty.get(), ownership))
			return false;
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto& flags = data.shaderProperty->flags;
		if (flags.none(Flag::kZBufferWrite))
			return false;
		if (flags.any(Flag::kTempRefraction))
			return false;
		if (flags.any(Flag::kRefraction))
			return false;
		if (flags.any(Flag::kEyeReflect))
			return false;
		if (flags.any(Flag::kDecal))
			return false;
		if (flags.any(Flag::kDynamicDecal))
			return false;
		if (flags.any(Flag::kLODLandscape))
			return false;
		if (flags.any(Flag::kMenuScreen))
			return false;

		const bool terrain = flags.all(Flag::kMultiTextureLandscape);
		if (terrain)
			return true;
		if (data.shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
			return false;

		// Keep authored STATIC/SCOL cutouts, but reject anonymous foliage cards.
		if (data.alphaProperty && data.alphaProperty->GetAlphaTesting() && !ownership.explicitStatic)
			return false;
		return true;
	}

	template <class Callback>
	// Summary: Visits visible geometry, following only the active child of switch nodes.
	// Args:
	//   a_object: Root of the subtree to visit.
	//   a_callback: Geometry visitor returning traversal control.
	// Returns: kStop when requested by the callback; otherwise kContinue.
	RE::BSVisit::BSVisitControl TraverseCaptureGeometries(
		RE::NiAVObject* a_object,
		Callback& a_callback)
	{
		using Control = RE::BSVisit::BSVisitControl;
		if (!a_object)
			return Control::kContinue;
		if (a_object->GetAppCulled())
			return Control::kContinue;
		if (auto* geometry = a_object->AsGeometry())
			return a_callback(geometry);
		auto* node = a_object->AsNode();
		if (!node)
			return Control::kContinue;

		const auto& children = node->GetChildren();
		if (auto* switchNode = a_object->AsSwitchNode()) {
			// Only the selected child belongs to the live scene.
			const auto activeIndex = switchNode->index;
			// Switch indices address the logical slot span, including null slots.
			if (activeIndex < 0 || static_cast<std::size_t>(activeIndex) >= children.free_idx())
				return Control::kContinue;
			return TraverseCaptureGeometries(
				children[static_cast<std::uint16_t>(activeIndex)].get(),
				a_callback);
		}
		for (std::uint16_t index = 0; index < children.free_idx(); ++index) {
			if (TraverseCaptureGeometries(children[index].get(), a_callback) == Control::kStop) {
				return Control::kStop;
			}
		}
		return Control::kContinue;
	}

	// Summary: Tests whether a water height is finite and within supported world bounds.
	// Args:
	//   a_height: World-space height to validate.
	// Returns: True when the height can be stored in the capture.
	bool IsValidWaterHeight(float a_height) noexcept
	{
		return std::isfinite(a_height) &&
			std::abs(a_height) < kMaximumValidWaterHeight;
	}

	// Summary: Evaluates a water plane at a world-space horizontal position.
	// Args:
	//   a_plane: NetImmerse plane using dot(normal, position) = constant.
	//   a_worldX: World-space X coordinate.
	//   a_worldY: World-space Y coordinate.
	//   a_height: Receives the evaluated world-space Z coordinate.
	// Returns: True when the plane and resulting height are valid.
	bool TryGetWaterPlaneHeight(
		const RE::NiPlane& a_plane,
		float a_worldX,
		float a_worldY,
		float& a_height) noexcept
	{
		if (!std::isfinite(a_plane.normal.x) || !std::isfinite(a_plane.normal.y) ||
			!std::isfinite(a_plane.normal.z) || !std::isfinite(a_plane.constant) ||
			std::abs(a_plane.normal.z) < 0.001F) {
			return false;
		}
		a_height =
			(a_plane.constant - a_plane.normal.x * a_worldX - a_plane.normal.y * a_worldY) /
			a_plane.normal.z;
		return IsValidWaterHeight(a_height);
	}

	// Summary: Compiles one orthographic-capture shader stage with its stage macro.
	// Args:
	//   a_define: Stage-selection preprocessor definition.
	//   a_target: Direct3D shader model target.
	// Returns: A caller-owned compiled blob, or nullptr on compilation failure.
	ID3DBlob* CompileCaptureStage(const char* a_define, const char* a_target)
	{
		const D3D_SHADER_MACRO defines[]{
			{ a_define, "" },
			{ "WINPC", "" },
			{ "DX11", "" },
			{ nullptr, nullptr }
		};
		winrt::com_ptr<ID3DBlob> shader;
		winrt::com_ptr<ID3DBlob> errors;
		const auto result = D3DCompileFromFile(
			L"Data\\Shaders\\ScreenSpacePhysicalParticles\\OrthographicHeightCapture.hlsl",
			defines,
			nullptr,
			"main",
			a_target,
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0,
			shader.put(),
			errors.put());
		if (FAILED(result)) {
			logger::error(
				"[SSPP] Orthographic height {} compilation failed: {}",
				a_target,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
			return nullptr;
		}
		return shader.detach();
	}

}

namespace ScreenSpacePhysicalParticles
{
	OrthographicHeightField& OrthographicHeightField::GetSingleton() noexcept
	{
		static OrthographicHeightField singleton;
		return singleton;
	}

	// Summary: Schedules at most one game-thread water-system snapshot for the current generation.
	// Args: None.
	// Returns: None.
	void OrthographicHeightField::QueueWaterSystemSnapshot()
	{
		const auto generation = waterSnapshotGeneration.load(std::memory_order_acquire);
		const auto taskToken = generation + 1;
		std::uint64_t expected{};
		if (!waterSnapshotTaskToken.compare_exchange_strong(
				expected, taskToken, std::memory_order_acq_rel, std::memory_order_relaxed)) {
			return;
		}

		if (auto* taskInterface = SKSE::GetTaskInterface()) {
			// Publish immutable POD so the render thread never waits on the water lock.
				taskInterface->AddTask([this, generation]() {
				BuildWaterSystemSnapshot(generation);
			});
		} else {
			std::uint64_t expectedToken = taskToken;
			(void)waterSnapshotTaskToken.compare_exchange_strong(
				expectedToken, 0, std::memory_order_acq_rel, std::memory_order_relaxed);
		}
	}

	// Summary: Copies bounded TESWaterSystem surfaces and publishes an immutable snapshot.
	// Args:
	//   a_generation: Reset generation that must still be current at publication.
	// Returns: None.
	void OrthographicHeightField::BuildWaterSystemSnapshot(std::uint64_t a_generation)
	{
		struct BoundedWaterSource
		{
			RE::NiPlane plane{};
			RE::NiPoint3 center{};
			RE::NiPoint3 size{};
			bool defaultDisplacement{};
		};

		if (a_generation != waterSnapshotGeneration.load(std::memory_order_acquire))
			return;

		auto snapshot = std::make_shared<WaterSystemSnapshot>();
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (cell) {
			auto* worldspace = cell->GetRuntimeData().worldSpace;
			snapshot->worldspaceKey = worldspace ? worldspace->GetFormID() : 0U;
			snapshot->cellKey = cell->GetFormID();
			snapshot->interior = cell->IsInteriorCell();

			if (auto* waterSystem = RE::TESWaterSystem::GetSingleton()) {
				thread_local std::vector<BoundedWaterSource> sources;
				sources.clear();
				if (sources.capacity() < kMaximumSnapshottedWaterBounds)
					sources.reserve(kMaximumSnapshottedWaterBounds);
				RE::TESWorldSpace* systemWorldspace{};
				{
					// Hold the engine lock only while copying bounded POD values.
					const RE::BSSpinLockGuard guard(waterSystem->lock);
					systemWorldspace = waterSystem->worldSpace;
					const auto* defaultDisplacement = waterSystem->defaultDisplacementWaterObject.get();
					for (const auto& waterObject : waterSystem->waterObjects) {
						if (!waterObject)
							continue;
						const bool isDefaultDisplacement = waterObject->shape &&
							waterObject->shape.get() == defaultDisplacement;
						for (const auto& bound : waterObject->multiBounds) {
							if (sources.size() >= kMaximumSnapshottedWaterBounds)
								break;
							if (bound) {
								sources.push_back({
									waterObject->plane,
									bound->center,
									bound->size,
									isDefaultDisplacement
								});
							}
						}
						if (sources.size() >= kMaximumSnapshottedWaterBounds)
							break;
					}
				}

				const bool matchingWorld = snapshot->interior || !worldspace || !systemWorldspace ||
					systemWorldspace == worldspace;
				if (matchingWorld) {
					const auto playerPosition = player->GetPosition();
					std::int32_t centerCellX{};
					std::int32_t centerCellY{};
					if (!snapshot->interior) {
						if (const auto* coordinates = cell->GetCoordinates()) {
							centerCellX = coordinates->cellX;
							centerCellY = coordinates->cellY;
						} else {
							Util::WorldToCell(playerPosition, centerCellX, centerCellY);
						}
					}
					const float2 snapshotCenter = snapshot->interior ?
						float2{ playerPosition.x, playerPosition.y } :
						float2{
							(static_cast<float>(centerCellX) + 0.5F) * kExteriorCellWidth,
							(static_cast<float>(centerCellY) + 0.5F) * kExteriorCellWidth
						};
					const float snapshotHalfExtent = kExteriorHalfExtent;
					snapshot->surfaces.reserve(std::min<std::size_t>(
						sources.size(), kMaximumWaterSurfaces));

					for (const auto& source : sources) {
						if (!snapshot->interior && source.defaultDisplacement)
							continue;
						const auto halfWidth = std::abs(source.size.x);
						const auto halfHeight = std::abs(source.size.y);
						if (!std::isfinite(source.center.x) || !std::isfinite(source.center.y) ||
							!std::isfinite(source.size.x) || !std::isfinite(source.size.y) ||
							!std::isfinite(source.size.z) || halfWidth <= 0.01F ||
							halfHeight <= 0.01F || std::abs(source.size.z) > 10.0F) {
							continue;
						}
						if (!snapshot->interior &&
							(halfWidth > kExteriorCellWidth * 0.5F ||
								halfHeight > kExteriorCellWidth * 0.5F ||
								std::abs(source.plane.normal.z) < 0.999F)) {
							continue;
						}

						float surfaceHeight{};
						if (!TryGetWaterPlaneHeight(
								source.plane, source.center.x, source.center.y, surfaceHeight)) {
							continue;
						}
						const float minimumX = std::max(
							source.center.x - halfWidth, snapshotCenter.x - snapshotHalfExtent);
						const float minimumY = std::max(
							source.center.y - halfHeight, snapshotCenter.y - snapshotHalfExtent);
						const float maximumX = std::min(
							source.center.x + halfWidth, snapshotCenter.x + snapshotHalfExtent);
						const float maximumY = std::min(
							source.center.y + halfHeight, snapshotCenter.y + snapshotHalfExtent);
						if (minimumX < maximumX && minimumY < maximumY) {
							snapshot->surfaces.push_back({
								{ minimumX, minimumY },
								{ maximumX, maximumY },
								surfaceHeight,
								{}
							});
						}
					}
				}
			}
		}

		if (a_generation != waterSnapshotGeneration.load(std::memory_order_acquire))
			return;
		snapshot->generation = a_generation;
		snapshot->revision = waterSnapshotSequence.fetch_add(1, std::memory_order_relaxed) + 1;
		waterSystemSnapshot.store(std::move(snapshot), std::memory_order_release);
		std::uint64_t expectedToken = a_generation + 1;
		(void)waterSnapshotTaskToken.compare_exchange_strong(
			expectedToken, 0, std::memory_order_acq_rel, std::memory_order_relaxed);
	}

	void OrthographicHeightField::SetupResources()
	{
		const auto requestedDimension = static_cast<std::uint32_t>(std::clamp(
			globals::features::screenSpacePhysicalParticles.settings.orthographicResolution, 256, 1024));
		if (dimension != requestedDimension) {
			dimension = requestedDimension;
			heightTexture = nullptr;
			heightRTV = nullptr;
			heightSRV = nullptr;
			debugTexture = nullptr;
			debugUAV = nullptr;
			debugSRV = nullptr;
			ready = false;
			lastSolidCaptureFrame = static_cast<std::uint32_t>(-1);
		}
		if (heightTexture && heightRTV && heightSRV && debugTexture && debugUAV && debugSRV &&
			waterHeightTexture && waterHeightRTV && waterHeightSRV &&
			waterTerrainDepthTexture && waterTerrainDepthDSV &&
			waterDebugTexture && waterDebugUAV && waterDebugSRV &&
			waterSurfaceBuffer && waterSurfaceSRV && captureCB && debugCB &&
			maxBlendState && waterDepthState && rasterizerState) {
			EnsureShaders();
			return;
		}

		auto* device = globals::d3d::device;
		if (!device || !globals::d3d::context)
			return;
		UINT formatSupport{};
		if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &formatSupport)) ||
			(formatSupport & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE)) !=
				(D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE)) {
			logger::critical("[SSPP] R32_FLOAT render-target MAX blending is unavailable; orthographic height capture is disabled");
			return;
		}

		D3D11_TEXTURE2D_DESC heightDesc{};
		heightDesc.Width = dimension;
		heightDesc.Height = dimension;
		heightDesc.MipLevels = 1;
		heightDesc.ArraySize = 1;
		heightDesc.Format = DXGI_FORMAT_R32_FLOAT;
		heightDesc.SampleDesc.Count = 1;
		heightDesc.Usage = D3D11_USAGE_DEFAULT;
		heightDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (!heightTexture) {
			DX::ThrowIfFailed(device->CreateTexture2D(&heightDesc, nullptr, heightTexture.put()));
			DX::ThrowIfFailed(device->CreateRenderTargetView(heightTexture.get(), nullptr, heightRTV.put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(heightTexture.get(), nullptr, heightSRV.put()));
			Util::SetResourceName(heightTexture.get(), "SSPP::OrthographicHeightField");
		}
		// Solid resolution does not invalidate the independent water capture.
		if (waterHeightTexture && waterHeightRTV && waterHeightSRV &&
			waterTerrainDepthTexture && waterTerrainDepthDSV &&
			debugTexture && debugUAV && debugSRV && waterDebugTexture && waterDebugUAV && waterDebugSRV &&
			waterSurfaceBuffer && waterSurfaceSRV && captureCB && debugCB && maxBlendState &&
			waterDepthState && rasterizerState) {
			EnsureShaders();
			return;
		}

		if (!waterHeightTexture) {
			heightDesc.Width = kWaterDimension;
			heightDesc.Height = kWaterDimension;
			DX::ThrowIfFailed(device->CreateTexture2D(&heightDesc, nullptr, waterHeightTexture.put()));
			DX::ThrowIfFailed(device->CreateRenderTargetView(waterHeightTexture.get(), nullptr, waterHeightRTV.put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(waterHeightTexture.get(), nullptr, waterHeightSRV.put()));
			Util::SetResourceName(waterHeightTexture.get(), "SSPP::OrthographicWaterHeightField");
		}
		if (!waterTerrainDepthTexture) {
			D3D11_TEXTURE2D_DESC depthDesc{};
			depthDesc.Width = kWaterDimension;
			depthDesc.Height = kWaterDimension;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			DX::ThrowIfFailed(device->CreateTexture2D(
				&depthDesc, nullptr, waterTerrainDepthTexture.put()));
			DX::ThrowIfFailed(device->CreateDepthStencilView(
				waterTerrainDepthTexture.get(), nullptr, waterTerrainDepthDSV.put()));
			Util::SetResourceName(
				waterTerrainDepthTexture.get(), "SSPP::OrthographicWaterTerrainDepth");
		}

		D3D11_TEXTURE2D_DESC debugDesc{};
		debugDesc.Width = dimension;
		debugDesc.Height = dimension;
		debugDesc.MipLevels = 1;
		debugDesc.ArraySize = 1;
		debugDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		debugDesc.SampleDesc.Count = 1;
		debugDesc.Usage = D3D11_USAGE_DEFAULT;
		debugDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (!debugTexture) {
			DX::ThrowIfFailed(device->CreateTexture2D(&debugDesc, nullptr, debugTexture.put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(debugTexture.get(), nullptr, debugSRV.put()));
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(debugTexture.get(), nullptr, debugUAV.put()));
			Util::SetResourceName(debugTexture.get(), "SSPP::OrthographicHeightDebug");
		}

		debugDesc.Width = kWaterDimension;
		debugDesc.Height = kWaterDimension;
		if (!waterDebugTexture) {
			DX::ThrowIfFailed(device->CreateTexture2D(&debugDesc, nullptr, waterDebugTexture.put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(waterDebugTexture.get(), nullptr, waterDebugSRV.put()));
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(waterDebugTexture.get(), nullptr, waterDebugUAV.put()));
			Util::SetResourceName(waterDebugTexture.get(), "SSPP::OrthographicWaterHeightDebug");
		}
		if (waterSurfaceBuffer && waterSurfaceSRV && captureCB && debugCB && maxBlendState &&
			waterDepthState && rasterizerState) {
			EnsureShaders();
			return;
		}

		const auto waterSurfaceDesc =
			StructuredBufferDesc<WaterSurfaceGPU>(kMaximumWaterSurfaces, false, true);
		DX::ThrowIfFailed(device->CreateBuffer(&waterSurfaceDesc, nullptr, waterSurfaceBuffer.put()));
		D3D11_SHADER_RESOURCE_VIEW_DESC waterSurfaceSRVDesc{};
		waterSurfaceSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		waterSurfaceSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		waterSurfaceSRVDesc.BufferEx.FirstElement = 0;
		waterSurfaceSRVDesc.BufferEx.NumElements = static_cast<UINT>(kMaximumWaterSurfaces);
		DX::ThrowIfFailed(device->CreateShaderResourceView(
			waterSurfaceBuffer.get(), &waterSurfaceSRVDesc, waterSurfaceSRV.put()));
		Util::SetResourceName(waterSurfaceBuffer.get(), "SSPP::OrthographicWaterSurfaces");

		D3D11_BLEND_DESC blendDesc{};
		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, maxBlendState.put()));
		Util::SetResourceName(maxBlendState.get(), "SSPP::OrthographicHeightMaxBlend");

		D3D11_DEPTH_STENCIL_DESC depthStateDesc{};
		depthStateDesc.DepthEnable = true;
		depthStateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStateDesc.DepthFunc = D3D11_COMPARISON_LESS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStateDesc, waterDepthState.put()));
		Util::SetResourceName(waterDepthState.get(), "SSPP::OrthographicWaterDepthState");

		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = false;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "SSPP::OrthographicHeightRasterizer");

		captureCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<CaptureCB>(), "SSPP::OrthographicHeightCaptureCB");
		debugCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<DebugCB>(), "SSPP::OrthographicHeightDebugCB");
		EnsureShaders();
}

	// Summary: Lazily compiles capture and debug shader stages.
	// Args: None.
	// Returns: None.
	void OrthographicHeightField::EnsureShaders()
	{
		auto* device = globals::d3d::device;
		if (!device)
			return;
		if (!captureVS) {
			captureVSBlob.attach(CompileCaptureStage("VSHADER", "vs_5_0"));
			if (captureVSBlob) {
				DX::ThrowIfFailed(device->CreateVertexShader(
					captureVSBlob->GetBufferPointer(), captureVSBlob->GetBufferSize(), nullptr, captureVS.put()));
				Util::SetResourceName(captureVS.get(), "SSPP::OrthographicHeightVS");
			}
		}
		if (!capturePS) {
			winrt::com_ptr<ID3DBlob> blob;
			blob.attach(CompileCaptureStage("PSHADER", "ps_5_0"));
			if (blob) {
				DX::ThrowIfFailed(device->CreatePixelShader(
					blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, capturePS.put()));
				Util::SetResourceName(capturePS.get(), "SSPP::OrthographicHeightPS");
			}
		}
		if (!waterTerrainVS) {
			winrt::com_ptr<ID3DBlob> blob;
			blob.attach(CompileCaptureStage("WATER_TERRAIN_VSHADER", "vs_5_0"));
			if (blob) {
				DX::ThrowIfFailed(device->CreateVertexShader(
					blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, waterTerrainVS.put()));
				Util::SetResourceName(waterTerrainVS.get(), "SSPP::OrthographicWaterTerrainVS");
			}
		}
		if (!waterCaptureVS) {
			winrt::com_ptr<ID3DBlob> blob;
			blob.attach(CompileCaptureStage("WATER_VSHADER", "vs_5_0"));
			if (blob) {
				DX::ThrowIfFailed(device->CreateVertexShader(
					blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, waterCaptureVS.put()));
				Util::SetResourceName(waterCaptureVS.get(), "SSPP::OrthographicWaterHeightVS");
			}
		}
		if (!waterCapturePS) {
			winrt::com_ptr<ID3DBlob> blob;
			blob.attach(CompileCaptureStage("WATER_PSHADER", "ps_5_0"));
			if (blob) {
				DX::ThrowIfFailed(device->CreatePixelShader(
					blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, waterCapturePS.put()));
				Util::SetResourceName(waterCapturePS.get(), "SSPP::OrthographicWaterHeightPS");
			}
		}
		if (!debugCS) {
			debugCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data\\Shaders\\ScreenSpacePhysicalParticles\\OrthographicHeightDebugCS.hlsl",
				{},
				"cs_5_0",
				"Main")));
		}
	}

	// Summary: Returns or creates a position-only input layout for a Skyrim vertex descriptor.
	// Args:
	//   a_desc: Source vertex descriptor.
	// Returns: Cached compatible layout, or nullptr for unsupported position formats.
	ID3D11InputLayout* OrthographicHeightField::GetInputLayout(
		const RE::BSGraphics::VertexDesc& a_desc)
	{
		if (!captureVSBlob)
			return nullptr;

		std::uint64_t key{};
		static_assert(sizeof(key) == sizeof(a_desc));
		std::memcpy(&key, &a_desc, sizeof(key));
		if (const auto found = inputLayouts.find(key); found != inputLayouts.end())
			return found->second.get();

		const auto stride = static_cast<std::uint32_t>(key & 0xFU) * 4U;
		std::uint32_t positionBytes = stride;
		static constexpr std::pair<RE::BSGraphics::Vertex::Flags, RE::BSGraphics::Vertex::Attribute> attributes[]{
			{ RE::BSGraphics::Vertex::VF_UV, RE::BSGraphics::Vertex::VA_TEXCOORD0 },
			{ RE::BSGraphics::Vertex::VF_UV_2, RE::BSGraphics::Vertex::VA_TEXCOORD1 },
			{ RE::BSGraphics::Vertex::VF_NORMAL, RE::BSGraphics::Vertex::VA_NORMAL },
			{ RE::BSGraphics::Vertex::VF_TANGENT, RE::BSGraphics::Vertex::VA_BINORMAL },
			{ RE::BSGraphics::Vertex::VF_COLORS, RE::BSGraphics::Vertex::VA_COLOR },
			{ RE::BSGraphics::Vertex::VF_SKINNED, RE::BSGraphics::Vertex::VA_SKINNING },
			{ RE::BSGraphics::Vertex::VF_LANDDATA, RE::BSGraphics::Vertex::VA_LANDDATA },
			{ RE::BSGraphics::Vertex::VF_EYEDATA, RE::BSGraphics::Vertex::VA_EYEDATA }
		};
		for (const auto& [flag, attribute] : attributes) {
			if (a_desc.HasFlag(flag)) {
				const auto offset = a_desc.GetAttributeOffset(attribute);
				if (offset > 0 && offset < positionBytes)
					positionBytes = offset;
			}
		}
		// The replacement shader supports Skyrim's half4 and float4 positions.
		if (stride == 0 || (positionBytes != 8 && positionBytes != 16)) {
			inputLayouts.try_emplace(key);
			return nullptr;
		}

		const D3D11_INPUT_ELEMENT_DESC element{
			"POSITION",
			0,
			positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT,
			0,
			0,
			D3D11_INPUT_PER_VERTEX_DATA,
			0
		};
		winrt::com_ptr<ID3D11InputLayout> layout;
		if (FAILED(globals::d3d::device->CreateInputLayout(
				&element,
				1,
				captureVSBlob->GetBufferPointer(),
				captureVSBlob->GetBufferSize(),
				layout.put()))) {
			inputLayouts.try_emplace(key);
			return nullptr;
		}
		auto* result = layout.get();
		inputLayouts.emplace(key, std::move(layout));
		return result;
	}

	// Summary: Draws one validated geometry into the currently bound height target.
	// Args:
	//   a_geometry: Geometry retained by the capture candidate set.
	//   a_center: Horizontal center of the capture.
	//   a_halfExtent: Horizontal half-width of the capture.
	//   a_topZ: Highest captured world-space Z coordinate.
	//   a_bottomZ: Lowest captured world-space Z coordinate.
	// Returns: True when indexed geometry was submitted.
	bool OrthographicHeightField::DrawGeometry(
		RE::BSGeometry* a_geometry,
		float2 a_center,
		float a_halfExtent,
		float a_topZ,
		float a_bottomZ)
	{
		if (!a_geometry)
			return false;

		auto* triShape = a_geometry->AsTriShape();
		const auto indexCount = static_cast<std::uint32_t>(triShape->GetTrishapeRuntimeData().triangleCount) * 3U;
		if (indexCount == 0)
			return false;
		const auto& data = a_geometry->GetGeometryRuntimeData();
		std::uint64_t descriptorKey{};
		std::memcpy(&descriptorKey, &data.vertexDesc, sizeof(descriptorKey));
		const UINT stride = static_cast<UINT>(descriptorKey & 0xFU) * 4U;
		auto* layout = GetInputLayout(data.vertexDesc);
		if (!layout || stride == 0)
			return false;

		CaptureCB constants{};
		const auto& world = a_geometry->world;
		const auto scale = world.scale;
		constants.worldRow0 = {
			world.rotate.entry[0][0] * scale,
			world.rotate.entry[0][1] * scale,
			world.rotate.entry[0][2] * scale,
			world.translate.x
		};
		constants.worldRow1 = {
			world.rotate.entry[1][0] * scale,
			world.rotate.entry[1][1] * scale,
			world.rotate.entry[1][2] * scale,
			world.translate.y
		};
		constants.worldRow2 = {
			world.rotate.entry[2][0] * scale,
			world.rotate.entry[2][1] * scale,
			world.rotate.entry[2][2] * scale,
			world.translate.z
		};
		constants.center = a_center;
		constants.halfExtent = a_halfExtent;
		constants.captureTopZ = a_topZ;
		constants.captureBottomZ = a_bottomZ;
		captureCB->Update(constants);

		auto* context = globals::d3d::context;
		auto* vertexBuffer = reinterpret_cast<ID3D11Buffer*>(data.rendererData->vertexBuffer);
		auto* indexBuffer = reinterpret_cast<ID3D11Buffer*>(data.rendererData->indexBuffer);
		UINT offset{};
		context->IASetInputLayout(layout);
		context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
		context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
		ID3D11Buffer* constantsBuffer = captureCB->CB();
		context->VSSetConstantBuffers(0, 1, &constantsBuffer);
		context->PSSetConstantBuffers(0, 1, &constantsBuffer);
		context->DrawIndexed(indexCount, 0, 0);
		return true;
	}

	void OrthographicHeightField::Capture()
	{
		SetupResources();
		ApplyPendingReset();
		EnsureShaders();
		if (!heightRTV || !waterHeightRTV || !waterTerrainDepthDSV || !captureVS || !capturePS ||
			!waterTerrainVS || !waterCaptureVS || !waterCapturePS ||
			!waterSurfaceBuffer || !waterSurfaceSRV || !waterDepthState ||
			!captureCB || !globals::game::tes || !globals::state) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!cell)
			return;
		const auto frame = globals::state->frameCount;
		if (!captureFrame.IsNewFrame(frame))
			return;

		auto* worldspace = cell->GetRuntimeData().worldSpace;
		const std::uint32_t worldspaceKey = worldspace ? worldspace->GetFormID() : 0U;
		const std::uint32_t cellKey = cell->GetFormID();
		const bool interior = cell->IsInteriorCell();
		const auto playerPosition = player->GetPosition();
		const auto measuredPlayerHeight = player->GetHeight();
		const auto playerHeight = std::isfinite(measuredPlayerHeight) ?
			std::clamp(measuredPlayerHeight, 32.0F, 512.0F) :
			128.0F;
		const auto refreshFrames = static_cast<std::uint32_t>(std::clamp(
			globals::features::screenSpacePhysicalParticles.settings.orthographicRefreshFrames, 10, 300));
		const bool manualRecapture = recaptureRequested;
		const bool solidCaptureDue = manualRecapture ||
			lastSolidCaptureFrame == static_cast<std::uint32_t>(-1) ||
			frame - lastSolidCaptureFrame >= refreshFrames;
		auto* solidWorldRoot = solidCaptureDue ? RE::Main::WorldRootNode() : nullptr;
		const bool captureSolid = solidCaptureDue && solidWorldRoot;
		if (solidCaptureDue && !solidWorldRoot) {
			if (!worldRootUnavailableLogged) {
				logger::warn("[SSPP] Orthographic solid capture skipped: world root unavailable");
				worldRootUnavailableLogged = true;
			}
			lastSolidCaptureFrame = frame;
		} else if (solidWorldRoot) {
			worldRootUnavailableLogged = false;
		}
		const bool waterContextChanged = !hasCaptureAnchor || worldspaceKey != lastWorldspaceKey ||
			cellKey != lastCellKey || interior != lastInterior;
		const auto waterSnapshot = waterSystemSnapshot.load(std::memory_order_acquire);
		const bool waterSnapshotMatches = waterSnapshot &&
			waterSnapshot->generation == waterSnapshotGeneration.load(std::memory_order_acquire) &&
			waterSnapshot->worldspaceKey == worldspaceKey &&
			waterSnapshot->cellKey == cellKey && waterSnapshot->interior == interior;
		const bool waterSnapshotChanged = waterSnapshotMatches &&
			waterSnapshot->revision != lastWaterSnapshotRevision;
		if (manualRecapture || waterContextChanged || !waterSnapshotMatches)
			QueueWaterSystemSnapshot();
		const bool captureWater = manualRecapture || waterContextChanged || waterSnapshotChanged;
		if (!captureSolid && !captureWater)
			return;
		recaptureRequested = false;

		std::int32_t centerCellX{};
		std::int32_t centerCellY{};
		if (!interior) {
			if (const auto* coordinates = cell->GetCoordinates()) {
				centerCellX = coordinates->cellX;
				centerCellY = coordinates->cellY;
			} else {
				Util::WorldToCell(playerPosition, centerCellX, centerCellY);
			}
		}
		auto* tes = globals::game::tes;
		if (captureSolid) {
			center = { playerPosition.x, playerPosition.y };
			// Resolution scales coverage at four world units per texel.
			halfExtent = static_cast<float>(dimension) * kSolidWorldUnitsPerTexel * 0.5F;
			topZ = playerPosition.z + playerHeight * (interior ? 1.0F : 3.0F);
			bottomZ = topZ - kVerticalRange;
		}
		if (captureWater) {
			waterCenter = interior ?
				float2{ playerPosition.x, playerPosition.y } :
				float2{
					(static_cast<float>(centerCellX) + 0.5F) * kExteriorCellWidth,
					(static_cast<float>(centerCellY) + 0.5F) * kExteriorCellWidth
				};
			waterHalfExtent = kExteriorHalfExtent;
		}

		auto& candidates = solidCandidateScratch;
		auto& seen = solidSeenScratch;
		auto& waterTerrainCandidates = waterTerrainCandidateScratch;
		auto& seenWaterTerrain = waterTerrainSeenScratch;
		candidates.clear();
		seen.clear();
		waterTerrainCandidates.clear();
		seenWaterTerrain.clear();
		candidates.reserve(2048);
		seen.reserve(4096);
		waterTerrainCandidates.reserve(1024);
		seenWaterTerrain.reserve(2048);
		bool captureTruncated = false;
		bool waterTerrainCaptureTruncated = false;
		// Summary: Tests whether a world bound intersects a capture volume.
		// Args:
		//   a_bound: World-space sphere bound to test.
		//   a_center: Horizontal capture center.
		//   a_halfExtent: Horizontal capture half-width.
		//   a_topZ: Capture ceiling.
		//   a_bottomZ: Capture floor.
		// Returns: True when the bound overlaps the volume.
		const auto overlapsCapture = [](
			const RE::NiBound& a_bound,
			float2 a_center,
			float a_halfExtent,
			float a_topZ,
			float a_bottomZ) {
			const auto radius = std::max(std::abs(a_bound.radius), 0.0F);
			return std::isfinite(a_bound.center.x) && std::isfinite(a_bound.center.y) &&
				std::isfinite(a_bound.center.z) && std::isfinite(radius) &&
				a_bound.center.z - radius <= a_topZ && a_bound.center.z + radius >= a_bottomZ &&
				a_bound.center.x + radius >= a_center.x - a_halfExtent &&
				a_bound.center.x - radius <= a_center.x + a_halfExtent &&
				a_bound.center.y + radius >= a_center.y - a_halfExtent &&
				a_bound.center.y - radius <= a_center.y + a_halfExtent;
		};
		// Summary: Adds one eligible, in-bounds geometry to the solid candidate set.
		// Args:
		//   a_geometry: Geometry reached by scene traversal.
		// Returns: kContinue so traversal visits the remaining scene.
		const auto collectSolidCandidate = [&](RE::BSGeometry* a_geometry) {
			if (!a_geometry || seen.contains(a_geometry))
				return RE::BSVisit::BSVisitControl::kContinue;
			if (!overlapsCapture(a_geometry->worldBound, center, halfExtent, topZ, bottomZ))
				return RE::BSVisit::BSVisitControl::kContinue;
			if (!IsEligibleStaticGeometry(a_geometry))
				return RE::BSVisit::BSVisitControl::kContinue;
			if (candidates.size() >= kMaximumCapturedGeometry) {
				captureTruncated = true;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			seen.insert(a_geometry);
			candidates.emplace_back(a_geometry);
			return RE::BSVisit::BSVisitControl::kContinue;
		};
		// Summary: Adds one eligible LAND geometry to the water-occlusion candidate set.
		// Args:
		//   a_geometry: Geometry reached by LAND traversal.
		// Returns: kContinue so traversal visits the remaining LAND scene.
		const auto collectWaterTerrainCandidate = [&](RE::BSGeometry* a_geometry) {
			if (!a_geometry || seenWaterTerrain.contains(a_geometry))
				return RE::BSVisit::BSVisitControl::kContinue;
			if (!overlapsCapture(
					a_geometry->worldBound,
					waterCenter,
					waterHalfExtent,
					kMaximumValidWaterHeight,
					-kMaximumValidWaterHeight)) {
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			if (!IsEligibleStaticGeometry(a_geometry))
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& data = a_geometry->GetGeometryRuntimeData();
			if (!data.shaderProperty || !data.shaderProperty->flags.all(
					RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			if (waterTerrainCandidates.size() >= kMaximumCapturedGeometry) {
				waterTerrainCaptureTruncated = true;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			seenWaterTerrain.insert(a_geometry);
			waterTerrainCandidates.emplace_back(a_geometry);
			return RE::BSVisit::BSVisitControl::kContinue;
		};
		const auto collectSolidRoot = [&](RE::NiAVObject* a_root) {
			if (captureSolid)
				TraverseCaptureGeometries(a_root, collectSolidCandidate);
		};
		const auto collectWaterTerrainRoot = [&](RE::NiAVObject* a_root) {
			if (captureWater && !interior)
				TraverseCaptureGeometries(a_root, collectWaterTerrainCandidate);
		};
		// LAND meshes are separate from the live world root and are collected below.
		if (captureSolid)
			collectSolidRoot(solidWorldRoot);

		std::array<RE::TESObjectCELL*, kExteriorGridWidth * kExteriorGridWidth> capturedCells{};
		std::size_t capturedCellCount{};
		if (!interior) {
			if (auto* gridCells = tes->gridCells) {
				// Visit loaded cells for separately managed LAND and water data.
				for (std::uint32_t gridX = 0; gridX < gridCells->length; ++gridX) {
					for (std::uint32_t gridY = 0; gridY < gridCells->length; ++gridY) {
						auto* gridCell = gridCells->GetCell(gridX, gridY);
						if (!gridCell || !gridCell->IsAttached() || gridCell->IsInteriorCell())
							continue;
						if (worldspace && gridCell->GetRuntimeData().worldSpace != worldspace)
							continue;
						const auto* coordinates = gridCell->GetCoordinates();
						if (!coordinates)
							continue;
						const float cellMinimumX = static_cast<float>(coordinates->cellX) * kExteriorCellWidth;
						const float cellMinimumY = static_cast<float>(coordinates->cellY) * kExteriorCellWidth;
						const bool overlapsSolid = captureSolid &&
							cellMinimumX + kExteriorCellWidth >= center.x - halfExtent &&
							cellMinimumX <= center.x + halfExtent &&
							cellMinimumY + kExteriorCellWidth >= center.y - halfExtent &&
							cellMinimumY <= center.y + halfExtent;
						const bool overlapsWater = captureWater &&
							std::abs(coordinates->cellX - centerCellX) <= 1 &&
							std::abs(coordinates->cellY - centerCellY) <= 1;
						if (!overlapsSolid && !overlapsWater)
							continue;

						if (overlapsWater && capturedCellCount < capturedCells.size())
							capturedCells[capturedCellCount++] = gridCell;
						auto& runtime = gridCell->GetRuntimeData();
						if (runtime.cellLand && runtime.cellLand->loadedData) {
							for (auto* terrainNode : runtime.cellLand->loadedData->mesh) {
								if (!terrainNode)
									continue;
								if (overlapsSolid)
									collectSolidRoot(terrainNode);
								if (overlapsWater)
									collectWaterTerrainRoot(terrainNode);
							}
						}
					}
				}
			}
		}
		auto& waterCandidates = waterCandidateScratch;
		waterCandidates.clear();
		waterCandidates.reserve(32);
		// Summary: Validates and appends one bounded water surface candidate.
		// Args:
		//   a_minimumX: Minimum world-space X coordinate.
		//   a_minimumY: Minimum world-space Y coordinate.
		//   a_maximumX: Maximum world-space X coordinate.
		//   a_maximumY: Maximum world-space Y coordinate.
		//   a_height: World-space surface height.
		// Returns: None.
		const auto addWaterSurface = [&](float a_minimumX,
			float a_minimumY,
			float a_maximumX,
			float a_maximumY,
			float a_height) {
			if (!std::isfinite(a_minimumX) || !std::isfinite(a_minimumY) ||
				!std::isfinite(a_maximumX) || !std::isfinite(a_maximumY) ||
				a_minimumX >= a_maximumX || a_minimumY >= a_maximumY ||
				!IsValidWaterHeight(a_height)) {
				return;
			}
			waterCandidates.push_back({
				{ a_minimumX, a_minimumY },
				{ a_maximumX, a_maximumY },
				a_height,
				{}
			});
		};
		if (captureWater && !interior) {
			for (std::size_t index = 0; index < capturedCellCount; ++index) {
				auto* waterCell = capturedCells[index];
				if (!waterCell || !waterCell->cellFlags.any(RE::TESObjectCELL::Flag::kHasWater))
					continue;
				const auto* coordinates = waterCell ? waterCell->GetCoordinates() : nullptr;
				if (!coordinates)
					continue;
				const auto minimumX = static_cast<float>(coordinates->cellX) * kExteriorCellWidth;
				const auto minimumY = static_cast<float>(coordinates->cellY) * kExteriorCellWidth;
				addWaterSurface(
					minimumX,
					minimumY,
					minimumX + kExteriorCellWidth,
					minimumY + kExteriorCellWidth,
					waterCell->GetExteriorWaterHeight());
			}
		}
		// Combine exterior XCLW planes with bounded TESWaterSystem surfaces.
		if (captureWater && waterSnapshotMatches) {
			for (const auto& surface : waterSnapshot->surfaces) {
				addWaterSurface(
					surface.minimum.x,
					surface.minimum.y,
					surface.maximum.x,
					surface.maximum.y,
					surface.height);
			}
		}

		std::array<WaterSurfaceGPU, kMaximumWaterSurfaces> waterSurfaces{};
		std::uint32_t waterSurfaceCount{};
		float waterMinimumHeight = std::numeric_limits<float>::max();
		float waterMaximumHeight = std::numeric_limits<float>::lowest();
		bool waterCaptureTruncated = false;
		for (const auto& surface : waterCandidates) {
			if (surface.maximum.x < waterCenter.x - waterHalfExtent ||
				surface.minimum.x > waterCenter.x + waterHalfExtent ||
				surface.maximum.y < waterCenter.y - waterHalfExtent ||
				surface.minimum.y > waterCenter.y + waterHalfExtent) {
				continue;
			}
			if (waterSurfaceCount >= waterSurfaces.size()) {
				waterCaptureTruncated = true;
				break;
			}
			waterSurfaces[waterSurfaceCount++] = surface;
			waterMinimumHeight = std::min(waterMinimumHeight, surface.height);
			waterMaximumHeight = std::max(waterMaximumHeight, surface.height);
		}

		auto* context = globals::d3d::context;
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		ID3D11ShaderResourceView* nullSRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
		context->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRVs);
		context->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRVs);
		context->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullSRVs);
		const float clear[4]{ kMissingHeight, 0.0F, 0.0F, 0.0F };
		context->OMSetBlendState(maxBlendState.get(), nullptr, 0xFFFFFFFFU);
		context->OMSetDepthStencilState(nullptr, 0U);
		context->RSSetState(rasterizerState.get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		if (captureSolid) {
			const D3D11_VIEWPORT viewport{ 0.0F, 0.0F, static_cast<float>(dimension), static_cast<float>(dimension), 0.0F, 1.0F };
			context->RSSetViewports(1, &viewport);
			context->ClearRenderTargetView(heightRTV.get(), clear);
			ID3D11RenderTargetView* target = heightRTV.get();
			context->OMSetRenderTargets(1, &target, nullptr);
			context->VSSetShader(captureVS.get(), nullptr, 0);
			context->PSSetShader(capturePS.get(), nullptr, 0);
			capturedGeometryCount = 0;
			capturedTerrainCount = 0;
			capturedStaticCount = 0;
			globals::profiler->BeginPass("SSPP::OrthographicSolidCapture");
			for (const auto& candidate : candidates) {
				if (DrawGeometry(candidate.get(), center, halfExtent, topZ, bottomZ)) {
					++capturedGeometryCount;
					const auto& flags = candidate->GetGeometryRuntimeData().shaderProperty->flags;
					flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape) ?
						++capturedTerrainCount :
						++capturedStaticCount;
				}
			}
			globals::profiler->EndPass();
			ready = capturedGeometryCount > 0;
			lastSolidCaptureFrame = frame;
		}

		if (captureWater) {
			const D3D11_VIEWPORT viewport{ 0.0F, 0.0F, static_cast<float>(kWaterDimension), static_cast<float>(kWaterDimension), 0.0F, 1.0F };
			context->RSSetViewports(1, &viewport);
			context->ClearRenderTargetView(waterHeightRTV.get(), clear);
			context->ClearDepthStencilView(
				waterTerrainDepthDSV.get(), D3D11_CLEAR_DEPTH, 1.0F, 0U);
			capturedWaterSurfaceCount = 0;
			if (waterSurfaceCount > 0) {
				globals::profiler->BeginPass("SSPP::OrthographicWaterCapture");
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(context->Map(
						waterSurfaceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					std::memcpy(mapped.pData, waterSurfaces.data(), sizeof(WaterSurfaceGPU) * waterSurfaceCount);
					context->Unmap(waterSurfaceBuffer.get(), 0);

					CaptureCB constants{};
					constants.center = waterCenter;
					constants.halfExtent = waterHalfExtent;
					constants.captureTopZ = kMaximumValidWaterHeight;
					constants.captureBottomZ = -kMaximumValidWaterHeight;
					captureCB->Update(constants);

					// LAND depth clips water planes to their exposed top-down footprint.
					context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
					context->OMSetDepthStencilState(waterDepthState.get(), 0U);
					context->OMSetRenderTargets(0, nullptr, waterTerrainDepthDSV.get());
					context->VSSetShader(waterTerrainVS.get(), nullptr, 0);
					context->PSSetShader(nullptr, nullptr, 0);
					for (const auto& candidate : waterTerrainCandidates) {
						DrawGeometry(
							candidate.get(),
							waterCenter,
							waterHalfExtent,
							kMaximumValidWaterHeight,
							-kMaximumValidWaterHeight);
					}

					ID3D11RenderTargetView* waterTarget = waterHeightRTV.get();
					context->OMSetBlendState(maxBlendState.get(), nullptr, 0xFFFFFFFFU);
					context->OMSetRenderTargets(1, &waterTarget, waterTerrainDepthDSV.get());
					context->IASetInputLayout(nullptr);
					context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
					ID3D11Buffer* nullVertexBuffer{};
					UINT zero{};
					context->IASetVertexBuffers(0, 1, &nullVertexBuffer, &zero, &zero);
					context->VSSetShader(waterCaptureVS.get(), nullptr, 0);
					context->PSSetShader(waterCapturePS.get(), nullptr, 0);
					ID3D11ShaderResourceView* surfaces = waterSurfaceSRV.get();
					ID3D11Buffer* constantsBuffer = captureCB->CB();
					context->VSSetShaderResources(0, 1, &surfaces);
					context->VSSetConstantBuffers(0, 1, &constantsBuffer);
					context->PSSetConstantBuffers(0, 1, &constantsBuffer);
					context->DrawInstanced(6, waterSurfaceCount, 0, 0);
					ID3D11ShaderResourceView* nullSRV{};
					context->VSSetShaderResources(0, 1, &nullSRV);
					capturedWaterSurfaceCount = waterSurfaceCount;
				}
				globals::profiler->EndPass();
			}
			capturedWaterMinimumHeight = waterSurfaceCount > 0 ? waterMinimumHeight : 0.0F;
			capturedWaterMaximumHeight = waterSurfaceCount > 0 ? waterMaximumHeight : 0.0F;
			waterReady = capturedWaterSurfaceCount > 0;
			lastWorldspaceKey = worldspaceKey;
			lastCellKey = cellKey;
			lastInterior = interior;
			hasCaptureAnchor = true;
			if (waterSnapshotMatches)
				lastWaterSnapshotRevision = waterSnapshot->revision;
		}
		context->OMSetRenderTargets(0, nullptr, nullptr);
		candidates.clear();
		seen.clear();
		waterTerrainCandidates.clear();
		seenWaterTerrain.clear();
		waterCandidates.clear();

		if (captureSolid && captureTruncated)
			logger::warn("[SSPP] Orthographic height capture reached its {}-geometry safety cap", kMaximumCapturedGeometry);
		if (captureWater && waterCaptureTruncated)
			logger::warn("[SSPP] Orthographic water capture reached its {}-surface safety cap", kMaximumWaterSurfaces);
		if (captureWater && waterTerrainCaptureTruncated)
			logger::warn("[SSPP] Orthographic water terrain capture reached its {}-geometry safety cap", kMaximumCapturedGeometry);
	}

	void OrthographicHeightField::UpdateDebug(bool a_solidEnabled, bool a_waterEnabled)
	{
		if ((!a_solidEnabled && !a_waterEnabled) || !debugCB)
			return;
		EnsureShaders();
		if (!debugCS)
			return;

		auto* context = globals::d3d::context;
		ID3D11Buffer* constantsBuffer = debugCB->CB();
		context->CSSetConstantBuffers(0, 1, &constantsBuffer);
		context->CSSetShader(debugCS.get(), nullptr, 0);
		const float clear[4]{};
		// Summary: Converts one ready height source into its debug texture.
		// Args:
		//   a_enabled: Whether conversion was requested.
		//   a_ready: Whether the source contains valid capture data.
		//   a_source: Height-field shader-resource view.
		//   a_output: Debug-texture unordered-access view.
		//   a_dimension: Source and destination dimension.
		//   a_minimumHeight: Lowest height used for visualization.
		//   a_maximumHeight: Highest height used for visualization.
		//   a_waterMode: Whether to use water visualization rules.
		//   a_profileName: GPU profiler pass name.
		// Returns: None.
		const auto dispatch = [&](bool a_enabled,
			bool a_ready,
			ID3D11ShaderResourceView* a_source,
			ID3D11UnorderedAccessView* a_output,
			std::uint32_t a_dimension,
			float a_minimumHeight,
			float a_maximumHeight,
			bool a_waterMode,
			const char* a_profileName) {
			if (!a_enabled || !a_output)
				return;
			if (!a_ready || !a_source) {
				context->ClearUnorderedAccessViewFloat(a_output, clear);
				return;
			}
			DebugCB constants{
				a_minimumHeight,
				a_maximumHeight,
				kMissingHeight,
				a_waterMode ? 1.0F : 0.0F
			};
			debugCB->Update(constants);
			context->CSSetShaderResources(0, 1, &a_source);
			context->CSSetUnorderedAccessViews(0, 1, &a_output, nullptr);
			globals::profiler->BeginPass(a_profileName);
			context->Dispatch(
				(a_dimension + kDebugThreads - 1) / kDebugThreads,
				(a_dimension + kDebugThreads - 1) / kDebugThreads,
				1);
			globals::profiler->EndPass();
			ID3D11ShaderResourceView* nullSRV{};
			ID3D11UnorderedAccessView* nullUAV{};
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		};
		dispatch(
			a_solidEnabled,
			ready,
			heightSRV.get(),
			debugUAV.get(),
			dimension,
			bottomZ,
			topZ,
			false,
			"SSPP::OrthographicHeightDebug");
		dispatch(
			a_waterEnabled,
			waterReady,
			waterHeightSRV.get(),
			waterDebugUAV.get(),
			kWaterDimension,
			capturedWaterMinimumHeight,
			capturedWaterMaximumHeight,
			true,
			"SSPP::OrthographicWaterHeightDebug");
		ID3D11ShaderResourceView* nullSRV{};
		ID3D11UnorderedAccessView* nullUAV{};
		ID3D11Buffer* nullCB{};
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	void OrthographicHeightField::ClearShaderCache()
	{
		captureVS = nullptr;
		capturePS = nullptr;
		waterTerrainVS = nullptr;
		waterCaptureVS = nullptr;
		waterCapturePS = nullptr;
		debugCS = nullptr;
		captureVSBlob = nullptr;
		inputLayouts.clear();
	}

	void OrthographicHeightField::Reset() noexcept
	{
		// Invalidate producers here; Capture() owns the render-state reset.
		waterSnapshotGeneration.fetch_add(1, std::memory_order_acq_rel);
		waterSnapshotTaskToken.store(0, std::memory_order_release);
		waterSystemSnapshot.store(
			std::shared_ptr<const WaterSystemSnapshot>{}, std::memory_order_release);
		resetPending.store(true, std::memory_order_release);
	}

	// Summary: Clears render-owned capture state after a cross-thread reset request.
	// Args: None.
	// Returns: None.
	void OrthographicHeightField::ApplyPendingReset() noexcept
	{
		if (!resetPending.exchange(false, std::memory_order_acq_rel))
			return;
		lastWaterSnapshotRevision = 0;
		ready = false;
		waterReady = false;
		center = {};
		halfExtent = static_cast<float>(dimension) * kSolidWorldUnitsPerTexel * 0.5F;
		topZ = 0.0F;
		bottomZ = 0.0F;
		capturedGeometryCount = 0;
		capturedTerrainCount = 0;
		capturedStaticCount = 0;
		capturedWaterSurfaceCount = 0;
		capturedWaterMinimumHeight = 0.0F;
		capturedWaterMaximumHeight = 0.0F;
		lastWorldspaceKey = 0;
		lastCellKey = 0;
		lastSolidCaptureFrame = static_cast<std::uint32_t>(-1);
		captureFrame = {};
		recaptureRequested = false;
		worldRootUnavailableLogged = false;
		waterCenter = {};
		waterHalfExtent = kExteriorHalfExtent;
		lastInterior = false;
		hasCaptureAnchor = false;
	}
}
