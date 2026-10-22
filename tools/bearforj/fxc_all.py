#!/usr/bin/env python3
"""Bearforj 0.6.3 offline fxc compile (wine + fxcw.exe / d3dcompiler_47, same D3DCompileFromFile + O3 as the DLL).
Matrix mirrors src/ShaderCache.cpp:
  * engine shaders: stage define (VSHADER/PSHADER) + technique defines + per-shader-type feature defines
    (feature sets taken from each Feature::HasShaderDefine), with all features off and all on;
  * Water: GetWaterShaderDefines (WATER FOG, flags, exactly one technique: SPECULAR+NUM_SPECULAR_LIGHTS / UNDERWATER / LOD / STENCIL / SIMPLE);
  * feature compute/aux shaders: every Util::CompileShader / LazyShader::Get call in src/ (file, literal defines, profile, entry).
Usage: fxc_all.py <merged_shader_root> <log>. Exit 1 if any job fails."""
import sys, os, subprocess, itertools, glob, re
from concurrent.futures import ThreadPoolExecutor
root, log = sys.argv[1], sys.argv[2]
SRC = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UNIV = "CLOUD_SHADOWS DYNAMIC_CUBEMAPS EXP_HEIGHT_FOG IBL LOD_BLENDING LIGHT_LIMIT_FIX PHYSICAL_SKY SKYLIGHTING SSS TERRAIN_BLENDING TERRAIN_SHADOWS VANILLA_FRESNEL WETNESS_EFFECTS VOLUMETRIC_SHADOWS WATER_EFFECTS SCREEN_SPACE_SHADOWS".split()
FEAT = {
 "Lighting": UNIV + "CS_UTILITY HAIR_BACKLIGHTING EXTENDED_MATERIALS EXTENDED_TRANSLUCENCY CS_HAIR CS_SKIN SUBMERSION_WETNESS TERRAIN_DEFORMATION TERRAIN_VARIATION WATER_WAVES".split(),  # TRUE_PBR is per-material (yaml entries)
 "Water": UNIV + "CS_UTILITY HORIZON_FIX WATER_WAVES".split(),
 "Sky": UNIV + "CLOUD_RELIGHT HDR_OUTPUT HORIZON_FIX PROCEDURAL_AURORA PROCEDURAL_SUN".split(),
 "Grass": UNIV + "GRASS_LIGHTING GRASS_COLLISION GRASS_OPTIMIZATIONS".split(),
 "Particle": UNIV + ["RAIN_REFRACTION"],
 "Other": UNIV,
}
TYPE = {"Lighting.hlsl": "Lighting", "Water.hlsl": "Water", "Sky.hlsl": "Sky", "RunGrass.hlsl": "Grass", "Particle.hlsl": "Particle"}
LIGHT_TECH = ["", "DEFERRED", "LANDSCAPE MULTI_TEXTURE", "LANDSCAPE MULTI_TEXTURE LANDSCAPE_SEAMS DEFERRED", "LANDSCAPE LOD_LAND_BLEND DEFERRED",
              "TREE_ANIM VC", "TREE_ANIM VC DEFERRED", "SKINNED DEFERRED", "SKINNED HAIR DEFERRED", "SKINNED HAIR", "SKINNED FACEGEN DEFERRED",
              "ENVMAP DEFERRED", "PARALLAX DEFERRED", "MULTI_LAYER_PARALLAX DEFERRED", "EYE SKINNED DEFERRED", "SNOW DEFERRED", "PROJECTED_UV DEFERRED", "LODOBJECTS", "DO_ALPHA_TEST"]
SKY_TECH = ["OCCLUSION", "TEX DITHER", "TEX MOONMASK", "HORIZFADE", "TEX CLOUDS", "TEX CLOUDS TEXLERP", "TEX CLOUDS TEXFADE", "TEX", "DITHER"]
W_FLAGS = ["REFLECTIONS REFRACTIONS DEPTH", "REFLECTIONS REFRACTIONS DEPTH VERTEX_ALPHA_DEPTH", "REFLECTIONS REFRACTIONS DEPTH FLOWMAP BLEND_NORMALS",
           "REFRACTIONS DEPTH INTERIOR", "REFLECTIONS REFRACTIONS DEPTH WADING", "VC NORMAL_TEXCOORD REFLECTIONS", "CUBEMAP", ""]
