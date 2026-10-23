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

	const WaveResponse calm = WaveResponseAt(0.0f);
	const WaveResponse breeze = WaveResponseAt(0.42f);
	const WaveResponse storm = WaveResponseAt(1.05f);
	const WaveResponse pastStorm = WaveResponseAt(1.5f);
	Expect(calm.amplitude < breeze.amplitude && breeze.amplitude < storm.amplitude, "calm swell is smaller than a breeze, which is smaller than a storm");
	Expect(calm.spread > breeze.spread && breeze.spread > storm.spread, "spread narrows as the wind rises so the swell runs with the wind");
	Expect(calm.length < breeze.length && breeze.length < storm.length, "wavelength grows with the wind");
	Expect(std::fabs(breeze.amplitude - 0.72f) < 1.0e-4f && std::fabs(breeze.storm - 0.08f) < 1.0e-4f, "the two weather segments meet at the breeze anchor");
	Expect(std::fabs(pastStorm.amplitude - storm.amplitude) < 1.0e-4f && std::fabs(pastStorm.storm - 1.0f) < 1.0e-4f, "wind past the storm anchor stays on the storm");
	Expect(disabled.jacobian == 1.0f && river.jacobian == 1.0f, "disabled and flowmap water keep a flat jacobian");

	Params flat = sea;
	flat.chop = 0.0f;
	flat.windSpeed = 1.2f;
	bool flatFold = true;
	for (int i = 0; i < 16 && flatFold; ++i) {
		flat.time = static_cast<float>(i) * 0.37f;
		const Sample s = Evaluate(static_cast<float>(i) * 80.0f, static_cast<float>(i) * -35.0f, 10.0f, 4.0f, flat, false);
		if (std::fabs(s.jacobian - 1.0f) > 1.0e-3f)
			flatFold = false;
	}
	Expect(flatFold, "chop 0 leaves the jacobian at 1");

	Params steep = sea;
	steep.chop = 1.5f;
	steep.windSpeed = 1.2f;
	steep.amplitude = 40.0f;
	float lowestJ = 1.0f;
	for (int i = 0; i < 48; ++i) {
		steep.time = static_cast<float>(i) * 0.17f;
		const Sample s = Evaluate(1200.0f + static_cast<float>(i) * 17.0f, -400.0f, 20.0f, 8.0f, steep, false);
		lowestJ = std::min(lowestJ, s.jacobian);
	}
	Expect(lowestJ < 0.9f, "a steep storm folds at least one crest");

	const WaveRgb scene{ 0.4f, 0.2f, 0.1f };
	const WaveRgb white{ 1.0f, 1.0f, 1.0f };
	const WaveRgb blue{ 0.0f, 0.0f, 1.0f };
	const WaveRgb shallow = WaveAbsorb(scene, 0.0f, 0.0f, 0.0f, white, white);
	Expect(std::fabs(shallow.r - scene.r) < 1.0e-5f && std::fabs(shallow.g - scene.g) < 1.0e-5f && std::fabs(shallow.b - scene.b) < 1.0e-5f, "a zero path returns the scene");
	Expect(WaveOpacity(0.0f, 0.0f) == 0.0f, "a zero path is fully clear");
	const float shallowOpacity = WaveOpacity(40.0f, 0.0f);
	const float deepOpacity = WaveOpacity(900.0f, 0.0f);
	Expect(shallowOpacity < 0.35f, "a short path stays mostly clear");
	Expect(deepOpacity > 0.75f, "a long path is mostly opaque");
	Expect(WaveOpacity(200.0f, 1.0f) > WaveOpacity(200.0f, 0.0f), "murk hides the bottom sooner than clear water");

	const WaveRgb shallowTint = WaveScatter(0.0f, 0.0f, white, white);
	Expect(shallowTint.g > shallowTint.r && shallowTint.b > shallowTint.r && shallowTint.r < 0.1f, "clear shallows scatter turquoise, not red");
	const WaveRgb deepTint = WaveScatter(800.0f, 0.0f, white, white);
	Expect(deepTint.r < 0.05f && deepTint.g < 0.1f && deepTint.b < 0.1f, "deep scatter stays dark");
	const WaveRgb authoredBlue = WaveScatter(800.0f, 0.0f, blue, blue);
	const float deepLum = authoredBlue.r * 0.25f + authoredBlue.g * 0.50f + authoredBlue.b * 0.25f;
	Expect(deepLum < 0.2f, "an authored blue only tints the deep scatter, it does not replace it with a flat blue");
	const WaveRgb mud = WaveScatter(0.0f, 1.0f, white, white);
	Expect(mud.r > shallowTint.r && mud.g < shallowTint.g, "rain and storm murk shifts the shallow scatter off turquoise");

	if (fails) {
		std::cerr << fails << " failed\n";
		return 1;
	}
	std::cout << "all water-wave checks passed\n";
	return 0;
}
