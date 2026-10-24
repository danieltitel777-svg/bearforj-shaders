#include "StressTest.h"

#include "Features/ScreenSpacePhysicalParticles/Core/ParticleRuntime.h"
#include "Globals.h"
#include "Profiler.h"
#include "Utils/Game.h"
#include "Utils/PerfUtils.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>

// Implements a render-thread benchmark that maintains a bounded particle load,
// samples each completed asynchronous diagnostics/profiler frame once, and emits
// aggregate and per-route results when the run ends.
namespace
{
	constexpr float kStressTestDurationSeconds = 60.0F;
	constexpr float kDebugParticlesPerBurstCall = 96.0F;

	using DiagnosticsSnapshot =
		ScreenSpacePhysicalParticles::Core::DiagnosticsSnapshot;

	struct StressTimingSeries
	{
		std::vector<float> gpu;
		std::vector<float> cpu;
	};

	struct StressTestState
	{
		bool running{};
		bool activeThisFrame{};
		bool finishAfterFrame{};
		float elapsedSeconds{};
		float emissionCredit{};
		std::uint64_t runtimeFrames{};
		std::uint64_t profilerSamples{};
		std::uint64_t diagnosticsSamples{};
		std::uint64_t lastProfilerSerial{};
		std::uint64_t lastDiagnosticsFrame{ std::numeric_limits<std::uint64_t>::max() };
		std::uint64_t burstCallsQueued{};
		std::uint64_t burstCallsRejected{};
		std::uint64_t framesWithoutPlayer{};
		std::vector<float> frameTimesMs;
		std::vector<float> totalTrackedGpuMs;
		std::vector<float> totalCoreGpuMs;
		std::vector<float> totalRuntimeGpuMs;
		std::vector<float> totalDebugGpuMs;
		std::vector<float> totalBloodGpuMs;
		std::vector<float> totalLeavesGpuMs;
		std::unordered_map<std::string, StressTimingSeries> timings;

		DiagnosticsSnapshot startDiagnostics{};
		DiagnosticsSnapshot latestDiagnostics{};
		bool hasStartDiagnostics{};
		bool hasLatestDiagnostics{};
		std::uint32_t peakAllocated{};
		std::uint32_t peakDynamic{};
		std::uint32_t peakPersistent{};
		std::uint32_t peakVisible{};
		std::uint32_t peakEmitters{};
		std::uint32_t peakOwnedParticles{};
		std::uint32_t peakClassifierCulled{};
		std::uint32_t peakRenderCandidates{};
		std::uint32_t peakFrustumCulled{};
		std::uint32_t peakDistanceCulled{};
		std::uint32_t peakDisabledRenderers{};
		std::uint32_t peakInvalidRenderClasses{};
		std::uint32_t peakRenderStaleReferences{};
		std::uint32_t peakRenderBinOverflows{};
		std::uint64_t sumClassifierCulled{};
		std::uint64_t sumRenderCandidates{};
		std::uint64_t sumFrustumCulled{};
		std::uint64_t sumDistanceCulled{};
		std::uint64_t sumDisabledRenderers{};
		std::uint64_t sumInvalidRenderClasses{};
		std::uint64_t sumRenderStaleReferences{};
		std::uint64_t sumRenderBinOverflows{};
		std::array<std::uint32_t, ScreenSpacePhysicalParticles::Core::ParticleRuntime::kSimulationClassCount>
			peakBySimulationClass{};
		std::array<std::uint32_t, ScreenSpacePhysicalParticles::Core::ParticleRuntime::kPersistentListCount>
			peakByPersistentList{};
		std::array<std::uint32_t, ScreenSpacePhysicalParticles::Core::ParticleRuntime::kRenderClassCount>
			peakVisibleByRenderClass{};

		std::string lastResult{ "No stress test has run in this session." };
	};

	struct StressSeriesSummary
	{
		float current{};
		float mean{};
		float p50{};
		float p95{};
		float p99{};
		float maximum{};
	};

	struct StressLog
	{
		std::shared_ptr<spdlog::logger> dedicated;
		std::string path{ "CommunityShaders.log" };
	};

	StressTestState stressTest;
	std::atomic_bool gameResetStopPending{};

	// Summary: Lazily creates the dedicated stress-test logger.
	// Args: None.
	// Returns: Process-lifetime logger state, falling back to CommunityShaders.log.
	StressLog& GetStressLog()
	{
		static StressLog log = []() {
			StressLog result{};
			try {
				auto path = logger::log_directory();
				if (!path)
					return result;
				*path /= "SSPPStressTest.log";
				auto sink =
					std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
				result.dedicated =
					std::make_shared<spdlog::logger>("SSPP stress diagnostics", std::move(sink));
				result.dedicated->set_level(spdlog::level::info);
				result.dedicated->flush_on(spdlog::level::info);
				result.dedicated->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
				result.path = path->string();
			} catch (const std::exception& e) {
				logger::error("[SSPP Stress] Failed to create SSPPStressTest.log: {}", e.what());
			}
			return result;
		}();
		return log;
	}

