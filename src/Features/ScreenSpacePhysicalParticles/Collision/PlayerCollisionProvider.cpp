#include "PlayerCollisionProvider.h"

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/VersionedRelocation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

// Implements a single-producer/single-consumer-style snapshot exchange over three
// atomic slots. The game thread publishes capsule POD after actor update; the render
// thread consumes only the newest snapshot from the current demand generation.
namespace ScreenSpacePhysicalParticles
{
	namespace
	{
		// Summary: Rotates a point about the local Z axis.
		// Args:
		//   a_point: Point to rotate.
		//   a_angle: Rotation in radians.
		// Returns: The rotated point.
		RE::NiPoint3 RotateAroundZ(const RE::NiPoint3& a_point, float a_angle) noexcept
		{
			const auto sine = std::sin(a_angle);
			const auto cosine = std::cos(a_angle);
			return {
				a_point.x * cosine - a_point.y * sine,
				a_point.x * sine + a_point.y * cosine,
				a_point.z
			};
		}
	}

	PlayerCollisionProvider& PlayerCollisionProvider::GetSingleton() noexcept
	{
		static PlayerCollisionProvider singleton;
		return singleton;
	}

	void PlayerCollisionProvider::SetupResources()
	{
		if (capsuleBuffer || !globals::d3d::device || !globals::d3d::context)
			return;

		const auto desc = StructuredBufferDesc<CapsuleGPU>(kMaximumVolumes, false, true);
		capsuleBuffer = std::make_unique<Buffer>(
			desc, nullptr, "SSPP::PlayerCollisionCapsules");

		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srv.Buffer.NumElements = kMaximumVolumes;
		capsuleBuffer->CreateSRV(srv);
	}

	// Summary: Captures player state after Skyrim completes its main update.
	// Args: None.
	// Returns: None.
	void PlayerCollisionProvider::MainUpdateCaptureHook::thunk()
	{
		func();
		GetSingleton().CaptureGameThreadSnapshot();
	}

	void PlayerCollisionProvider::InstallGameUpdateHook()
	{
		if (hookInstalled)
			return;

		// The thunk chains through any hook already installed at this call site.
		stl::write_thunk_call<MainUpdateCaptureHook>(
			REL::RelocationID(35565, 36564).address() +
			Util::VersionedRelocation::Select(0x748, 0xC26, 0xC38));
		hookInstalled = true;
		logger::info("[SSPP] Installed game-thread player-collision snapshot hook");
	}

	void PlayerCollisionProvider::PrepareRenderFrame(bool a_required)
	{
		ApplyPendingReset();
		if (!a_required) {
			if (samplingRequired.exchange(false, std::memory_order_acq_rel)) {
				capsules.fill({});
				velocity = {};
				count = 0;
			}
			return;
		}

		if (!samplingRequired.load(std::memory_order_acquire)) {
			// Reject snapshots still completing from an earlier demand interval.
			samplingGeneration.fetch_add(1U, std::memory_order_acq_rel);
			samplingRequired.store(true, std::memory_order_release);
			capsules.fill({});
			velocity = {};
			count = 0;
		}

		SetupResources();
		Snapshot snapshot;
		if (ConsumeLatestSnapshot(snapshot))
			UploadSnapshot(snapshot);
	}

	// Summary: Samples a conservative player capsule and velocity through public actor APIs.
	// Args: None.
	// Returns: None.
	void PlayerCollisionProvider::CaptureGameThreadSnapshot()
	{
		if (!samplingRequired.load(std::memory_order_acquire))
			return;

		Snapshot snapshot;
		snapshot.generation = samplingGeneration.load(std::memory_order_acquire);
		snapshot.sequence = ++nextSequence;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			PublishGameThreadSnapshot(std::move(snapshot));
			return;
		}

		// Summary: Validates and appends one capsule to the pending snapshot.
		// Args:
		//   a_pointA: First world-space capsule endpoint.
		//   a_pointB: Second world-space capsule endpoint.
		//   a_radius: Capsule radius in world units.
		// Returns: True when the capsule was appended.
		const auto appendVolume = [&](const RE::NiPoint3& a_pointA,
									  const RE::NiPoint3& a_pointB,
									  float a_radius) {
			if (snapshot.count >= kMaximumVolumes ||
				!std::isfinite(a_pointA.x) || !std::isfinite(a_pointA.y) || !std::isfinite(a_pointA.z) ||
				!std::isfinite(a_pointB.x) || !std::isfinite(a_pointB.y) || !std::isfinite(a_pointB.z) ||
				!std::isfinite(a_radius) || a_radius <= 0.0F || a_radius > 256.0F) {
				return false;
			}
			auto& output = snapshot.capsules[snapshot.count++];
			output.pointA = { a_pointA.x, a_pointA.y, a_pointA.z };
			output.pointB = { a_pointB.x, a_pointB.y, a_pointB.z };
			output.radius = a_radius;
			return true;
		};

