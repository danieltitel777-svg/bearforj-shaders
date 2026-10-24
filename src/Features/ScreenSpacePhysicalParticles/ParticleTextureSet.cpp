#include "ParticleTextureSet.h"

#include "Features/ScreenSpacePhysicalParticles/ParticleTexturePaths.h"
#include "Globals.h"
#include "Utils/D3D.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace ScreenSpacePhysicalParticles
{
	namespace
	{
		struct FallbackSpan
		{
			const char* group;
			std::size_t begin;
			std::size_t count;
		};

		constexpr FallbackSpan kFallbackSpans[] = {
			{ "leaves", 0, 96 },
			{ "sparks", 96, 4 },
			{ "fluff", 100, 2 },
			{ "dandelions", 102, 2 },
			{ "spores", 104, 2 },
			{ "fireflies", 106, 2 },
			{ "moths", 108, 4 },
		};

		std::string FileStem(std::string_view a_path)
		{
			const auto slash = a_path.find_last_of("\\/");
			auto name = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
			const auto dot = name.find_last_of('.');
			if (dot != std::string_view::npos)
				name = name.substr(0, dot);
			return std::string(name);
		}

		void ParseLeafName(
			const std::string& a_stem,
			std::string& a_species,
			std::string& a_region,
			std::string& a_frame)
		{
			if (a_stem.starts_with("needles_")) {
				a_species = "pine";
				a_region = "pine_hjaal";
				const auto split = a_stem.rfind('_');
				if (split != std::string::npos)
					a_frame = a_stem.substr(split + 1);
				return;
			}
			constexpr std::string_view kPrefix = "leaf_";
			if (!a_stem.starts_with(kPrefix))
				return;
			const auto body = std::string_view(a_stem).substr(kPrefix.size());
			const auto frameSplit = body.rfind('_');
			if (frameSplit == std::string_view::npos)
				return;
			a_frame = std::string(body.substr(frameSplit + 1));
			const auto rest = body.substr(0, frameSplit);
			const auto speciesSplit = rest.find('_');
			if (speciesSplit == std::string_view::npos)
				return;
			a_species = std::string(rest.substr(0, speciesSplit));
			a_region = std::string(rest.substr(speciesSplit + 1));
		}

		std::vector<ParticleTextureSet::Slice> FallbackSlices(std::string_view a_group)
		{
			std::vector<ParticleTextureSet::Slice> slices;
			for (const auto& span : kFallbackSpans) {
				if (a_group != span.group)
					continue;
				const auto end = std::min(span.begin + span.count, kParticleTexturePathCount);
				slices.reserve(end - span.begin);
				for (std::size_t index = span.begin; index < end; ++index) {
					ParticleTextureSet::Slice slice;
					slice.path = kParticleTexturePaths[index];
					slice.id = FileStem(slice.path);
					if (a_group == "leaves")
						ParseLeafName(slice.id, slice.species, slice.region, slice.frame);
					slices.push_back(std::move(slice));
				}
				break;
			}
			return slices;
		}

		std::vector<ParticleTextureSet::Slice> ConfigSlices(std::string_view a_group)
		{
			std::ifstream input(kParticleTextureConfigPath);
			if (!input)
				return {};
			nlohmann::json document;
			try {
				input >> document;
			} catch (const std::exception& error) {
				logger::error("[PhysicalParticles] Failed to parse {}: {}", kParticleTextureConfigPath, error.what());
				return {};
			}
			const auto group = document.find(std::string(a_group));
			if (group == document.end() || !group->is_array())
				return {};

			std::vector<ParticleTextureSet::Slice> slices;
			slices.reserve(group->size());
			for (const auto& entry : *group) {
				if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string())
					continue;
				ParticleTextureSet::Slice slice;
				slice.path = entry["path"].get<std::string>();
				slice.id = entry.value("id", FileStem(slice.path));
				slice.species = entry.value("species", std::string{});
				slice.region = entry.value("region", std::string{});
				slice.frame = entry.value("frame", std::string{});
				if (a_group == "leaves" && slice.species.empty())
					ParseLeafName(slice.id, slice.species, slice.region, slice.frame);
				slices.push_back(std::move(slice));
			}
			return slices;
		}
	}

	bool ParticleTextureSet::LoadGroup(std::string_view a_group)
	{
		if (loaded && srv)
			return true;

		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context)
			return false;

		slices = ConfigSlices(a_group);
		if (slices.empty()) {
			logger::warn(
				"[PhysicalParticles] {} missing or empty for group {}; using the built-in path list",
				kParticleTextureConfigPath,
				a_group);
			slices = FallbackSlices(a_group);
		}
		if (slices.empty()) {
			logger::error("[PhysicalParticles] No texture paths for group {}", a_group);
			return false;
		}

		struct Source
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
		};
		std::vector<Source> sources(slices.size());
		D3D11_TEXTURE2D_DESC expected{};
		bool haveExpected = false;
		std::size_t loadedCount = 0;
		std::size_t donor = 0;
		for (std::size_t index = 0; index < slices.size(); ++index) {
			ID3D11Texture2D* rawTexture = nullptr;
			ID3D11ShaderResourceView* rawView = nullptr;
			const auto result = Util::LoadTextureFromFile(
				device,
				std::filesystem::path(slices[index].path),
				&rawTexture,
				&rawView);
			if (FAILED(result) || !rawTexture) {
				logger::error(
					"[PhysicalParticles] Failed to load {} ({:#010x})",
					slices[index].path,
					static_cast<std::uint32_t>(result));
				if (rawView)
					rawView->Release();
				continue;
			}
			if (rawView)
				rawView->Release();

			winrt::com_ptr<ID3D11Texture2D> textureSlot;
			textureSlot.attach(rawTexture);
			D3D11_TEXTURE2D_DESC desc{};
			textureSlot->GetDesc(&desc);
			if (desc.Width == 0 || desc.Height == 0 || desc.Width > 2048 || desc.Height > 2048) {
				logger::error(
					"[PhysicalParticles] {} is {}x{}, outside 1..2048",
					slices[index].path,
					desc.Width,
					desc.Height);
				continue;
			}
			if (!haveExpected) {
				expected = desc;
				haveExpected = true;
				donor = index;
			} else if (desc.Width != expected.Width || desc.Height != expected.Height ||
					   desc.Format != expected.Format) {
				logger::error(
					"[PhysicalParticles] {} does not match the {} group size or format",
					slices[index].path,
					a_group);
				continue;
			}
			sources[index].texture = std::move(textureSlot);
			++loadedCount;
		}

		if (!haveExpected || loadedCount == 0) {
			slices.clear();
			return false;
		}

		D3D11_TEXTURE2D_DESC arrayDesc = expected;
		arrayDesc.MipLevels = 1;
		arrayDesc.ArraySize = static_cast<UINT>(slices.size());
		arrayDesc.SampleDesc.Count = 1;
		arrayDesc.SampleDesc.Quality = 0;
		arrayDesc.Usage = D3D11_USAGE_DEFAULT;
		arrayDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		arrayDesc.CPUAccessFlags = 0;
		arrayDesc.MiscFlags = 0;
		if (FAILED(device->CreateTexture2D(&arrayDesc, nullptr, texture.put()))) {
			logger::error("[PhysicalParticles] Failed to create the {} texture array", a_group);
			slices.clear();
			texture = nullptr;
			return false;
		}

		for (std::size_t index = 0; index < sources.size(); ++index) {
			auto* source = sources[index].texture ? sources[index].texture.get() : sources[donor].texture.get();
			if (!source)
				continue;
			const UINT destination = D3D11CalcSubresource(0, static_cast<UINT>(index), 1);
			context->CopySubresourceRegion(texture.get(), destination, 0, 0, 0, source, 0, nullptr);
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		viewDesc.Format = arrayDesc.Format;
		viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		viewDesc.Texture2DArray.MostDetailedMip = 0;
		viewDesc.Texture2DArray.MipLevels = 1;
		viewDesc.Texture2DArray.FirstArraySlice = 0;
		viewDesc.Texture2DArray.ArraySize = arrayDesc.ArraySize;
		if (FAILED(device->CreateShaderResourceView(texture.get(), &viewDesc, srv.put()))) {
			logger::error("[PhysicalParticles] Failed to create the {} texture-array view", a_group);
			texture = nullptr;
			srv = nullptr;
			slices.clear();
			return false;
		}

		loaded = true;
		logger::info(
			"[PhysicalParticles] Loaded {}/{} individual DDS slices for {}",
			loadedCount,
			slices.size(),
			a_group);
		return true;
	}

	std::uint32_t ParticleTextureSet::Find(
		std::string_view a_species,
		std::string_view a_region,
		std::string_view a_frame) const noexcept
	{
		const auto find = [&](std::string_view a_wantedSpecies,
							  std::string_view a_wantedRegion,
							  std::string_view a_wantedFrame) -> std::uint32_t {
			for (std::uint32_t index = 0; index < slices.size(); ++index) {
				if (slices[index].species == a_wantedSpecies &&
					slices[index].region == a_wantedRegion &&
					(a_wantedFrame.empty() || slices[index].frame == a_wantedFrame))
					return index;
			}
			return UINT32_MAX;
		};
		if (const auto exact = find(a_species, a_region, a_frame); exact != UINT32_MAX)
			return exact;
		if (const auto anyFrame = find(a_species, a_region, {}); anyFrame != UINT32_MAX)
			return anyFrame;
		if (a_species == "pine") {
			if (const auto needles = find("pine", "pine_hjaal", a_frame); needles != UINT32_MAX)
				return needles;
			if (const auto needles = find("pine", "pine_hjaal", {}); needles != UINT32_MAX)
				return needles;
		}
		if (const auto neutral = find(a_species, "neutral", a_frame); neutral != UINT32_MAX)
			return neutral;
		if (const auto neutral = find(a_species, "neutral", {}); neutral != UINT32_MAX)
			return neutral;
		if (const auto oak = find("oak", "neutral", a_frame); oak != UINT32_MAX)
			return oak;
		if (const auto oak = find("oak", "neutral", {}); oak != UINT32_MAX)
			return oak;
		return 0;
	}

	std::uint32_t ParticleTextureSet::Index(std::string_view a_species, std::string_view a_region) const noexcept
	{
		return Find(a_species, a_region, "01");
	}
}
