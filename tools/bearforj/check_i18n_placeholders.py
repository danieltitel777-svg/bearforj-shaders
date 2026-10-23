#!/usr/bin/env python3
"""Fail when a translation is missing a {name} placeholder that en.json has, or adds one en.json does not have.

Keys missing from a language file are reported too. Empty strings are allowed when the English text has no placeholders.
"""
import json
import re
import sys
from pathlib import Path

PH = re.compile(r"\{[A-Za-z0-9_]+\}")


def main():
    root = Path(__file__).resolve().parents[2]
    folder = root / "package/SKSE/Plugins/CommunityShaders/Translations"
    en = json.loads((folder / "en.json").read_text(encoding="utf-8"))
    # Bearforj ships English and Russian. Other locale files are upstream leftovers
    # and are not part of this parity check.
    failed = False
    for path in [folder / "ru.json"]:
        data = json.loads(path.read_text(encoding="utf-8"))
        missing = []
        bad = []
        for key, english in en.items():
            if key == "_meta" or not isinstance(english, str):
                continue
            if key not in data:
                missing.append(key)
                continue
            value = data[key] if isinstance(data[key], str) else ""
            if set(PH.findall(english)) != set(PH.findall(value)):
                bad.append(key)
        if missing or bad:
            failed = True
            print(f"{path.name}: {len(missing)} missing, {len(bad)} placeholder mismatches")
            for key in bad[:30]:
                print(f"  placeholder {key}")
        else:
            print(f"{path.name}: placeholders match")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
