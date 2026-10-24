#pragma once

#include <cstddef>

namespace ScreenSpacePhysicalParticles
{
	// Shipped DDS atlases under Data\Shaders\PhysicalParticles\.
	// particles.json names every cell. The 36 source images are packed into
	// these three 2048 DXT5 straight-alpha atlases and are added at integration.
	extern const char* const kParticleTexturePaths[];
	extern const std::size_t kParticleTexturePathCount;
	extern const char* const kParticleTextureConfigPath;
}
