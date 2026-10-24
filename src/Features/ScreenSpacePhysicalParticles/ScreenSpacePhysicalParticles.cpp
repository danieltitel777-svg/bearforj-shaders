#include "ScreenSpacePhysicalParticles.h"

#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/ScreenSpacePhysicalParticles/Collision/PlayerCollisionProvider.h"
#include "Features/ScreenSpacePhysicalParticles/Core/ParticleRuntime.h"
#include "Features/ScreenSpacePhysicalParticles/OrthographicHeightField.h"
#include "Features/ScreenSpacePhysicalParticles/ScreenSpaceCollisionDepth.h"
#include "Features/ScreenSpacePhysicalParticles/StressTest.h"
#include "Globals.h"
#include "Profiler.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <d3d11_1.h>
#include <format>
#include <memory>
#include <string_view>

// Implements the SSPP feature lifecycle and diagnostics UI. Rendering work is
// isolated from Skyrim's D3D11 state, then delegated to collision providers and
// the shared ParticleRuntime in a fixed per-frame order.
namespace
{
	constexpr std::string_view kUnavailableValue = "--";

	struct ContextStateCache
	{
		ID3D11Device* device{};
		ID3D11DeviceContext* context{};
		winrt::com_ptr<ID3D11DeviceContext1> context1;
		winrt::com_ptr<ID3DDeviceContextState> state;

		// Summary: Creates or reuses D3D11.1 state for a device/context pair.
		// Args:
		//   a_device: Active D3D11 device.
		//   a_context: Active immediate context.
		// Returns: True when context-state swapping is available.
		bool Ensure(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
		{
			if (device == a_device && context == a_context)
				return context1 && state;

			device = a_device;
			context = a_context;
			context1 = nullptr;
			state = nullptr;
			if (!device || !context)
				return false;

			winrt::com_ptr<ID3D11Device1> device1;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(device1.put()))) ||
				FAILED(context->QueryInterface(IID_PPV_ARGS(context1.put())))) {
				context1 = nullptr;
				return false;
			}

			constexpr D3D_FEATURE_LEVEL levels[]{
				D3D_FEATURE_LEVEL_11_1,
				D3D_FEATURE_LEVEL_11_0
			};
			D3D_FEATURE_LEVEL selected{};
			if (FAILED(device1->CreateDeviceContextState(
					0,
					levels,
					static_cast<UINT>(std::size(levels)),
					D3D11_SDK_VERSION,
					__uuidof(ID3D11Device1),
					&selected,
					state.put()))) {
				context1 = nullptr;
				return false;
			}
			return true;
		}
	};

	ContextStateCache& GetContextStateCache()
	{
		static ContextStateCache cache;
		return cache;
	}

	class ScopedParticleContextState
	{
	public:
		// Summary: Isolates SSPP rendering state until destruction.
		// Args:
		//   a_context: Immediate context to isolate until destruction.
		// Returns: Constructed state guard.
		explicit ScopedParticleContextState(ID3D11DeviceContext* a_context) : context(a_context)
		{
			auto& cache = GetContextStateCache();
			if (cache.Ensure(globals::d3d::device, context)) {
				context1 = cache.context1.get();
				context1->SwapDeviceContextState(cache.state.get(), previousState.put());
				context->ClearState();
				isolated = true;
			} else {
				static bool warned{};
				if (!warned) {
					logger::warn("[SSPP] D3D11.1 context-state isolation unavailable; using compatibility state backup");
					warned = true;
				}
				backup = std::make_unique<Effects11Util::D3D11FullStateBackup>();
				backup->Save(context);
				context->GSGetShader(savedGS.put(), nullptr, nullptr);
				context->HSGetShader(savedHS.put(), nullptr, nullptr);
				context->DSGetShader(savedDS.put(), nullptr, nullptr);
			}
		}

		// Summary: Restores the D3D11 state present at construction.
		// Args: None.
		// Returns: None.
		~ScopedParticleContextState()
		{
			if (isolated) {
				context1->SwapDeviceContextState(previousState.get(), nullptr);
			} else if (backup) {
				backup->Restore(context);
				backup->Release();
				context->GSSetShader(savedGS.get(), nullptr, 0);
				context->HSSetShader(savedHS.get(), nullptr, 0);
				context->DSSetShader(savedDS.get(), nullptr, 0);
			}
		}

		ScopedParticleContextState(const ScopedParticleContextState&) = delete;
		ScopedParticleContextState& operator=(const ScopedParticleContextState&) = delete;

	private:
		ID3D11DeviceContext* context{};
		ID3D11DeviceContext1* context1{};
		winrt::com_ptr<ID3DDeviceContextState> previousState;
		std::unique_ptr<Effects11Util::D3D11FullStateBackup> backup;
		winrt::com_ptr<ID3D11GeometryShader> savedGS;
		winrt::com_ptr<ID3D11HullShader> savedHS;
		winrt::com_ptr<ID3D11DomainShader> savedDS;
		bool isolated{};
	};

	struct DebugPreviewMarker
	{
		ImVec2 uv{};
		// Preview-UV delta for one short step along the player's facing direction.
		ImVec2 directionUV{};
		bool visible{};
	};

	// Summary: Overlays a player-position and facing marker on the last ImGui image.
	// Args:
	//   a_marker: Marker UV, direction, and visibility state.
	// Returns: None.
	void DrawDebugPreviewMarker(const DebugPreviewMarker& a_marker)
	{
		if (!a_marker.visible || !std::isfinite(a_marker.uv.x) || !std::isfinite(a_marker.uv.y) ||
			a_marker.uv.x < 0.0F || a_marker.uv.x > 1.0F ||
			a_marker.uv.y < 0.0F || a_marker.uv.y > 1.0F) {
			return;
		}

		const auto minimum = ImGui::GetItemRectMin();
		const auto maximum = ImGui::GetItemRectMax();
		const ImVec2 size{ maximum.x - minimum.x, maximum.y - minimum.y };
		if (size.x <= 0.0F || size.y <= 0.0F)
			return;

		float directionX = a_marker.directionUV.x * size.x;
		float directionY = a_marker.directionUV.y * size.y;
		const float directionLength = std::sqrt(directionX * directionX + directionY * directionY);
		if (!std::isfinite(directionLength) || directionLength <= 1.0e-6F)
			return;
		directionX /= directionLength;
		directionY /= directionLength;

		const ImVec2 origin{
			minimum.x + a_marker.uv.x * size.x,
			minimum.y + a_marker.uv.y * size.y
		};
		const float arrowLength = std::clamp(std::min(size.x, size.y) * 0.09F, 16.0F, 30.0F);
		const float headLength = std::clamp(arrowLength * 0.34F, 6.0F, 10.0F);
		const float headHalfWidth = headLength * 0.62F;
		const ImVec2 tip{
			origin.x + directionX * arrowLength,
			origin.y + directionY * arrowLength
		};
		const ImVec2 headBase{
			tip.x - directionX * headLength,
			tip.y - directionY * headLength
		};
		const ImVec2 left{
			headBase.x - directionY * headHalfWidth,
			headBase.y + directionX * headHalfWidth
		};
		const ImVec2 right{
			headBase.x + directionY * headHalfWidth,
			headBase.y - directionX * headHalfWidth
		};

		constexpr ImU32 outline = IM_COL32(0, 0, 0, 230);
		constexpr ImU32 marker = IM_COL32(255, 220, 32, 255);
		auto* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(minimum, maximum, true);
		drawList->AddLine(origin, headBase, outline, 6.0F);
		drawList->AddLine(origin, headBase, marker, 3.0F);
		drawList->AddTriangleFilled(tip, left, right, marker);
		drawList->AddTriangle(tip, left, right, outline, 2.0F);
		drawList->AddCircleFilled(origin, 5.0F, outline);
		drawList->AddCircleFilled(origin, 3.0F, marker);
		drawList->PopClipRect();
	}

	// Summary: Starts a current-versus-total diagnostics table.
	// Args:
	//   a_id: ImGui table identifier.
	// Returns: True when callers must emit rows and call ImGui::EndTable.
	bool BeginCurrentTotalTable(const char* a_id)
	{
		if (!ImGui::BeginTable(
				a_id,
				3,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_RowBg)) {
			return false;
		}
		ImGui::TableSetupColumn(
			T("feature.screen_space_physical_particles.metric", "Metric"),
			ImGuiTableColumnFlags_WidthStretch,
			1.35F);
		ImGui::TableSetupColumn(
			T("feature.screen_space_physical_particles.current_snapshot", "Current snapshot"),
			ImGuiTableColumnFlags_WidthStretch,
			1.0F);
		ImGui::TableSetupColumn(
			T("feature.screen_space_physical_particles.since_reset", "Since reset"),
			ImGuiTableColumnFlags_WidthStretch,
			1.0F);
		ImGui::TableHeadersRow();
		return true;
	}

	// Summary: Draws one current-versus-total diagnostics row.
	// Args:
	//   a_label: Localized metric label.
	//   a_current: Current asynchronous snapshot value.
	//   a_total: Accumulated value since reset.
	//   a_tooltip: Optional description shown on hover.
	// Returns: None.
	void DrawCurrentTotalRow(
		const char* a_label,
		std::string_view a_current,
		std::string_view a_total,
		const char* a_tooltip = nullptr)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextDisabled("%s", a_label);
		if (a_tooltip)
			Util::AddTooltip(a_tooltip);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(a_current.data(), a_current.data() + a_current.size());
		ImGui::TableSetColumnIndex(2);
		ImGui::TextUnformatted(a_total.data(), a_total.data() + a_total.size());
	}

	std::string Count(std::uint64_t a_value)
	{
		return std::format("{}", a_value);
	}

	std::string Ratio(std::uint64_t a_numerator, std::uint64_t a_denominator)
	{
		return a_denominator == 0 ? std::string(kUnavailableValue) :
		                            std::format("{:.1f}%", 100.0 * static_cast<double>(a_numerator) /
															   static_cast<double>(a_denominator));
	}

	bool TreeSection(const char* a_label, const char* a_tooltip = nullptr)
	{
		const bool open = ImGui::TreeNodeEx(a_label, ImGuiTreeNodeFlags_SpanAvailWidth);
		if (a_tooltip)
			Util::AddTooltip(a_tooltip);
		return open;
	}
}

