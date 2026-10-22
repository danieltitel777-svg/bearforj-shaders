#!/usr/bin/env python3
"""Bearforj build-time slot audit.
Preprocesses each top-level shader for a matrix of permutations (C preprocessor, real #if logic)
and fails if two DIFFERENT resources share one t/s/b/u slot in the same stage+permutation.
Usage: slot_audit.py <shader_root> [--log FILE]   (shader_root = merged Shaders folder)"""
import re, subprocess, sys, itertools, os, tempfile
root = sys.argv[1]; log = sys.argv[sys.argv.index("--log") + 1] if "--log" in sys.argv else None
ALL_PS = "CLOUD_SHADOWS CS_HAIR CS_SKIN CS_UTILITY DYNAMIC_CUBEMAPS EXP_HEIGHT_FOG EXTENDED_MATERIALS EXTENDED_TRANSLUCENCY HAIR_BACKLIGHTING IBL LIGHT_LIMIT_FIX LOD_BLENDING PHYSICAL_SKY PSEUDO_SUN_BOUNCE REVERSE_Z SCREEN_SPACE_SHADOWS SKYLIGHTING SSS SUBMERSION_WETNESS TERRAIN_BLENDING TERRAIN_DEFORMATION TERRAIN_SHADOWS TERRAIN_VARIATION TRUE_PBR UNIFIED_WATER VANILLA_FRESNEL VOLUMETRIC_SHADOWS WATER_EFFECTS WETNESS_EFFECTS GRASS_LIGHTING GRASS_COLLISION CLOUD_RELIGHT PROCEDURAL_SUN PROCEDURAL_AURORA HDR_OUTPUT HORIZON_FIX".split()
LIGHT_TECH = ["", "DEFERRED", "LANDSCAPE MULTI_TEXTURE", "LANDSCAPE MULTI_TEXTURE LANDSCAPE_SEAMS DEFERRED", "LANDSCAPE LOD_LAND_BLEND DEFERRED",
              "TREE_ANIM VC", "TREE_ANIM VC DEFERRED", "SKINNED DEFERRED", "SKINNED HAIR DEFERRED", "SKINNED HAIR", "SKINNED FACEGEN DEFERRED",
              "ENVMAP DEFERRED", "PARALLAX DEFERRED", "MULTI_LAYER_PARALLAX DEFERRED", "EYE SKINNED DEFERRED", "SNOW DEFERRED", "PROJECTED_UV DEFERRED", "LODOBJECTS", "DO_ALPHA_TEST"]
SKY_TECH = ["OCCLUSION", "TEX DITHER", "TEX MOONMASK", "HORIZFADE", "TEX CLOUDS", "TEX CLOUDS TEXLERP", "TEX CLOUDS TEXFADE", "TEX", "DITHER"]
JOBS = []
for stage in ("PSHADER", "VSHADER"):
    for t in LIGHT_TECH:
        for f in ("", " ".join(ALL_PS)):
            JOBS.append(("Lighting.hlsl", stage, t + " " + f))
    for t in SKY_TECH:
        for d in ("", "DEFERRED"):
            for f in ("", " ".join(ALL_PS)):
                JOBS.append(("Sky.hlsl", stage, f"{t} {d} {f}"))
    for sh in ("Water.hlsl", "Effect.hlsl", "DistantTree.hlsl", "RunGrass.hlsl", "Particle.hlsl", "Utility.hlsl", "ImageSpaceEffect.hlsl"):
        for f in ("", " ".join(ALL_PS)):
            JOBS.append((sh, stage, f))
for tech in ("RENDER_DEPTH TERRAIN_DEFORMATION", "RENDER_SHADOWMAP TERRAIN_DEFORMATION", "LOD_LANDSCAPE RENDER_DEPTH TERRAIN_DEFORMATION", "SKINNED RENDER_DEPTH TERRAIN_DEFORMATION"):
    for f in ("", " ".join(ALL_PS)):
        JOBS.append(("Utility.hlsl", "VSHADER", tech + " " + f))
# Hull/domain read FrameBuffer::PerFrame at b12. A second cbuffer there is a collision.
for stage in ("HSHADER", "DSHADER"):
    for f in ("", " ".join(ALL_PS)):
        JOBS.append(("Lighting.hlsl", stage, "LANDSCAPE TERRAIN_DEFORMATION " + f))
for sh in ("DeferredCompositeCS.hlsl",):
    for f in ("", " ".join(ALL_PS)):
        JOBS.append((sh, "CSHADER", f))
RX = re.compile(r'(\w+)\s*(?:\[\s*([^\]]*)\])?\s*:\s*register\(\s*([tsbu])(\d+)')
CB = re.compile(r'cbuffer\s+(\w+)\s*:\s*register\(\s*b(\d+)')
def arr(n, src):
    try: return int(n)
    except Exception:
        m = re.search(r'\b' + re.escape(n.strip()) + r'\s*=\s*(\d+)', src); return int(m.group(1)) if m else 1
allow = set()
if "--allow" in sys.argv:
    allow = {l.strip() for l in open(sys.argv[sys.argv.index("--allow") + 1]) if l.strip() and not l.startswith("#")}
bad = 0; allowed = 0; lines = []
for sh, stage, defs in JOBS:
    p = os.path.join(root, sh)
    if not os.path.exists(p): continue
    args = ["cpp", "-P", "-w", "-undef", "-nostdinc", "-I", root, "-D" + stage] + ["-D" + d for d in defs.split()] + [p]
    r = subprocess.run(args, capture_output=True, text=True, errors="ignore")
    src = r.stdout
    slots = {}
    for m in CB.finditer(src):
        slots.setdefault(("b", int(m.group(2))), set()).add(m.group(1))
    for m in RX.finditer(src):
        name, a, k, n = m.group(1), m.group(2), m.group(3), int(m.group(4))
        if k == "b": continue
        for i in range(arr(a, src) if a else 1):
            slots.setdefault((k, n + i), set()).add(name)
    for (k, n), names in sorted(slots.items()):
        if len(names) > 1:
            if f"{sh} {stage}: {k}{n} <- {sorted(names)}" in allow:
                allowed += 1; continue
            bad += 1; lines.append(f"COLLISION {sh} {stage} [{defs.strip()}]: {k}{n} <- {sorted(names)}")
lines.append(f"slot audit: {len(JOBS)} permutations, {bad} collisions ({allowed} allowlisted: same in Open Shaders 2.19 or documented)")
out = "\n".join(dict.fromkeys(lines))
print(out)
if log: open(log, "w").write(out + "\n")
sys.exit(1 if bad else 0)
