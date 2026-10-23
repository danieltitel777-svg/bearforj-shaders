#pragma once

/**
 * @brief Bearforj 0.6 world context for the living-world particles (plan step 2).
 *
 * Refreshed on the CPU about twice a second (cheap), uploaded to the GPU with the particle frame.
 * Everything is derived from engine data that every mod uses - weather classification/flags,
 * location keywords, loaded references - never from specific weather/lighting mod FormIDs,
 * so NAT III CS, NAT, Azurite, Obsidian, Cathedral, Vespro, Rustic, etc. all work the same.
 */
namespace Bearforj::Context
{
	enum LocFlags : uint32_t
	{
		kLocCave = 1 << 0,
		kLocDungeon = 1 << 1,  // dungeon / ruin / nordic ruin / draugr crypt
		kLocMine = 1 << 2,
		kLocCity = 1 << 3,     // city / town / settlement / habitation
		kLocDwemer = 1 << 4,
		kLocInterior = 1 << 5,
	};

	struct State
	{
		uint32_t loc = 0;
		int weatherSlot = 0;       // WeatherPresets::Slot
		float hour = 12.0f;        // game hour 0..24
		float trees = 0.0f;        // 0..1 deciduous tree density around the player
		float conifers = 0.0f;     // 0..1 conifer density
		float cold = 0.0f;         // 0..1 (snow weather / snowy region)
		bool night = false;
	};

	/** @brief Throttled update (0.5 s). Call every frame from the render thread. */
	void Update();
	const State& Get();
}
