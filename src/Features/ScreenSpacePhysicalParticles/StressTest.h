#pragma once

#include <cstdint>
#include <string_view>

namespace ScreenSpacePhysicalParticles::Core
{
	class ParticleRuntime;
}

// Drives a bounded one-minute load test on the render thread and writes runtime,
// diagnostics, and profiler summaries to SSPPStressTest.log.
namespace ScreenSpacePhysicalParticles::StressTest
{
	// Summary: Starts a test, or stops and logs the active test as a partial run.
	// Args:
	//   a_runtime: Shared runtime used to inject and inspect test particles.
	// Returns: None.
	void Toggle(Core::ParticleRuntime& a_runtime);

	// Summary: Requests termination before a game-load reset clears the runtime.
	// Args: None.
	// Returns: None.
	void RequestStopForGameReset() noexcept;

	// Summary: Advances active test time and queues capacity-scaled particle bursts.
	// Args:
	//   a_runtime: Runtime that receives debug burst requests.
	//   a_deltaTime: Unpaused game-frame duration in seconds.
	// Returns: None.
	void UpdateBeforeFrame(Core::ParticleRuntime& a_runtime, float a_deltaTime);

	// Summary: Samples completed frame data and finalizes an expired test.
	// Args:
	//   a_runtime: Runtime supplying asynchronous diagnostics.
	// Returns: None.
	void UpdateAfterFrame(const Core::ParticleRuntime& a_runtime);

	// Summary: Reports whether a stress-test run is active.
	// Args: None.
	// Returns: True while a stress-test run is active.
	[[nodiscard]] bool Running() noexcept;
	// Summary: Reports whether an active test cannot advance this frame.
	// Args:
	//   a_runtime: Runtime whose readiness is tested.
	// Returns: True when test time and injection are currently suspended.
	[[nodiscard]] bool Waiting(const Core::ParticleRuntime& a_runtime) noexcept;
	// Summary: Returns the configured stress-test duration.
	// Args: None.
	// Returns: Duration in seconds.
	[[nodiscard]] float DurationSeconds() noexcept;
	// Summary: Returns active elapsed time for the current run.
	// Args: None.
	// Returns: Active, unpaused seconds.
	[[nodiscard]] float ElapsedSeconds() noexcept;
	// Summary: Returns runtime frames sampled in the current run.
	// Args: None.
	// Returns: Runtime frame count.
	[[nodiscard]] std::uint64_t RuntimeFrames() noexcept;
	// Summary: Returns completed profiler frames sampled in the current run.
	// Args: None.
	// Returns: Profiler sample count.
	[[nodiscard]] std::uint64_t ProfilerSamples() noexcept;
	// Summary: Returns the latest completed or stopped run summary.
	// Args: None.
	// Returns: Process-lifetime view of the result text.
	[[nodiscard]] std::string_view LastResult() noexcept;
}
