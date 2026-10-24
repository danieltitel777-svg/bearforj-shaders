#pragma once

#include "Buffer.h"
#include "Utils/Game.h"

#include <ankerl/unordered_dense.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

namespace ScreenSpacePhysicalParticles
{
	// Captures world-space surface heights for collisions outside the current view.
	// The solid field follows the player and combines attached scene geometry with
	// separately owned LAND meshes. The water field covers a cell-aligned 3x3 area
	// and stores exposed water planes after terrain occlusion.
	class OrthographicHeightField final
	{
	public:
		static constexpr std::uint32_t kDefaultSolidDimension = 1024;
		static constexpr std::uint32_t kWaterDimension = 1024;
		static constexpr float kExteriorCellWidth = 4096.0F;
		static constexpr std::uint32_t kExteriorGridWidth = 3;
		static constexpr float kExteriorExtent = kExteriorCellWidth * kExteriorGridWidth;
		static constexpr float kExteriorHalfExtent = kExteriorExtent * 0.5F;
		static constexpr float kSolidWorldUnitsPerTexel = 4.0F;
		static constexpr float kVerticalRange = 8192.0F;
		static constexpr float kMissingHeight = -100000.0F;

		// Summary: Returns the process-wide height-field provider.
		// Args: None.
		// Returns: The shared provider instance.
		static OrthographicHeightField& GetSingleton() noexcept;

		// Summary: Creates capture, debug, and water resources for the configured dimensions.
		// Args: None.
		// Returns: None.
		void SetupResources();
		// Summary: Updates due solid and water captures from the current world state.
		// Args: None.
		// Returns: None.
		void Capture();
		// Summary: Converts requested height fields into displayable debug textures.
		// Args:
		//   a_solidEnabled: Whether to update the solid-height preview.
		//   a_waterEnabled: Whether to update the water-height preview.
		// Returns: None.
		void UpdateDebug(bool a_solidEnabled, bool a_waterEnabled);
		// Summary: Releases compiled shaders and cached input layouts for recompilation.
		// Args: None.
		// Returns: None.
		void ClearShaderCache();
		// Summary: Invalidates snapshots immediately and schedules render-owned state reset.
		// Args: None.
		// Returns: None.
		void Reset() noexcept;
		// Summary: Requests solid and water recapture during the next eligible frame.
		// Args: None.
		// Returns: None.
		void RequestRecapture() noexcept { recaptureRequested = true; }

		[[nodiscard]] bool Ready() const noexcept { return ready; }
		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return heightSRV.get(); }
		[[nodiscard]] ID3D11ShaderResourceView* DebugSRV() const noexcept { return debugSRV.get(); }
		[[nodiscard]] bool WaterReady() const noexcept { return waterReady; }
		[[nodiscard]] ID3D11ShaderResourceView* WaterSRV() const noexcept { return waterHeightSRV.get(); }
		[[nodiscard]] ID3D11ShaderResourceView* WaterDebugSRV() const noexcept { return waterDebugSRV.get(); }
		[[nodiscard]] float2 Center() const noexcept { return center; }
		[[nodiscard]] float Extent() const noexcept { return halfExtent * 2.0F; }
		[[nodiscard]] float HalfExtent() const noexcept { return halfExtent; }
		[[nodiscard]] float TexelSize() const noexcept { return Extent() / static_cast<float>(dimension); }
		[[nodiscard]] float2 WaterCenter() const noexcept { return waterCenter; }
		[[nodiscard]] float WaterHalfExtent() const noexcept { return waterHalfExtent; }
		[[nodiscard]] float WaterTexelSize() const noexcept
		{
			return waterHalfExtent * 2.0F / static_cast<float>(kWaterDimension);
		}
		[[nodiscard]] float TopZ() const noexcept { return topZ; }
		[[nodiscard]] std::uint32_t Dimension() const noexcept { return dimension; }
		[[nodiscard]] std::uint32_t WaterDimension() const noexcept { return kWaterDimension; }
		[[nodiscard]] std::uint32_t CapturedTerrainCount() const noexcept { return capturedTerrainCount; }
		[[nodiscard]] std::uint32_t CapturedStaticCount() const noexcept { return capturedStaticCount; }
		[[nodiscard]] std::uint32_t CapturedWaterSurfaceCount() const noexcept { return capturedWaterSurfaceCount; }
		[[nodiscard]] float CapturedWaterMinimumHeight() const noexcept { return capturedWaterMinimumHeight; }
		[[nodiscard]] float CapturedWaterMaximumHeight() const noexcept { return capturedWaterMaximumHeight; }
		[[nodiscard]] bool RecaptureRequested() const noexcept { return recaptureRequested; }

	private:
		OrthographicHeightField() = default;

		struct alignas(16) CaptureCB
		{
			float4 worldRow0{};
			float4 worldRow1{};
			float4 worldRow2{};
			float2 center{};
			float halfExtent{};
			float captureTopZ{};
			float captureBottomZ{};
			float3 pad{};
		};
		STATIC_ASSERT_ALIGNAS_16(CaptureCB);
		static_assert(sizeof(CaptureCB) == 80);

		struct alignas(16) DebugCB
		{
			float bottomZ{};
			float topZ{};
			float missingHeight{};
			float waterMode{};
		};
		STATIC_ASSERT_ALIGNAS_16(DebugCB);
		static_assert(sizeof(DebugCB) == 16);

