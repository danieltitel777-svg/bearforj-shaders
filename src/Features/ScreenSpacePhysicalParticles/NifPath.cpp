#include "NifPath.h"

#include <cctype>

// Implements allocation-free filename/validation queries over the canonical
// string produced by Normalize.
namespace ScreenSpacePhysicalParticles::NifPath
{
	std::string Normalize(std::string_view a_modelPath)
	{
		while (!a_modelPath.empty() && std::isspace(static_cast<unsigned char>(a_modelPath.front())))
			a_modelPath.remove_prefix(1);
		while (!a_modelPath.empty() && std::isspace(static_cast<unsigned char>(a_modelPath.back())))
			a_modelPath.remove_suffix(1);
		if (a_modelPath.size() >= 2 &&
			((a_modelPath.front() == '"' && a_modelPath.back() == '"') ||
			 (a_modelPath.front() == '\'' && a_modelPath.back() == '\''))) {
			a_modelPath.remove_prefix(1);
			a_modelPath.remove_suffix(1);
		}

		std::string normalized;
		normalized.reserve(a_modelPath.size());
		bool previousSeparator = false;
		for (const auto character : a_modelPath) {
			const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			const bool separator = lowered == '\\' || lowered == '/';
			if (separator) {
				if (!previousSeparator)
					normalized.push_back('\\');
			} else {
				normalized.push_back(lowered);
			}
			previousSeparator = separator;
		}

		while (!normalized.empty() && normalized.front() == '\\')
			normalized.erase(normalized.begin());
		bool strippedPrefix = true;
		while (strippedPrefix) {
			strippedPrefix = false;
			if (normalized.starts_with(".\\")) {
				normalized.erase(0, 2);
				strippedPrefix = true;
			}
			if (normalized.starts_with("data\\")) {
				normalized.erase(0, 5);
				strippedPrefix = true;
			}
			if (normalized.starts_with("meshes\\")) {
				normalized.erase(0, 7);
				strippedPrefix = true;
			}
			while (!normalized.empty() && normalized.front() == '\\') {
				normalized.erase(normalized.begin());
				strippedPrefix = true;
			}
		}
		return normalized;
	}

	std::string_view Filename(std::string_view a_normalizedPath) noexcept
	{
		const auto separator = a_normalizedPath.find_last_of('\\');
		return separator == std::string_view::npos ? a_normalizedPath : a_normalizedPath.substr(separator + 1);
	}

	bool IsValid(std::string_view a_normalizedPath, std::size_t a_maxLength) noexcept
	{
		if (a_normalizedPath.empty() || a_normalizedPath.size() > a_maxLength ||
			!a_normalizedPath.ends_with(".nif") || Filename(a_normalizedPath).size() <= 4 ||
			a_normalizedPath.contains(':')) {
			return false;
		}

		std::size_t segmentStart{};
		while (segmentStart <= a_normalizedPath.size()) {
			const auto segmentEnd = a_normalizedPath.find('\\', segmentStart);
			const auto segment = a_normalizedPath.substr(
				segmentStart,
				segmentEnd == std::string_view::npos ? std::string_view::npos : segmentEnd - segmentStart);
			if (segment.empty() || segment == "." || segment == "..")
				return false;
			if (segmentEnd == std::string_view::npos)
				break;
			segmentStart = segmentEnd + 1;
		}
		return true;
	}
}
