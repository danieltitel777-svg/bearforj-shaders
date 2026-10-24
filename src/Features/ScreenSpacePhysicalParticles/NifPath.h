#pragma once

#include <string>
#include <string_view>

// Canonicalizes model paths used as stable NIF-hijack lookup keys. Normalized
// paths are lowercase, backslash-delimited, and relative to Data\Meshes.
namespace ScreenSpacePhysicalParticles::NifPath
{
	// Summary: Normalizes a model path for lookup and validation.
	// Args:
	//   a_modelPath: Path that may contain quotes, whitespace, mixed separators,
	//     or Data/Meshes prefixes.
	// Returns: Canonical lowercase path relative to Data\Meshes.
	[[nodiscard]] std::string Normalize(std::string_view a_modelPath);

	// Summary: Extracts the final component from a normalized NIF path.
	// Args:
	//   a_normalizedPath: Backslash-delimited canonical path.
	// Returns: A view of the filename within a_normalizedPath.
	[[nodiscard]] std::string_view Filename(std::string_view a_normalizedPath) noexcept;

	// Summary: Validates a normalized, relative NIF path.
	// Args:
	//   a_normalizedPath: Canonical path returned by Normalize.
	//   a_maxLength: Maximum accepted path length.
	// Returns: True for a non-traversing relative path ending in a named .nif file.
	[[nodiscard]] bool IsValid(std::string_view a_normalizedPath, std::size_t a_maxLength = 260) noexcept;
}
