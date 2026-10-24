#include "PhysicalWorldParticles.h"

#include "Deferred.h"
#include "Features/ScreenSpacePhysicalParticles/BearforjWorldQuery.h"
#include "Features/ScreenSpacePhysicalParticles/ScreenSpacePhysicalParticles.h"
#include "Globals.h"
#include "State.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
	using namespace ScreenSpacePhysicalParticles::Core;

	constexpr const char* kGroups[] = {
		"sparks", "fluff", "dandelions", "spores", "fireflies", "moths"
	};
	constexpr std::uint32_t kShaderMode[] = { 0U, 1U, 1U, 2U, 2U, 3U };
	constexpr bool kAdditive[] = { true, false, false, true, true, false };
	constexpr const char* kRenderKeys[] = {
		"PhysicalWorld.Magic",
		"PhysicalWorld.Fluff",
		"PhysicalWorld.Dandelion",
		"PhysicalWorld.Spores",
		"PhysicalWorld.Fireflies",
		"PhysicalWorld.Moths"
	};

	ScalarDistributionGPU Range(float a_min, float a_max, DistributionType a_type = DistributionType::kUniform)
	{
		ScalarDistributionGPU distribution{};
		distribution.range = { a_min, a_max };
		distribution.type = static_cast<std::uint32_t>(a_type);
		distribution.shape = 1.0F;
		return distribution;
	}

	template <class Tag>
	struct MagicImpactHook
	{
		static void thunk(
			RE::Projectile* a_self,
			RE::TESObjectREFR* a_ref,
			const RE::NiPoint3& a_target,
			const RE::NiPoint3& a_velocity,
			RE::hkpCollidable* a_collidable,
			std::int32_t a_arg6,
			std::uint32_t a_arg7)
		{
			globals::features::physicalWorldParticles.OnMagicImpact(a_self, a_target);
			func(a_self, a_ref, a_target, a_velocity, a_collidable, a_arg6, a_arg7);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MissileImpactTag;
	struct FlameImpactTag;
	struct BeamImpactTag;
	struct ConeImpactTag;
	struct GrenadeImpactTag;

	bool CellHasGrass(const RE::TESObjectCELL* a_cell)
	{
		if (!a_cell || !a_cell->IsExteriorCell())
			return false;
		const auto* land = a_cell->GetRuntimeData().cellLand;
		const auto* loaded = land ? land->loadedData : nullptr;
		if (!loaded)
			return false;
		for (std::uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
			for (std::uint32_t layer = 0; layer < 6; ++layer) {
				const auto* texture = loaded->quadTextures[quadrant][layer];
				if (texture && !texture->textureGrassList.empty())
					return true;
			}
		}
		return false;
	}
}

class PhysicalWorldParticles::SharedRenderer final : public ScreenSpacePhysicalParticles::Core::IParticleRenderer
{
public:
	explicit SharedRenderer(PhysicalWorldParticles& a_owner) noexcept :
		owner(a_owner) {}

	void ClearShaderCache() override
	{
		owner.vertexShader = nullptr;
		owner.pixelShader = nullptr;
	}
	[[nodiscard]] bool IsReady() const noexcept override
	{
		return owner.vertexShader && owner.pixelShader && owner.sampler && owner.renderCB &&
		       owner.alphaBlend && owner.additiveBlend && owner.depthState && owner.rasterizerState;
	}
	void Prepare(const ScreenSpacePhysicalParticles::Core::ParticleRenderContext&) override {}
	void Draw(const ScreenSpacePhysicalParticles::Core::ParticleRenderContext& a_context) override
	{
		if (!IsReady() || !a_context.context)
			return;
		const auto kind = owner.KindFromClass(a_context.renderClass);
		if (kind >= PhysicalWorldParticles::kKindCount || !owner.textures[kind].SRV())
			return;
		auto* renderer = globals::game::renderer;
		if (!renderer || !globals::deferred)
			return;
		const auto mainTarget = globals::deferred->forwardRenderTargets[0];
		auto& main = renderer->GetRuntimeData().renderTargets[mainTarget];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		ID3D11RenderTargetView* target = main.RTV;
		ID3D11DepthStencilView* depthView = depth.readOnlyViews[0] ? depth.readOnlyViews[0] : depth.views[0];
		if (!target)
			return;

		auto* context = a_context.context;
		context->OMSetRenderTargets(1, &target, depthView);
		auto resolution = float2{
			static_cast<float>(globals::game::graphicsState->screenWidth),
			static_cast<float>(globals::game::graphicsState->screenHeight)
		};
		resolution = Util::ConvertToDynamic(resolution, true);
		const D3D11_VIEWPORT viewport{ 0.0F, 0.0F, resolution.x, resolution.y, 0.0F, 1.0F };
		context->RSSetViewports(1, &viewport);
		context->RSSetState(owner.rasterizerState.get());
		context->OMSetBlendState(
			kAdditive[kind] ? owner.additiveBlend.get() : owner.alphaBlend.get(),
			nullptr,
			0xFFFFFFFFU);
		context->OMSetDepthStencilState(owner.depthState.get(), 0);
		ID3D11Buffer* nullVertex{};
		UINT stride{};
		UINT offset{};
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 1, &nullVertex, &stride, &offset);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		RenderCB constants{};
		constants.mode = kShaderMode[kind];
		constants.visibleSegmentBase = a_context.visibleSegmentBase;
		constants.particleCapacity = a_context.particleCapacity;
		owner.renderCB->Update(constants);
		ID3D11Buffer* constantBuffer = owner.renderCB->CB();
		context->VSSetConstantBuffers(0, 1, &constantBuffer);
		context->PSSetConstantBuffers(0, 1, &constantBuffer);
		ID3D11ShaderResourceView* texture = owner.textures[kind].SRV();
		context->PSSetShaderResources(2, 1, &texture);
		ID3D11SamplerState* sampler = owner.sampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		context->VSSetShader(owner.vertexShader.get(), nullptr, 0);
		context->PSSetShader(owner.pixelShader.get(), nullptr, 0);
		a_context.DrawInstancedIndirect();
		ID3D11ShaderResourceView* nullTexture{};
		context->PSSetShaderResources(2, 1, &nullTexture);
	}

private:
	PhysicalWorldParticles& owner;
};

