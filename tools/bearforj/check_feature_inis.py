"""0.6.3 build check: every features/*/Shaders/Features/<Name>.ini must belong to a registered Feature
(a GetShortName() returning "<Name>" in src/, or an SSPP sub-feature short name). Exit 1 on orphans."""
import glob, os, re, sys
root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
names = set()
for f in glob.glob(f"{root}/src/**/*.[ch]*", recursive=True):
    t = open(f, encoding="utf-8", errors="ignore").read()
    names |= set(re.findall(r'GetShortName\(\)[^{]*\{\s*return\s*"([A-Za-z0-9_]+)"', t))
    if f.endswith("SSPPSubFeatures.cpp"):
        names |= set(re.findall(r'SSPPSubFeature f\{ K::\w+, "[^"]*", "([A-Za-z0-9_]+)"', t))
inis = sorted(glob.glob(f"{root}/features/*/Shaders/Features/*.ini"))
orph = [i for i in inis if os.path.splitext(os.path.basename(i))[0] not in names]
for i in orph:
    print(f"ORPHAN feature INI (no registered feature): {os.path.relpath(i, root)}")
print(f"check_feature_inis: {len(inis)} INIs, {len(orph)} orphans")
sys.exit(1 if orph else 0)