		struct alignas(16) WaterSurfaceGPU
		{
			float2 minimum{};
			float2 maximum{};
			float height{};
			float3 pad{};
		};
		STATIC_ASSERT_ALIGNAS_16(WaterSurfaceGPU);
		static_assert(sizeof(WaterSurfaceGPU) == 32);

		struct WaterSystemSnapshot
		{
			std::uint64_t generation{};
			std::uint64_t revision{};
			std::uint32_t worldspaceKey{};
			std::uint32_t cellKey{};
			bool interior{};
			std::vector<WaterSurfaceGPU> surfaces;
		};

		void EnsureShaders();
		void ApplyPendingReset() noexcept;
		void QueueWaterSystemSnapshot();
		void BuildWaterSystemSnapshot(std::uint64_t a_generation);
		ID3D11InputLayout* GetInputLayout(const RE::BSGraphics::VertexDesc& a_desc);
		bool DrawGeometry(
			RE::BSGeometry* a_geometry,
			float2 a_center,
			float a_halfExtent,
			float a_topZ,
			float a_bottomZ);

		std::unique_ptr<ConstantBuffer> captureCB;
		std::unique_ptr<ConstantBuffer> debugCB;
		winrt::com_ptr<ID3D11Texture2D> heightTexture;
		winrt::com_ptr<ID3D11RenderTargetView> heightRTV;
		winrt::com_ptr<ID3D11ShaderResourceView> heightSRV;
		winrt::com_ptr<ID3D11Texture2D> debugTexture;
		winrt::com_ptr<ID3D11UnorderedAccessView> debugUAV;
		winrt::com_ptr<ID3D11ShaderResourceView> debugSRV;
		winrt::com_ptr<ID3D11Texture2D> waterHeightTexture;
		winrt::com_ptr<ID3D11RenderTargetView> waterHeightRTV;
		winrt::com_ptr<ID3D11ShaderResourceView> waterHeightSRV;
		winrt::com_ptr<ID3D11Texture2D> waterTerrainDepthTexture;
		winrt::com_ptr<ID3D11DepthStencilView> waterTerrainDepthDSV;
		winrt::com_ptr<ID3D11Texture2D> waterDebugTexture;
		winrt::com_ptr<ID3D11UnorderedAccessView> waterDebugUAV;
		winrt::com_ptr<ID3D11ShaderResourceView> waterDebugSRV;
		winrt::com_ptr<ID3D11VertexShader> captureVS;
		winrt::com_ptr<ID3D11PixelShader> capturePS;
		winrt::com_ptr<ID3D11VertexShader> waterTerrainVS;
		winrt::com_ptr<ID3D11VertexShader> waterCaptureVS;
		winrt::com_ptr<ID3D11PixelShader> waterCapturePS;
		winrt::com_ptr<ID3D11ComputeShader> debugCS;
		winrt::com_ptr<ID3DBlob> captureVSBlob;
		winrt::com_ptr<ID3D11Buffer> waterSurfaceBuffer;
		winrt::com_ptr<ID3D11ShaderResourceView> waterSurfaceSRV;
		winrt::com_ptr<ID3D11BlendState> maxBlendState;
		winrt::com_ptr<ID3D11DepthStencilState> waterDepthState;
		winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
		std::unordered_map<std::uint64_t, winrt::com_ptr<ID3D11InputLayout>> inputLayouts;
		std::vector<RE::NiPointer<RE::BSGeometry>> solidCandidateScratch;
		ankerl::unordered_dense::set<RE::BSGeometry*> solidSeenScratch;
		std::vector<RE::NiPointer<RE::BSGeometry>> waterTerrainCandidateScratch;
		ankerl::unordered_dense::set<RE::BSGeometry*> waterTerrainSeenScratch;
		std::vector<WaterSurfaceGPU> waterCandidateScratch;

		float2 center{};
		float halfExtent{ kDefaultSolidDimension * kSolidWorldUnitsPerTexel * 0.5F };
		float2 waterCenter{};
		float waterHalfExtent{ kExteriorHalfExtent };
		std::uint32_t dimension{ kDefaultSolidDimension };
		float topZ{};
		float bottomZ{};
		std::uint32_t capturedGeometryCount{};
		std::uint32_t capturedTerrainCount{};
		std::uint32_t capturedStaticCount{};
		std::uint32_t capturedWaterSurfaceCount{};
		float capturedWaterMinimumHeight{};
		float capturedWaterMaximumHeight{};
		std::uint32_t lastWorldspaceKey{};
		std::uint32_t lastCellKey{};
		std::uint32_t lastSolidCaptureFrame{ static_cast<std::uint32_t>(-1) };
		Util::FrameChecker captureFrame;
		bool lastInterior{};
		bool hasCaptureAnchor{};
		bool recaptureRequested{};
		bool worldRootUnavailableLogged{};
		bool ready{};
		bool waterReady{};
		std::atomic_bool resetPending{};
		std::atomic<std::shared_ptr<const WaterSystemSnapshot>> waterSystemSnapshot{};
		std::atomic<std::uint64_t> waterSnapshotGeneration{};
		std::atomic<std::uint64_t> waterSnapshotSequence{};
		std::atomic<std::uint64_t> waterSnapshotTaskToken{};
		std::uint64_t lastWaterSnapshotRevision{};
	};
}