	// Summary: Writes one stress-test record to the main and dedicated logs.
	// Args:
	//   a_line: Unprefixed record text.
	// Returns: None.
	void StressLogLine(std::string_view a_line)
	{
		logger::info("[SSPP Stress] {}", a_line);
		if (const auto& dedicated = GetStressLog().dedicated)
			dedicated->info("[SSPP Stress] {}", a_line);
	}

	// Summary: Tests whether a timer belongs to SSPP or a particle feature.
	// Args:
	//   a_name: Profiler timer name.
	// Returns: True when the timer contributes to tracked GPU cost.
	bool IsTrackedStressTimer(std::string_view a_name)
	{
		return a_name.starts_with("SSPP::") || a_name.starts_with("PhysicalBlood::") ||
		       a_name.starts_with("PhysicalLeaves::");
	}

	// Summary: Tests whether an SSPP timer measures optional diagnostics rendering.
	// Args:
	//   a_name: Profiler timer name.
	// Returns: True for stress particles or collision-buffer visualizers.
	bool IsStressDebugTimer(std::string_view a_name)
	{
		return a_name == "SSPP::DebugParticlePrepare" ||
		       a_name == "SSPP::DebugParticleRender" ||
		       a_name == "SSPP::OrthographicHeightDebug" ||
		       a_name == "SSPP::OrthographicWaterHeightDebug" ||
		       a_name == "SSPP::SceneDepthDebug";
	}

	// Summary: Computes descriptive statistics for a sample series.
	// Args:
	//   a_samples: Chronological measurements.
	// Returns: Zeroed summary for an empty series; otherwise aggregate statistics.
	StressSeriesSummary SummarizeSeries(const std::vector<float>& a_samples)
	{
		StressSeriesSummary summary{};
		if (a_samples.empty())
			return summary;

		summary.current = a_samples.back();
		summary.mean = Util::Mean(a_samples);
		summary.maximum = *std::max_element(a_samples.begin(), a_samples.end());
		auto sorted = a_samples;
		std::sort(sorted.begin(), sorted.end());
		const auto percentile = [&sorted](float a_percentile) {
			const auto position = a_percentile * static_cast<float>(sorted.size() - 1U);
			const auto lower = static_cast<std::size_t>(position);
			const auto upper = std::min(lower + 1U, sorted.size() - 1U);
			const auto fraction = position - static_cast<float>(lower);
			return sorted[lower] * (1.0F - fraction) + sorted[upper] * fraction;
		};
		summary.p50 = percentile(0.50F);
		summary.p95 = percentile(0.95F);
		summary.p99 = percentile(0.99F);
		return summary;
	}

	// Summary: Computes a monotonic counter delta while tolerating a reset.
	// Args:
	//   a_end: Most recent counter value.
	//   a_start: Counter value at test start.
	// Returns: Difference, or a_end when the counter restarted below a_start.
	std::uint64_t CounterDelta(std::uint64_t a_end, std::uint64_t a_start)
	{
		return a_end >= a_start ? a_end - a_start : a_end;
	}