std::uint32_t PhysicalWorldParticles::KindFromClass(std::uint32_t a_renderClass) const noexcept
{
	for (std::uint32_t kind = 0; kind < kKindCount; ++kind) {
		if (renderClasses[kind] == a_renderClass)
			return kind;
	}
	return kKindCount;
}

std::pair<std::string, std::vector<std::string>> PhysicalWorldParticles::GetFeatureSummary()
{
	return {
		T("feature.physical_world_particles.description", "Magic sparks, grass fluff, dandelions, spores, fireflies, and moths."),
		{ T("feature.physical_world_particles.key_feature_1", "One spark type, tinted to the spell color"),
			T("feature.physical_world_particles.key_feature_2", "Separate DDS files per kind, including a moth flipbook"),
			T("feature.physical_world_particles.key_feature_3", "No particle lights; sparks only glow in the shader") }
	};
}

void PhysicalWorldParticles::PostPostLoad()
{
	if (!globals::features::screenSpacePhysicalParticles.loaded) {
		loaded = false;
		failedLoadedMessage = "Physical World Particles requires Screen-Space Physical Particles";
		logger::error("[PhysicalWorldParticles] Screen-Space Physical Particles is not loaded");
		return;
	}
	if (REL::Module::IsVR()) {
		logger::warn("[PhysicalWorldParticles] Magic impact hooks are unavailable on VR");
		return;
	}
	stl::write_vfunc<0xBD, MagicImpactHook<MissileImpactTag>>(RE::VTABLE_MissileProjectile[0]);
	stl::write_vfunc<0xBD, MagicImpactHook<FlameImpactTag>>(RE::VTABLE_FlameProjectile[0]);
	stl::write_vfunc<0xBD, MagicImpactHook<BeamImpactTag>>(RE::VTABLE_BeamProjectile[0]);
	stl::write_vfunc<0xBD, MagicImpactHook<ConeImpactTag>>(RE::VTABLE_ConeProjectile[0]);
	stl::write_vfunc<0xBD, MagicImpactHook<GrenadeImpactTag>>(RE::VTABLE_GrenadeProjectile[0]);
	logger::info("[PhysicalWorldParticles] Installed magic projectile impact hooks");
}

