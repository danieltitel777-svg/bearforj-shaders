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

## Not in the shader

- Environment vertical-drop shading from the wetness branch is not in Bottled's wetness shader. There is no wall-rain flow to attach it to. The settings and the drying timer are packed. The menu says ordinary wetness still covers those surfaces.
- Volumetric puddles do not shift the bottom albedo. Bottled wetness has no `GetParallaxSampleOffset`. Edge darkening and the raindrop normal are applied.
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