	// Summary: Samples one new runtime snapshot and updates peaks and sums.
	// Args:
	//   a_runtime: Runtime supplying the latest diagnostics snapshot.
	// Returns: None.
	void ObserveStressDiagnostics(
		const ScreenSpacePhysicalParticles::Core::ParticleRuntime& a_runtime)
	{
		const auto& diagnostics = a_runtime.Diagnostics();
		if (!stressTest.running || !stressTest.activeThisFrame || !diagnostics.valid ||
			diagnostics.sampledFrame == stressTest.lastDiagnosticsFrame) {
			return;
		}

		stressTest.lastDiagnosticsFrame = diagnostics.sampledFrame;
		stressTest.latestDiagnostics = diagnostics;
		stressTest.hasLatestDiagnostics = true;
		++stressTest.diagnosticsSamples;
		stressTest.peakAllocated = std::max(stressTest.peakAllocated, diagnostics.allocatedCount);
		stressTest.peakDynamic = std::max(stressTest.peakDynamic, diagnostics.dynamicCount);
		stressTest.peakPersistent = std::max(stressTest.peakPersistent, diagnostics.persistentCount);
		stressTest.peakVisible = std::max(stressTest.peakVisible, diagnostics.visibleCount);
		stressTest.peakEmitters = std::max(stressTest.peakEmitters, diagnostics.activeEmitters);
		stressTest.peakOwnedParticles =
			std::max(stressTest.peakOwnedParticles, diagnostics.ownedParticles);
		stressTest.peakClassifierCulled =
			std::max(stressTest.peakClassifierCulled, diagnostics.culledCount);
		stressTest.peakRenderCandidates =
			std::max(stressTest.peakRenderCandidates, diagnostics.renderCandidates);
		stressTest.peakFrustumCulled =
			std::max(stressTest.peakFrustumCulled, diagnostics.frustumCulled);
		stressTest.peakDistanceCulled =
			std::max(stressTest.peakDistanceCulled, diagnostics.distanceCulled);
		stressTest.peakDisabledRenderers =
			std::max(stressTest.peakDisabledRenderers, diagnostics.disabledOrMissingRenderers);
		stressTest.peakInvalidRenderClasses =
			std::max(stressTest.peakInvalidRenderClasses, diagnostics.invalidRenderClasses);
		stressTest.peakRenderStaleReferences =
			std::max(stressTest.peakRenderStaleReferences, diagnostics.renderStaleReferences);
		stressTest.peakRenderBinOverflows =
			std::max(stressTest.peakRenderBinOverflows, diagnostics.renderBinOverflows);
		stressTest.sumClassifierCulled += diagnostics.culledCount;
		stressTest.sumRenderCandidates += diagnostics.renderCandidates;
		stressTest.sumFrustumCulled += diagnostics.frustumCulled;
		stressTest.sumDistanceCulled += diagnostics.distanceCulled;
		stressTest.sumDisabledRenderers += diagnostics.disabledOrMissingRenderers;
		stressTest.sumInvalidRenderClasses += diagnostics.invalidRenderClasses;
		stressTest.sumRenderStaleReferences += diagnostics.renderStaleReferences;
		stressTest.sumRenderBinOverflows += diagnostics.renderBinOverflows;
		for (std::size_t index = 0; index < stressTest.peakBySimulationClass.size(); ++index) {
			stressTest.peakBySimulationClass[index] = std::max(
				stressTest.peakBySimulationClass[index], diagnostics.activeBySimulationClass[index]);
		}
		for (std::size_t index = 0; index < stressTest.peakByPersistentList.size(); ++index) {
			stressTest.peakByPersistentList[index] = std::max(
				stressTest.peakByPersistentList[index], diagnostics.activeByPersistentList[index]);
		}
		for (std::size_t index = 0; index < stressTest.peakVisibleByRenderClass.size(); ++index) {
			stressTest.peakVisibleByRenderClass[index] = std::max(
				stressTest.peakVisibleByRenderClass[index], diagnostics.visibleByRenderClass[index]);
		}
	}

	// Summary: Samples one new profiler frame into per-pass and grouped series.
	// Args: None.
	// Returns: None.
	void ObserveStressTimings()
	{
		if (!stressTest.running || !stressTest.activeThisFrame || !globals::profiler)
			return;
		const auto serial = globals::profiler->GetCollectedFrameCount();
		if (serial == stressTest.lastProfilerSerial)
			return;
		stressTest.lastProfilerSerial = serial;
		++stressTest.profilerSamples;

		for (auto& [_, series] : stressTest.timings) {
			series.gpu.push_back(0.0F);
			series.cpu.push_back(0.0F);
		}

		float totalTracked{};
		float totalCore{};
		float totalRuntime{};
		float totalDebug{};
		float totalBlood{};
		float totalLeaves{};
		for (const auto& timing : globals::profiler->GetResults()) {
			if (!timing.valid || !IsTrackedStressTimer(timing.name))
				continue;
			auto [iterator, inserted] = stressTest.timings.try_emplace(timing.name);
			if (inserted) {
				iterator->second.gpu.resize(stressTest.profilerSamples, 0.0F);
				iterator->second.cpu.resize(stressTest.profilerSamples, 0.0F);
			}
			const float gpuMs = timing.sampledThisFrame ? timing.gpuTimeMs : 0.0F;
			const float cpuMs = timing.sampledThisFrame ? timing.cpuTimeMs : 0.0F;
			iterator->second.gpu.back() = gpuMs;
			iterator->second.cpu.back() = cpuMs;
			totalTracked += gpuMs;
			const std::string_view name = timing.name;
			if (name.starts_with("SSPP::")) {
				totalCore += gpuMs;
				if (IsStressDebugTimer(name))
					totalDebug += gpuMs;
				else
					totalRuntime += gpuMs;
			} else if (name.starts_with("PhysicalBlood::"))
				totalBlood += gpuMs;
			else if (name.starts_with("PhysicalLeaves::"))
				totalLeaves += gpuMs;
		}
		stressTest.totalTrackedGpuMs.push_back(totalTracked);
		stressTest.totalCoreGpuMs.push_back(totalCore);
		stressTest.totalRuntimeGpuMs.push_back(totalRuntime);
		stressTest.totalDebugGpuMs.push_back(totalDebug);
		stressTest.totalBloodGpuMs.push_back(totalBlood);
		stressTest.totalLeavesGpuMs.push_back(totalLeaves);
	}

