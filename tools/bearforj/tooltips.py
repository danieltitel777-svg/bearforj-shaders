"""Generates Bearforj setting tooltips (en/ru) for every ImGui setting in src/Features.
Output: package/.../Bearforj/Tooltips.json (feature|label -> id) and tt.<id> strings in en.json/ru.json."""
import re, json, glob, os, collections, hashlib
ROOT=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TR=f"{ROOT}/package/SKSE/Plugins/CommunityShaders/Translations"
en=json.load(open(f"{TR}/en.json"),object_pairs_hook=collections.OrderedDict)
ru=json.load(open(f"{TR}/ru.json"),object_pairs_hook=collections.OrderedDict)
CALLS="SliderFloat|SliderInt|Checkbox|DragFloat|DragInt|Combo|ColorEdit3|ColorEdit4|SliderFloat2|SliderFloat3|SliderAngle|InputFloat|RadioButton|SliderScalar"
HIGH={"ScreenSpaceGI","VolumetricLighting","Skylighting","PhysicalSky","Upscaling","ScreenSpaceShadows","TerrainShadows","ScreenSpaceReflections","ScreenSpacePhysicalParticles","LightLimitFix","ExponentialHeightFog","ProceduralAurora","GrassCollision","TerrainDeformation"}
MED={"WetnessEffects","SubsurfaceScattering","CloudShadows","Bloom","DynamicCubemaps","ExtendedMaterials","WaterEffects","UnifiedWater","SubmersionWetness","PrecipitationPhysics","PhysicalLeaves","HairSpecular","TerrainBlending"}
COSTLY=re.compile(r"sample|quality|step|resolution|slice|direction|count|distance|radius|range|res\b|iteration|cascade|lod|max .*lights|taps|rays",re.I)
def feature_of(path):
    rel=os.path.relpath(path,f"{ROOT}/src/Features").replace("\\","/")
    return rel.split("/")[0].rsplit(".",1)[0]
def what(label):
    l=label.lower()
    rules=[("enable","Turns this part of the feature on or off.","Включает или выключает эту часть фичи."),
           ("intensity|strength|power|amount|mult|scale|boost","How strong the effect is. Higher = more visible.","Сила эффекта. Больше = заметнее."),
           ("radius|distance|range|width|size","How far / how wide the effect reaches.","Насколько далеко/широко действует эффект."),
           ("color|tint|colour","Color tint of the effect.","Цветовой оттенок эффекта."),
           ("sample|quality|step|slice|direction|iteration|taps|rays","Quality vs speed: more = cleaner but slower.","Качество против скорости: больше = чище, но медленнее."),
           ("threshold|cutoff|min|max","Limit where the effect starts or stops.","Порог, где эффект начинается или заканчивается."),
           ("speed|rate|time|duration|fade","How fast it changes over time.","Как быстро меняется со временем."),
           ("bias|offset","Fine shift to fix artifacts (acne, halos, leaks).","Тонкий сдвиг против артефактов (полосы, ореолы, протечки)."),
           ("debug|show|visualize|overlay","Debug view, keep off while playing.","Отладочный вид, в игре держи выключенным."),
           ("mode|type|method|preset","Chooses how the effect is computed.","Выбирает способ расчёта эффекта.")]
    for k,e,r in rules:
        if re.search(k,l): return e,r
    return "Adjusts this effect.","Настраивает этот эффект."
items={}
for path in glob.glob(f"{ROOT}/src/Features/**/*.cpp",recursive=True):
    src=open(path,encoding="utf-8",errors="ignore").read()
    pm=re.search(r'#\s*define\s+I18N_KEY_PREFIX\s+"([^"]*)"',src); prefix=pm.group(1) if pm else ""
    feat=feature_of(path)
    for m in re.finditer(r'(?:ImGui|BFUI)::('+CALLS+r')\(\s*(T\(\s*(TKEY\(\s*"([^"]+)"\s*\)|"([^"]+)")\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)|"((?:[^"\\]|\\.)*)")([^;]*)',src):
        call=m.group(1)
        key=(prefix+m.group(4)) if m.group(4) else m.group(5)
        label=m.group(6) if m.group(6) is not None else m.group(7)
        if not label: continue
        label=label.split("##")[0].strip()
        if not label: continue
        rest=m.group(8)
        rng=""
        if call in("SliderFloat","SliderInt","SliderFloat2","SliderFloat3","DragFloat","SliderAngle"):
            nums=re.findall(r',\s*(-?[0-9.]+)f?\b',rest)
            if len(nums)>=2: rng=f"{nums[0]} .. {nums[1]}"
        ruLabel=ru.get(key,label) if key else label
        fid=f"{feat}|{label}"
        if fid in items: continue
        cost=("high" if feat in HIGH else "medium" if feat in MED else "low") if (COSTLY.search(label) or call=="Checkbox" and "enable" in label.lower()) else "none"
        items[fid]=dict(feat=feat,label=label,ru_label=ruLabel,call=call,rng=rng,cost=cost)
COST_EN={"high":"High: changes FPS noticeably.","medium":"Medium: a few FPS.","low":"Low.","none":"~0 (value only, no extra GPU work)."}
COST_RU={"high":"Высокая: заметно влияет на FPS.","medium":"Средняя: несколько FPS.","low":"Низкая.","none":"~0 (только значение, без доп. нагрузки на GPU)."}
labels={}
for fid,it in items.items():
    h=hashlib.md5(fid.encode()).hexdigest()[:10]; tid=f"tt.{h}"
    we,wr=what(it["label"])
    rng_en=f"\nRange {it['rng']}; recommended: stay near the default, change in small steps." if it["rng"] else ("\nRecommended: default." if it["call"]!="Checkbox" else "")
    rng_ru=f"\nДиапазон {it['rng']}; рекомендуется: держаться около значения по умолчанию, менять маленькими шагами." if it["rng"] else ("\nРекомендуется: по умолчанию." if it["call"]!="Checkbox" else "")
    en[tid]=f"{it['label']} ({it['feat']}): {we}{rng_en}\nPerf cost: {COST_EN[it['cost']]}"
    ru[tid]=f"{it['ru_label']} ({it['feat']}): {wr}{rng_ru}\nНагрузка: {COST_RU[it['cost']]}"
    labels[fid]=tid
    if it["ru_label"]!=it["label"]: labels[f"{it['feat']}|{it['ru_label']}"]=tid
json.dump(en,open(f"{TR}/en.json","w"),ensure_ascii=False,indent=4)
json.dump(ru,open(f"{TR}/ru.json","w"),ensure_ascii=False,indent=4)
out=f"{ROOT}/package/SKSE/Plugins/CommunityShaders/Bearforj"; os.makedirs(out,exist_ok=True)
json.dump(labels,open(f"{out}/Tooltips.json","w"),ensure_ascii=False,indent=0)
print(len(items),"settings,",len(labels),"label keys")