W_TECH = ["SPECULAR NUM_SPECULAR_LIGHTS=0", "SPECULAR NUM_SPECULAR_LIGHTS=4", "UNDERWATER", "LOD", "STENCIL", "SIMPLE"]
jobs = []  # (file, profile, entry, defines-list)
# Engine shaders: the real game permutation list from upstream .github/configs/shader-validation.yaml
# (generated from Skyrim logs), plus Bearforj's extra per-type feature defines from FEAT.
import yaml
cfg = yaml.safe_load(open(f"{SRC}/.github/configs/shader-validation.yaml"))
PROF = {"PSHADER": "ps_5_0", "VSHADER": "vs_5_0", "CSHADER": "cs_5_0"}
for shd in cfg["shaders"]:
    t = TYPE.get(shd["file"], "Other")
    for st, c in shd["configs"].items():
        common = [d for d in c.get("common_defines", []) if not d.startswith("D3DCOMPILE_")]
        extra = [d for d in FEAT[t] if d not in common] if st != "CSHADER" else []
        # Utility depth/shadow passes displace landscape when the feature is loaded (HasShaderDefine).
        if shd["file"] == "Utility.hlsl" and st != "CSHADER" and "TERRAIN_DEFORMATION" not in common and "TERRAIN_DEFORMATION" not in extra:
            extra.append("TERRAIN_DEFORMATION")
        for e in c["entries"]:
            jobs.append((shd["file"], PROF[st], "main", common + extra + list(e.get("defines") or [])))
jobs.append(("DeferredCompositeCS.hlsl", "cs_5_0", "main", ["CSHADER"])); jobs.append(("DeferredCompositeCS.hlsl", "cs_5_0", "main", ["CSHADER"] + FEAT["Lighting"]))
# TerrainDeformation::CompileWorker builds these from the landscape vertex defines plus HSHADER/DSHADER.
for stage, prof in (("HSHADER", "hs_5_0"), ("DSHADER", "ds_5_0")):
    jobs.append(("Lighting.hlsl", prof, "main", [stage, "LANDSCAPE", "TERRAIN_DEFORMATION", "VC", "MODELSPACENORMALS"]))
    jobs.append(("Lighting.hlsl", prof, "main", [stage, "LANDSCAPE", "TERRAIN_DEFORMATION", "VC"]))
    jobs.append(("Water.hlsl", prof, "main", [stage, "WATER", "FOG", "WATER_WAVES", "SPECULAR", "NUM_SPECULAR_LIGHTS=0", "REFLECTIONS", "REFRACTIONS", "DEPTH"]))
    jobs.append(("Water.hlsl", prof, "main", [stage, "WATER", "FOG", "WATER_WAVES", "UNDERWATER", "REFRACTIONS", "DEPTH"]))
    jobs.append(("Water.hlsl", prof, "main", [stage, "WATER", "FOG", "WATER_WAVES", "STENCIL"]))
    jobs.append(("Water.hlsl", prof, "main", [stage, "WATER", "FOG", "WATER_WAVES", "SIMPLE", "REFRACTIONS", "DEPTH"]))
CALL = re.compile(r'L"Data\\\\Shaders\\\\([^"]+\.hlsl)"\s*,\s*(\{(?:[^{}]|\{[^{}]*\})*\}|[\w:.\->]+)\s*,\s*"(\w\w_5_0)"(?:\s*,\s*"(\w+)")?')
DEF = re.compile(r'\{\s*"(\w+)"\s*,\s*([^{}]*?)\s*\}')
STAGE = {"ps": "PSHADER", "vs": "VSHADER", "hs": "HULLSHADER", "ds": "DOMAINSHADER", "cs": "COMPUTESHADER"}  # BuildStandaloneCompileKey
def parse_defs(body):
    out = []
    for n, v in DEF.findall(body):
        v = v.strip()
        if v in ("nullptr", '""', ""): out.append(n)
        elif v.startswith('"') and v.endswith('"'): out.append(f"{n}={v[1:-1]}")
        else: out.append(f"{n}=60" if n == "SAMPLE_COUNT" else f"{n}=1")   # runtime value (e.g. sampleCount.c_str())
    return out
