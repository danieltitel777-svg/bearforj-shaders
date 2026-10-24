#include "ParticleTextureSet.h"

#include "Features/ScreenSpacePhysicalParticles/BearforjWorldQuery.h"
#include "Features/ScreenSpacePhysicalParticles/ParticleTexturePaths.h"
#include "Globals.h"
#include "Utils/D3D.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace ScreenSpacePhysicalParticles
{
	namespace
	{
		struct AtlasInfo
		{
			std::string path;
			std::uint32_t grid{ 4 };
		};

		struct LoadedAtlas
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			UINT width{};
			UINT height{};
			DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
			std::uint32_t grid{ 4 };
		};

		void AddCell(
			std::vector<ParticleTextureSet::Slice>& a_slices,
			std::string_view a_group,
			std::string_view a_wanted,
			const char* a_id,
			const char* a_species,
			const char* a_frame,
			const char* a_region,
			const char* a_path,
			std::uint32_t a_cell,
			std::uint32_t a_grid,
			bool a_tint)
		{
			if (a_group != a_wanted)
				return;
			ParticleTextureSet::Slice slice;
			slice.id = a_id;
			slice.species = a_species;
			slice.frame = a_frame;
			slice.region = a_region;
			slice.path = a_path;
			slice.cell = a_cell;
			slice.grid = a_grid;
			slice.tint = a_tint;
			a_slices.push_back(std::move(slice));
		}

		std::vector<ParticleTextureSet::Slice> FallbackSlices(std::string_view a_group)
		{
			const char* leaves = kParticleTexturePaths[0];
			const char* sparks = kParticleTexturePaths[1];
			const char* small = kParticleTexturePaths[2];
			std::vector<ParticleTextureSet::Slice> slices;
			const auto add4 = [&](const char* a_groupName, const char* a_prefix, const char* a_species, const char* a_region, const char* a_path, std::uint32_t a_base, std::uint32_t a_grid, bool a_tint) {
				for (std::uint32_t frame = 0; frame < 4; ++frame) {
					const auto name = std::string(a_prefix) + (frame == 0 ? "01" : frame == 1 ? "02" :
					                                                          frame == 2 ? "03" :
					                                                                       "04");
					AddCell(slices, a_group, a_groupName, name.c_str(), a_species, frame == 0 ? "01" : frame == 1 ? "02" :
					                                                                              frame == 2 ? "03" :
					                                                                                           "04",
						a_region, a_path, a_base + frame, a_grid, a_tint);
				}
			};
			add4("leaves", "leaf_oak_", "oak", "", leaves, 0, 4, true);
			add4("leaves", "leaf_birch_", "birch", "", leaves, 4, 4, true);
			add4("leaves", "leaf_aspen_", "aspen", "", leaves, 8, 4, true);
			add4("leaves", "leaf_maple_", "maple", "", leaves, 12, 4, true);
			add4("leaves", "needles_pine_hjaal_", "pine", "pine_hjaal", small, 10, 4, false);
			add4("sparks", "spark_", "", "", sparks, 0, 2, true);
			AddCell(slices, a_group, "fluff", "fluff_01", "", "01", "", small, 0, 4, true);
			AddCell(slices, a_group, "fluff", "fluff_02", "", "02", "", small, 1, 4, true);
			AddCell(slices, a_group, "dandelions", "dandelion_01", "", "01", "", small, 14, 4, true);
			AddCell(slices, a_group, "dandelions", "dandelion_02", "", "02", "", small, 15, 4, true);
			AddCell(slices, a_group, "spores", "spore_01", "", "01", "", small, 2, 4, true);
			AddCell(slices, a_group, "spores", "spore_cluster_01", "", "01", "", small, 3, 4, true);
			AddCell(slices, a_group, "fireflies", "firefly_01", "", "01", "", small, 4, 4, true);
			AddCell(slices, a_group, "fireflies", "firefly_glow_01", "", "01", "", small, 5, 4, true);
			add4("moths", "moth_frame_", "", "", small, 6, 4, true);
			return slices;
		}

		void ApplyRegionTints(const nlohmann::json& a_document)
		{
			const auto tints = a_document.find("regionTints");
			if (tints == a_document.end() || !tints->is_object())
				return;
			for (std::uint32_t index = 0; index < BearforjParticles::kRegionCount; ++index) {
				const auto entry = tints->find(BearforjParticles::kRegionNames[index]);
				if (entry == tints->end() || !entry->is_array() || entry->size() < 3)
					continue;
				BearforjParticles::SetRegionTint(
					index,
					entry->at(0).get<float>(),
					entry->at(1).get<float>(),
					entry->at(2).get<float>());
			}
		}

		std::unordered_map<std::string, AtlasInfo> ReadAtlases(const nlohmann::json& a_document)
		{
			std::unordered_map<std::string, AtlasInfo> atlases;
			const auto entries = a_document.find("atlases");
			if (entries == a_document.end() || !entries->is_object())
				return atlases;
			for (const auto& [key, value] : entries->items()) {
				if (!value.is_object() || !value.contains("path") || !value["path"].is_string())
					continue;
				AtlasInfo info;
				info.path = value["path"].get<std::string>();
				info.grid = value.value("grid", 4U);
				atlases.emplace(key, std::move(info));
			}
			return atlases;
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
			ApplyRegionTints(document);
			const auto atlases = ReadAtlases(document);
			const auto group = document.find(std::string(a_group));
			if (group == document.end() || !group->is_array())
				return {};

			std::vector<ParticleTextureSet::Slice> slices;
			slices.reserve(group->size());
			for (const auto& entry : *group) {
				if (!entry.is_object() || !entry.contains("atlas") || !entry["atlas"].is_string())
					continue;
				const auto atlas = atlases.find(entry["atlas"].get<std::string>());
				if (atlas == atlases.end())
					continue;
				ParticleTextureSet::Slice slice;
				slice.path = atlas->second.path;
				slice.id = entry.value("id", std::string{});
				slice.species = entry.value("species", std::string{});
				slice.region = entry.value("region", std::string{});
				slice.frame = entry.value("frame", std::string{});
				slice.cell = entry.value("cell", 0U);
				slice.grid = atlas->second.grid;
				const bool defaultTint = slice.species != "pine" && slice.id.rfind("needles_", 0) != 0;
				slice.tint = entry.value("tint", defaultTint);
				slices.push_back(std::move(slice));
			}
			return slices;
		}

		bool LoadAtlas(ID3D11Device* a_device, const ParticleTextureSet::Slice& a_slice, LoadedAtlas& a_atlas)
		{
			ID3D11Texture2D* rawTexture = nullptr;
			ID3D11ShaderResourceView* rawView = nullptr;
			const auto result = Util::LoadTextureFromFile(
				a_device,
				std::filesystem::path(a_slice.path),
				&rawTexture,
				&rawView);
			if (FAILED(result) || !rawTexture) {
				logger::error(
					"[PhysicalParticles] Failed to load atlas {} ({:#010x})",
					a_slice.path,
					static_cast<std::uint32_t>(result));
				if (rawView)
					rawView->Release();
				return false;
			}
			if (rawView)
				rawView->Release();
			a_atlas.texture.attach(rawTexture);
			D3D11_TEXTURE2D_DESC desc{};
			a_atlas.texture->GetDesc(&desc);
			if (desc.Width != 2048 || desc.Height != 2048 || a_slice.grid == 0 || desc.Width % a_slice.grid != 0) {
				logger::error(
					"[PhysicalParticles] {} is {}x{} grid {}, expected a 2048 atlas",
					a_slice.path,
					desc.Width,
					desc.Height,
					a_slice.grid);
				a_atlas.texture = nullptr;
				return false;
			}
			a_atlas.width = desc.Width;
			a_atlas.height = desc.Height;
			a_atlas.format = desc.Format;
			a_atlas.grid = a_slice.grid;
			return true;
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
				"[PhysicalParticles] {} missing or empty for group {}; using the built-in atlas layout",
				kParticleTextureConfigPath,
				a_group);
			slices = FallbackSlices(a_group);
		}
		if (slices.empty()) {
			logger::error("[PhysicalParticles] No atlas cells for group {}", a_group);
			return false;
		}

		std::unordered_map<std::string, LoadedAtlas> atlases;
		for (const auto& slice : slices) {
			if (atlases.contains(slice.path))
				continue;
			LoadedAtlas atlas;
			if (!LoadAtlas(device, slice, atlas))
				atlas = {};
			atlases.emplace(slice.path, std::move(atlas));
		}

		struct Source
		{
			bool copied{};
		};
		std::vector<Source> sources(slices.size());
		D3D11_TEXTURE2D_DESC expected{};
		bool haveExpected = false;
		std::size_t loadedCount = 0;
		std::size_t donor = 0;
		const auto cellBox = [](const LoadedAtlas& a_atlas, std::uint32_t a_cell) {
			const UINT cell = a_atlas.width / a_atlas.grid;
			D3D11_BOX box{};
			box.left = (a_cell % a_atlas.grid) * cell;
			box.top = (a_cell / a_atlas.grid) * cell;
			box.front = 0;
			box.right = box.left + cell;
			box.bottom = box.top + cell;
			box.back = 1;
			return box;
		};

		for (std::size_t index = 0; index < slices.size(); ++index) {
			const auto atlas = atlases.find(slices[index].path);
			if (atlas == atlases.end() || !atlas->second.texture)
				continue;
			const auto cellCount = atlas->second.grid * atlas->second.grid;
			if (slices[index].cell >= cellCount) {
				logger::error(
					"[PhysicalParticles] {} cell {} is outside the {}x{} atlas",
					slices[index].id,
					slices[index].cell,
					atlas->second.grid,
					atlas->second.grid);
				continue;
			}
			const UINT cell = atlas->second.width / atlas->second.grid;
			if (!haveExpected) {
				expected.Width = cell;
				expected.Height = cell;
				expected.Format = atlas->second.format;
				expected.MipLevels = 1;
				expected.ArraySize = 1;
				expected.SampleDesc.Count = 1;
				haveExpected = true;
				donor = index;
			} else if (cell != expected.Width || atlas->second.format != expected.Format) {
				logger::error(
					"[PhysicalParticles] {} does not match the {} group cell size or format",
					slices[index].id,
					a_group);
				continue;
			}
			sources[index].copied = true;
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

		const auto copyCell = [&](std::size_t a_index, std::size_t a_source) {
			const auto atlas = atlases.find(slices[a_source].path);
			if (atlas == atlases.end() || !atlas->second.texture)
				return;
			const auto box = cellBox(atlas->second, slices[a_source].cell);
			const UINT destination = D3D11CalcSubresource(0, static_cast<UINT>(a_index), 1);
			context->CopySubresourceRegion(
				texture.get(),
				destination,
				0,
				0,
				0,
				atlas->second.texture.get(),
				0,
				&box);
		};
		for (std::size_t index = 0; index < sources.size(); ++index)
			copyCell(index, sources[index].copied ? index : donor);

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
			"[PhysicalParticles] Copied {}/{} atlas cells for {}",
			loadedCount,
			slices.size(),
			a_group);
		return true;
	}

	std::uint32_t ParticleTextureSet::UntintedSliceBase() const noexcept
	{
		for (std::uint32_t index = 0; index < slices.size(); ++index) {
			if (!slices[index].tint)
				return index;
		}
		return slices.empty() ? 0xFFFFFFFFU : static_cast<std::uint32_t>(slices.size());
	}

	std::uint32_t ParticleTextureSet::Find(
		std::string_view a_species,
		std::string_view a_region,
		std::string_view a_frame) const noexcept
	{
		(void)a_region;
		const auto find = [&](std::string_view a_wantedSpecies, std::string_view a_wantedFrame) -> std::uint32_t {
			for (std::uint32_t index = 0; index < slices.size(); ++index) {
				if (slices[index].species == a_wantedSpecies &&
					(a_wantedFrame.empty() || slices[index].frame == a_wantedFrame))
					return index;
			}
			return UINT32_MAX;
		};
		if (a_species == "pine") {
			if (const auto needles = find("pine", a_frame); needles != UINT32_MAX)
				return needles;
			if (const auto needles = find("pine", {}); needles != UINT32_MAX)
				return needles;
		}
		if (const auto exact = find(a_species, a_frame); exact != UINT32_MAX)
			return exact;
		if (const auto anyFrame = find(a_species, {}); anyFrame != UINT32_MAX)
			return anyFrame;
		if (const auto oak = find("oak", a_frame); oak != UINT32_MAX)
			return oak;
		if (const auto oak = find("oak", {}); oak != UINT32_MAX)
			return oak;
		return 0;
	}

	std::uint32_t ParticleTextureSet::Index(std::string_view a_species, std::string_view a_region) const noexcept
	{
		return Find(a_species, a_region, "01");
	}
}
