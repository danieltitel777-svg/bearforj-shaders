#pragma once

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <winrt/base.h>

namespace ScreenSpacePhysicalParticles::Utils
{
	struct GeometryOwnership;
}

namespace RE
{
	class BSGeometry;
	class BSRenderPass;
}

// Captures actor-free camera depth for SSPP collision queries. Eligible terrain
// and static passes are replayed into a private depth target during Skyrim's
// depth and forward-world phases, then finalized before particle simulation.
class ScreenSpaceCollisionDepth final
{
public:
	// Summary: Returns the process-wide perspective-depth provider.
	// Args: None.
	// Returns: The shared provider instance.
	static ScreenSpaceCollisionDepth& GetSingleton() noexcept;

	// Summary: Creates a depth target compatible with Skyrim's current main-depth target.
	// Args: None.
	// Returns: None.
	void SetupResources();
	// Summary: Installs the depth-phase and immediate-render hooks used by capture replay.
	// Args: None.
	// Returns: None.
	void InstallHooks();
	// Summary: Requests one capture; repeated requests before the next capture coalesce.
	// Args: None.
	// Returns: None.
	void RequestCapture() noexcept;
	// Summary: Starts a requested capture at the beginning of Skyrim's depth pass.
	// Args: None.
	// Returns: None.
	void BeginCapture();
	// Summary: Advances capture from the depth prepass to forward static passes.
	// Args: None.
	// Returns: None.
	void FinishDepthPrepass() noexcept;
	// Summary: Replays an eligible game render pass into the actor-free depth target.
	// Args:
	//   a_pass: The completed Skyrim render pass to consider.
	//   a_technique: The technique used by the original draw.
	//   a_alphaTest: Whether the original draw used alpha testing.
	//   a_renderFlags: Flags supplied to Skyrim's immediate renderer.
	// Returns: None.
	void CapturePassImmediately(
		RE::BSRenderPass* a_pass,
		std::uint32_t a_technique,
		bool a_alphaTest,
		std::uint32_t a_renderFlags);
	// Summary: Finalizes capture readiness for the current simulation frame.
	// Args: None.
	// Returns: None.
	void EndCapture();
	// Summary: Rebinds SSPP's depth target after Skyrim mutates replay render state.
	// Args: None.
	// Returns: None.
	void RestoreReplayState();
	// Summary: Converts captured depth into the diagnostic preview texture.
	// Args:
	//   a_enabled: Whether the preview should be updated.
	// Returns: None.
	void UpdateDebug(bool a_enabled);
	// Summary: Releases the debug shader for recompilation.
	// Args: None.
	// Returns: None.
	void ClearShaderCache();
	// Summary: Cancels pending work and schedules render-owned state reset.
	// Args: None.
	// Returns: None.
	void Reset() noexcept;

	[[nodiscard]] bool Ready() const noexcept { return ready; }
	[[nodiscard]] bool ReadyForFrame(std::uint32_t a_frame) const noexcept
	{
		return ready && lastCompletedFrame == a_frame;
	}
	[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return depthSRV.get(); }
	[[nodiscard]] ID3D11ShaderResourceView* DebugSRV() const noexcept { return debugSRV.get(); }
	[[nodiscard]] std::uint32_t TerrainPasses() const noexcept { return terrainPasses; }
	[[nodiscard]] std::uint32_t StaticPasses() const noexcept { return staticPasses; }

private:
	ScreenSpaceCollisionDepth() = default;

	struct MainRenderDepthHook
	{
		static void thunk(bool a_first, bool a_second);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RenderPassImmediatelyHook
	{
		static void thunk(
			RE::BSRenderPass* a_pass,
			std::uint32_t a_technique,
			bool a_alphaTest,
			std::uint32_t a_renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	enum class CapturePhase : std::uint8_t
	{
		kIdle,
		kDepthPrepass,
		kForwardStatic
	};

	[[nodiscard]] static bool IsStaticDepthOccluder(
		const RE::BSRenderPass& a_pass,
		const ScreenSpacePhysicalParticles::Utils::GeometryOwnership& a_ownership);
	void ApplyPendingReset() noexcept;
	void EnsureDebugResources();

	std::atomic_bool captureRequested{};
	std::atomic_bool resetPending{};
	bool captureActive{};
	bool replayActive{};
	CapturePhase capturePhase{ CapturePhase::kIdle };
	bool ready{};
	std::uint32_t lastCompletedFrame{ static_cast<std::uint32_t>(-1) };
	std::uint32_t terrainPasses{};
	std::uint32_t staticPasses{};
	ankerl::unordered_dense::set<RE::BSGeometry*> capturedGeometry;

	winrt::com_ptr<ID3D11Texture2D> depthTexture;
	winrt::com_ptr<ID3D11Texture2D> sourceDepthTexture;
	winrt::com_ptr<ID3D11DepthStencilView> depthDSV;
	winrt::com_ptr<ID3D11ShaderResourceView> depthSRV;
	winrt::com_ptr<ID3D11DepthStencilState> captureDepthState;

	winrt::com_ptr<ID3D11Texture2D> debugTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> debugSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> debugUAV;
	winrt::com_ptr<ID3D11ComputeShader> debugCS;
};