used = set()
for f in glob.glob(f"{SRC}/src/**/*.cpp", recursive=True) + glob.glob(f"{SRC}/src/**/*.h", recursive=True):
    txt = open(f, encoding="utf-8", errors="ignore").read()
    for m in CALL.finditer(txt):
        path = m.group(1).replace("\\\\", "/"); used.add(path)
        g = m.group(2)
        if g.startswith("{"):
            defs = parse_defs(g)
        else:  # variable: nearest initializer before the call, e.g. `defines{ {...} }` or `defines = { {...} }`
            var = g.split("->")[-1].split(".")[-1]
            inits = list(re.finditer(r'\b' + re.escape(var) + r'\s*=?\s*(\{(?:[^{}]|\{[^{}]*\})*\})\s*;', txt[:m.start()]))
            defs = parse_defs(inits[-1].group(1)) if inits else []
        jobs.append((path, m.group(3), m.group(4) or "main", [STAGE[m.group(3)[:2]]] + defs))
# Defines built at runtime from lambda args / conditional push_back (read from the C++ by hand):
OVERRIDES = {
    "Effects11/SunRaysPS.hlsl": [["PSHADER", "MASK"], ["PSHADER", "MASK", "CLOUD_SHADOWS"], ["PSHADER", "BLUR"], ["PSHADER", "COMPOSITE"]],  # Effects11.cpp:1308-1319
    "VRStereoOptimizations/StencilCS.hlsl": [["COMPUTESHADER", "VR", "FRAMEBUFFER"]],  # VRStereoOptimizations.cpp:298 (VR only)
    "VR/StereoBlendCS.hlsl": [["COMPUTESHADER", "VR", "FRAMEBUFFER"]],  # VR/StereoBlend.cpp:13 (VR only)
}
jobs = [j for j in jobs if j[0] not in OVERRIDES] + [(f, "cs_5_0" if "CS" in f else "ps_5_0", "main", d) for f, ds in OVERRIDES.items() for d in ds]
ONLY = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
if ONLY: jobs = [j for j in jobs if re.search(ONLY, j[0])]
FXC = "/workspace/bearforj/fxc/fxcw.exe"
def run(j):
    sh, prof, entry, defs = j
    if not os.path.exists(os.path.join(root, sh)): return j, False, ["missing file"]
    r = subprocess.run(["wine", FXC, sh, entry, prof] + defs, cwd=root, capture_output=True, text=True, errors="ignore", env={**os.environ, "WINEDEBUG": "-all"})
    return j, "RESULT OK" in r.stdout, [l.strip() for l in r.stdout.splitlines() if "error" in l.lower()]
seen = set(); uniq = []
for j in jobs:
    k = (j[0], j[1], j[2], tuple(sorted(j[3])))
    if k not in seen: seen.add(k); uniq.append(j)
with ThreadPoolExecutor(12) as ex: res = list(ex.map(run, uniq))
bad = [r for r in res if not r[1]]
allcs = {os.path.relpath(p, root).replace(os.sep, "/") for p in glob.glob(f"{root}/**/*.hlsl", recursive=True)}
with open(log, "w") as L:
    for (sh, prof, entry, defs), ok, errs in res:
        L.write(f"{'OK  ' if ok else 'FAIL'} {sh} {prof}:{entry} [{' '.join(defs)}]\n")
        for e in errs[:20]: L.write(f"      {e}\n")
    L.write(f"\nTOTAL {len(res)} jobs, {len(res)-len(bad)} OK, {len(bad)} FAIL\n")
print(f"TOTAL {len(res)} jobs, {len(res)-len(bad)} OK, {len(bad)} FAIL -> {log}")
sys.exit(1 if bad else 0)