	// Summary: Initializes a run and records its baseline diagnostics.
	// Args:
	//   a_runtime: Runtime supplying capacity and baseline counters.
	// Returns: None.
	void BeginStressTest(
		const ScreenSpacePhysicalParticles::Core::ParticleRuntime& a_runtime)
	{
		const auto previousResult = std::move(stressTest.lastResult);
		stressTest = {};
		stressTest.running = true;
		stressTest.lastResult = previousResult;
		stressTest.lastProfilerSerial =
			globals::profiler ? globals::profiler->GetCollectedFrameCount() : 0;
		const auto& diagnostics = a_runtime.Diagnostics();
		if (diagnostics.valid) {
			stressTest.startDiagnostics = diagnostics;
			stressTest.latestDiagnostics = diagnostics;
			stressTest.hasStartDiagnostics = true;
			stressTest.hasLatestDiagnostics = true;
			stressTest.lastDiagnosticsFrame = diagnostics.sampledFrame;
			stressTest.peakAllocated = diagnostics.allocatedCount;
			stressTest.peakDynamic = diagnostics.dynamicCount;
			stressTest.peakPersistent = diagnostics.persistentCount;
			stressTest.peakVisible = diagnostics.visibleCount;
			stressTest.peakEmitters = diagnostics.activeEmitters;
			stressTest.peakOwnedParticles = diagnostics.ownedParticles;
			std::copy(
				std::begin(diagnostics.activeBySimulationClass),
				std::end(diagnostics.activeBySimulationClass),
				stressTest.peakBySimulationClass.begin());
			std::copy(
				std::begin(diagnostics.activeByPersistentList),
				std::end(diagnostics.activeByPersistentList),
				stressTest.peakByPersistentList.begin());
			std::copy(
				std::begin(diagnostics.visibleByRenderClass),
				std::end(diagnostics.visibleByRenderClass),
				stressTest.peakVisibleByRenderClass.begin());
		}

		StressLogLine(std::format(
			"BEGIN duration_s={:.1f} capacity={} target_load=75% log=\"{}\"",
			kStressTestDurationSeconds,
			a_runtime.Capacity(),
			GetStressLog().path));
	}

	// Summary: Writes the principal values from one runtime diagnostics snapshot.
	// Args:
	//   a_label: Record label such as start or end.
	//   a_snapshot: Snapshot to serialize.
	// Returns: None.
	void LogStressSnapshot(std::string_view a_label, const DiagnosticsSnapshot& a_snapshot)
	{
		StressLogLine(std::format(
			"DIAGNOSTICS {} sampled_frame={} capacity={} free={} allocated={} dynamic={} persistent={} visible={} culled={} emitters={} owned={} high_water={}",
			a_label,
			a_snapshot.sampledFrame,
			a_snapshot.capacity,
			a_snapshot.freeCount,
			a_snapshot.allocatedCount,
			a_snapshot.dynamicCount,
			a_snapshot.persistentCount,
			a_snapshot.visibleCount,
			a_snapshot.culledCount,
			a_snapshot.activeEmitters,
			a_snapshot.ownedParticles,
			a_snapshot.highWaterMark));
	}