std::pair<std::string, std::vector<std::string>> ScreenSpacePhysicalParticlesFeature::GetFeatureSummary()
{
	return {
		T(
			"feature.screen_space_physical_particles.description",
			"Shared collision captures and diagnostics for GPU physical-particle effects."),
		{ T("feature.screen_space_physical_particles.key_feature_1", "Shared stable-slot GPU particle runtime"),
			T("feature.screen_space_physical_particles.key_feature_2", "Active-list simulation and indirect rendering"),
			T("feature.screen_space_physical_particles.key_feature_3", "Centralized particle and collision diagnostics") }
	};
}

void ScreenSpacePhysicalParticlesFeature::PostPostLoad()
{
	ScreenSpaceCollisionDepth::GetSingleton().InstallHooks();
	ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton().InstallGameUpdateHook();
}

void ScreenSpacePhysicalParticlesFeature::SetupResources()
{
	ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton().SetupResources(
		static_cast<std::uint32_t>(settings.particleCapacity));
	ScreenSpaceCollisionDepth::GetSingleton().SetupResources();
	ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton().SetupResources();
	ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton().SetupResources();
}

void ScreenSpacePhysicalParticlesFeature::Reset()
{
	ScreenSpaceCollisionDepth::GetSingleton().SetupResources();
}

void ScreenSpacePhysicalParticlesFeature::EarlyPrepass()
{
	const auto& particleRuntime =
		ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	const auto requiredProviders = particleRuntime.RequiredCollisionProviders();
	const auto perspectiveDepth = static_cast<std::uint32_t>(
		ScreenSpacePhysicalParticles::Core::CollisionQueryPolicy::kPerspectiveDepth);
	if (settings.sceneDepthDebug || (requiredProviders & perspectiveDepth) != 0U)
		ScreenSpaceCollisionDepth::GetSingleton().RequestCapture();
}

void ScreenSpacePhysicalParticlesFeature::PostComposite()
{
	ScreenSpaceCollisionDepth::GetSingleton().EndCapture();
	ScopedParticleContextState stateScope(globals::d3d::context);
	auto& heightField = ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton();
	auto& particleRuntime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	const auto requestedCapacity = static_cast<std::uint32_t>(settings.particleCapacity);
	if (!particleRuntime.Ready() || particleRuntime.Capacity() != requestedCapacity)
		particleRuntime.SetupResources(requestedCapacity);
	const auto registeredFeatureCount = particleRuntime.RegisteredFeatureCount();
	if (featureBudgetsPending || budgetedFeatureCount != registeredFeatureCount) {
		for (const auto& feature : particleRuntime.FeatureInfos()) {
			const auto budget = settings.featureParticleBudgets.find(feature.stableKey);
			(void)particleRuntime.SetFeatureLiveParticleBudget(
				feature.id,
				budget == settings.featureParticleBudgets.end() ? 0U : budget->second);
		}
		budgetedFeatureCount = registeredFeatureCount;
		featureBudgetsPending = false;
	}
	const auto requiredProviders = particleRuntime.RequiredCollisionProviders();
	const auto heightFieldProviders =
		static_cast<std::uint32_t>(
			ScreenSpacePhysicalParticles::Core::CollisionQueryPolicy::kSolidHeightField) |
		static_cast<std::uint32_t>(
			ScreenSpacePhysicalParticles::Core::CollisionQueryPolicy::kWaterHeightField);
	const auto playerCapsules = static_cast<std::uint32_t>(
		ScreenSpacePhysicalParticles::Core::CollisionQueryPolicy::kPlayerCapsules);
	if ((requiredProviders & heightFieldProviders) != 0U || settings.heightFieldDebug ||
		settings.waterHeightFieldDebug ||
		heightField.RecaptureRequested()) {
		heightField.Capture();
	}
	ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton().PrepareRenderFrame(
		(requiredProviders & playerCapsules) != 0U);

	const bool updateCollisionDebug = settings.heightFieldDebug || settings.waterHeightFieldDebug ||
	                                  settings.sceneDepthDebug;

	const auto deltaTime = globals::game::ui->GameIsPaused() ? 0.0F : *globals::game::deltaTime;
	ScreenSpacePhysicalParticles::StressTest::UpdateBeforeFrame(particleRuntime, deltaTime);
	particleRuntime.RunFrame(deltaTime);
	ScreenSpacePhysicalParticles::StressTest::UpdateAfterFrame(particleRuntime);

	if (updateCollisionDebug) {
		heightField.UpdateDebug(settings.heightFieldDebug, settings.waterHeightFieldDebug);
		ScreenSpaceCollisionDepth::GetSingleton().UpdateDebug(settings.sceneDepthDebug);
	}
}

