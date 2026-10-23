"""Bearforj package assembler v2 (0.6.1+).
Output: dist07/<version>_<stamp>/Bearforj_Shaders_<version>.zip with fomod/ at the archive root.
Options auto-select: a profile whose plugin is ACTIVE in the load order (MO2 checks plugins.txt /
VFS) becomes Recommended (pre-checked); otherwise Optional."""
import json, os, shutil, glob, sys, re, zipfile, hashlib
from xml.sax.saxutils import escape as E, quoteattr as QA
stage, dll, version, stamp = sys.argv[1:5]      # stage = unpacked previous package root (has '00 Core', '20 Presets', 'fomod/images')
SRC = "/workspace/bearforj/src"
OUTDIR = f"/workspace/bearforj/dist07/{version}_{stamp}"
ROOT = f"/tmp/bf_pkg_{version}"
if os.path.exists(ROOT): shutil.rmtree(ROOT)
shutil.copytree(stage, ROOT)
core = f"{ROOT}/00 Core"
shutil.copy2(dll, f"{core}/SKSE/Plugins/CommunityShaders.dll")
pdb = dll[:-4] + ".pdb"
if os.path.exists(pdb): shutil.copy2(pdb, f"{core}/SKSE/Plugins/CommunityShaders.pdb")
# 0.6.3: overlay current repo shaders/INIs/translations (package/ + every features/<X>/) onto Core,
# so the zip never carries stale shaders and restored feature INIs are always present.
def overlay(src):
    for dp, dn, fn in os.walk(src):
        rel = os.path.relpath(dp, src)
        os.makedirs(os.path.join(core, rel), exist_ok=True)
        for f in fn:
            if f in ("CORE",) or f.endswith((".md",)): continue
            shutil.copy2(os.path.join(dp, f), os.path.join(core, rel, f))
overlay(f"{SRC}/package")
for fd in sorted(glob.glob(f"{SRC}/features/*/")):
    overlay(fd)
# 0.6.3: 0.5 shaders/textures that no C++ in the DLL compiles or loads (SSPP v2 replaced them) are not shipped.
for dead in ("Shaders/PhysicalBlood", "Shaders/PhysicalLeaves"):
    shutil.rmtree(f"{core}/{dead}", ignore_errors=True)
# 0.6.5: overlay copies a previous stage, so files the repo deleted would ship again.
# Mirrors the local package fix that drops stale Unified Water and the pre-helmet mascots.
# 0.6.6: SSPP, FootstepKick, old FootstepParticles, PhysicalBlood/Leaves, PrecipitationPhysics and NativeParticlePhysics are gone from the repo; their stage leftovers must not ship.
for dead in ("Shaders/ScreenSpacePhysicalParticles", "Textures/Bearforj/Particles", "textures/BearForge/FX", "meshes/BearForge/FX"):
    shutil.rmtree(f"{core}/{dead}", ignore_errors=True)
for ini in ("FootstepKickParticles", "FootstepParticles", "NativeParticlePhysics", "PhysicalBlood", "PhysicalLeaves", "PrecipitationPhysics", "ScreenSpacePhysicalParticles"):
    for base in ("Shaders/Features", "SKSE/Plugins/CommunityShaders/Shaders/Features"):
        try:
            os.remove(f"{core}/{base}/{ini}.ini")
        except OSError:
            pass
for dead in ("Shaders/UnifiedWater", "SKSE/Plugins/CommunityShaders/UnifiedWaterCache"):
    shutil.rmtree(f"{core}/{dead}", ignore_errors=True)
for stale in (
    "SKSE/Plugins/CommunityShaders/Shaders/Features/UnifiedWater.ini",
    "Shaders/Features/UnifiedWater.ini",
    "Shaders/Features/WaterCache.ini",
    "SKSE/Plugins/CommunityShaders/Shaders/Features/WaterCache.ini",
    "Shaders/PrecipitationPhysics/PrecipitationPhysics.hlsli",  # dead 0.5 include
    "meshes/water/WaterMesh.nif",
    "Interface/CommunityShaders/Bearforj/emotions/happy.png",
    "Interface/CommunityShaders/Bearforj/head_grumpy.png",
    "Interface/CommunityShaders/Bearforj/head_happy.png",
    "Interface/CommunityShaders/Bearforj/head_worried.png",
    "Interface/CommunityShaders/Bearforj/mishmish_front.png",
):
    try:
        os.remove(f"{core}/{stale}")
    except OSError:
        pass
for dp, dn, fn in os.walk(core):  # no water file left
    for f in fn + dn:
        assert not re.search(r"unified.?water|water.?cache", f, re.I), os.path.join(dp, f)
for dp, dn, fn in os.walk(core):
    for f in fn + dn:
        assert not re.search(r"sspp|ScreenSpacePhysicalParticles|FootstepKick|^FootstepParticles", f, re.I), os.path.join(dp, f)
for need in ("PhysicalEffectParticles", "GPUFootstepParticles"):
    assert os.path.exists(f"{core}/SKSE/Plugins/CommunityShaders/Shaders/Features/{need}.ini") or os.path.exists(f"{core}/Shaders/Features/{need}.ini"), need
lib = f"{core}/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Library"
shutil.rmtree(f"{ROOT}/10 Profiles", ignore_errors=True)
profiles = [json.load(open(f, encoding="utf-8")) for f in sorted(glob.glob(lib + "/*.json"))]
for p in profiles:
    d = f"{ROOT}/10 Profiles/{p['id']}/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Selected"; os.makedirs(d)
    shutil.copy2(f"{lib}/{p['id']}.json", d)