void PhysicalWorldParticles::SetupResources()
{
	if (!globals::features::screenSpacePhysicalParticles.loaded) {
		loaded = false;
		failedLoadedMessage = "Physical World Particles requires Screen-Space Physical Particles";
		return;
	}
	RegisterRuntime();
	EnsureGraphics();
}

void PhysicalWorldParticles::RegisterRuntime()
{
	using namespace ScreenSpacePhysicalParticles::Core;
	if (!renderer)
		renderer = std::make_unique<SharedRenderer>(*this);
	auto& runtime = ParticleRuntime::GetSingleton();
	owner = runtime.RegisterFeature("PhysicalWorldParticles");
	magicRest = runtime.RegisterPersistentList(
		{
			"PhysicalWorld.MagicRest",
			owner,
			0.0F,
			0U,
			BuiltinMaintenancePolicy::kKeepUntilLifetime,
			0U,
		},
		nullptr);

	SimulationProfileGPU magicFlight{};
	magicFlight.acceleration = { 0.0F, 0.0F, -140.0F };
	magicFlight.drag = 0.45F;
	magicFlight.restitution = 0.2F;
	magicFlight.friction = 0.4F;
	magicFlight.settleSpeed = 30.0F;
	magicFlight.surfaceOffset = 0.5F;
	magicFlight.collisionMask =
		static_cast<std::uint32_t>(CollisionChannel::kWorldSolid) |
		static_cast<std::uint32_t>(CollisionChannel::kPlayer);
	magicFlight.queryPolicy =
		static_cast<std::uint32_t>(CollisionQueryPolicy::kPerspectiveDepth) |
		static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField) |
		static_cast<std::uint32_t>(CollisionQueryPolicy::kPlayerCapsules);
	magicFlight.simulationClass = static_cast<std::uint32_t>(SimulationClass::kWorldPlayer);
	const auto magicProfile = runtime.RegisterSimulationProfile("PhysicalWorld.MagicFlight", magicFlight);
	CollisionRuleGPU settle{};
	settle.response = static_cast<std::uint32_t>(CollisionResponse::kSettle);
	settle.destinationListID = magicRest.index;
	settle.restitutionOverride = 0.16F;
	settle.frictionOverride = 0.45F;
	settle.minimumSettleNormalZ = 0.2F;
	runtime.SetCollisionRule(magicProfile, CollisionChannel::kWorldSolid, settle);
	CollisionRuleGPU bounce{};
	bounce.response = static_cast<std::uint32_t>(CollisionResponse::kBounce);
	bounce.restitutionOverride = 0.25F;
	bounce.collisionRadiusScale = 0.15F;
	bounce.minimumSettleNormalZ = -1.0F;
	runtime.SetCollisionRule(magicProfile, CollisionChannel::kPlayer, bounce);

	SimulationProfileGPU rise{};
	rise.acceleration = { 0.0F, 0.0F, 35.0F };
	rise.drag = 0.85F;
	rise.restitution = 0.05F;
	rise.friction = 0.2F;
	rise.settleSpeed = 1000.0F;
	rise.surfaceOffset = 1.0F;
	rise.collisionMask = static_cast<std::uint32_t>(CollisionChannel::kWorldSolid);
	rise.queryPolicy = static_cast<std::uint32_t>(CollisionQueryPolicy::kSolidHeightField);
	rise.simulationClass = static_cast<std::uint32_t>(SimulationClass::kWorld);
	const auto riseProfile = runtime.RegisterSimulationProfile("PhysicalWorld.Rise", rise);

	SimulationProfileGPU drift = rise;
	drift.acceleration = { 0.0F, 0.0F, 8.0F };
	drift.drag = 1.1F;
	const auto driftProfile = runtime.RegisterSimulationProfile("PhysicalWorld.Drift", drift);

	const SimulationProfileID profiles[] = {
		magicProfile, riseProfile, driftProfile, riseProfile, driftProfile, driftProfile
	};
	for (std::uint32_t kind = 0; kind < kKindCount; ++kind) {
		RenderClassDesc desc{};
		desc.name = kRenderKeys[kind];
		desc.drawOrder = 320U + kind;
		desc.vertexCountPerInstance = 4U;
		desc.cullRadiusScale = 1.5F;
		desc.maximumDistance = 3072.0F;
		desc.cullingFlags =
			static_cast<std::uint32_t>(RenderCullFlags::kFrustum) |
			static_cast<std::uint32_t>(RenderCullFlags::kDistance);
		desc.drawProfilerName = "PhysicalWorld::ParticleRender";
		renderClasses[kind] = runtime.RegisterRenderer(kRenderKeys[kind], desc, renderer.get());

		ParticleTemplateGPU particle{};
		particle.simulationProfile = profiles[kind];
		particle.renderClass = renderClasses[kind];
		particle.lifecyclePhase = static_cast<std::uint32_t>(LifecyclePhase::kDynamic);
		particle.persistentListID = kInvalidRoute;
		particle.customInitialization = kInvalidRoute;
		particle.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
		templates[kind] = runtime.RegisterParticleTemplate(kRenderKeys[kind], particle);

		EmissionProfileGPU emission{};
		emission.initialSpeed = kind == 0 ? Range(40.0F, 120.0F) : Range(8.0F, 36.0F);
		emission.horizontalAngleDegrees = Range(0.0F, 360.0F);
		emission.verticalAngleDegrees = Range(0.0F, kind == 0 ? 25.0F : 40.0F);
		emission.lifetime = kind == 0 ? Range(5.0F, 8.0F) : Range(6.0F, 12.0F);
		emission.radius = kind == 0 ? Range(2.2F, 4.5F) : Range(4.0F, 9.0F);
		emission.opacity = Range(0.85F, 1.0F);
		emission.generalScalar = 1.0F;
		emission.spawnShape = static_cast<std::uint32_t>(EmitterShape::kPoint);
		emission.spawnPlacement = static_cast<std::uint32_t>(SpawnPlacement::kDirect);
		emission.trajectoryPolicy = static_cast<std::uint32_t>(EmissionTrajectoryPolicy::kBoundedIndependent);
		emissions[kind] = emission;
	}
	magicTemplate.store(templates[0], std::memory_order_release);
	magicOwner.store(owner, std::memory_order_release);
	registered = templates[0] != kInvalidRoute;
}