	// Summary: Stops a run and logs timings, counter deltas, and peak occupancy.
	// Args:
	//   a_runtime: Runtime supplying the final snapshot and registry metadata.
	//   a_reason: Stable completion reason written to logs and the UI summary.
	// Returns: None.
	void EndStressTest(
		const ScreenSpacePhysicalParticles::Core::ParticleRuntime& a_runtime,
		std::string_view a_reason)
	{
		if (!stressTest.running)
			return;

		ObserveStressDiagnostics(a_runtime);
		ObserveStressTimings();
		stressTest.running = false;
		stressTest.activeThisFrame = false;
		stressTest.finishAfterFrame = false;

		StressLogLine(std::format(
			"END status={} active_duration_s={:.3f} runtime_frames={} profiler_samples={} diagnostics_samples={} burst_calls_queued={} burst_calls_rejected={} requested_debug_particles={} frames_without_player={}",
			a_reason,
			stressTest.elapsedSeconds,
			stressTest.runtimeFrames,
			stressTest.profilerSamples,
			stressTest.diagnosticsSamples,
			stressTest.burstCallsQueued,
			stressTest.burstCallsRejected,
			static_cast<std::uint64_t>(
				stressTest.burstCallsQueued * static_cast<std::uint64_t>(kDebugParticlesPerBurstCall)),
			stressTest.framesWithoutPlayer));

		if (stressTest.hasStartDiagnostics)
			LogStressSnapshot("start", stressTest.startDiagnostics);
		else
			StressLogLine("DIAGNOSTICS start unavailable");
		if (stressTest.hasLatestDiagnostics)
			LogStressSnapshot("end", stressTest.latestDiagnostics);
		else
			StressLogLine("DIAGNOSTICS end unavailable");
		StressLogLine(std::format(
			"PEAKS allocated={} dynamic={} persistent={} visible={} emitters={} owned={}",
			stressTest.peakAllocated,
			stressTest.peakDynamic,
			stressTest.peakPersistent,
			stressTest.peakVisible,
			stressTest.peakEmitters,
			stressTest.peakOwnedParticles));

		const auto logSeries = [](std::string_view a_kind, const std::vector<float>& a_samples) {
			const auto summary = SummarizeSeries(a_samples);
			StressLogLine(std::format(
				"TIMING {} samples={} current_ms={:.4f} mean_ms={:.4f} p50_ms={:.4f} p95_ms={:.4f} p99_ms={:.4f} max_ms={:.4f}",
				a_kind,
				a_samples.size(),
				summary.current,
				summary.mean,
				summary.p50,
				summary.p95,
				summary.p99,
				summary.maximum));
		};
		logSeries("total_tracked_gpu", stressTest.totalTrackedGpuMs);
		logSeries("sspp_core_gpu", stressTest.totalCoreGpuMs);
		logSeries("sspp_runtime_gpu", stressTest.totalRuntimeGpuMs);
		logSeries("sspp_debug_gpu", stressTest.totalDebugGpuMs);
		logSeries("physical_blood_gpu", stressTest.totalBloodGpuMs);
		logSeries("physical_leaves_gpu", stressTest.totalLeavesGpuMs);
		const auto frameSummary = SummarizeSeries(stressTest.frameTimesMs);
		StressLogLine(std::format(
			"FRAME_TIME samples={} mean_ms={:.4f} p50_ms={:.4f} p95_ms={:.4f} p99_ms={:.4f} max_ms={:.4f} mean_fps={:.2f} tracked_gpu_percent_of_mean_frame={:.2f}",
			stressTest.frameTimesMs.size(),
			frameSummary.mean,
			frameSummary.p50,
			frameSummary.p95,
			frameSummary.p99,
			frameSummary.maximum,
			Util::CalcFPS(frameSummary.mean),
			frameSummary.mean > 0.0F ?
				100.0F * SummarizeSeries(stressTest.totalTrackedGpuMs).mean / frameSummary.mean :
				0.0F));

		std::vector<std::string> timerNames;
		timerNames.reserve(stressTest.timings.size());
		for (const auto& [name, _] : stressTest.timings)
			timerNames.push_back(name);
		std::sort(timerNames.begin(), timerNames.end());
		for (const auto& name : timerNames) {
			const auto& series = stressTest.timings.at(name);
			const auto gpu = SummarizeSeries(series.gpu);
			const auto cpu = SummarizeSeries(series.cpu);
			StressLogLine(std::format(
				"PASS name=\"{}\" samples={} gpu_current_ms={:.4f} gpu_mean_ms={:.4f} gpu_p50_ms={:.4f} gpu_p95_ms={:.4f} gpu_p99_ms={:.4f} gpu_max_ms={:.4f} cpu_mean_ms={:.4f} cpu_p95_ms={:.4f} cpu_max_ms={:.4f}",
				name,
				series.gpu.size(),
				gpu.current,
				gpu.mean,
				gpu.p50,
				gpu.p95,
				gpu.p99,
				gpu.maximum,
				cpu.mean,
				cpu.p95,
				cpu.maximum));
		}

		const DiagnosticsSnapshot empty{};
		const auto& start = stressTest.hasStartDiagnostics ? stressTest.startDiagnostics : empty;
		const auto& end = stressTest.hasLatestDiagnostics ? stressTest.latestDiagnostics : empty;
		const auto logCounter = [&](std::string_view a_name, std::uint64_t a_start, std::uint64_t a_end) {
			StressLogLine(std::format(
				"COUNTER name={} start={} end={} delta={}",
				a_name,
				a_start,
				a_end,
				CounterDelta(a_end, a_start)));
		};
		logCounter("burst_requests", start.burstRequests, end.burstRequests);
		logCounter("requested_particles", start.requestedParticles, end.requestedParticles);
		logCounter("granted_particles", start.grantedParticles, end.grantedParticles);
		logCounter("dropped_particles", start.droppedParticles, end.droppedParticles);
		logCounter("recycled_particles", start.recycledParticles, end.recycledParticles);
		logCounter("persistent_requests", start.persistentRequests, end.persistentRequests);
		logCounter(
			"persistent_particles_requested",
			start.persistentParticlesRequested,
			end.persistentParticlesRequested);
		logCounter(
			"persistent_particles_granted",
			start.persistentParticlesGranted,
			end.persistentParticlesGranted);
		logCounter(
			"persistent_particles_dropped",
			start.persistentParticlesDropped,
			end.persistentParticlesDropped);
		logCounter("free_returns", start.freeReturns, end.freeReturns);
		logCounter("maintenance_runs", start.maintenanceRuns, end.maintenanceRuns);
		logCounter("events_produced", start.eventsProduced, end.eventsProduced);
		logCounter("events_consumed", start.eventsConsumed, end.eventsConsumed);
		logCounter("stale_references", start.staleReferences, end.staleReferences);
		logCounter("duplicate_routes", start.duplicateRoutes, end.duplicateRoutes);
		logCounter("invalid_destinations", start.invalidDestinations, end.invalidDestinations);
		logCounter("quarantined_particles", start.quarantinedParticles, end.quarantinedParticles);
		logCounter("invalid_profiles", start.invalidProfiles, end.invalidProfiles);
		logCounter("list_overflows", start.listOverflows, end.listOverflows);
		logCounter("event_overflows", start.eventOverflows, end.eventOverflows);
		logCounter("invalid_collision_rules", start.invalidCollisionRules, end.invalidCollisionRules);
		logCounter("invalid_emitter_commands", start.invalidEmitterCommands, end.invalidEmitterCommands);
		logCounter("request_arena_overflows", start.requestArenaOverflows, end.requestArenaOverflows);
		logCounter("emission_chunk_overflows", start.emissionChunkOverflows, end.emissionChunkOverflows);
		logCounter("emitter_budget_clamps", start.emitterBudgetClamps, end.emitterBudgetClamps);
		logCounter(
			"owner_generation_mismatches",
			start.ownerGenerationMismatches,
			end.ownerGenerationMismatches);
		logCounter(
			"rejected_emitter_commands",
			start.rejectedEmitterCommands,
			end.rejectedEmitterCommands);
		logCounter(
			"rejected_emitter_handles",
			start.rejectedEmitterHandles,
			end.rejectedEmitterHandles);
		const auto logFrameCounter = [&](std::string_view a_name,
									  std::uint32_t a_final,
									  std::uint32_t a_peak,
									  std::uint64_t a_sum) {
			StressLogLine(std::format(
				"FRAME_COUNTER name={} final={} sampled_mean={:.2f} peak={} samples={}",
				a_name,
				a_final,
				stressTest.diagnosticsSamples != 0 ?
					static_cast<double>(a_sum) /
						static_cast<double>(stressTest.diagnosticsSamples) :
					0.0,
				a_peak,
				stressTest.diagnosticsSamples));
		};
		logFrameCounter(
			"classifier_culled",
			end.culledCount,
			stressTest.peakClassifierCulled,
			stressTest.sumClassifierCulled);
		logFrameCounter(
			"render_candidates",
			end.renderCandidates,
			stressTest.peakRenderCandidates,
			stressTest.sumRenderCandidates);
		logFrameCounter(
			"frustum_culled",
			end.frustumCulled,
			stressTest.peakFrustumCulled,
			stressTest.sumFrustumCulled);
		logFrameCounter(
			"distance_culled",
			end.distanceCulled,
			stressTest.peakDistanceCulled,
			stressTest.sumDistanceCulled);
		logFrameCounter(
			"disabled_or_missing_renderers",
			end.disabledOrMissingRenderers,
			stressTest.peakDisabledRenderers,
			stressTest.sumDisabledRenderers);
		logFrameCounter(
			"invalid_render_classes",
			end.invalidRenderClasses,
			stressTest.peakInvalidRenderClasses,
			stressTest.sumInvalidRenderClasses);
		logFrameCounter(
			"render_stale_references",
			end.renderStaleReferences,
			stressTest.peakRenderStaleReferences,
			stressTest.sumRenderStaleReferences);
		logFrameCounter(
			"render_bin_overflows",
			end.renderBinOverflows,
			stressTest.peakRenderBinOverflows,
			stressTest.sumRenderBinOverflows);

		for (std::size_t index = 0; index < std::size(end.collisionQueries); ++index) {
			StressLogLine(std::format(
				"COLLISION channel={} queries_delta={} hits_delta={}",
				index,
				CounterDelta(end.collisionQueries[index], start.collisionQueries[index]),
				CounterDelta(end.collisionHits[index], start.collisionHits[index])));
		}
		for (std::size_t index = 0; index < std::size(end.collisionResponses); ++index) {
			StressLogLine(std::format(
				"COLLISION_RESPONSE response={} delta={}",
				index,
				CounterDelta(end.collisionResponses[index], start.collisionResponses[index])));
		}
		for (std::size_t index = 0; index < stressTest.peakBySimulationClass.size(); ++index) {
			StressLogLine(std::format(
				"SIMULATION_CLASS id={} final_active={} peak_active={} transitions_delta={}",
				index,
				end.activeBySimulationClass[index],
				stressTest.peakBySimulationClass[index],
				CounterDelta(end.dynamicTransitions[index], start.dynamicTransitions[index])));
		}

		for (const auto& list : a_runtime.PersistentListInfos()) {
			if (list.id >= stressTest.peakByPersistentList.size())
				continue;
			StressLogLine(std::format(
				"PERSISTENT_LIST id={} name=\"{}\" final_active={} peak_active={} capacity={} transitions_delta={} maintenance_interval_s={:.3f}",
				list.id,
				list.name,
				end.activeByPersistentList[list.id],
				stressTest.peakByPersistentList[list.id],
				list.capacity,
				CounterDelta(
					end.persistentTransitions[list.id], start.persistentTransitions[list.id]),
				list.maintenanceIntervalSeconds));
		}
		for (const auto& route : a_runtime.EventRouteInfos()) {
			if (route.id >= std::size(end.eventsProducedByRoute))
				continue;
			StressLogLine(std::format(
				"EVENT_ROUTE id={} name=\"{}\" produced_delta={} consumed_delta={}",
				route.id,
				route.name,
				CounterDelta(
					end.eventsProducedByRoute[route.id], start.eventsProducedByRoute[route.id]),
				CounterDelta(
					end.eventsConsumedByRoute[route.id], start.eventsConsumedByRoute[route.id])));
		}
		for (const auto& renderer : a_runtime.RendererInfos()) {
			if (renderer.id >= stressTest.peakVisibleByRenderClass.size())
				continue;
			StressLogLine(std::format(
				"RENDERER id={} name=\"{}\" enabled={} ready={} final_visible={} peak_visible={}",
				renderer.id,
				renderer.name,
				renderer.enabled,
				renderer.ready,
				end.visibleByRenderClass[renderer.id],
				stressTest.peakVisibleByRenderClass[renderer.id]));
		}
		for (const auto& feature : a_runtime.FeatureInfos()) {
			if (feature.id >= std::size(end.liveByFeature))
				continue;
			StressLogLine(std::format(
				"FEATURE id={} key=\"{}\" final_live={} budget={} requested_delta={} granted_delta={} dropped_delta={} budget_clamped_delta={}",
				feature.id,
				feature.stableKey,
				end.liveByFeature[feature.id],
				feature.maximumLiveParticles,
				CounterDelta(end.requestedByFeature[feature.id], start.requestedByFeature[feature.id]),
				CounterDelta(end.grantedByFeature[feature.id], start.grantedByFeature[feature.id]),
				CounterDelta(end.droppedByFeature[feature.id], start.droppedByFeature[feature.id]),
				CounterDelta(
					end.budgetClampedByFeature[feature.id], start.budgetClampedByFeature[feature.id])));
		}

		StressLogLine(
			"NOTE total_tracked_gpu includes all SSPP::, PhysicalBlood::, and PhysicalLeaves:: timers; sspp_debug_gpu isolates the stress-test quad renderer and enabled buffer visualizers.");
		stressTest.lastResult = std::format(
			"{} after {:.1f} active seconds; {} profiler samples. Wrote {}",
			a_reason,
			stressTest.elapsedSeconds,
			stressTest.profilerSamples,
			GetStressLog().path);
	}

