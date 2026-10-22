import json, os, sys, glob
OUT=sys.argv[1]
LIB="/workspace/bearforj/src/package/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Library"
# slot modifiers on top of the profile base look: (tonemap, warmth, nightlift, aurora) deltas + night override
MOD={"clear":(0,0.05,0,0),"cloudy":(-0.03,-0.08,0.03,0),"rain":(-0.05,-0.15,0.05,0),"storm":(-0.08,-0.2,0.0,0),
     "snow":(0.0,-0.12,0.08,0.1),"blizzard":(-0.06,-0.2,0.05,0),"fog":(-0.08,-0.05,0.04,0),"ash":(0.05,0.15,-0.05,0),"aurora_night":(0,-0.05,0.1,0.4)}
NAMES={"clear":"Clear","cloudy":"Cloudy","rain":"Rain","storm":"Storm","snow":"Snow","blizzard":"Blizzard","fog":"Fog","ash":"Ash / volcanic","aurora_night":"Aurora night"}
c=lambda v,lo,hi: round(min(hi,max(lo,v)),3)
packs=[]
for f in sorted(glob.glob(LIB+"/weather_*.json")):
    p=json.load(open(f)); base={"TonemapStrength":0.35,"Warmth":0.5,"NightLift":0.45,"AuroraSnow":0.6,"TonemapMix":100}
    base.update(p["patch"].get("BearforjLook",{}))
    pack=p["id"][8:]; packs.append((pack,p["name"]))
    d=f"{OUT}/{pack}/SKSE/Plugins/CommunityShaders/Bearforj/Presets/{pack}"; os.makedirs(d,exist_ok=True)
    for slot,(t,w,n,a) in MOD.items():
        look={"TonemapStrength":c(base["TonemapStrength"]+t,0,1),"Warmth":c(base["Warmth"]+w,0,1),"NightLift":c(base["NightLift"]+n,0,1.5),"AuroraSnow":c(base["AuroraSnow"]+a,0,2),"TonemapMix":base.get("TonemapMix",100)}
        night=dict(look); night["Warmth"]=c(look["Warmth"]-0.15,0,1)
        json.dump({"name":f'{p["name"]} - {NAMES[slot]}',"BearforjLook":look,"night":{"BearforjLook":night}},open(f"{d}/{slot}.json","w"),indent=2,ensure_ascii=False)
json.dump(packs,open("/workspace/bearforj/tools_bf/packs.json","w"))
print(len(packs),"packs")
