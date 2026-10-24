#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

namespace ScreenSpacePhysicalParticles
{
	// One Texture2DArray built by copying cells out of a 2048 DXT5 atlas.
	// particles.json lists the atlas, grid, and cell for every slice. The C++
	// table is the fallback when that file cannot be read.
	class ParticleTextureSet
	{
	public:
		struct Slice
		{
			std::string id;
			std::string species;
			std::string region;
			std::string frame;
			std::string path;
			std::uint32_t cell{};
			std::uint32_t grid{ 4 };
			bool tint{ true };
		};

		// Summary: Loads one config group by copying atlas cells into a texture array.
		// Args:
		//   a_group: particles.json group name, such as "leaves" or "sparks".
		// Returns: True when at least one cell was uploaded.
		bool LoadGroup(std::string_view a_group);

		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return srv.get(); }
		[[nodiscard]] std::uint32_t SliceCount() const noexcept
		{
			return static_cast<std::uint32_t>(slices.size());
		}
		[[nodiscard]] const std::vector<Slice>& Slices() const noexcept { return slices; }
		// Summary: Returns the first slice that must not receive a region tint.
		// Args: None.
		// Returns: Needle slice index, or the slice count when every slice is tinted.
		[[nodiscard]] std::uint32_t UntintedSliceBase() const noexcept;

		// Summary: Resolves one leaf frame inside the neutral leaf atlas.
		// Args:
		//   a_species: aspen, birch, oak, maple, or pine.
		//   a_region: Kept for callers. Region selects a tint, not a different cell.
		//   a_frame: Variant or flipbook frame, "01" through "04".
		// Returns: Slice index. Pine uses the needle cells. Other species ignore region.
		[[nodiscard]] std::uint32_t Find(
			std::string_view a_species,
			std::string_view a_region,
			std::string_view a_frame) const noexcept;
		// Summary: Resolves frame 01 of a species.
		// Args:
		//   a_species: Leaf species key.
		//   a_region: Unused texture key. Region tint is applied in the leaf shader.
		// Returns: Slice index of frame 01. Pine returns the first needle cell.
		[[nodiscard]] std::uint32_t Index(std::string_view a_species, std::string_view a_region) const noexcept;

	private:
		std::vector<Slice> slices;
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool loaded{};
	};
}
