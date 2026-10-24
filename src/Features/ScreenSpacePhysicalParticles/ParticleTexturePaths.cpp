#include "ParticleTexturePaths.h"

#include <cstddef>

namespace ScreenSpacePhysicalParticles
{
	const char* const kParticleTexturePaths[] = {
		R"(Data\Shaders\PhysicalParticles\leaves_atlas.dds)",
		R"(Data\Shaders\PhysicalParticles\sparks_atlas.dds)",
		R"(Data\Shaders\PhysicalParticles\small_atlas.dds)",
	};

	const std::size_t kParticleTexturePathCount = sizeof(kParticleTexturePaths) / sizeof(kParticleTexturePaths[0]);

	const char* const kParticleTextureConfigPath = R"(Data\Shaders\PhysicalParticles\particles.json)";
}
