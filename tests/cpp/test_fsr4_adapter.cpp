#include "Features/Upscaling/FidelityFX/Fsr4AdapterClassify.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace
{
	int g_failures = 0;

	void Expect(const char* a_name, BearforjFsr4::Support a_actual, BearforjFsr4::Support a_expected)
	{
		if (a_actual == a_expected)
			return;
		std::fprintf(stderr, "%s: got %d, expected %d\n", a_name, static_cast<int>(a_actual), static_cast<int>(a_expected));
		++g_failures;
	}

	BearforjFsr4::Support Class(const char* a_name)
	{
		std::string upper = a_name;
		for (char& c : upper)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return BearforjFsr4::ClassifyUpper(0x1002u, upper);
	}
}

int main()
{
	using S = BearforjFsr4::Support;
	Expect("6950 XT", Class("AMD Radeon RX 6950 XT"), S::Rx6000);
	Expect("6900 XT", Class("AMD Radeon RX 6900 XT"), S::Rx6000);
	Expect("6800", Class("AMD Radeon RX 6800"), S::Rx6000);
	Expect("6700 XT", Class("AMD Radeon RX 6700 XT"), S::Rx6000);
	Expect("6600 XT", Class("AMD Radeon RX 6600 XT"), S::Rx6000);
	Expect("6600M", Class("AMD Radeon RX 6600M"), S::Rx6000);
	Expect("6500 XT", Class("AMD Radeon RX 6500 XT"), S::Rx6000);
	Expect("6400", Class("AMD Radeon RX 6400"), S::Rx6000);
	Expect("navi 21", Class("AMD Navi 21"), S::Rx6000);
	Expect("NAVI24", Class("NAVI24"), S::Rx6000);
	Expect("7900 XTX", Class("AMD Radeon RX 7900 XTX"), S::Rx7000);
	Expect("7800 XT", Class("AMD Radeon RX 7800 XT"), S::Rx7000);
	Expect("7600", Class("AMD Radeon RX 7600"), S::Rx7000);
	Expect("navi 31", Class("Navi 31"), S::Rx7000);
	Expect("9070 XT", Class("AMD Radeon RX 9070 XT"), S::Rx9000);
	Expect("rdna 4", Class("AMD RDNA 4"), S::Rx9000);
	Expect("navi 44", Class("NAVI 44"), S::Rx9000);
	Expect("hd 7970", Class("AMD Radeon HD 7970"), S::Unsupported);
	Expect("5700 XT", Class("AMD Radeon RX 5700 XT"), S::Unsupported);
	Expect("6300", Class("AMD Radeon RX 6300"), S::Unsupported);
	Expect("680M", Class("AMD Radeon 680M"), S::Unsupported);
	Expect("780M", Class("AMD Radeon 780M Graphics"), S::Unsupported);
	Expect("radeon graphics", Class("AMD Radeon(TM) Graphics"), S::Unsupported);
	Expect("ryzen 6600H", Class("AMD Ryzen 5 6600H with Radeon Graphics"), S::Unsupported);
	Expect("rdna 2", Class("AMD RDNA 2"), S::Unsupported);
	Expect("rdna 3", Class("AMD RDNA 3"), S::Unsupported);
	Expect("steam deck", Class("AMD Custom GPU 0405"), S::Unsupported);
	Expect("nvidia", BearforjFsr4::ClassifyUpper(0x10DEu, "NVIDIA GEFORCE RTX 4090"), S::Unsupported);
	return g_failures == 0 ? 0 : 1;
}
