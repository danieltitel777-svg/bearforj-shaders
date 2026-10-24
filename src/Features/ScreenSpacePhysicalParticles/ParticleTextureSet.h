#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

namespace ScreenSpacePhysicalParticles
{
	// One Texture2DArray built from the individual DDS files of a particles.json group.
	// Every slice in a group must share width, height, and format. Paths come from the
	// JSON list, with the C++ path table as the fallback when the file cannot be read.
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
		};

		// Summary: Loads one config group into a texture array.
		// Args:
		//   a_group: particles.json group name, such as "leaves" or "sparks".
		// Returns: True when at least one slice was uploaded.
		bool LoadGroup(std::string_view a_group);

		[[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return srv.get(); }
		[[nodiscard]] std::uint32_t SliceCount() const noexcept
		{
			return static_cast<std::uint32_t>(slices.size());
		}
		[[nodiscard]] const std::vector<Slice>& Slices() const noexcept { return slices; }

		// Summary: Resolves one leaf frame. Missing files keep their config index.
		// Args:
		//   a_species: aspen, birch, oak, maple, or pine.
		//   a_region: Region-set key, including neutral and pine_hjaal.
		//   a_frame: Variant or flipbook frame, "01" through "04".
		// Returns: Slice index. Unknown sets fall back to neutral, then oak neutral.
		[[nodiscard]] std::uint32_t Find(
			std::string_view a_species,
			std::string_view a_region,
			std::string_view a_frame) const noexcept;
		// Summary: Resolves frame 01 of a species and region set.
		// Args:
		//   a_species: Leaf species key.
		//   a_region: Region-set key.
		// Returns: Slice index of frame 01 after neutral fallback.
		[[nodiscard]] std::uint32_t Index(std::string_view a_species, std::string_view a_region) const noexcept;

	private:
		std::vector<Slice> slices;
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool loaded{};
	};
}
