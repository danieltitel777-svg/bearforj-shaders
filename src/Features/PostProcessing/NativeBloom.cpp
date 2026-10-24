#include "NativeBloom.h"

#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NativeBloom::Settings,
	Threshold,
	UpsampleRadius,
	BlendFactor,
	MipBlendFactor)

void NativeBloom::DrawSettings()
{
	ImGui::SliderFloat(T("feature.post_processing.codbloom.threshold", "Threshold"), &settings.Threshold, -7.f, 23.f, "%+.2f EV100");
	ImGui::SliderFloat(T("feature.post_processing.codbloom.upsampling_radius", "Upsampling Radius"), &settings.UpsampleRadius, 1.f, 5.f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.codbloom.a_greater_radius_makes_the_bloom_slightly_blurrier", "A greater radius makes the bloom slightly blurrier."));

	ImGui::SliderFloat(T("feature.post_processing.codbloom.mix", "Mix"), &settings.BlendFactor, 0.f, 1.f, "%.2f");

	ImGui::Separator();

	static int mipLevel = 1;
	const int maxMipLevel = static_cast<int>(settings.MipBlendFactor.size());
	if (mipLevel > maxMipLevel)
		mipLevel = maxMipLevel;
	ImGui::SliderInt(T("feature.post_processing.codbloom.mip_level", "Mip Level"), &mipLevel, 1, maxMipLevel, "%d", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.codbloom.the_greater_the_level_the_blurrier_the_part", "The greater the level, the blurrier the part it controls"));
	ImGui::Indent();
	{
		ImGui::SliderFloat(T("feature.post_processing.codbloom.intensity", "Intensity"), &settings.MipBlendFactor[mipLevel - 1], 0.f, 1.f, "%.2f");
	}
	ImGui::Unindent();

	if (Util::ShowDebugSections() && ImGui::CollapsingHeader(T("feature.post_processing.codbloom.debug", "Debug"))) {
		static int mip = 0;
		ImGui::SliderInt(T("feature.post_processing.codbloom.debug_mip_level", "Debug Mip Level"), &mip, 0, (int)s_BloomMips - 1, "%d", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);

		if (texBloom) {
			ImGui::BulletText(T("feature.post_processing.codbloom.texbloom", "texBloom"));
			ImGui::Image(texBloomMipSRVs[mip].get(), { texBloom->desc.Width * .2f, texBloom->desc.Height * .2f });
		}
	}
}

void NativeBloom::RestoreDefaultSettings()
{
	settings = {};
}

void NativeBloom::LoadSettings(json& o_json)
{
	settings = o_json;
}

void NativeBloom::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NativeBloom::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		bloomCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<BloomCB>(), "NativeBloom::Constants");
	}

	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = s_BloomMips;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texBloom = std::make_unique<Texture2D>(texDesc, "NativeBloom::Bloom");
		texBloom->CreateSRV(srvDesc);

		for (uint i = 0; i < s_BloomMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};
			texBloomMipSRVs[i] = nullptr;
			DX::ThrowIfFailed(device->CreateShaderResourceView(texBloom->resource.get(), &mipSrvDesc, texBloomMipSRVs[i].put()));
			Util::SetResourceName(texBloomMipSRVs[i].get(), "NativeBloom::Bloom SRV mip%u", i);
		}

		for (uint i = 0; i < s_BloomMips; i++) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};
			texBloomMipUAVs[i] = nullptr;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texBloom->resource.get(), &mipUavDesc, texBloomMipUAVs[i].put()));
			Util::SetResourceName(texBloomMipUAVs[i].get(), "NativeBloom::Bloom UAV mip%u", i);
		}
	}

	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
			.AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
			.AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};

		colorSampler = nullptr;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, colorSampler.put()));
		Util::SetResourceName(colorSampler.get(), "NativeBloom::ColorSampler");
	}

	CompileComputeShaders();
}

void NativeBloom::ClearShaderCache()
{
	outputReady = false;
	BumpShaderGeneration();
	{
		std::lock_guard lock(shaderMutex);
		thresholdCS = nullptr;
		downsampleCS = nullptr;
		downsampleFirstMipCS = nullptr;
		upsampleCS = nullptr;
		compositeCS = nullptr;
	}

	globals::shaderCache->InvalidateShaderSources();
	CompileComputeShaders();
}