void ScreenSpacePhysicalParticlesFeature::GameLoaded()
{
	auto& particleRuntime =
		ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();
	ScreenSpacePhysicalParticles::StressTest::RequestStopForGameReset();
	ScreenSpaceCollisionDepth::GetSingleton().Reset();
	ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton().Reset();
	ScreenSpacePhysicalParticles::PlayerCollisionProvider::GetSingleton().Reset();
	particleRuntime.Reset();
}

void ScreenSpacePhysicalParticlesFeature::ClearShaderCache()
{
	ScreenSpaceCollisionDepth::GetSingleton().ClearShaderCache();
	ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton().ClearShaderCache();
	ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton().ClearShaderCache();
}

void ScreenSpacePhysicalParticlesFeature::DrawSettings()
{
	auto& heightField = ScreenSpacePhysicalParticles::OrthographicHeightField::GetSingleton();
	auto& sceneDepth = ScreenSpaceCollisionDepth::GetSingleton();
	auto& particleRuntime = ScreenSpacePhysicalParticles::Core::ParticleRuntime::GetSingleton();

	const auto readyText = [](bool a_ready) {
		return a_ready ?
		           T("feature.screen_space_physical_particles.ready", "Ready") :
		           T("feature.screen_space_physical_particles.not_ready", "Not ready");
	};
	const auto drawPreview = [](
		ID3D11ShaderResourceView* a_srv,
		float a_aspectRatio,
		const DebugPreviewMarker* a_marker) {
		if (!a_srv) {
			ImGui::TextDisabled(
				"%s",
				T("feature.screen_space_physical_particles.preview_unavailable", "Preview unavailable"));
			return;
		}
		const auto width = std::max(ImGui::GetContentRegionAvail().x, 64.0F);
		ImGui::Image(a_srv, { width, width / std::max(a_aspectRatio, 0.01F) });
		if (a_marker)
			DrawDebugPreviewMarker(*a_marker);
	};
	const auto drawCard = [&](const char* a_id,
							  const char* a_title,
							  bool& a_enabled,
							  const std::string& a_status,
							  ID3D11ShaderResourceView* a_srv,
							  float a_aspectRatio,
							  const DebugPreviewMarker* a_marker) {
		ImGui::PushID(a_id);
		ImGui::Checkbox("##Enabled", &a_enabled);
		ImGui::SameLine();
		ImGui::TextUnformatted(a_title);
		ImGui::TextDisabled("%s", a_status.c_str());
		if (a_enabled)
			drawPreview(a_srv, a_aspectRatio, a_marker);
		else
			ImGui::TextDisabled(
				"%s",
				T("feature.screen_space_physical_particles.enable_to_preview", "Enable to preview"));
		ImGui::PopID();
	};

	ImGui::SeparatorText(T("feature.screen_space_physical_particles.particle_runtime", "Particle runtime"));
	ImGui::TextDisabled(
		"%s",
		T(
			"feature.screen_space_physical_particles.phase_one_diagnostics_note",
			"Shared runtime: Physical Blood + Physical Leaves + SSPP test particles"));
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.phase_one_diagnostics_tooltip",
		"These counters cover every feature registered with the shared SSPP stable-slot pool."));
	ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 360.0F));
	ImGui::SliderInt(
		T("feature.screen_space_physical_particles.particle_capacity", "Global particle capacity"),
		&particleCapacityDraft,
		static_cast<int>(ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMinimumCapacity),
		static_cast<int>(ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMaximumCapacity));
	if (ImGui::IsItemDeactivatedAfterEdit())
		settings.particleCapacity = particleCapacityDraft;
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.particle_capacity_tooltip",
		"Shared stable-slot capacity for SSPP 2.0 clients. Changing it rebuilds the pool and clears its current particles."));
	ImGui::BeginDisabled(
		!particleRuntime.Ready() && !ScreenSpacePhysicalParticles::StressTest::Running());
	if (ImGui::Button(T(
			"feature.screen_space_physical_particles.toggle_stress_test",
			"Toggle stress test"))) {
		ScreenSpacePhysicalParticles::StressTest::Toggle(particleRuntime);
	}
	ImGui::EndDisabled();
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.toggle_stress_test_tooltip",
		"Starts or stops a 60-second SSPP benchmark. It applies a bounded, capacity-scaled debug-particle load, samples each asynchronously collected profiler frame once, and writes detailed results to CommunityShaders.log and SSPPStressTest.log. Time and injection pause while the game is paused."));
	if (ScreenSpacePhysicalParticles::StressTest::Running()) {
		const auto duration = ScreenSpacePhysicalParticles::StressTest::DurationSeconds();
		const auto remaining =
			std::max(duration - ScreenSpacePhysicalParticles::StressTest::ElapsedSeconds(), 0.0F);
		const bool waiting = ScreenSpacePhysicalParticles::StressTest::Waiting(particleRuntime);
		const auto status = waiting ?
			std::format(
				"Waiting while paused/not ready | {:.1f} s remaining | {} frames | {} profiler samples",
				remaining,
				ScreenSpacePhysicalParticles::StressTest::RuntimeFrames(),
				ScreenSpacePhysicalParticles::StressTest::ProfilerSamples()) :
			std::format(
				"Running | {:.1f} s remaining | {} frames | {} profiler samples",
				remaining,
				ScreenSpacePhysicalParticles::StressTest::RuntimeFrames(),
				ScreenSpacePhysicalParticles::StressTest::ProfilerSamples());
		ImGui::ProgressBar(
			std::clamp(
				ScreenSpacePhysicalParticles::StressTest::ElapsedSeconds() / duration, 0.0F, 1.0F),
			{ -1.0F, 0.0F },
			status.c_str());
		if (waiting) {
			ImGui::TextDisabled(
				"%s",
				T(
					"feature.screen_space_physical_particles.stress_test_waiting",
					"No benchmark time or particles are added until unpaused gameplay resumes."));
		}
	} else {
		const auto lastResult = ScreenSpacePhysicalParticles::StressTest::LastResult();
		ImGui::TextDisabled(
			"%.*s", static_cast<int>(lastResult.size()), lastResult.data());
	}

	ImGui::BeginDisabled(!particleRuntime.Ready());
	if (ImGui::Button(T("feature.screen_space_physical_particles.emit_debug_burst", "Emit SSPP 2.0 test burst"))) {
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const auto eye = Util::GetEyePosition();
			const auto angle = player->GetAngleZ();
			const float3 direction{ std::sin(angle), std::cos(angle), 0.18F };
			debugBurstRejected = !particleRuntime.QueueDebugBurst({ eye.x, eye.y, eye.z }, direction);
		}
	}
	ImGui::EndDisabled();
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.emit_debug_burst_tooltip",
		"Queues white debug particles through the new GPU free list, compact simulation lists, classifier, and indirect renderer."));
	ImGui::SameLine();
	if (ImGui::Button(T("feature.screen_space_physical_particles.reset_particle_pool", "Reset particle pool"))) {
		particleRuntime.Reset();
		debugBurstRejected = false;
	}
	if (debugBurstRejected) {
		ImGui::TextColored(
			ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
			"%s",
			T(
				"feature.screen_space_physical_particles.debug_burst_rejected",
				"The test burst was rejected because the CPU command queue is full."));
	}

	const auto& particleDiagnostics = particleRuntime.Diagnostics();
	if (!particleDiagnostics.valid) {
		ImGui::TextDisabled(
			"%s",
			T(
				"feature.screen_space_physical_particles.diagnostics_warming_up",
				"GPU diagnostics are warming up (asynchronous readback)."));
	} else {
		const auto snapshotAge = particleRuntime.FrameCounter() >= particleDiagnostics.sampledFrame ?
		                             particleRuntime.FrameCounter() - particleDiagnostics.sampledFrame :
		                             0ULL;
		const auto activeCount = static_cast<std::uint64_t>(particleDiagnostics.dynamicCount) +
		                         particleDiagnostics.persistentCount;
		const auto capacity = static_cast<std::uint64_t>(particleDiagnostics.capacity);
		const auto allocated = static_cast<std::uint64_t>(particleDiagnostics.allocatedCount);
		const float utilization = capacity == 0 ? 0.0F :
		                                          std::clamp(static_cast<float>(allocated) / static_cast<float>(capacity), 0.0F, 1.0F);
		const auto utilizationLabel = std::format(
			"{} allocated | {} active | {} capacity ({:.1f}%)",
			allocated,
			activeCount,
			capacity,
			utilization * 100.0F);
		ImGui::ProgressBar(utilization, { -1.0F, 0.0F }, utilizationLabel.c_str());
		Util::AddTooltip(T(
			"feature.screen_space_physical_particles.pool_utilization_tooltip",
			"Allocated counts every non-free stable slot. Active is the dynamic plus persistent lists; a temporary difference can represent particles moving through lifecycle routes."));

		const int summaryColumns = ImGui::GetContentRegionAvail().x >= 640.0F ? 4 : 2;
		if (ImGui::BeginTable(
				"SSPPPoolOverview",
				summaryColumns,
				ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
			const auto summary = [](const char* a_label, const std::string& a_value, const char* a_tooltip) {
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", a_label);
				if (a_tooltip)
					Util::AddTooltip(a_tooltip);
				ImGui::SameLine();
				ImGui::TextUnformatted(a_value.c_str());
			};
			summary(
				T("feature.screen_space_physical_particles.free", "Free"),
				Count(particleDiagnostics.freeCount),
				T("feature.screen_space_physical_particles.free_tooltip", "Stable slots currently available for allocation."));
			summary(
				T("feature.screen_space_physical_particles.high_water", "High-water"),
				Count(particleDiagnostics.highWaterMark),
				T("feature.screen_space_physical_particles.high_water_tooltip", "Largest allocated-slot count observed since the pool was reset."));
			summary(
				T("feature.screen_space_physical_particles.live_emitters", "Emitters"),
				std::format("{} / {}", particleDiagnostics.activeEmitters,
					ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMaxPersistentEmitters),
				T("feature.screen_space_physical_particles.live_emitters_tooltip", "Persistent emitters active in the sampled frame."));
			summary(
				T("feature.screen_space_physical_particles.snapshot", "Snapshot"),
				std::format("#{} | {}f old", particleDiagnostics.sampledFrame, snapshotAge),
				T("feature.screen_space_physical_particles.snapshot_tooltip", "GPU counters are read back asynchronously. The frame number and age show exactly how fresh this view is."));
			ImGui::EndTable();
		}
		const auto accountedSlots = static_cast<std::uint64_t>(particleDiagnostics.freeCount) + allocated;
		if (accountedSlots != capacity) {
			ImGui::TextColored(
				ImVec4(1.0F, 0.35F, 0.25F, 1.0F),
				"%s",
				std::format(
					"Pool accounting mismatch: free ({}) + allocated ({}) = {}, capacity = {}",
					particleDiagnostics.freeCount,
					particleDiagnostics.allocatedCount,
					accountedSlots,
					capacity)
					.c_str());
			Util::AddTooltip(T(
				"feature.screen_space_physical_particles.pool_mismatch_tooltip",
				"This invariant should always hold. A mismatch indicates allocator corruption or counters sampled across an invalid reset boundary."));
		}

		if (TreeSection(
				T("feature.screen_space_physical_particles.pool_emission", "Pool and emission"),
				T("feature.screen_space_physical_particles.pool_emission_tooltip", "Current columns are sampled GPU state. Cumulative columns count work since the particle pool was reset."))) {
			if (BeginCurrentTotalTable("SSPPPoolEmissionDiagnostics")) {
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.allocated", "Allocated slots"),
					Count(particleDiagnostics.allocatedCount),
					kUnavailableValue);
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.active", "Active particles"),
					Count(activeCount),
					kUnavailableValue);
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.burst_requests", "Burst requests"),
					kUnavailableValue,
					Count(particleDiagnostics.burstRequests));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.requested", "Particles requested"),
					kUnavailableValue,
					Count(particleDiagnostics.requestedParticles));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.granted", "Particles granted"),
					kUnavailableValue,
					Count(particleDiagnostics.grantedParticles));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.dropped", "Particles dropped"),
					kUnavailableValue,
					Count(particleDiagnostics.droppedParticles));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.grant_ratio", "Grant ratio"),
					kUnavailableValue,
					Ratio(particleDiagnostics.grantedParticles, particleDiagnostics.requestedParticles),
					T("feature.screen_space_physical_particles.grant_ratio_tooltip", "Particles granted divided by particles requested since reset."));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.drop_ratio", "Drop ratio"),
					kUnavailableValue,
					Ratio(particleDiagnostics.droppedParticles, particleDiagnostics.requestedParticles),
					T("feature.screen_space_physical_particles.drop_ratio_tooltip", "Particles dropped divided by particles requested since reset."));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.recycled", "Particles recycled"),
					kUnavailableValue,
					Count(particleDiagnostics.recycledParticles));
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		if (TreeSection(
				T("feature.screen_space_physical_particles.simulation_persistence", "Simulation and persistent lists"),
				T("feature.screen_space_physical_particles.simulation_persistence_tooltip", "Groups are the 64-thread simulation groups needed for the sampled active count. Transitions are cumulative since reset."))) {
			const std::array<const char*, ScreenSpacePhysicalParticles::Core::ParticleRuntime::kSimulationClassCount>
				classNames{
					T("feature.screen_space_physical_particles.simulation_none", "No collision"),
					T("feature.screen_space_physical_particles.simulation_world", "World"),
					T("feature.screen_space_physical_particles.simulation_world_water", "World + water"),
					T("feature.screen_space_physical_particles.simulation_world_player", "World + player")
				};
			if (BeginCurrentTotalTable("SSPPSimulationDiagnostics")) {
				for (std::uint32_t index = 0; index < classNames.size(); ++index) {
					const auto active = particleDiagnostics.activeBySimulationClass[index];
					DrawCurrentTotalRow(
						classNames[index],
						std::format("{} particles | {} groups", active, (active + 63U) / 64U),
						std::format("{} transitions", particleDiagnostics.dynamicTransitions[index]));
				}
				ImGui::EndTable();
			}

			const auto persistentListInfos = particleRuntime.PersistentListInfos();
			bool hasPersistentListActivity = false;
			for (const auto& list : persistentListInfos)
				hasPersistentListActivity = hasPersistentListActivity || list.activeCount != 0 ||
				                            particleDiagnostics.persistentTransitions[list.id] != 0;
			if (!hasPersistentListActivity) {
				ImGui::TextDisabled(
					"%s",
					T("feature.screen_space_physical_particles.no_persistent_activity", "No persistent-list activity in this snapshot."));
			} else if (BeginCurrentTotalTable("SSPPPersistentListDiagnostics")) {
				for (const auto& list : persistentListInfos) {
					const auto transitions = particleDiagnostics.persistentTransitions[list.id];
					if (list.activeCount == 0 && transitions == 0)
						continue;
					DrawCurrentTotalRow(
						std::format("{} [{}]", list.name, list.id).c_str(),
						std::format("{} / {} particles | {} groups", list.activeCount, list.capacity,
							(list.activeCount + 63U) / 64U),
						std::format("{} transitions", transitions),
						std::format("Owner feature {} | maintenance every {:.3f} s", list.ownerFeature,
							list.maintenanceIntervalSeconds)
							.c_str());
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		if (TreeSection(
				T("feature.screen_space_physical_particles.collision_diagnostics", "Collision queries and responses"),
				T("feature.screen_space_physical_particles.collision_diagnostics_tooltip", "Query, hit, and response counters are cumulative since the pool was reset."))) {
			if (ImGui::BeginTable(
					"SSPPCollisionQueries",
					4,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.channel", "Channel"));
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.queries", "Queries"));
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.hits", "Hits"));
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.hit_rate", "Hit rate"));
				ImGui::TableHeadersRow();
				const std::array<const char*, 3> channels{
					T("feature.screen_space_physical_particles.world", "World"),
					T("feature.screen_space_physical_particles.water", "Water"),
					T("feature.screen_space_physical_particles.player", "Player")
				};
				for (std::uint32_t index = 0; index < channels.size(); ++index) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextDisabled("%s", channels[index]);
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%llu", particleDiagnostics.collisionQueries[index]);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%llu", particleDiagnostics.collisionHits[index]);
					ImGui::TableSetColumnIndex(3);
					ImGui::TextUnformatted(
						Ratio(particleDiagnostics.collisionHits[index], particleDiagnostics.collisionQueries[index]).c_str());
				}
				ImGui::EndTable();
			}
			ImGui::Spacing();
			if (ImGui::BeginTable(
					"SSPPCollisionResponses",
					2,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.response", "Response"));
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.since_reset", "Since reset"));
				ImGui::TableHeadersRow();
				const std::array<const char*, 7> responses{
					T("feature.screen_space_physical_particles.response_ignore", "Ignore"),
					T("feature.screen_space_physical_particles.response_bounce", "Bounce"),
					T("feature.screen_space_physical_particles.response_slide", "Slide"),
					T("feature.screen_space_physical_particles.response_settle", "Settle"),
					T("feature.screen_space_physical_particles.response_stick", "Stick"),
					T("feature.screen_space_physical_particles.response_kill", "Kill"),
					T("feature.screen_space_physical_particles.response_event_kill", "Emit event + kill")
				};
				for (std::uint32_t index = 0; index < responses.size(); ++index) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextDisabled("%s", responses[index]);
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%llu", particleDiagnostics.collisionResponses[index]);
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		if (TreeSection(
				T("feature.screen_space_physical_particles.emitter_pressure", "Emitter pressure"),
				T("feature.screen_space_physical_particles.emitter_pressure_tooltip", "Live emitter and owned-particle counts are current; command and emission counters are cumulative since reset."))) {
			if (BeginCurrentTotalTable("SSPPEmitterDiagnostics")) {
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.live_emitters", "Live emitters"),
					std::format("{} / {}", particleDiagnostics.activeEmitters,
						ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMaxPersistentEmitters),
					kUnavailableValue);
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.owned_particles", "Emitter-owned particles"),
					Count(particleDiagnostics.ownedParticles),
					kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.emitter_creates", "Creates applied"), kUnavailableValue, Count(particleDiagnostics.emitterCreatesApplied));
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.emitter_updates", "Updates applied"), kUnavailableValue, Count(particleDiagnostics.emitterUpdatesApplied));
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.emitter_destroys", "Destroys applied"), kUnavailableValue, Count(particleDiagnostics.emitterDestroysApplied));
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.persistent_requests", "Persistent emission requests"), kUnavailableValue, Count(particleDiagnostics.persistentRequests));
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.persistent_requested", "Persistent particles requested"), kUnavailableValue, Count(particleDiagnostics.persistentParticlesRequested));
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.persistent_granted", "Persistent particles granted"), kUnavailableValue, Count(particleDiagnostics.persistentParticlesGranted));
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.persistent_dropped", "Persistent particles dropped"), kUnavailableValue, Count(particleDiagnostics.persistentParticlesDropped));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.persistent_grant_ratio", "Persistent grant ratio"),
					kUnavailableValue,
					Ratio(particleDiagnostics.persistentParticlesGranted, particleDiagnostics.persistentParticlesRequested));
				DrawCurrentTotalRow(
					T("feature.screen_space_physical_particles.persistent_drop_ratio", "Persistent drop ratio"),
					kUnavailableValue,
					Ratio(particleDiagnostics.persistentParticlesDropped, particleDiagnostics.persistentParticlesRequested));
				if (particleDiagnostics.emitterBudgetClamps != 0) {
					DrawCurrentTotalRow(
						T("feature.screen_space_physical_particles.emitter_budget_clamps", "Emitter budget clamps"),
						kUnavailableValue,
						Count(particleDiagnostics.emitterBudgetClamps),
						T("feature.screen_space_physical_particles.emitter_budget_clamps_tooltip", "Emission demand exceeded an emitter's configured live-particle budget."));
				}
				ImGui::EndTable();
			}

			ImGui::Spacing();
			ImGui::TextDisabled(
				"%s",
				T(
					"feature.screen_space_physical_particles.feature_budgets_note",
					"Per-feature soft limits (0 = global capacity)"));
			if (ImGui::BeginTable(
					"SSPPFeatureBudgetDiagnostics",
					5,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(
					T("feature.screen_space_physical_particles.feature", "Feature"),
					ImGuiTableColumnFlags_WidthStretch,
					1.4F);
				ImGui::TableSetupColumn(
					T("feature.screen_space_physical_particles.live_budget", "Live / limit"),
					ImGuiTableColumnFlags_WidthStretch,
					0.8F);
				ImGui::TableSetupColumn(
					T("feature.screen_space_physical_particles.soft_maximum", "Soft maximum"),
					ImGuiTableColumnFlags_WidthStretch,
					1.4F);
				ImGui::TableSetupColumn(
					T("feature.screen_space_physical_particles.granted_requested", "Granted / requested"),
					ImGuiTableColumnFlags_WidthStretch,
					1.0F);
				ImGui::TableSetupColumn(
					T("feature.screen_space_physical_particles.dropped_budget", "Dropped (budget)"),
					ImGuiTableColumnFlags_WidthStretch,
					0.9F);
				ImGui::TableHeadersRow();
				for (const auto& feature : particleRuntime.FeatureInfos()) {
					ImGui::PushID(static_cast<int>(feature.id));
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(feature.stableKey.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%u / %u", feature.liveParticles, feature.maximumLiveParticles);
					ImGui::TableSetColumnIndex(2);
					int configuredBudget = 0;
					if (const auto configured = settings.featureParticleBudgets.find(feature.stableKey);
						configured != settings.featureParticleBudgets.end()) {
						configuredBudget = static_cast<int>(configured->second);
					}
					ImGui::SetNextItemWidth(-1.0F);
					if (ImGui::SliderInt(
							"##FeatureBudget",
							&configuredBudget,
							0,
							settings.particleCapacity,
							configuredBudget == 0 ? "Global" : "%d")) {
						if (configuredBudget == 0)
							settings.featureParticleBudgets.erase(feature.stableKey);
						else
							settings.featureParticleBudgets[feature.stableKey] =
								static_cast<std::uint32_t>(configuredBudget);
						(void)particleRuntime.SetFeatureLiveParticleBudget(
							feature.id, static_cast<std::uint32_t>(configuredBudget));
					}
					Util::AddTooltip(T(
						"feature.screen_space_physical_particles.feature_budget_tooltip",
						"The planner clamps this feature before reserving global slots. Unused slots remain shared; lowering a limit does not evict existing particles."));
					ImGui::TableSetColumnIndex(3);
					ImGui::Text(
						"%llu / %llu", feature.grantedParticles, feature.requestedParticles);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text(
						"%llu (%llu)", feature.droppedParticles, feature.budgetClampedParticles);
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		const bool hasLifecycleActivity = particleDiagnostics.staleReferences != 0 ||
		                                  particleDiagnostics.duplicateRoutes != 0 || particleDiagnostics.invalidDestinations != 0 ||
		                                  particleDiagnostics.quarantinedParticles != 0 || particleDiagnostics.freeReturns != 0 ||
		                                  particleDiagnostics.maintenanceRuns != 0;
		const bool hasEventActivity = particleDiagnostics.eventsProduced != 0 ||
		                              particleDiagnostics.eventsConsumed != 0 || particleDiagnostics.eventOverflows != 0;
		const bool hasErrors = particleDiagnostics.invalidProfiles != 0 ||
		                       particleDiagnostics.listOverflows != 0 || particleDiagnostics.invalidCollisionRules != 0 ||
		                       particleDiagnostics.invalidEmitterCommands != 0 || particleDiagnostics.requestArenaOverflows != 0 ||
		                       particleDiagnostics.emissionChunkOverflows != 0 || particleDiagnostics.ownerGenerationMismatches != 0 ||
		                       particleDiagnostics.rejectedEmitterCommands != 0 || particleDiagnostics.rejectedEmitterHandles != 0;
		if ((hasLifecycleActivity || hasEventActivity || hasErrors) && TreeSection(
																		   T("feature.screen_space_physical_particles.lifecycle_events_errors", "Lifecycle, events, and errors"),
																		   T("feature.screen_space_physical_particles.lifecycle_events_errors_tooltip", "This section appears only when at least one cumulative lifecycle, event, or error counter is nonzero."))) {
			if (BeginCurrentTotalTable("SSPPLifecycleDiagnostics")) {
				const auto nonzero = [&](const char* a_label, std::uint64_t a_value, const char* a_tooltip = nullptr) {
					if (a_value != 0)
						DrawCurrentTotalRow(a_label, kUnavailableValue, Count(a_value), a_tooltip);
				};
				nonzero(T("feature.screen_space_physical_particles.maintenance_runs", "Maintenance runs"), particleDiagnostics.maintenanceRuns);
				nonzero(T("feature.screen_space_physical_particles.free_returns", "Free returns"), particleDiagnostics.freeReturns);
				nonzero(T("feature.screen_space_physical_particles.quarantined", "Quarantined particles"), particleDiagnostics.quarantinedParticles);
				nonzero(T("feature.screen_space_physical_particles.events_produced", "Events produced"), particleDiagnostics.eventsProduced);
				nonzero(T("feature.screen_space_physical_particles.events_consumed", "Events consumed"), particleDiagnostics.eventsConsumed);
				nonzero(T("feature.screen_space_physical_particles.stale_references", "Stale references"), particleDiagnostics.staleReferences);
				nonzero(T("feature.screen_space_physical_particles.duplicate_routes", "Duplicate routes"), particleDiagnostics.duplicateRoutes);
				nonzero(T("feature.screen_space_physical_particles.invalid_destinations", "Invalid destinations"), particleDiagnostics.invalidDestinations);
				nonzero(T("feature.screen_space_physical_particles.invalid_profiles", "Invalid profiles"), particleDiagnostics.invalidProfiles);
				nonzero(T("feature.screen_space_physical_particles.list_overflows", "List overflows"), particleDiagnostics.listOverflows);
				nonzero(T("feature.screen_space_physical_particles.event_overflows", "Event overflows"), particleDiagnostics.eventOverflows);
				nonzero(T("feature.screen_space_physical_particles.invalid_collision_rules", "Invalid collision rules"), particleDiagnostics.invalidCollisionRules);
				nonzero(T("feature.screen_space_physical_particles.invalid_emitter_commands", "Invalid emitter commands"), particleDiagnostics.invalidEmitterCommands);
				nonzero(T("feature.screen_space_physical_particles.request_arena_overflows", "Request-arena overflows"), particleDiagnostics.requestArenaOverflows);
				nonzero(T("feature.screen_space_physical_particles.emission_chunk_overflows", "Emission-chunk overflows"), particleDiagnostics.emissionChunkOverflows);
				nonzero(T("feature.screen_space_physical_particles.owner_generation_mismatches", "Owner-generation mismatches"), particleDiagnostics.ownerGenerationMismatches);
				nonzero(T("feature.screen_space_physical_particles.rejected_emitter_commands", "CPU-rejected emitter commands"), particleDiagnostics.rejectedEmitterCommands);
				nonzero(T("feature.screen_space_physical_particles.rejected_emitter_handles", "CPU-rejected emitter handles"), particleDiagnostics.rejectedEmitterHandles);
				ImGui::EndTable();
			}
			if (hasEventActivity) {
				for (const auto& route : particleRuntime.EventRouteInfos()) {
					if (route.produced != 0 || route.consumed != 0) {
						ImGui::BulletText(
							"%s [%u]: %u produced | %u consumed",
							route.name.c_str(),
							route.id,
							route.produced,
							route.consumed);
						const auto tooltip = std::format("Owner feature: {}", route.ownerFeature);
						Util::AddTooltip(tooltip.c_str());
					}
				}
			}
			ImGui::TreePop();
		}

		if (TreeSection(
				T("feature.screen_space_physical_particles.renderer_diagnostics", "Registered renderers"),
				T("feature.screen_space_physical_particles.renderer_diagnostics_tooltip", "Renderer names and status come from the live CPU registry. Visibility and culling counters come from the asynchronous sampled frame."))) {
			const auto rendererInfos = particleRuntime.RendererInfos();
			if (ImGui::BeginTable(
					"SSPPRendererRegistry",
					3,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.renderer", "Renderer"), ImGuiTableColumnFlags_WidthStretch, 1.5F);
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.state", "State"), ImGuiTableColumnFlags_WidthStretch, 0.8F);
				ImGui::TableSetupColumn(T("feature.screen_space_physical_particles.visible", "Visible"), ImGuiTableColumnFlags_WidthStretch, 0.6F);
				ImGui::TableHeadersRow();
				for (const auto& renderer : rendererInfos) {
					const char* state = !renderer.enabled         ? "disabled" :
					                    !renderer.rendererPresent ? "missing" :
					                    !renderer.ready           ? "warming" :
					                                                "ready";
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Text("%u: %s", renderer.id, renderer.name.c_str());
					const auto tooltip = std::format("Stable key: {}", renderer.stableKey);
					Util::AddTooltip(tooltip.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(state);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%u", renderer.visibleCount);
				}
				if (rendererInfos.empty()) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextDisabled("%s", T("feature.screen_space_physical_particles.no_renderers", "No renderers registered"));
				}
				ImGui::EndTable();
			}
			ImGui::Spacing();
			if (BeginCurrentTotalTable("SSPPRendererCounters")) {
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.registered", "Registered classes"), Count(particleDiagnostics.registeredRenderClasses), kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.render_candidates", "Render candidates"), Count(particleDiagnostics.renderCandidates), kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.visible", "Visible particles"), Count(particleDiagnostics.visibleCount), kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.frustum_culled", "Frustum culled"), Count(particleDiagnostics.frustumCulled), kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.distance_culled", "Distance culled"), Count(particleDiagnostics.distanceCulled), kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.classifier_culled", "Classifier culled"), Count(particleDiagnostics.culledCount), kUnavailableValue);
				DrawCurrentTotalRow(T("feature.screen_space_physical_particles.disabled_missing", "Disabled/missing renderer rejects"), Count(particleDiagnostics.disabledOrMissingRenderers), kUnavailableValue);
				if (particleDiagnostics.invalidRenderClasses != 0)
					DrawCurrentTotalRow(T("feature.screen_space_physical_particles.invalid_render_classes", "Invalid render classes"), Count(particleDiagnostics.invalidRenderClasses), kUnavailableValue);
				if (particleDiagnostics.renderStaleReferences != 0)
					DrawCurrentTotalRow(T("feature.screen_space_physical_particles.render_stale_references", "Stale render references"), Count(particleDiagnostics.renderStaleReferences), kUnavailableValue);
				if (particleDiagnostics.renderBinOverflows != 0)
					DrawCurrentTotalRow(T("feature.screen_space_physical_particles.render_bin_overflows", "Render-bin overflows"), Count(particleDiagnostics.renderBinOverflows), kUnavailableValue);
				ImGui::EndTable();
			}
			ImGui::TextDisabled(
				"%s",
				T("feature.screen_space_physical_particles.renderer_global_note", "Cull and overflow counters are global; the public snapshot exposes per-renderer visible counts only."));
			ImGui::TreePop();
		}
	}

	if (TreeSection(
			T("feature.screen_space_physical_particles.gpu_timings", "GPU timings"),
			T(
				"feature.screen_space_physical_particles.gpu_timings_tooltip",
				"Asynchronous GPU timers for SSPP and its adapters. Current is zero when a pass was skipped; average and P95 include frames in which that timer was submitted."))) {
		bool anyTiming = false;
		if (ImGui::BeginTable(
				"SSPPGPUTimings",
				4,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn(
				T("feature.screen_space_physical_particles.pass", "Pass"),
				ImGuiTableColumnFlags_WidthStretch,
				2.0F);
			ImGui::TableSetupColumn(
				T("feature.screen_space_physical_particles.current_ms", "Current ms"),
				ImGuiTableColumnFlags_WidthStretch,
				0.7F);
			ImGui::TableSetupColumn(
				T("feature.screen_space_physical_particles.average_ms", "Avg ms"),
				ImGuiTableColumnFlags_WidthStretch,
				0.7F);
			ImGui::TableSetupColumn(
				T("feature.screen_space_physical_particles.p95_ms", "P95 ms"),
				ImGuiTableColumnFlags_WidthStretch,
				0.7F);
			ImGui::TableHeadersRow();

			if (globals::profiler) {
				for (const auto& timing : globals::profiler->GetResults()) {
					const std::string_view name = timing.name;
					if (!timing.valid ||
						(!name.starts_with("SSPP::") && !name.starts_with("PhysicalBlood::") &&
							!name.starts_with("PhysicalLeaves::"))) {
						continue;
					}
					anyTiming = true;
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(timing.name.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.3f", timing.sampledThisFrame ? timing.gpuTimeMs : 0.0F);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%.3f", timing.avgMs);
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.3f", timing.p95Ms);
				}
			}

			if (!anyTiming) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled(
					"%s",
					T(
						"feature.screen_space_physical_particles.timings_warming_up",
						"Timing history is warming up"));
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	ImGui::TextWrapped(
		"%s",
		T(
			"feature.screen_space_physical_particles.diagnostics_intro",
			"These views show the shared collision inputs consumed by Physical Blood, Physical Leaves, and future SSPP effects."));
	ImGui::SeparatorText(T("feature.screen_space_physical_particles.collision_buffers", "Collision buffers"));
	ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 360.0F));
	ImGui::SliderInt(
		T(
			"feature.screen_space_physical_particles.orthographic_refresh_frames",
			"Solid height refresh interval (frames)"),
		&settings.orthographicRefreshFrames,
		10,
		300);
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.orthographic_refresh_frames_tooltip",
		"Refresh the player-centered solid height field every 10-300 frames. Coverage depends on the selected resolution."));
	ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 360.0F));
	ImGui::SliderInt(
		T(
			"feature.screen_space_physical_particles.orthographic_resolution",
			"Solid height resolution"),
		&settings.orthographicResolution,
		256,
		1024);
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.orthographic_resolution_tooltip",
		"Solid-map resolution and coverage, from 256 to 1024 pixels per side at 4 world units per texel. 256 covers 1024 units; 1024 covers one full exterior cell. This does not resize or refresh the separate 3x3 water-height field."));
	if (ImGui::Button(T(
			"feature.screen_space_physical_particles.recapture_top_down",
			"Recapture top-down fields"))) {
		heightField.RequestRecapture();
	}
	Util::AddTooltip(T(
		"feature.screen_space_physical_particles.recapture_top_down_tooltip",
		"Rebuild the solid and water height fields once on the next frame."));

	const auto center = heightField.Center();
	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto playerPosition = player ? player->GetPosition() : RE::NiPoint3{};
	const float playerYaw = player ? player->GetAngleZ() : 0.0F;
	const float playerFacingX = std::sin(playerYaw);
	const float playerFacingY = std::cos(playerYaw);
	const auto makeWorldMarker = [&](float2 a_center, float a_halfExtent) {
		DebugPreviewMarker result{};
		if (!player || !std::isfinite(a_halfExtent) || a_halfExtent <= 0.0F ||
			!std::isfinite(playerPosition.x) || !std::isfinite(playerPosition.y) ||
			!std::isfinite(playerFacingX) || !std::isfinite(playerFacingY)) {
			return result;
		}
		const float extent = a_halfExtent * 2.0F;
		result.uv = {
			0.5F + (playerPosition.x - a_center.x) / extent,
			0.5F - (playerPosition.y - a_center.y) / extent
		};
		result.directionUV = { playerFacingX / extent, -playerFacingY / extent };
		result.visible = true;
		return result;
	};
	const auto solidMarker = makeWorldMarker(center, heightField.HalfExtent());
	const auto waterMarker = makeWorldMarker(heightField.WaterCenter(), heightField.WaterHalfExtent());

	const auto heightStatus = std::format(
		"{} | {}x{} | {}: {}/{} | XY ({:.0f}, {:.0f}) +/-{:.0f} | top Z {:.0f}",
		readyText(heightField.Ready()),
		heightField.Dimension(),
		heightField.Dimension(),
		T("feature.screen_space_physical_particles.terrain_static", "terrain/static"),
		heightField.CapturedTerrainCount(),
		heightField.CapturedStaticCount(),
		center.x,
		center.y,
		heightField.HalfExtent(),
		heightField.TopZ());
	const auto waterHeightRange = std::abs(
		heightField.CapturedWaterMaximumHeight() - heightField.CapturedWaterMinimumHeight());
	const auto waterHeightDetail = heightField.WaterReady() && waterHeightRange < 0.5F ?
	                                   std::format("flat Z: {:.0f}", heightField.CapturedWaterMinimumHeight()) :
	                                   std::format(
										   "Z: {:.0f}..{:.0f}",
										   heightField.CapturedWaterMinimumHeight(),
										   heightField.CapturedWaterMaximumHeight());
	const auto waterHeightStatus = std::format(
		"{} | {}x{} | {}: {} | {} | ({:.0f}, {:.0f}) +/-{:.0f}",
		readyText(heightField.WaterReady()),
		heightField.WaterDimension(),
		heightField.WaterDimension(),
		T("feature.screen_space_physical_particles.surfaces", "surfaces"),
		heightField.CapturedWaterSurfaceCount(),
		waterHeightDetail,
		heightField.WaterCenter().x,
		heightField.WaterCenter().y,
		heightField.WaterHalfExtent());
	const auto sceneStatus = std::format(
		"{} | {}: {}/{}",
		readyText(sceneDepth.Ready()),
		T("feature.screen_space_physical_particles.terrain_static", "terrain/static"),
		sceneDepth.TerrainPasses(),
		sceneDepth.StaticPasses());

	const bool wideLayout = ImGui::GetContentRegionAvail().x >= 720.0F;
	if (wideLayout && ImGui::BeginTable(
						  "SSPPCollisionBuffers",
						  2,
						  ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn(
			T("feature.screen_space_physical_particles.solid_geometry", "Solid geometry"),
			ImGuiTableColumnFlags_WidthStretch,
			1.0F);
		ImGui::TableSetupColumn(
			T("feature.screen_space_physical_particles.water", "Water"),
			ImGuiTableColumnFlags_WidthStretch,
			1.0F);
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		drawCard(
			"HeightField",
			T("feature.screen_space_physical_particles.orthographic_height", "Orthographic height"),
			settings.heightFieldDebug,
			heightStatus,
			heightField.DebugSRV(),
			1.0F,
			&solidMarker);
		ImGui::TableSetColumnIndex(1);
		drawCard(
			"WaterHeightField",
			T("feature.screen_space_physical_particles.orthographic_water_height", "Resolved water collision height"),
			settings.waterHeightFieldDebug,
			waterHeightStatus,
			heightField.WaterDebugSRV(),
			1.0F,
			&waterMarker);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		drawCard(
			"SceneDepth",
			T("feature.screen_space_physical_particles.actor_free_depth", "Actor-free depth"),
			settings.sceneDepthDebug,
			sceneStatus,
			sceneDepth.DebugSRV(),
			16.0F / 9.0F,
			nullptr);
		ImGui::EndTable();
	} else if (!wideLayout) {
		drawCard(
			"HeightField",
			T("feature.screen_space_physical_particles.orthographic_height", "Orthographic height"),
			settings.heightFieldDebug,
			heightStatus,
			heightField.DebugSRV(),
			1.0F,
			&solidMarker);
		ImGui::Separator();
		drawCard(
			"WaterHeightField",
			T("feature.screen_space_physical_particles.orthographic_water_height", "Resolved water collision height"),
			settings.waterHeightFieldDebug,
			waterHeightStatus,
			heightField.WaterDebugSRV(),
			1.0F,
			&waterMarker);
		ImGui::Separator();
		drawCard(
			"SceneDepth",
			T("feature.screen_space_physical_particles.actor_free_depth", "Actor-free depth"),
			settings.sceneDepthDebug,
			sceneStatus,
			sceneDepth.DebugSRV(),
			16.0F / 9.0F,
			nullptr);
	}
	ImGui::TextDisabled(
		"%s",
		T(
			"feature.screen_space_physical_particles.world_debug_orientation",
			"Top-down orientation: +Y is up, +X is right; yellow arrow shows player position and facing."));

}

