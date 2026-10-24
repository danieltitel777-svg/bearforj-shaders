#pragma once

#include "Buffer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace ScreenSpacePhysicalParticles
{
	// Publishes the player's collision capsule and velocity to SSPP shaders. The
	// game thread samples public actor state, and fixed hand-off slots transfer POD
	// snapshots to the render thread without blocking either thread.
	class PlayerCollisionProvider final
	{
	public:
		static constexpr std::uint32_t kMaximumVolumes = 1;

		struct alignas(16) CapsuleGPU
		{
			float3 pointA{};
			float radius{};
			float3 pointB{};
			float pad{};
		};
		STATIC_ASSERT_ALIGNAS_16(CapsuleGPU);
		static_assert(sizeof(CapsuleGPU) == 32);

		// Summary: Returns the process-wide player-collision provider.
		// Args: None.
		// Returns: The shared provider instance.
		static PlayerCollisionProvider& GetSingleton() noexcept;

		// Summary: Installs the post-update hook that samples player collision state.
		// Args: None.
		// Returns: None.
		void InstallGameUpdateHook();
		// Summary: Enables or disables sampling demand and uploads the newest valid snapshot.
		// Args:
		//   a_required: Whether any active simulation needs player collision.
		// Returns: None.
		void PrepareRenderFrame(bool a_required);
		// Summary: Creates the structured capsule buffer and its shader-resource view.
		// Args: None.
		// Returns: None.
		void SetupResources();
		// Summary: Invalidates in-flight snapshots and schedules render-thread state reset.
		// Args: None.
		// Returns: None.
		void Reset() noexcept;

		[[nodiscard]] bool Ready() const noexcept
		{
			return capsuleBuffer && capsuleBuffer->srv;
		}
		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept
		{
			return capsuleBuffer ? capsuleBuffer->srv.get() : nullptr;
		}
		[[nodiscard]] std::uint32_t Count() const noexcept { return count; }
		[[nodiscard]] float3 Velocity() const noexcept { return velocity; }
		[[nodiscard]] std::span<const CapsuleGPU> Capsules() const noexcept
		{
			return { capsules.data(), count };
		}

	private:
		enum class SlotState : std::uint8_t
		{
			kEmpty,
			kWriting,
			kReady,
			kReading
		};

		struct Snapshot
		{
			std::array<CapsuleGPU, kMaximumVolumes> capsules{};
			float3 velocity{};
			std::uint32_t count{};
			std::uint32_t generation{};
			std::uint64_t sequence{};
		};

		struct SnapshotSlot
		{
			std::atomic<SlotState> state{ SlotState::kEmpty };
			Snapshot snapshot{};
		};

		struct MainUpdateCaptureHook
		{
			static void thunk();
			static inline REL::Relocation<decltype(thunk)> func;
		};

		PlayerCollisionProvider() = default;
		void ApplyPendingReset() noexcept;
		void CaptureGameThreadSnapshot();
		void PublishGameThreadSnapshot(Snapshot&& a_snapshot) noexcept;
		[[nodiscard]] bool ConsumeLatestSnapshot(Snapshot& a_snapshot) noexcept;
		void UploadSnapshot(const Snapshot& a_snapshot);

		std::array<CapsuleGPU, kMaximumVolumes> capsules{};
		std::unique_ptr<Buffer> capsuleBuffer;
		float3 velocity{};
		std::uint32_t count{};
		std::array<SnapshotSlot, 3> snapshotSlots{};
		std::atomic_bool resetPending{};
		std::atomic<bool> samplingRequired{};
		std::atomic<std::uint32_t> samplingGeneration{};
		std::uint64_t nextSequence{};  // game-thread only
		bool hookInstalled{};
	};
}
