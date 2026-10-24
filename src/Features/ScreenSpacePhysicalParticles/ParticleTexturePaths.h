#pragma once

#include <cstddef>

namespace ScreenSpacePhysicalParticles
{
	// Every individual DDS under Data\Shaders\PhysicalParticles\.
	// particles.json is the runtime list; this array is the same set so the
	// package audit can see each path from C++. Files are 2048 DXT5 straight
	// alpha and are added at integration.
	extern const char* const kParticleTexturePaths[];
	extern const std::size_t kParticleTexturePathCount;
	extern const char* const kParticleTextureConfigPath;
}
