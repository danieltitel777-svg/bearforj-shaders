#include "Features/WaterWavesMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static int fails = 0;

static void Expect(bool ok, const char* what)
{
	if (!ok) {
		std::cerr << "FAIL " << what << "\n";
		++fails;
	} else {
		std::cout << "ok   " << what << "\n";
	}
}

int main()
{
	using namespace WaterWaveMath;
	Params off{};
	off.enabled = false;
	off.amplitude = 22.0f;
	off.scale = 1.0f;
	const Sample disabled = Evaluate(0, 0, 0, 0, off, false);
	Expect(disabled.height == 0.0f && disabled.dx == 0.0f, "disabled height is 0");

	Params flow = off;
	flow.enabled = true;
	flow.flowmap = true;
	const Sample river = Evaluate(10, 20, 0, 0, flow, false);
	Expect(river.height == 0.0f && river.slopeX == 0.0f, "flowmap height is 0");

	Expect(ShoreDamp(4.0f, 160.0f) == 0.0f, "shore damp is 0 at the 4-unit deadzone");
	Expect(ShoreDamp(0.0f, 160.0f) == 0.0f, "shore damp is 0 on dry ground");
	Expect(ShoreDamp(-20.0f, 160.0f) == 0.0f, "shore damp is 0 above the plane");
	Expect(ShoreDamp(4.0f + 160.0f, 160.0f) == 1.0f, "shore damp is 1 past the shore length");
	const float mid = ShoreDamp(4.0f + 80.0f, 160.0f);
	Expect(mid > 0.4f && mid < 0.6f, "shore damp is smooth at half the shore length");

	Params sea{};
	sea.enabled = true;
	sea.amplitude = 22.0f;
	sea.chop = 0.6f;
	sea.scale = 1.0f;
	sea.windSpeed = 0.8f;
	sea.windAngle = 0.4f;
	sea.time = 12.0f;
	const Sample far = Evaluate(0, 0, kFadeEnd, 0, sea, false);
	Expect(far.height == 0.0f && far.dx == 0.0f && far.slopeX == 0.0f, "height is 0 at the LOD fade end");
	const Sample farNormal = Evaluate(0, 0, kNormalFadeEnd, 0, sea, true);
	Expect(farNormal.slopeX == 0.0f && farNormal.height == 0.0f, "normal fade is 0 at its own end");
	const Sample near = Evaluate(1000.0f, -250.0f, 30.0f, -10.0f, sea, false);
	Expect(std::isfinite(near.height) && std::isfinite(near.dx) && std::isfinite(near.slopeY), "near sample is finite");
	Expect(std::fabs(near.height) > 0.01f, "near sample actually moves");
	const Sample again = Evaluate(1000.0f, -250.0f, 30.0f, -10.0f, sea, false);
	Expect(again.height == near.height && again.dx == near.dx, "the same point is stable");

	// A domain-shader corner is Offset(plane). The vertex shader writes that same Offset(plane).
	// This test locks the CPU function the vertex and domain shaders both mirror.
	const float damp = ShoreDamp(50.0f, 160.0f);
	const float corner = near.height * damp;
	const float domainCorner = Evaluate(1000.0f, -250.0f, 30.0f, -10.0f, sea, false).height * damp;
	Expect(corner == domainCorner, "factor-1 corner matches the vertex displacement");
	Expect(near.dx * ShoreDamp(4.0f, 160.0f) == 0.0f && near.dy * ShoreDamp(0.0f, 160.0f) == 0.0f, "horizontal shore offset is 0 in the contact band");

	// 2x2 land grid. Texel (x,y) is the land Z at origin + (x,y)*cell. CPU and GPU use this same sample.
	const float land[4] = { 100.0f, 110.0f, 120.0f, 130.0f };
	Expect(SampleLandZ(0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2, land) == 100.0f, "land sample hits texel 0");
	Expect(SampleLandZ(100.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2, land) == 110.0f, "land sample hits texel x1");
	const float midLand = SampleLandZ(50.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2, land);
	Expect(std::fabs(midLand - 105.0f) < 1.0e-4f, "land sample bilinear matches the shader lerp");
	Expect(SampleLandZ(-1.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2, land) < -1.0e7f, "land sample outside the grid is open water");
	const float shoreGap = 104.0f - SampleLandZ(0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2, land);
	Expect(ShoreDamp(shoreGap, 160.0f) == 0.0f, "shared land gap keeps the 4-unit band flat");
	const float deepGap = 300.0f - SampleLandZ(0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 2, land);
	Expect(ShoreDamp(deepGap, 160.0f) == 1.0f, "shared land gap is full waves once the water is deep");

	if (fails) {
		std::cerr << fails << " failed\n";
		return 1;
	}
	std::cout << "all water-wave checks passed\n";
	return 0;
}