void PhysicalWorldParticles::EnsureGraphics()
{
	auto* device = globals::d3d::device;
	if (!device)
		return;
	for (std::uint32_t kind = 0; kind < kKindCount; ++kind) {
		if (!textures[kind].SRV())
			textures[kind].LoadGroup(kGroups[kind]);
	}
	if (!renderCB) {
		renderCB = std::make_unique<ConstantBuffer>(
			ConstantBufferDesc<RenderCB>(),
			"PhysicalWorld::RenderCB");
	}
	if (!sampler) {
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&desc, sampler.put()));
	}
	if (!alphaBlend) {
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = true;
		desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(device->CreateBlendState(&desc, alphaBlend.put()));
	}
	if (!additiveBlend) {
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = true;
		desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DX::ThrowIfFailed(device->CreateBlendState(&desc, additiveBlend.put()));
	}
	if (!depthState) {
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = true;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&desc, depthState.put()));
	}
	if (!rasterizerState) {
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.DepthClipEnable = true;
		DX::ThrowIfFailed(device->CreateRasterizerState(&desc, rasterizerState.put()));
	}
	if (!vertexShader) {
		vertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalWorld\\PhysicalWorldParticles.hlsl",
			{ { "VSHADER", "" } },
			"vs_5_0",
			"VSMain")));
	}
	if (!pixelShader) {
		pixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\PhysicalWorld\\PhysicalWorldParticles.hlsl",
			{ { "PSHADER", "" } },
			"ps_5_0",
			"PSMain")));
	}
}

