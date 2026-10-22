# SSPP particle type IDs

Stable IDs for atlas mapping. `Particle.Kind` bits **0–7** are the type. Do not renumber.

| ID | Name | HLSL | C++ `Kind` | Motion / draw |
|----|------|------|------------|----------------|
| 0 | Rain | `SSPP_RAIN` | `kRain` | Streak. A few hits leave a water stain. |
| 1 | Snow | `SSPP_SNOW` | `kSnow` | Flutter, then settle. Ash weather stays this type. |
| 2 | Leaf | `SSPP_LEAF` | `kLeaf` | Glide. Broad deciduous leaf. |
| 3 | Splash | `SSPP_SPLASH` | `kSplash` | Short ballistic spray. |
| 4 | Spark | `SSPP_SPARK` | `kSpark` | Ballistic, bounce, rest. |
| 5 | Dust | `SSPP_DUST` | `kDust` | Drag, no hard ground collision. |
| 6 | Blood | `SSPP_BLOOD` | `kBlood` | Ballistic drop. On hit it becomes a blood stain. |
| 7 | Ember | `SSPP_EMBER` | `kEmber` | Fire magic. Ballistic, then cools on the ground. |
| 8 | Ice | `SSPP_ICE` | `kIce` | Frost magic. Spinning shard, then melts. |
| 9 | Pollen | `SSPP_POLLEN` | `kPollen` | Mote. Wind and bob, no gravity, no collision. |
| 10 | Firefly | `SSPP_FIREFLY` | `kFirefly` | Mote, additive, rises. |
| 11 | Butterfly | `SSPP_BUTTERFLY` | `kButterfly` | Mote, wide bob. |
| 12 | Gnat | `SSPP_GNAT` | `kGnat` | Mote, small bob. Near water. |
| 13 | Fly | `SSPP_FLY` | `kFly` | Mote, small bob. Near corpses. |
| 14 | Lamp insect | `SSPP_LAMP` | `kLampInsect` | Mote, small bob. Flickering lights. |
| 15 | Needle | `SSPP_NEEDLE` | `kNeedle` | Foliage. Faster fall, less flutter, tall and thin. |
| 16 | Twig | `SSPP_TWIG` | `kTwig` | Foliage. Between a needle and a broad leaf. |
| 17 | Restoration | `SSPP_RESTORATION` | `kRestoration` | Mote, additive, rises. |
| 18 | Alteration | `SSPP_ALTERATION` | `kAlteration` | Ballistic, light gravity, spins. |
| 19 | Illusion | `SSPP_ILLUSION` | `kIllusion` | Mote, additive. |
| 20 | Conjuration | `SSPP_CONJURATION` | `kConjuration` | Mote, rises. |
| 21 | Shout | `SSPP_SHOUT` | `kShout` | Radial wave. Low gravity, grows. |
| 22 | Ward shard | `SSPP_WARD_SHARD` | `kWardShard` | Shatter. Spark motion, additive. |
| 23 | Ward ripple | `SSPP_WARD_RIPPLE` | `kWardRipple` | Flat ground ring. No velocity, grows and fades. |
| 24 | Blood stain | `SSPP_BLOOD_STAIN` | `kBloodStain` | Flat ground decal. Lingers, then fades. |
| 25 | Water stain | `SSPP_WATER_STAIN` | `kWaterStain` | Flat ground decal. Rain hits and drips. |
| 26 | Water drip | `SSPP_WATER_DRIP` | `kWaterDrip` | Streak off a wet character. Becomes a water stain. |

`kKindCount` is 27. `ReactMask` is one bit per ID (`1u << id`).

## Other Kind bits

- **Bit 8** (`0x100`, `SSPP_FLAG_SETTLED`): resting on the ground. Stains and the ward ripple are spawned settled.
- **Bits 16–23** (`SSPP_SPRITE_SHIFT`): optional atlas-cell override. Types **9–26 leave these bits at 0**. Map their cells from the type ID in bits 0–7. Some type 0–8 emits still set a cell here (rain, snow, ash, broad/narrow leaf, splash, spark, ember, ice, dust, blood). A value of 0 means “no override”.

## Not type IDs

`ScreenSpacePhysicalParticles::Sprite` (`kSprRain` = 1 … `kSprFly` = 16) is the old 4×4 atlas index used only as that bits 16–23 override. It is not this table. Needles and twigs are types 15 and 16, not a leaf plus a sprite bit.