	// Summary: Advances an active run and queues feedback-controlled test bursts.
	// Args:
	//   a_runtime: Runtime receiving debug bursts.
	//   a_deltaTime: Unpaused frame duration in seconds.
	// Returns: None.
	void UpdateStressTestBeforeFrame(
		ScreenSpacePhysicalParticles::Core::ParticleRuntime& a_runtime,
		float a_deltaTime)
	{
		stressTest.activeThisFrame = false;
		if (!stressTest.running)
			return;

		const bool paused = !globals::game::ui || globals::game::ui->GameIsPaused() ||
		                    a_deltaTime <= 0.0F;
		if (paused || !a_runtime.Ready()) {
			if (globals::profiler)
				stressTest.lastProfilerSerial = globals::profiler->GetCollectedFrameCount();
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			++stressTest.framesWithoutPlayer;
			if (globals::profiler)
				stressTest.lastProfilerSerial = globals::profiler->GetCollectedFrameCount();
			return;
		}

		stressTest.activeThisFrame = true;
		stressTest.elapsedSeconds += a_deltaTime;
		++stressTest.runtimeFrames;
		stressTest.frameTimesMs.push_back(a_deltaTime * 1000.0F);

		const auto capacity = static_cast<float>(a_runtime.Capacity());
		float occupancy{};
		if (stressTest.hasLatestDiagnostics && stressTest.latestDiagnostics.capacity != 0) {
			occupancy = static_cast<float>(stressTest.latestDiagnostics.allocatedCount) /
			            static_cast<float>(stressTest.latestDiagnostics.capacity);
		}
		float pressureScale = 1.0F;
		if (occupancy >= 0.95F)
			pressureScale = 0.0F;
		else if (occupancy >= 0.85F)
			pressureScale = 0.35F;
		else if (occupancy < 0.60F)
			pressureScale = 1.25F;

		// Back pressure keeps occupancy near 75% while preserving lifecycle traffic.
		if (pressureScale == 0.0F) {
			stressTest.emissionCredit = 0.0F;
		} else {
			const float callsPerSecond =
				capacity * 0.75F / (kDebugParticlesPerBurstCall * 3.0F);
			stressTest.emissionCredit = std::min(
				stressTest.emissionCredit + a_deltaTime * callsPerSecond * pressureScale,
				16.0F);
		}
		const auto burstCalls = std::min(
			static_cast<std::uint32_t>(stressTest.emissionCredit), 8U);
		stressTest.emissionCredit -= static_cast<float>(burstCalls);

		const auto eye = Util::GetEyePosition();
		const auto yaw = player->GetAngleZ();
		const float3 cameraForward{ std::sin(yaw), std::cos(yaw), 0.0F };
		const float3 cameraRight{ std::cos(yaw), -std::sin(yaw), 0.0F };
		for (std::uint32_t index = 0; index < burstCalls; ++index) {
			const auto phase = static_cast<float>(stressTest.runtimeFrames) * 0.173F +
			                   static_cast<float>(index) * 2.39996F;
			const auto lateral = std::sin(phase);
			const float3 origin{
				eye.x + cameraRight.x * lateral * 48.0F,
				eye.y + cameraRight.y * lateral * 48.0F,
				eye.z + std::cos(phase * 0.73F) * 24.0F
			};
			const float3 direction{
				cameraForward.x + cameraRight.x * lateral * 0.35F,
				cameraForward.y + cameraRight.y * lateral * 0.35F,
				0.08F + 0.24F * std::abs(std::cos(phase))
			};
			if (a_runtime.QueueDebugBurst(origin, direction))
				++stressTest.burstCallsQueued;
			else
				++stressTest.burstCallsRejected;
		}

		stressTest.finishAfterFrame =
			stressTest.elapsedSeconds >= kStressTestDurationSeconds;
	}