void ScreenSpacePhysicalParticlesFeature::LoadSettings(json& a_json)
{
	settings.particleCapacity = std::clamp(
		a_json.value("ParticleCapacity", settings.particleCapacity),
		static_cast<int>(ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMinimumCapacity),
		static_cast<int>(ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMaximumCapacity));
	particleCapacityDraft = settings.particleCapacity;
	settings.featureParticleBudgets.clear();
	if (const auto budgets = a_json.find("FeatureParticleBudgets");
		budgets != a_json.end() && budgets->is_object()) {
		for (const auto& [key, value] : budgets->items()) {
			if (!value.is_number_unsigned() && !value.is_number_integer())
				continue;
			const auto configured = value.get<std::int64_t>();
			if (configured > 0) {
				settings.featureParticleBudgets[key] = static_cast<std::uint32_t>(
					std::min<std::int64_t>(
						configured,
						ScreenSpacePhysicalParticles::Core::ParticleRuntime::kMaximumCapacity));
			}
		}
	}
	settings.orthographicRefreshFrames = std::clamp(
		a_json.value("OrthographicRefreshFrames", settings.orthographicRefreshFrames), 10, 300);
	settings.orthographicResolution = std::clamp(
		a_json.value("OrthographicResolution", settings.orthographicResolution), 256, 1024);
	settings.heightFieldDebug = a_json.value("HeightFieldDebug", settings.heightFieldDebug);
	settings.waterHeightFieldDebug = a_json.value("WaterHeightFieldDebug", settings.waterHeightFieldDebug);
	settings.sceneDepthDebug = a_json.value("SceneDepthDebug", settings.sceneDepthDebug);
	featureBudgetsPending = true;
}

void ScreenSpacePhysicalParticlesFeature::SaveSettings(json& a_json)
{
	a_json = {
		{ "ParticleCapacity", settings.particleCapacity },
		{ "FeatureParticleBudgets", settings.featureParticleBudgets },
		{ "OrthographicRefreshFrames", settings.orthographicRefreshFrames },
		{ "OrthographicResolution", settings.orthographicResolution },
		{ "HeightFieldDebug", settings.heightFieldDebug },
		{ "WaterHeightFieldDebug", settings.waterHeightFieldDebug },
		{ "SceneDepthDebug", settings.sceneDepthDebug }
	};
}

void ScreenSpacePhysicalParticlesFeature::RestoreDefaultSettings()
{
	settings = {};
	particleCapacityDraft = settings.particleCapacity;
	debugBurstRejected = false;
	featureBudgetsPending = true;
}