std::uint32_t PhysicalWorldParticles::TakeBudget(std::uint32_t a_count)
{
	auto available = spawnBudget.load(std::memory_order_relaxed);
	while (available > 0U) {
		const auto taken = std::min(available, a_count);
		if (spawnBudget.compare_exchange_weak(available, available - taken, std::memory_order_relaxed))
			return taken;
	}
	return 0U;
}

bool PhysicalWorldParticles::QueueKind(
	Kind a_kind,
	const RE::NiPoint3& a_origin,
	const RE::NiPoint3& a_forward,
	std::uint32_t a_count,
	float a_generalScalar,
	float a_radiusScale)
{
	using namespace ScreenSpacePhysicalParticles::Core;
	const auto index = static_cast<std::uint32_t>(a_kind);
	if (!registered || index >= kKindCount || templates[index] == kInvalidRoute || a_count == 0U)
		return false;
	const auto granted = TakeBudget(a_count);
	if (granted == 0U)
		return false;

	const RE::NiPoint3 forward = a_forward.SqrLength() > 1.0e-5F ? a_forward : RE::NiPoint3{ 0.0F, 0.0F, 1.0F };
	const auto unit = forward / std::sqrt(forward.SqrLength());
	const RE::NiPoint3 reference = std::abs(unit.z) > 0.95F ? RE::NiPoint3{ 0.0F, 1.0F, 0.0F } : RE::NiPoint3{ 0.0F, 0.0F, 1.0F };
	const auto right = unit.UnitCross(reference);
	const auto vertical = right.UnitCross(unit);
	randomState = randomState * 1664525U + 1013904223U;

	BurstRequestGPU request{};
	request.origin = { a_origin.x, a_origin.y, a_origin.z };
	request.strength = 1.0F;
	request.forward = { unit.x, unit.y, unit.z };
	request.seed = randomState;
	request.right = { right.x, right.y, right.z };
	request.count = granted;
	request.vertical = { vertical.x, vertical.y, vertical.z };
	request.particleTemplate = templates[index];
	request.initialSpeed = emissions[index].initialSpeed;
	request.horizontalAngleDegrees = emissions[index].horizontalAngleDegrees;
	request.verticalAngleDegrees = emissions[index].verticalAngleDegrees;
	request.lifetime = emissions[index].lifetime;
	request.radius = emissions[index].radius;
	request.radius.range.x *= a_radiusScale;
	request.radius.range.y *= a_radiusScale;
	request.opacity = emissions[index].opacity;
	request.generalScalar = a_generalScalar;
	request.ownerFeature = owner;
	request.trajectoryPolicy = emissions[index].trajectoryPolicy;
	request.spawnShape = emissions[index].spawnShape;
	request.spawnPlacement = emissions[index].spawnPlacement;
	return ParticleRuntime::GetSingleton().QueueBurst(request);
}

