# Bearforj Shaders vb 1.0 checklist

Branch `bearforj/vb-1.0` from `bearforj/1-base` (`967ea4c`). Feature histories were replayed. They were not merged. Jiaye, D7ry, and 0.6.7 trees were not merged in.

## Order

| Step | What landed | Where |
| --- | --- | --- |
| 1 Base | `bearforj/1-base` | `967ea4c` |
| 6 Deformation | Cherry-picked before the later features because it did not touch their files | `24e0bb95`, `a7dfe148` |
| 2 FSR | RX 6000 defaults FSR 4.1 on. Menu name follows a confirmed 4.x driver. FSR 3 frame generation stays. Bottled FidelityFX DLLs were not replaced | `5749c1eb` |
| 3 Water | Tessellated swells, shore, crest, actor, and flow foam. Calm open water has no foam. FeetCoat no longer adds leg snow | `c506f2a6` |
| 4 Wetness | Character drops, wet hair, one Advanced Skin drying timer. Held-weapon coat intensity is 0 | this integration |
| 5 Puddles | Volumetric puddles on landscape parallax when Skylighting is on. `Raindrops.png` on PS t66 | this integration |
| 7 Particles | SSPP, blood, leaves, world particles, three DXT5 atlases | this integration |

## Decisions

- `Lighting.hlsl` keeps `#if defined(SKIN) || defined(HAIR)` for rain wetness. Bottled hair specular and backlighting blocks stay.
- One actor wetness value. Rain sets Advanced Skin wetness and a waterline of 4096 so the whole body, head, and hair get wet. Drying uses `WetFadeTime` (10s). Sweat thresholds stay StartSweat 0.75 and FullSweat 0.15. Weather transition speed stays 3.0 with slider 0.2–8.0.
- `hair_sss.dds` is the wet-hair look only, bound on PS t75. That register is skin wetness normal only inside `SKIN && CS_SKIN`. The hair texture is declared only under `HAIR`.
- Held weapons still get drops and spot darkening. `characterCoatIntensity` is 0, so `EvaluateLighting` adds no extra specular.
- Spray droplets use `sparks_atlas.dds`. `WaterWaves` enables spray when that file loads.
- Actor snow does not add FeetCoat leg snow. Mud (`feet.y`) and `EICold::Snow` stay. Rain #610 and `RainDrop.png` stay.
- No OIT, OptiScaler, old SSPP, WorldSnow, old DTD, Windswept, needles-as-snow, snowdrift, roof drops, embers, or wind reaction.

## Shader behavior added on this branch

- Environment vertical drops use the wetness-branch streak shading. `EnvironmentRetainedWetness` and the slope sliders drive it. Drop size, speed, density, layering, and normal strength stay at that branch's defaults, because this constant buffer has no wall-rain sliders. Character rain surfaces are skipped. Flat surfaces keep ordinary wetness.
- Volumetric puddle floors shift the sampled albedo with a local `GetParallaxSampleOffset`. Edge darkening and the raindrop normal stay.
- Fur wetness reads Advanced Skin's shared rain coat (`Skin::GetWetness` `.y`, saturated) from the fur pass buffer. It no longer uses the global Skin Wetness slider.

## Still absent

- Terrain Variation `Default.json` is not in Bottled `CommunityShaders_AIO-2026-10-21T07-10Z.7z`, Jiaye `CommunityShaders_AIO-2026-10-08T11-42Z.7z`, or mainline `CommunityShaders_AIO-2026-10-24T13-39Z.7z` (the v1.9.1 feature package is the same). `LoadSettings` only assigns the settings JSON. No file was added. Open Shaders v2.19.0 shipped `TerrainVariation/MeshRules/Default.json`; commit `4bd1db0` removed that file and its loader to match upstream.
- `rgbnoise.dds` and `starburst.dds` are in this tree and in Open Shaders Post Processing textures. No shader or C++ file in either tree loads those names.
- The plan lists a Jiaye Physical Sky LUT as a file. Jiaye `pp` has no Physical Sky feature or LUT, and this branch has no shader that samples one. The feature is not in the vb 1.0 integration order, so it was not ported. `default ll psky.json` is a post-processing preset already in the tree.

## Lighting::40100001

Open Shaders stores `FurShells` at bit 30, so `0x40100001` is vertex color, alpha test, and fur shells. That permutation's vertex shader is `main(VS_INPUT)` with `SV_INSTANCEID` on the input, and the pixel shader uses an `earlydepthstencil` macro. Both now match Open Shaders. In this tree bit 30 is `NoSnowCover`, so the same hex is vertex color, alpha test, and no snow cover. That path already omits `SNOW_COVER` and still compiles actor snow.
- Source PNGs are 2048. Atlas cells are 512 or 1024. `tools/bearforj/build_particle_atlases.py` integer-box downscales them. Pixel content is not redrawn.

## Slots

| Slot | Owner | Notes |
| --- | --- | --- |
| PS t66 | `TexPuddleRaindrops` in lighting wetness | Free on lighting. Not t75 |
| PS t66 | Physical blood water density | Water pixel shader only, under `PHYSICAL_BLOOD` |
| PS t75 | `HairSSSTex` | Hair permutations only |
| PS t75 | Skin wetness normal | `SKIN && CS_SKIN` only |
| Water t59, t107, t111, t113, b13 | Shore gap, foam, foam height, spark atlas, shore waves | Water shader, not lighting |
| Water b10, s12 | Blood water field | Water pixel shader. Lighting s12 is the rim sampler. Lighting b10 is fur or bones, not this cbuffer |
| Extra flags bit 29, bit 30 | Held weapon, character rain surface | Clear of bits 0–20 |

## Assets this integration adds

- `features/Wetness Effects/Shaders/WetnessEffects/hair_sss.dds`
- `features/Wetness Effects/Shaders/WetnessEffects/Raindrops.png`
- `features/Wetness Effects/Shaders/WetnessEffects/CharacterRainSpots.hlsli`
- `features/Wetness Effects/Shaders/WetnessEffects/CharacterRainLighting.hlsli`
- `features/Wetness Effects/Shaders/WetnessEffects/WetHair.hlsli`
- `features/Water Waves/textures/ElementalContact/foam.dds`
- `features/Water Waves/textures/ElementalContact/foam_height.dds`
- `features/Screen Space Physical Particles/Shaders/PhysicalParticles/leaves_atlas.dds`
- `features/Screen Space Physical Particles/Shaders/PhysicalParticles/sparks_atlas.dds`
- `features/Screen Space Physical Particles/Shaders/PhysicalParticles/small_atlas.dds`
- `features/Screen Space Physical Particles/Shaders/PhysicalParticles/particles.json`
- `features/Physical Blood/Shaders/PhysicalBlood/blooddropatlas5x5.dds`
- `features/Physical Blood/Shaders/PhysicalBlood/blooddropatlas5x5_n.dds`
- `features/Physical Blood/Shaders/PhysicalBlood/blooddropatlas5x5_blend.dds`
- `features/Physical Blood/Shaders/PhysicalBlood/blooddropatlas5x5_blendaltalpha.dds`

Runtime copies feature `Shaders/` to `Data/Shaders/` and feature `textures/` to `Data/textures/`.

## Release archive

MO2 layout, stored zip, 257014556 bytes, MD5 `b6ae7d6bcf1bad329c243be44286e8e1`. The archive has no `CommunityShaders.dll`. Shader compile and the plugin build were not run on this machine. The artifact store rejects one file above 100 MiB, so the zip is saved as three parts that concatenate back to that MD5.