packs = json.load(open("/workspace/bearforj/tools_bf/packs.json"))

def deps(files, op="Or"):
    return f'<dependencies operator="{op}">' + "".join(f'<fileDependency file={QA(f)} state="Active"/>' for f in files) + "</dependencies>"
def detect_type(files, hit="Recommended", miss="Optional"):
    if not files: return f'<typeDescriptor><type name="{miss}"/></typeDescriptor>'
    return (f'<typeDescriptor><dependencyType><defaultType name="{miss}"/><patterns><pattern>{deps(files)}'
            f'<type name="{hit}"/></pattern></patterns></dependencyType></typeDescriptor>')
def opt(name, desc, image, files, td):
    img = f'<image path="fomod\\images\\{image}.png"/>' if os.path.exists(f"{ROOT}/fomod/images/{image}.png") else ""
    fl = f"<files>{files}</files>"
    return f'<plugin name={QA(name)}><description>{E(desc)}</description>{img}{fl}{td}</plugin>'
def folder(src): return f'<folder source={QA(src)} destination="" priority="0"/>'
weather_plugins = sorted({f for p in profiles if p["kind"] == "weather" for f in p["plugins"]})
# Auto: Recommended only when NO known weather plugin is active.
auto_td = (f'<typeDescriptor><dependencyType><defaultType name="Recommended"/><patterns><pattern>{deps(weather_plugins)}'
           f'<type name="Optional"/></pattern></patterns></dependencyType></typeDescriptor>')
W = [opt("Auto-detect (no fixed choice)", "Bearforj finds your weather mod by its plugin at startup. / Bearforj сам найдёт погодный мод по плагину.", "auto", "", auto_td)]
L = []; S = []; PP = []
for p in profiles:
    desc = f"{p['notes']}\n{p['url']}".strip()
    if p["plugins"]: desc += "\nDetected by / ищем плагин: " + ", ".join(p["plugins"])
    o = opt(p["name"], desc, p["id"], folder(f"10 Profiles\\{p['id']}"), detect_type(p["plugins"]))
    {"weather": W, "storm": S, "post": PP}.get(p["kind"], L).append(o)
P = [opt(f"Preset pack: {n}", f"9 weather presets tuned for {n}. Any preset can be assigned to any weather slot in the menu.", f"weather_{k}", folder(f"20 Presets\\{k}"),
         '<typeDescriptor><type name="Recommended"/></typeDescriptor>') for k, n in packs if os.path.isdir(f"{ROOT}/20 Presets/{k}")]
O = [opt("Advanced menu", "Show debug tools and advanced settings from the first start. / Сразу показывать отладку и продвинутые настройки.", "advanced", folder("30 Options\\advanced"),
         '<typeDescriptor><type name="Optional"/></typeDescriptor>')]
def group(n, t, o): return f'<group name={QA(n)} type="{t}"><plugins order="Explicit">{"".join(o)}</plugins></group>'
def step(n, g): return f'<installStep name={QA(n)}><optionalFileGroups order="Explicit">{"".join(g)}</optionalFileGroups></installStep>'
steps = [step("Weather / Погода", [group("Weather mod (pick one, detected one is pre-selected)", "SelectExactlyOne", W)]),
         step("Lighting / Свет", [group("Lighting mods (detected ones are pre-checked)", "SelectAny", L)])]
if S: steps.append(step("Storms / Грозы", [group("Storm / lightning mods (detected ones are pre-checked)", "SelectAny", S)]))
if PP: steps.append(step("Post-processing", [group("Post-processing mods (detected ones are pre-checked)", "SelectAny", PP)]))
steps += [step("Weather presets / Пресеты погоды", [group("Preset packs", "SelectAny", P)]), step("Options / Опции", [group("Menu", "SelectAny", O)])]
xml = f'''<?xml version="1.0" encoding="UTF-16"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
<moduleName>Bearforj Shaders {version}</moduleName>
<moduleImage path="fomod\\images\\header.png"/>
<requiredInstallFiles>{folder("00 Core")}</requiredInstallFiles>
<installSteps order="Explicit">{"".join(steps)}</installSteps>
</config>'''
open(f"{ROOT}/fomod/ModuleConfig.xml", "w", encoding="utf-16").write(xml)
open(f"{ROOT}/fomod/info.xml", "w", encoding="utf-16").write(f'''<?xml version="1.0" encoding="UTF-16"?>
<fomod><Name>Bearforj Shaders</Name><Author>Bearforj team (fork of Open Shaders / Community Shaders)</Author><Version>{version}</Version><Description>Bearforj Shaders {version} with weather/lighting compatibility profiles (auto-detected).</Description><Groups><element>Visuals and Graphics</element></Groups></fomod>''')
from lxml import etree
schema = etree.XMLSchema(etree.parse(f"{SRC}/tools/bearforj/ModuleConfig.xsd"))
ok = schema.validate(etree.parse(f"{ROOT}/fomod/ModuleConfig.xml")); print("schema valid:", ok, schema.error_log)
assert ok
for m in re.findall(r'(?:source|path)="([^"]+)"', xml):
    assert os.path.exists(os.path.join(ROOT, m.replace("\\", "/"))), m
os.makedirs(OUTDIR, exist_ok=True)
z = f"{OUTDIR}/Bearforj_Shaders_{version}.zip"
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for dp, dn, fn in os.walk(ROOT):
        for f in fn:
            a = os.path.join(dp, f); zf.write(a, os.path.relpath(a, ROOT))
print(z, hashlib.md5(open(z, "rb").read()).hexdigest())