void PhysicalWorldParticles::OnMagicImpact(RE::Projectile* a_projectile, const RE::NiPoint3& a_position)
{
	if (!a_projectile)
		return;
	auto& particles = globals::features::screenSpacePhysicalParticles;
	if (!particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kMagic))
		return;
	const auto scale = particles.ParticleAmountScale();
	if (scale <= 0.0F)
		return;
	const auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;
	const auto playerPosition = player->GetPosition();
	const auto dx = a_position.x - playerPosition.x;
	const auto dy = a_position.y - playerPosition.y;
	if (dx * dx + dy * dy > 2048.0F * 2048.0F)
		return;

	const auto& runtime = a_projectile->GetProjectileRuntimeData();
	if (!runtime.spell)
		return;
	float color = 1.0F;
	if (!BearforjParticles::TrySpellColor(runtime.spell, color))
		return;

	const auto now = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (impactThrottle.size() > 32) {
		for (auto it = impactThrottle.begin(); it != impactThrottle.end();) {
			if (now - it->second.first > 2.0F)
				it = impactThrottle.erase(it);
			else
				++it;
		}
	}
	const auto formID = a_projectile->GetFormID();
	if (const auto found = impactThrottle.find(formID); found != impactThrottle.end()) {
		const auto age = now - found->second.first;
		const auto moved = a_position.GetSquaredDistance(found->second.second);
		if (age < 0.1F && moved < 32.0F * 32.0F)
			return;
	}
	impactThrottle[formID] = { now, a_position };
	const auto count = std::max(1U, static_cast<std::uint32_t>(std::lround(12.0F * scale)));
	const auto velocity = runtime.velocity;
	QueueKind(Kind::kMagic, a_position, velocity, count, color, 0.55F);
}

void PhysicalWorldParticles::SpawnHandSparks(float a_deltaTime)
{
	auto& particles = globals::features::screenSpacePhysicalParticles;
	if (!particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kMagic))
		return;
	const auto scale = particles.ParticleAmountScale();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player || scale <= 0.0F)
		return;
	const auto hands = {
		RE::MagicSystem::CastingSource::kLeftHand,
		RE::MagicSystem::CastingSource::kRightHand
	};
	std::uint32_t handIndex = 0;
	for (const auto hand : hands) {
		auto* caster = player->GetMagicCaster(hand);
		const bool holding = caster &&
			(caster->state == RE::MagicCaster::State::kReady ||
				caster->state == RE::MagicCaster::State::kCharging ||
				caster->state == RE::MagicCaster::State::kCasting);
		if (!holding || !caster->currentSpell) {
			handAccumulator[handIndex++] = 0.0F;
			continue;
		}
		float color = 1.0F;
		if (!BearforjParticles::TrySpellColor(caster->currentSpell, color)) {
			handAccumulator[handIndex++] = 0.0F;
			continue;
		}
		handAccumulator[handIndex] += a_deltaTime * 2.0F * scale;
		const auto count = static_cast<std::uint32_t>(handAccumulator[handIndex]);
		handAccumulator[handIndex] -= static_cast<float>(count);
		if (count == 0U) {
			++handIndex;
			continue;
		}
		RE::NiPoint3 origin = player->GetPosition();
		origin.z += 96.0F;
		if (auto* node = caster->GetMagicNode())
			origin = node->world.translate;
		QueueKind(Kind::kMagic, origin, { 0.0F, 0.0F, -1.0F }, count, color, 0.28F);
		++handIndex;
	}
}

