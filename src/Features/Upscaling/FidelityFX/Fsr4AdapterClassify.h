#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

// Name rules for runtime FSR Upscaling 4.1. The description is already ASCII-uppercased.
// RX 6000 is Navi 21/22/23/24 only. Integrated Radeon parts and other series stay unsupported.
namespace BearforjFsr4
{
	enum class Support
	{
		Unsupported,
		Rx6000,
		Rx7000,
		Rx9000
	};

	inline bool ContainsText(const std::string& a_description, const char* a_needle)
	{
		return a_description.find(a_needle) != std::string::npos;
	}

	inline bool HasDiscreteRxToken(const std::string& a_description)
	{
		for (size_t i = 0; i + 1 < a_description.size(); ++i) {
			if (a_description[i] != 'R' || a_description[i + 1] != 'X')
				continue;
			const bool leftOk = i == 0 || !std::isalnum(static_cast<unsigned char>(a_description[i - 1]));
			const bool rightOk = i + 2 >= a_description.size() || !std::isalpha(static_cast<unsigned char>(a_description[i + 2]));
			if (leftOk && rightOk)
				return true;
		}
		return false;
	}

	inline bool IsNavi6000Die(const std::string& a_description)
	{
		for (const char* die : { "NAVI21", "NAVI 21", "NAVI22", "NAVI 22", "NAVI23", "NAVI 23", "NAVI24", "NAVI 24" }) {
			if (ContainsText(a_description, die))
				return true;
		}
		return false;
	}

	// Radeon 680M / 780M style names are integrated GPUs. A discrete "RX 6600M" is four digits and is left alone.
	inline bool IsThreeDigitMModel(const std::string& a_description)
	{
		for (size_t i = 0; i + 3 < a_description.size(); ++i) {
			if (!std::isdigit(static_cast<unsigned char>(a_description[i])))
				continue;
			if (i > 0 && std::isdigit(static_cast<unsigned char>(a_description[i - 1])))
				continue;
			if (!std::isdigit(static_cast<unsigned char>(a_description[i + 1])) ||
				!std::isdigit(static_cast<unsigned char>(a_description[i + 2])) ||
				std::isdigit(static_cast<unsigned char>(a_description[i + 3])))
				continue;
			if (a_description[i + 3] == 'M' &&
				(i + 4 >= a_description.size() || !std::isalnum(static_cast<unsigned char>(a_description[i + 4]))))
				return true;
		}
		return false;
	}

	inline bool IsIntegratedAmdDescription(const std::string& a_description)
	{
		if (IsNavi6000Die(a_description) || HasDiscreteRxToken(a_description))
			return false;
		if (IsThreeDigitMModel(a_description))
			return true;
		return ContainsText(a_description, "RYZEN") ||
		       ContainsText(a_description, "GRAPHICS") ||
		       ContainsText(a_description, "APU") ||
		       ContainsText(a_description, "IGPU") ||
		       ContainsText(a_description, "INTEGRATED");
	}

	inline bool IsAbbreviatedRx6000(const std::string& a_description)
	{
		for (const char* token : { "RX 64", "RX64", "RX 65", "RX65", "RX 66", "RX66", "RX 67", "RX67", "RX 68", "RX68", "RX 69", "RX69" }) {
			if (ContainsText(a_description, token))
				return true;
		}
		return false;
	}

	inline Support ClassifyUpper(uint32_t a_vendorId, const std::string& a_description)
	{
		constexpr uint32_t kAmdVendorId = 0x1002u;
		if (a_vendorId != kAmdVendorId)
			return Support::Unsupported;

		// Pre-RDNA "HD"-branded GCN cards (e.g. "Radeon HD 7970") fall in the same 7000-7999
		// numeric range as RX 7000 without being FSR 4.1.1-capable; reject them before the scan.
		if (a_description.find("RADEON HD") != std::string::npos)
			return Support::Unsupported;

		// A Ryzen APU string can contain 6600/6900 from the CPU name. Discrete dies are named on their own.
		if (ContainsText(a_description, "RYZEN") && !IsNavi6000Die(a_description))
			return Support::Unsupported;

		// Ryzen iGPUs and "Radeon Graphics" share numbers with discrete cards. Navi 21-24 and an RX token stay.
		if (IsIntegratedAmdDescription(a_description))
			return Support::Unsupported;

		// Navi 21/22/23/24 are the discrete RX 6000 dies (including RX 6950 XT). Bare "RDNA 2" is not used:
		// that string also matches integrated RDNA2.
		if (IsNavi6000Die(a_description))
			return Support::Rx6000;

		// Only accept explicit discrete RDNA3 dies here, never a bare "RDNA 3"/"RDNA3" marker --
		// that also matches RDNA3 integrated GPUs, which FSR 4.1.1 does not support.
		if (a_description.find("NAVI31") != std::string::npos ||
			a_description.find("NAVI 31") != std::string::npos ||
			a_description.find("NAVI32") != std::string::npos ||
			a_description.find("NAVI 32") != std::string::npos ||
			a_description.find("NAVI33") != std::string::npos ||
			a_description.find("NAVI 33") != std::string::npos) {
			return Support::Rx7000;
		}

		if (a_description.find("RDNA4") != std::string::npos ||
			a_description.find("RDNA 4") != std::string::npos ||
			a_description.find("NAVI4") != std::string::npos ||
			a_description.find("NAVI 4") != std::string::npos) {
			return Support::Rx9000;
		}

		const bool discreteRx = HasDiscreteRxToken(a_description);
		size_t searchPosition = 0;
		while (searchPosition < a_description.length()) {
			while (searchPosition < a_description.length() && !std::isdigit(static_cast<unsigned char>(a_description[searchPosition])))
				searchPosition++;

			const size_t modelStart = searchPosition;
			while (searchPosition < a_description.length() && std::isdigit(static_cast<unsigned char>(a_description[searchPosition])))
				searchPosition++;

			if (searchPosition > modelStart) {
				const std::string modelText = a_description.substr(modelStart, searchPosition - modelStart);
				char* parseEnd = nullptr;
				const unsigned long modelNumber = std::strtoul(modelText.c_str(), &parseEnd, 10);
				if (parseEnd != modelText.c_str()) {
					// 6400 through 6950 XT. 6300 and older Navi 2 parts are not this class.
					if (discreteRx && modelNumber >= 6400ul && modelNumber <= 6999ul)
						return Support::Rx6000;
					if (modelNumber >= 7000ul && modelNumber < 8000ul)
						return Support::Rx7000;
					if (modelNumber >= 9000ul && modelNumber < 10000ul)
						return Support::Rx9000;
				}
			}
		}

		if (discreteRx && IsAbbreviatedRx6000(a_description))
			return Support::Rx6000;

		// Keep fallbacks for abbreviated naming variants that don't include full numeric model text.
		if (a_description.find("RX 70") != std::string::npos ||
			a_description.find("RX70") != std::string::npos ||
			a_description.find("RADEON 70") != std::string::npos) {
			return Support::Rx7000;
		}
		if (a_description.find("RX 90") != std::string::npos ||
			a_description.find("RX90") != std::string::npos ||
			a_description.find("RADEON 90") != std::string::npos) {
			return Support::Rx9000;
		}

		return Support::Unsupported;
	}
}