		// Avoid Havok-owned controller and shape graphs; actor bounds remain valid here.
		const auto localMin = player->GetBoundMin();
		const auto localMax = player->GetBoundMax();
		const auto actorScale = std::abs(player->GetBaseHeight());
		const auto localCenter = (localMin + localMax) * (0.5F * actorScale);
		const auto worldCenter =
			player->GetPosition() + RotateAroundZ(localCenter, -player->GetAngleZ());
		const auto widthX = std::abs(localMax.x - localMin.x) * actorScale;
		const auto widthY = std::abs(localMax.y - localMin.y) * actorScale;
		const auto height = std::abs(localMax.z - localMin.z) * actorScale;
		const auto radius = 0.5F * std::max(widthX, widthY);
		const auto halfSegment = std::max(0.5F * height - radius, 0.0F);
		appendVolume(
			worldCenter + RE::NiPoint3{ 0.0F, 0.0F, -halfSegment },
			worldCenter + RE::NiPoint3{ 0.0F, 0.0F, halfSegment },
			radius);

		RE::NiPoint3 playerVelocity{};
		player->GetLinearVelocity(playerVelocity);
		if (std::isfinite(playerVelocity.x) &&
			std::isfinite(playerVelocity.y) &&
			std::isfinite(playerVelocity.z)) {
			snapshot.velocity = {
				playerVelocity.x,
				playerVelocity.y,
				playerVelocity.z
			};
		}

		// Discard a sample invalidated while engine state was being read.
		if (!samplingRequired.load(std::memory_order_acquire) ||
			snapshot.generation != samplingGeneration.load(std::memory_order_acquire)) {
			return;
		}
		PublishGameThreadSnapshot(std::move(snapshot));
	}

	// Summary: Publishes a snapshot into the first empty hand-off slot without blocking.
	// Args:
	//   a_snapshot: Completed game-thread snapshot to transfer.
	// Returns: None.
	void PlayerCollisionProvider::PublishGameThreadSnapshot(Snapshot&& a_snapshot) noexcept
	{
		for (auto& slot : snapshotSlots) {
			auto expected = SlotState::kEmpty;
			if (!slot.state.compare_exchange_strong(
					expected, SlotState::kWriting, std::memory_order_acq_rel)) {
				continue;
			}
			slot.snapshot = std::move(a_snapshot);
			slot.state.store(SlotState::kReady, std::memory_order_release);
			return;
		}
		// Drop when the consumer is behind; the next game update publishes fresher data.
	}

	// Summary: Consumes all ready slots and retains the newest snapshot in the current generation.
	// Args:
	//   a_snapshot: Receives the newest valid snapshot.
	// Returns: True when a valid snapshot was consumed.
	bool PlayerCollisionProvider::ConsumeLatestSnapshot(Snapshot& a_snapshot) noexcept
	{
		const auto expectedGeneration = samplingGeneration.load(std::memory_order_acquire);
		bool found{};
		for (auto& slot : snapshotSlots) {
			auto expected = SlotState::kReady;
			if (!slot.state.compare_exchange_strong(
					expected, SlotState::kReading, std::memory_order_acq_rel)) {
				continue;
			}

			if (slot.snapshot.generation == expectedGeneration &&
				(!found || slot.snapshot.sequence > a_snapshot.sequence)) {
				a_snapshot = slot.snapshot;
				found = true;
			}
			slot.state.store(SlotState::kEmpty, std::memory_order_release);
		}
		return found;
	}

	// Summary: Copies a consumed snapshot into CPU accessors and the GPU structured buffer.
	// Args:
	//   a_snapshot: Snapshot to expose to the current render frame.
	// Returns: None.
	void PlayerCollisionProvider::UploadSnapshot(const Snapshot& a_snapshot)
	{
		capsules = a_snapshot.capsules;
		velocity = a_snapshot.velocity;
		count = std::min(a_snapshot.count, kMaximumVolumes);

		if (capsuleBuffer) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			DX::ThrowIfFailed(globals::d3d::context->Map(
				capsuleBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			std::memcpy(mapped.pData, capsules.data(), sizeof(capsules));
			globals::d3d::context->Unmap(capsuleBuffer->resource.get(), 0);
		}

	}

	void PlayerCollisionProvider::Reset() noexcept
	{
		// Invalidate producers here; PrepareRenderFrame() owns render-state reset.
		samplingRequired.store(false, std::memory_order_release);
		samplingGeneration.fetch_add(1U, std::memory_order_acq_rel);
		resetPending.store(true, std::memory_order_release);
	}

	// Summary: Discards published snapshots and clears render-thread collision state.
	// Args: None.
	// Returns: None.
	void PlayerCollisionProvider::ApplyPendingReset() noexcept
	{
		if (!resetPending.exchange(false, std::memory_order_acq_rel))
			return;
		for (auto& slot : snapshotSlots) {
			// A writing slot remains owned by its producer and is stale by generation.
			auto expected = SlotState::kReady;
			(void)slot.state.compare_exchange_strong(
				expected, SlotState::kEmpty, std::memory_order_acq_rel);
		}
		capsules.fill({});
		velocity = {};
		count = 0;
	}
}