void PhysicalWorldParticles::SpawnAmbient()
{
	auto& particles = globals::features::screenSpacePhysicalParticles;
	const auto scale = particles.ParticleAmountScale();
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell || scale <= 0.0F)
		return;
	const auto playerPosition = player->GetPosition();
	const bool evening = BearforjParticles::IsEvening();
	const bool night = BearforjParticles::IsNight();
	const bool water = BearforjParticles::CurrentCellHasWater();
	const bool grassLand = CellHasGrass(cell);
	const auto distanceScale = [](const RE::NiPoint3& a_position, const RE::NiPoint3& a_player) {
		const auto dx = a_position.x - a_player.x;
		const auto dy = a_position.y - a_player.y;
		const auto distance = std::sqrt(dx * dx + dy * dy);
		return std::clamp(1.0F - distance / 2048.0F, 0.0F, 1.0F);
	};

	auto visit = [&](RE::TESObjectCELL* a_cell) {
		if (!a_cell)
			return;
		a_cell->ForEachReferenceInRange(playerPosition, 2048.0F, [&](RE::TESObjectREFR* a_ref) {
			if (!a_ref)
				return RE::BSContainer::ForEachResult::kContinue;
			const auto position = a_ref->GetPosition();
			const auto falloff = distanceScale(position, playerPosition) * scale;
			if (falloff <= 0.05F)
				return RE::BSContainer::ForEachResult::kContinue;
			randomState = randomState * 1664525U + 1013904223U;
			if (BearforjParticles::IsGrassReference(a_ref)) {
				if (particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kFluff))
					QueueKind(Kind::kFluff, position, { 0.0F, 0.0F, 1.0F }, 1U, 0.0F, 1.0F);
				if ((randomState & 3U) == 0U &&
					particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kDandelion))
					QueueKind(Kind::kDandelion, position, { 0.2F, 0.1F, 1.0F }, 1U, 0.0F, 1.0F);
			}
			if (particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kSpores) &&
				BearforjParticles::IsGlowingMushroom(a_ref)) {
				auto origin = position;
				origin.z += 24.0F;
				QueueKind(Kind::kSpores, origin, { 0.0F, 0.0F, 1.0F }, 1U, 0.0F, 1.0F);
			}
			if (evening && particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kFireflies) &&
				BearforjParticles::IsGrassReference(a_ref)) {
				QueueKind(Kind::kFireflies, position, { 0.0F, 0.0F, 0.2F }, water ? 2U : 1U, 0.0F, 0.7F);
			}
			if (night && particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kMoths) &&
				BearforjParticles::IsLampReference(a_ref)) {
				auto origin = position;
				origin.z += 18.0F;
				QueueKind(Kind::kMoths, origin, { 0.0F, 1.0F, 0.2F }, 1U, 0.0F, 1.0F);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	};
	visit(cell);
	if (grassLand) {
		auto origin = playerPosition;
		origin.z -= 8.0F;
		if (particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kFluff))
			QueueKind(Kind::kFluff, origin, { 0.0F, 0.0F, 1.0F }, 2U, 0.0F, 1.0F);
		if (particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kDandelion))
			QueueKind(Kind::kDandelion, origin, { 0.3F, 0.0F, 1.0F }, 1U, 0.0F, 1.0F);
		if (evening && water &&
			particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kFireflies))
			QueueKind(Kind::kFireflies, origin, { 0.0F, 0.2F, 0.4F }, 2U, 0.0F, 0.7F);
	}

	if (night && particles.IsKindEnabled(ScreenSpacePhysicalParticlesFeature::ParticleKind::kMoths)) {
		for (const auto left : { false, true }) {
			const auto* equipped = player->GetEquippedObject(left);
			if (!equipped || equipped->GetFormType() != RE::FormType::Light)
				continue;
			auto origin = player->GetPosition();
			origin.z += 110.0F;
			QueueKind(Kind::kMoths, origin, { 1.0F, 0.0F, 0.3F }, 1U, 0.0F, 0.8F);
		}
	}
}

void PhysicalWorldParticles::PostComposite()
{
	if (!registered)
		RegisterRuntime();
	EnsureGraphics();
	const auto scale = globals::features::screenSpacePhysicalParticles.ParticleAmountScale();
	spawnBudget.store(
		scale <= 0.0F ? 0U : static_cast<std::uint32_t>(std::lround(48.0F * scale)),
		std::memory_order_relaxed);
	const auto deltaTime = globals::game::ui->GameIsPaused() ? 0.0F : *globals::game::deltaTime;
	SpawnHandSparks(deltaTime);
	ambientTimer += deltaTime;
	if (ambientTimer < 0.45F)
		return;
	ambientTimer = 0.0F;
	SpawnAmbient();
}

void PhysicalWorldParticles::ClearShaderCache()
{
	vertexShader = nullptr;
	pixelShader = nullptr;
}

void PhysicalWorldParticles::DrawSettings()
{
	ImGui::TextWrapped(
		"Kinds are switched on the Screen-Space Physical Particles page. Textures are individual 2048 DXT5 files in Data\\Shaders\\PhysicalParticles\\.");
	for (std::uint32_t kind = 0; kind < kKindCount; ++kind) {
		ImGui::TextDisabled(
			"%s: %u slices",
			kGroups[kind],
			textures[kind].SliceCount());
	}
}