void NativeBloom::CompileComputeShaders()
{
	const std::vector<ComputeShaderCompileInfo> shaderInfos = {
		{ &thresholdCS, "bloom.cs.hlsl", {}, "CS_Threshold" },
		{ &downsampleCS, "bloom.cs.hlsl", {}, "CS_Downsample" },
		{ &downsampleFirstMipCS, "bloom.cs.hlsl", { { "FIRST_MIP", "" } }, "CS_Downsample" },
		{ &upsampleCS, "bloom.cs.hlsl", {}, "CS_Upsample" },
		{ &compositeCS, "bloom.cs.hlsl", {}, "CS_Composite" }
	};

	CompileComputeShadersAsync(L"Data\\Shaders\\PostProcessing\\CODBloom", shaderInfos);
}

void NativeBloom::Draw(TextureInfo& inout_tex)
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	// Same entry points the compute bloom Draw dispatches. CS_Composite is compiled
	// and not dispatched; mip 0 is the bloom result Composite samples.
	if (!AllShadersReady({ &thresholdCS, &downsampleFirstMipCS, &downsampleCS, &upsampleCS }))
		return;

	state->BeginPerfEvent("Native Bloom");

	BloomCB cbData = {
		.Threshold = exp2(settings.Threshold - 3.0f),
		.UpsampleRadius = settings.UpsampleRadius,
		.UpsampleMult = 1.f,
		.CurrentMipMult = 1.f
	};
	bloomCB->Update(cbData);

	std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 1> samplers = { colorSampler.get() };
	auto cb = bloomCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	{
		globals::profiler->BeginPass("PostProcessing::NativeBloom::Threshold");
		srvs.at(0) = inout_tex.srv;
		uavs.at(0) = texBloomMipUAVs[0].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(thresholdCS.get(), nullptr, 0);
		context->Dispatch(((texBloom->desc.Width - 1) >> 5) + 1, ((texBloom->desc.Height - 1) >> 5) + 1, 1);
		globals::profiler->EndPass();
	}

	{
		globals::profiler->BeginPass("PostProcessing::NativeBloom::Downsample");
		context->CSSetShader(downsampleFirstMipCS.get(), nullptr, 0);
		for (int i = 0; i < static_cast<int>(s_BloomMips) - 1; i++) {
			resetViews();

			srvs.at(1) = texBloomMipSRVs[i].get();
			uavs.at(0) = texBloomMipUAVs[i + 1].get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

			if (i == 1)
				context->CSSetShader(downsampleCS.get(), nullptr, 0);

			uint mipWidth = texBloom->desc.Width >> (i + 1);
			uint mipHeight = texBloom->desc.Height >> (i + 1);
			context->Dispatch(((mipWidth - 1) >> 5) + 1, ((mipHeight - 1) >> 5) + 1, 1);
		}
		globals::profiler->EndPass();
	}

	{
		globals::profiler->BeginPass("PostProcessing::NativeBloom::Upsample");
		context->CSSetShader(upsampleCS.get(), nullptr, 0);
		for (int i = static_cast<int>(s_BloomMips) - 2; i >= 1; i--) {
			resetViews();

			cbData.UpsampleMult = 1.f;
			if (i == static_cast<int>(s_BloomMips) - 2)
				cbData.UpsampleMult = settings.MipBlendFactor[i];
			cbData.CurrentMipMult = settings.MipBlendFactor[i - 1];
			bloomCB->Update(cbData);

			srvs.at(1) = texBloomMipSRVs[i + 1].get();
			uavs.at(0) = texBloomMipUAVs[i].get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

			uint mipWidth = texBloom->desc.Width >> i;
			uint mipHeight = texBloom->desc.Height >> i;
			context->Dispatch(((mipWidth - 1) >> 5) + 1, ((mipHeight - 1) >> 5) + 1, 1);
		}

		resetViews();

		cbData.UpsampleMult = settings.BlendFactor;
		cbData.CurrentMipMult = 0.f;
		bloomCB->Update(cbData);

		srvs.at(1) = texBloomMipSRVs[1].get();
		uavs.at(0) = texBloomMipUAVs[0].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->Dispatch(((texBloom->desc.Width - 1) >> 5) + 1, ((texBloom->desc.Height - 1) >> 5) + 1, 1);
		globals::profiler->EndPass();
	}

	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texBloom->resource.get(), texBloomMipSRVs[0].get() };

	state->EndPerfEvent();
	outputReady = true;
}
