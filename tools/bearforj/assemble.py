import json, os, shutil, subprocess, glob, sys
from xml.sax.saxutils import escape as E
D="/workspace/bearforj/dist07"; MOD=f"{D}/Bearforj Shaders 0.6"
SRC="/workspace/bearforj/src"; BASE="/workspace/bearforj/dist/Bearforj Shaders"
if os.path.exists(D): shutil.rmtree(D)
core=f"{MOD}/00 Core"
shutil.copytree(BASE, core)                       # read-only use of dist (Модер copies from it)
shutil.copytree(f"{SRC}/package", core, dirs_exist_ok=True)
lib=f"{core}/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Library"
dll=f"{SRC}/build/Linux-ClangCL/CommunityShaders.dll"
shutil.copy2(dll, f"{core}/SKSE/Plugins/CommunityShaders.dll")
pdb=dll[:-4]+".pdb"
if os.path.exists(pdb): shutil.copy2(pdb, f"{core}/SKSE/Plugins/CommunityShaders.pdb")
fom=f"{MOD}/fomod"; img=f"{fom}/images"; os.makedirs(img)
subprocess.check_call(["python3","/workspace/bearforj/tools_bf/fomod_images.py",img])
subprocess.check_call(["python3","/workspace/bearforj/tools_bf/presets.py",f"{MOD}/20 Presets"])
profiles=[json.load(open(f)) for f in sorted(glob.glob(lib+"/*.json"))]
for p in profiles:
    d=f"{MOD}/10 Profiles/{p['id']}/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Selected"; os.makedirs(d)
    shutil.copy2(f"{lib}/{p['id']}.json", d)
adv=f"{MOD}/30 Options/advanced/SKSE/Plugins/CommunityShaders/Bearforj"; os.makedirs(adv)
json.dump({"advancedMode":True},open(f"{adv}/CompatDefaults.json","w"))
packs=json.load(open("/workspace/bearforj/tools_bf/packs.json"))
def opt(name,desc,image,files,typ):
    return f'''<plugin name="{E(name)}"><description>{E(desc)}</description><image path="fomod\\images\\{image}.png"/><files>{files}</files><typeDescriptor><type name="{typ}"/></typeDescriptor></plugin>'''
def folder(src): return f'<folder source="{E(src)}" destination="" priority="0"/>'
wopts=[opt("Auto-detect (no fixed choice)","Bearforj finds your weather mod by its plugin at startup. / Bearforj сам найдёт погодный мод по плагину.","auto","","Optional")]
lopts=[]; sopts=[]; ppopts=[]
for p in profiles:
    desc=f"{p['notes']}\n{p['url']}".strip()
    o=opt(p["name"],desc,p["id"],folder(f"10 Profiles\\{p['id']}"),"Optional")
    {"weather":wopts,"storm":sopts,"post":ppopts}.get(p["kind"],lopts).append(o)
popts=[opt(f"Preset pack: {n}",f"9 weather presets (clear ... aurora night) tuned for {n}. Any preset can be assigned to any weather slot in the menu.",f"weather_{k}",folder(f"20 Presets\\{k}"),"Recommended") for k,n in packs]
oopts=[opt("Advanced menu","Show debug tools and advanced settings from the first start. / Сразу показывать отладку и продвинутые настройки.","advanced",folder("30 Options\\advanced"),"Optional")]
def group(name,typ,opts): return f'<group name="{E(name)}" type="{typ}"><plugins order="Explicit">{"".join(opts)}</plugins></group>'
def step(name,groups): return f'<installStep name="{E(name)}"><optionalFileGroups order="Explicit">{"".join(groups)}</optionalFileGroups></installStep>'
xml=f'''<?xml version="1.0" encoding="UTF-16"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
<moduleName>Bearforj Shaders 0.6</moduleName>
<moduleImage path="fomod\\images\\header.png"/>
<requiredInstallFiles>{folder("00 Core")}</requiredInstallFiles>
<installSteps order="Explicit">
{step("Weather / Погода",[group("Weather mod (pick one)","SelectExactlyOne",wopts)])}
{step("Lighting / Свет",[group("Lighting mods (any)","SelectAny",lopts)])}
{step("Storms / Грозы",[group("Storm / lightning mods (any)","SelectAny",sopts)])}
{step("Weather presets / Пресеты погоды",[group("Preset packs (all recommended)","SelectAny",popts)])}
{step("Options / Опции",[group("Menu","SelectAny",oopts)])}
</installSteps>
</config>'''
open(f"{fom}/ModuleConfig.xml","w",encoding="utf-16").write(xml)
info='''<?xml version="1.0" encoding="UTF-16"?>
<fomod><Name>Bearforj Shaders</Name><Author>Bearforj team (fork of Open Shaders / Community Shaders)</Author><Version>0.6.0</Version><Description>Bearforj Shaders 0.6 with weather/lighting compatibility profiles.</Description><Groups><element>Visuals and Graphics</element></Groups></fomod>'''
open(f"{fom}/info.xml","w",encoding="utf-16").write(info)
from lxml import etree
schema=etree.XMLSchema(etree.parse("/workspace/bearforj/tools_bf/ModuleConfig.xsd"))
doc=etree.parse(f"{fom}/ModuleConfig.xml"); ok=schema.validate(doc); print("schema valid:",ok, schema.error_log)
etree.parse(f"{fom}/info.xml"); print("info.xml well-formed")
# check every referenced path exists
import re
for m in re.findall(r'(?:source|path)="([^"]+)"',xml):
    if m.startswith("http"): continue
    assert os.path.exists(os.path.join(MOD,m.replace("\\","/"))), m
print("paths ok")