	// Summary: Captures completed frame data and finalizes an expired run.
	// Args:
	//   a_runtime: Runtime supplying diagnostics and registry metadata.
	// Returns: None.
	void UpdateStressTestAfterFrame(
		const ScreenSpacePhysicalParticles::Core::ParticleRuntime& a_runtime)
	{
		if (!stressTest.running)
			return;
		ObserveStressDiagnostics(a_runtime);
		ObserveStressTimings();
		if (stressTest.finishAfterFrame)
			EndStressTest(a_runtime, "completed");
	}

}

namespace ScreenSpacePhysicalParticles::StressTest
{
	void Toggle(Core::ParticleRuntime& a_runtime)
	{
		if (stressTest.running)
			EndStressTest(a_runtime, "manual_stop_partial");
		else
			BeginStressTest(a_runtime);
	}

	void RequestStopForGameReset() noexcept
	{
		gameResetStopPending.store(true, std::memory_order_release);
	}

	void UpdateBeforeFrame(Core::ParticleRuntime& a_runtime, float a_deltaTime)
	{
		if (gameResetStopPending.exchange(false, std::memory_order_acq_rel) &&
			stressTest.running) {
			EndStressTest(a_runtime, "game_reset_partial");
		}
		UpdateStressTestBeforeFrame(a_runtime, a_deltaTime);
	}

	void UpdateAfterFrame(const Core::ParticleRuntime& a_runtime)
	{
		UpdateStressTestAfterFrame(a_runtime);
	}

	bool Running() noexcept
	{
		return stressTest.running;
	}

	bool Waiting(const Core::ParticleRuntime& a_runtime) noexcept
	{
		return stressTest.running &&
		       (!a_runtime.Ready() || !globals::game::ui || globals::game::ui->GameIsPaused() ||
				!RE::PlayerCharacter::GetSingleton());
	}

	float DurationSeconds() noexcept
	{
		return kStressTestDurationSeconds;
	}

	float ElapsedSeconds() noexcept
	{
		return stressTest.elapsedSeconds;
	}

	std::uint64_t RuntimeFrames() noexcept
	{
		return stressTest.runtimeFrames;
	}

	std::uint64_t ProfilerSamples() noexcept
	{
		return stressTest.profilerSamples;
	}

	std::string_view LastResult() noexcept
	{
		return stressTest.lastResult;
	}
}
