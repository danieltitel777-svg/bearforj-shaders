import json, os
OUT = "/workspace/bearforj/src/package/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Library"
os.makedirs(OUT, exist_ok=True)
N = "https://www.nexusmods.com/skyrimspecialedition/mods/"
MIX = {}
def look(t, w, n, aur=None, mix=None):
    d = {"TonemapStrength": t, "Warmth": w, "NightLift": n}
    if mix is not None: d["TonemapMix"] = mix
    if aur is not None: d["AuroraSnow"] = aur
    return d
P = [
 ("weather_vespro_cs", "Vespro + Vespro Weather - CS (Oct 2026)", "weather", N+"194707", ["Vespro.esp", "Vespro Weather.esp", "Vespro Weather - CS.esp", "Vespro CS.esp"],
   {"BearforjLook": look(0.3, 0.35, 0.25, mix=30), "PseudoSunBounce": {"intensity": 0.2}},
   "Vespro (" + N + "194250, 8 Oct 2026) + CS tonemap patch (10 Oct 2026). Own CS tonemap -> Bearforj tonemap mix 30%. Cloudy/foggy nights are meant to be very dark: low night lift. Needs Helios (vanilla option) + Moons and Stars. Built for ENB; with Effects 11 the CS patch is not needed."),
 ("weather_vanilla_cs_darker", "Vanilla Weathers for CS + Darker Nights (Oct 2026)", "weather", N+"194556", ["Darker Nights For Vanilla Weathers For CS.esp", "Darker Nights for Vanilla CS.esp"],
   {"BearforjLook": look(0.3, 0.45, 0.1, mix=60)}, "Darker nights addon (8 Oct 2026) for Vanilla CS; night lift kept low so torches matter. Author: game brightness min, then +5 notches."),
 ("weather_faithful", "Faithful Weathers", "weather", N+"119387", ["Faithful Weathers.esp"],
   {"BearforjLook": look(0.3, 0.4, 0.3, mix=35)}, "Ships an HDR plugin/ini with filmic tonemap for CS."),
 # id, name, kind, url, plugins, patch, notes
 ("weather_vanilla", "Vanilla / no weather mod", "weather", "", [], {"BearforjLook": look(0.35, 0.5, 0.45, mix=100)}, "Bearforj default look (Crimson Desert style)."),
 ("weather_nat_cs3", "NAT.CS III", "weather", N+"139567", ["NAT.ENB - ESP WEATHER PLUGIN.esp", "NAT.ENB III.esp", "NAT.CS III.esp", "NAT.CS.esp", "NAT-ENB.esp", "NAT-CS.esp", "NAT.ENB - ESP WEATHER PLUGIN v3.1.1C - Fixed.esp"],
   {"BearforjLook": look(0.15, 0.3, 0.30, mix=35), "PhysicalSky": {"vanillaMix": 0.35}, "WetnessEffects": {"MaxRainWetness": 1.0, "WeatherTransitionSpeed": 3.0}},
   "Has its own filmic tonemap + HDR sun; Bearforj look kept light to avoid double tonemap. Requires Cloud Shadows + Skylighting (both in Open Shaders core)."),
 ("weather_cathedral_cs", "Cathedral Weathers + Cathedral CS", "weather", N+"162681", ["Cathedral Weathers.esp", "Cathedral CS.esp", "Cathedral CS.esl", "Cathedral Weathers.esl"],
   {"BearforjLook": look(0.15, 0.35, 0.15, mix=40), "SkySync": {"FullMoonIntensity": 1.0}},
   "Cathedral CS 2.0 already brightened nights + DALC; low night lift."),
 ("weather_obsidian_cs", "Obsidian Weathers + Obsidian CS", "weather", N+"162627", ["Obsidian Weathers.esp", "Obsidian CS.esp"],
   {"BearforjLook": look(0.10, 0.45, 0.35, mix=30), "ExponentialHeightFog": {"fogDensity": 0.004}},
   "Filmic tonemap + new VL in Obsidian CS; moody/dark nights so moderate lift, slightly thinner height fog over its volumetric mists."),
 ("weather_azurite_cs", "Azurite III CS / Azurite III Enhanced", "weather", N+"162153", ["Azurite Weathers.esp", "Azurite III CS.esp", "Azurite Weathers III - Enhanced.esp"],
   {"BearforjLook": look(0.15, 0.4, 0.35, mix=35)}, "Enhanced: " + N + "150269. Own filmic tonemap."),
 ("weather_astralite", "Astralite Weathers", "weather", N+"156979", ["Astralite Weathers.esp", "Astralite.esp"],
   {"BearforjLook": look(0.15, 0.3, 0.30, mix=40)}, "High-fantasy CS weather; needs Cloud Shadows + Sky Sync (Bearforj has both)."),
 ("weather_rwr", "Real Weathers Remastered (2026)", "weather", N+"168776", ["Real Weathers Remastered.esp", "RealWeathersRemastered.esp", "Real Weathers.esp", "RealWeathers.esp", "Real Weathers Remastered - Natural and Realistic Weathers for Skyrim - Powered by Community Shaders.esp"],
   {"BearforjLook": look(0.05, 0.2, 0.20, mix=15), "PhysicalSky": {"vanillaMix": 0.5}},
   "Newest hyped CS weather (v2.1.0, Jul 2026). Photoreal post-processing of its own -> Bearforj look almost off. Requires Smoother Skies. Author: Jiaye builds unsupported, LL/IBL experimental."),
 ("weather_nirn_whispers", "Nirn Whispers Weathers", "weather", N+"161243", ["Nirn Whispers.esp", "NirnWhispers.esp", "Nirn Whispers.esm", "NirnWhispers.esm"],
   {"BearforjLook": look(0.2, 0.4, 0.35, mix=60)}, "Ships optional CS/OS settings overrides; those load via the Overrides system and win over this profile."),
 ("weather_vanilla_cs", "Vanilla Weathers for CS", "weather", N+"165393", ["Vanilla Weathers for CS.esp", "Vanilla CS.esp"],
   {"BearforjLook": look(0.25, 0.45, 0.40, mix=70)}, ""),
 ("weather_mythical_ages", "Mythical Ages + Patka's Imagespaces for CS", "weather", N+"11578", ["Mythical Ages.esp", "Patka's Mythical Ages Imagespaces.esp"],
   {"BearforjLook": look(0.25, 0.35, 0.25, mix=80), "WetnessEffects": {"MaxRainWetness": 1.2}},
   "Imagespaces: " + N + "155146. Very intense storms -> more wetness. Darker fantasy look."),
 ("storm_true_storms", "True Storms SE", "storm", N+"2472", ["TrueStormsSE.esp", "True Storms SE.esp", "TrueStorms.esp", "TrueStormsSE_AOS.esp"],
   {"WetnessEffects": {"MaxRainWetness": 1.2, "WeatherTransitionSpeed": 4.0}},
   "Heavy/light storm variants, ash and dust storms (Solstheim). Rain and snow stay the weather mod's own particles; wetness ramps faster in a storm."),
 ("storm_storm_lightning", "Storm Lightning for SSE and VR (Minty Lightning 2019)", "storm", N+"29243", ["Storm Lightning.esp", "StormLightning.esp", "Minty Lightning 2019.esp"],
   {}, "SKSE lightning (up to 50 flashes/s). Flashes are game light spikes; Bearforj weather presets blend over ~4 s so the look does not pump with every flash; the image analyzer may report flicker during storms (expected). Do not use old Lightning During Storms with it. MCM can lower flash brightness."),
 ("post_dlizzio_pp", "Dlizzio's Post Processing (Open Shaders, Sep 2026)", "post", N+"193337", ["Dlizzio's Post Processing.esp"],
   {"BearforjLook": {"TonemapMix": 0}}, "Full post-processing chain for OS (Azurite III CS / NAT.CS III / Astralite / Vanilla CS). Bearforj tonemap mix set to 0% so the image is not tonemapped twice. Detection: plugin name guessed; pick it in the installer to be sure."),
 ("light_lux_cs", "Lux + Lux CS", "lighting", N+"153919", ["Lux.esp", "Lux - Master plugin.esm", "Lux CS.esp", "Lux - Resources.esp"],
   {"PseudoSunBounce": {"intensity": 0.25}}, "Lux: " + N + "43158. Lux CS adds ISL + tonemap. NOT compatible with True Light, WSU, ATLM. Reinstall Lux without 'optimized' meshes."),
 ("light_lux_orbis", "Lux Orbis (exteriors)", "lighting", N+"56095", ["Lux Orbis.esp", "Lux Orbis - Master plugin.esm"], {}, "Exterior lights only; no Bearforj changes needed."),
 ("light_lux_via", "Lux Via (roads)", "lighting", N+"63588", ["Lux Via.esp", "Lux Via - Master plugin.esm", "Lux Via - Lanterns.esm"], {}, "Road lanterns; no Bearforj changes needed."),
 ("light_true_light", "True Light (+ CS Light)", "lighting", N+"135488", ["True Light.esp", "TrueLight.esp"],
   {"PseudoSunBounce": {"intensity": 0.2}}, "Uses Light Placer + ISL; own image spaces. CS Light: " + N + "138443. Exterior option conflicts with Lux Orbis."),
 ("light_elfx", "ELFX - Enhanced Lights and FX", "lighting", N+"2424", ["EnhancedLightsandFX.esp", "ELFX - Exteriors.esp"],
   {"PseudoSunBounce": {"intensity": 0.3}, "BearforjLook": {"NightLift": 0.35}}, "Old non-CS mod: do not use its Enhancer/Hardcore imagespace modules with a CS weather mod."),
 ("light_luminosity", "Luminosity Lighting Overhaul", "lighting", N+"16830", ["Luminosity Lighting Overhaul.esp", "Luminosity.esp"],
   {"PseudoSunBounce": {"intensity": 0.3}}, "Interiors only."),
 ("light_helios", "Helios (interior weathers)", "lighting", N+"181533", ["Helios.esp", "Helios.esl", "Helios.esm"],
   {"PseudoSunBounce": {"intensity": 0.15}},
   "Already drives interior ambient from exterior weather; Pseudo Sun Bounce lowered to avoid double window light. Needs MMSF, Luma Utility, XEMI. Oct 2026: Helios 1.01 breaks with Skyrim 1.7 / Luma 2.0."),
]
for pid, name, kind, url, plugins, patch, notes in P:
    json.dump({"id": pid, "name": name, "kind": kind, "url": url, "plugins": plugins, "notes": notes, "patch": patch},
              open(f"{OUT}/{pid}.json", "w"), indent=2, ensure_ascii=False)
json.dump([p[:5] for p in P], open("/workspace/bearforj/tools_bf/profiles_index.json", "w"), indent=1)
print(len(P))
