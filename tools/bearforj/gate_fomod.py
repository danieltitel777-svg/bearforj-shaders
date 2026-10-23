"""Bearforj 0.6.7 dependency-gated installer page.

One weather and one light. True Light and Light Placer are the beauty path.
Lux is an optional, off-by-default choice and is never required. The picture
preset stays hidden until every gate flag is yes. A failed check installs only
the stop note.

Prism has no profile: the weather library does not name its plugin, and the
filename was not found, so none is invented here.
"""
import json
import sys
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = ROOT / "package/SKSE/Plugins/CommunityShaders/Bearforj/Profiles/Library"
SCHEMA = Path(__file__).with_name("ModuleConfig.xsd")

STOP = "fomod/stop"
PRESET = "preset/default"


# Darker Nights is an addon for Vanilla CS, not a second weather.
ADDON_OF = {
    "weather_vanilla_cs_darker": "weather_vanilla_cs",
}


def weather_profiles(library=LIBRARY):
    """One gate profile per weather file. Alternate filenames of the same file stay together.

    A profile with an empty plugin list (vanilla, no esp) cannot be detected and is omitted.
    An addon listed in ADDON_OF is merged into its parent so both plugins still count as one weather.
    """
    raw = []
    for path in sorted(library.glob("weather_*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        plugins = []
        for name in data.get("plugins") or []:
            if name and name not in plugins:
                plugins.append(name)
        raw.append({
            "id": data.get("id") or path.stem,
            "name": data.get("name") or path.stem,
            "plugins": plugins,
        })
    by_id = {item["id"]: item for item in raw}
    for addon, parent in ADDON_OF.items():
        if addon not in by_id or parent not in by_id:
            continue
        for name in by_id[addon]["plugins"]:
            if name not in by_id[parent]["plugins"]:
                by_id[parent]["plugins"].append(name)
        del by_id[addon]
    profiles = [item for item in raw if item["id"] in by_id and item["plugins"]]
    seen = {}
    for profile in profiles:
        for name in profile["plugins"]:
            other = seen.get(name)
            if other and other != profile["id"]:
                raise SystemExit(f"{name} is listed by both {other} and {profile['id']}")
            seen[name] = profile["id"]
    return profiles


def weather_plugin_names(library=LIBRARY):
    names = []
    for profile in weather_profiles(library):
        names.extend(profile["plugins"])
    return names


def _file_deps(names, state="Active", operator="Or"):
    inner = "".join(f'<fileDependency file="{escape(name)}" state="{state}"/>' for name in names)
    return f'<dependencies operator="{operator}">{inner}</dependencies>'


def _not_active(names):
    """Every named file is missing or disabled. Active fails the group."""
    parts = []
    for name in names:
        parts.append(
            '<dependencies operator="Or">'
            f'<fileDependency file="{escape(name)}" state="Missing"/>'
            f'<fileDependency file="{escape(name)}" state="Inactive"/>'
            "</dependencies>"
        )
    return f'<dependencies operator="And">{"".join(parts)}</dependencies>'


def _exactly_one_weather(profiles, index):
    """This profile has one active plugin, and every other profile has none."""
    chosen = _file_deps(profiles[index]["plugins"], state="Active", operator="Or")
    others = "".join(_not_active(profile["plugins"]) for i, profile in enumerate(profiles) if i != index)
    return f'<dependencies operator="And">{chosen}{others}</dependencies>'


def _typed(default, patterns):
    body = "".join(patterns)
    return (
        "<typeDescriptor><dependencyType>"
        f"<defaultType name=\"{default}\"/>"
        f"<patterns>{body}</patterns>"
        "</dependencyType></typeDescriptor>"
    )


def _pattern(dependencies, type_name):
    return f"<pattern>{dependencies}<type name=\"{type_name}\"/></pattern>"


def _plugin(name, description, flag, value, type_xml, folder=None):
    files = ""
    if folder:
        files = f'<files><folder source="{escape(folder)}" destination="" priority="0"/></files>'
    flags = f'<conditionFlags><flag name="{flag}">{value}</flag></conditionFlags>'
    # Flags are required when a plugin installs nothing. When it installs a folder
    # the schema still accepts the flags after the files.
    body = f"{files}{flags}" if files else flags
    return (
        f'<plugin name="{escape(name)}">'
        f"<description><![CDATA[{description}]]></description>"
        f"{body}{type_xml}</plugin>"
    )


def _as_patterns(dependencies):
    if isinstance(dependencies, (list, tuple)):
        return list(dependencies)
    return [dependencies]


def _stop_found(group, stop_name, found_name, description, flag, dependencies, stop_folder=STOP):
    patterns = _as_patterns(dependencies)
    stop = _plugin(
        stop_name,
        description,
        flag,
        "no",
        _typed("Required", [_pattern(item, "NotUsable") for item in patterns]),
        stop_folder,
    )
    found = _plugin(
        found_name,
        description,
        flag,
        "yes",
        _typed("NotUsable", [_pattern(item, "Required") for item in patterns]),
    )
    return (
        f'<group name="{escape(group)}" type="SelectExactlyOne">'
        f"<plugins>{stop}{found}</plugins></group>"
    )


def module_xml(weather_files=None, core_source=None, stop_source=STOP, preset_source=PRESET, profiles=None):
    if profiles is None:
        profiles = [{"id": name, "name": name, "plugins": [name]} for name in (weather_files or [])]
    if not profiles:
        raise SystemExit("FOMOD needs at least one weather profile")
    true_light = _file_deps(["True Light.esp", "TrueLight.esp"])
    placer = _file_deps(["SKSE/Plugins/po3_LightPlacer.dll"], operator="And")
    weather_patterns = [_exactly_one_weather(profiles, index) for index in range(len(profiles))]
    windows = _file_deps(["Window Shadows Ultimate Supplement.esp"], operator="And")
    embers = _file_deps(["Embers XD.esp"], operator="And")
    helios_names = ["Helios.esp", "Helios.esl", "Helios.esm"]
    helios_files = _file_deps(helios_names)
    helios_absent = _not_active(helios_names)
    game_17 = "<dependencies><gameDependency version=\"1.7.0\"/></dependencies>"
    game_161170 = "<dependencies><gameDependency version=\"1.6.1170\"/></dependencies>"
    helios_window_missing = f'<dependencies operator="And">{game_161170}{helios_absent}</dependencies>'
    helios_window_found = f'<dependencies operator="And">{game_161170}{helios_files}</dependencies>'
    lux_files = ["Lux.esp", "Lux - Master plugin.esm", "Lux CS.esp", "Lux Orbis.esp", "Lux Via.esp"]

    light = """RU: Красивый путь ждёт True Light 3.6.5 (Bottle, ведёт SkrubbySkrubInAShrub).
Lux этот пункт не закрывает и по умолчанию выключен.
https://www.nexusmods.com/skyrimspecialedition/mods/135488
Нужны ещё Light Placer 5.0.0, CS Light, Luma Utility, ATLM и powerofthree's Tweaks.
Имена файлов Tweaks, Luma, ATLM, XEMI и MMSF на открытых страницах не напечатаны, поэтому здесь их нет.
Отмените установщик, поставьте моды, запустите снова.
У fileDependency нет атрибута версии: номера 3.6.5, 5.0.0, 1.4.6 и 3.3.0 в XML не проверяются, только имя файла.
EN: The beauty path needs True Light 3.6.5. Lux does not count and stays off.
Also install Light Placer 5.0.0, CS Light, Luma Utility, ATLM and powerofthree's Tweaks.
Cancel, install the mods, run this installer again.
XML fileDependency has no version attribute, so 3.6.5, Light Placer 5.0.0, Window Shadows Ultimate 1.4.6 and Embers XD 3.3.0 are named here and are not numeric checks. The file name is all the schema can test."""
    placer_text = """RU: Нужен Light Placer 5.0.0, powerofthree.
https://www.nexusmods.com/skyrimspecialedition/mods/127557
Файл: SKSE/Plugins/po3_LightPlacer.dll и его требование powerofthree's Tweaks.
EN: Install Light Placer 5.0.0 and powerofthree's Tweaks, then run this again."""
    weather_text = """RU: Нужна ровно одна погода из профилей Bearforj. Две сразу — стоп.
Несколько имён одного мода (NAT.esp и NAT.CS.esp) считаются одной погодой.
Darker Nights для Vanilla CS — дополнение той же погоды, не вторая.
Ваниль без своего esp установщик не видит. Prism не добавлен: имя плагина неизвестно.
EN: Exactly one Bearforj weather profile may be active. Two profiles stop the install.
Alternate filenames of one mod count as that one weather.
Darker Nights for Vanilla CS is the same weather, not a second one.
Vanilla with no plugin file cannot be detected. No Prism profile: the plugin filename is unknown."""
    windows_text = """RU: Нужен Window Shadows Ultimate 1.4.6, CarbonDice и Dlizzio.
https://www.nexusmods.com/skyrimspecialedition/mods/150494
Проверяем файл из журнала 1.2: Window Shadows Ultimate Supplement.esp.
Имя основного esp страница не напечатала, оно здесь не выдумано.
С True Light не берите чужие тени окон Lux и ELFX Shadows.
EN: Install Window Shadows Ultimate 1.4.6. The check looks for the supplement plugin named in the 1.2 changelog."""
    embers_text = """RU: Нужен Embers XD 3.3.0, mindflux, не старый Embers HD.
https://www.nexusmods.com/skyrimspecialedition/mods/37085
В его установщике: стиль пламени Community Shaders и None (CS/OS) для чужих ламп.
Свои лампы Light Placer из Embers XD не включать.
EN: Install Embers XD 3.3.0. Embers HD does not pass this check."""
    helios_text = """RU: Helios нужен, когда версия игры не ниже 1.6.1170 и ниже 1.7.0.
Ниже 1.6.1170 и на 1.7.0 шаг пропускается. Номер 1.01 самого Helios XML не читает.
https://www.nexusmods.com/skyrimspecialedition/mods/181533
Luma 2.0 с Helios 1.01 ломает дом. На 1.7 Helios 2.0 ещё нет, 1.01 не ставить.
EN: Helios is required when the game is at least 1.6.1170 and below 1.7.0.
Below 1.6.1170, and on 1.7.0, this step is skipped. The XML cannot read Helios 1.01."""
    lux_off = """RU: По умолчанию. Lux не требуется и не ставится.
EN: Default. Lux is not required and is not installed."""
    lux_on = """RU: Не рекомендуем. Если True Light уже активен, этот пункт выбрать нельзя.
Файлы Lux здесь не ставятся: Lux.esp, Lux - Master plugin.esm, Lux CS.esp, Lux Orbis.esp, Lux Via.esp.
EN: Optional, off by default, never required. Not selectable while True Light is active.
This choice does not install Lux."""
    advice = """RU: Не блокирует установку. Faultier's PBR (2K) и одна комплексная трава с вики Community Shaders.
ENB в корне игры снимите сами: d3d11.dll установщик как файл мода часто не видит.
EN: Not required. Faultier's PBR at 2K, and one complex-grass mod from the Community Shaders wiki.
Remove an ENB d3d11.dll yourself; the installer often cannot see it."""
    preset_text = """RU: Кладёт SettingsDefault.json из пресета картинки 0.6.7. Девять ключей fVolumetricLighting* плагин применяет в памяти при старте и не переписывает Skyrim.ini.
На 4K, если эти значения ещё не сохранены игроком, сетка лучей Medium, марш облаков выключен, шаги 32.
EN: Installs the 0.6.7 picture preset. The nine fVolumetricLighting keys are applied in memory at startup and Skyrim.ini is not rewritten."""

    groups = [
        _stop_found("Свет / Light replacer", "Стоп: нет True Light / Stop: True Light missing", "True Light найден / True Light found", light, "gate_light", true_light, stop_source),
        _stop_found("Light Placer", "Стоп: нет Light Placer / Stop: Light Placer missing", "Light Placer найден / Light Placer found", placer_text, "gate_placer", placer, stop_source),
        _stop_found(
            "Погода / Weather",
            "Стоп: нет ровно одной погоды / Stop: not exactly one weather",
            "Ровно одна погода / Exactly one weather",
            weather_text,
            "gate_weather",
            weather_patterns,
            stop_source,
        ),
        _stop_found("Окна / Windows", "Стоп: нет Window Shadows Ultimate", "Window Shadows Ultimate найден", windows_text, "gate_wsu", windows, stop_source),
        _stop_found("Огонь / Fire", "Стоп: нет Embers XD", "Embers XD найден", embers_text, "gate_embers", embers, stop_source),
    ]
    # gameDependency is a minimum. 1.7 matches 1.6.1170 as well, so the 1.7 pattern is first.
    helios_stop = _plugin(
        "Стоп: нет Helios на 1.6.1170 / Stop: Helios missing on 1.6.1170",
        helios_text,
        "gate_helios",
        "no",
        _typed("NotUsable", [_pattern(game_17, "NotUsable"), _pattern(helios_window_missing, "Required")]),
        stop_source,
    )
    helios_found = _plugin(
        "Helios найден / Helios found",
        helios_text,
        "gate_helios",
        "yes",
        _typed("NotUsable", [_pattern(game_17, "NotUsable"), _pattern(helios_window_found, "Required")]),
    )
    helios_skip = _plugin(
        "Helios не нужен на этой версии игры / Helios not required on this game",
        helios_text,
        "gate_helios",
        "yes",
        _typed("Required", [_pattern(game_17, "Required"), _pattern(game_161170, "NotUsable")]),
    )
    groups.append(
        '<group name="Helios" type="SelectExactlyOne"><plugins>'
        + helios_stop + helios_found + helios_skip + "</plugins></group>"
    )
    lux_off_plugin = _plugin(
        "Не ставить Lux / Do not install Lux",
        lux_off,
        "lux",
        "off",
        '<typeDescriptor><type name="Recommended"/></typeDescriptor>',
    )
    lux_on_plugin = _plugin(
        "Lux уже стоит, оставить / Lux is already installed",
        lux_on + "\n" + ", ".join(lux_files),
        "lux",
        "on",
        _typed("Optional", [_pattern(true_light, "NotUsable")]),
    )
    groups.append(
        '<group name="Lux" type="SelectExactlyOne"><plugins>' + lux_off_plugin + lux_on_plugin + "</plugins></group>"
    )
    groups.append(
        '<group name="Рекомендуем / Suggested" type="SelectAny"><plugins>'
        + _plugin(
            "PBR и трава, не обязательно / PBR and grass, not required",
            advice,
            "advice",
            "shown",
            '<typeDescriptor><type name="Optional"/></typeDescriptor>',
        )
        + "</plugins></group>"
    )

    preset_files = ""
    if core_source:
        preset_files += f'<folder source="{escape(core_source)}" destination="" priority="0"/>'
    preset_files += f'<folder source="{escape(preset_source)}" destination="SKSE/Plugins/CommunityShaders" priority="1"/>'
    preset_plugin = (
        '<plugin name="Пресет Bearforj по умолчанию / Bearforj default preset">'
        f"<description><![CDATA[{preset_text}]]></description>"
        f"<files>{preset_files}</files>"
        '<conditionFlags><flag name="preset">yes</flag></conditionFlags>'
        '<typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>'
    )
    visible = (
        "<visible>"
        '<flagDependency flag="gate_light" value="yes"/>'
        '<flagDependency flag="gate_placer" value="yes"/>'
        '<flagDependency flag="gate_weather" value="yes"/>'
        '<flagDependency flag="gate_wsu" value="yes"/>'
        '<flagDependency flag="gate_embers" value="yes"/>'
        '<flagDependency flag="gate_helios" value="yes"/>'
        "</visible>"
    )
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
<moduleName>Bearforj Shaders</moduleName>
<installSteps order="Explicit">
<installStep name="Проверка / Check">
<optionalFileGroups>{"".join(groups)}</optionalFileGroups>
</installStep>
<installStep name="Пресет картинки / Picture preset">
{visible}
<optionalFileGroups>
<group name="Пресет" type="SelectExactlyOne"><plugins>{preset_plugin}</plugins></group>
</optionalFileGroups>
</installStep>
</installSteps>
</config>
'''


def write_checked_in(root=ROOT):
    names = weather_plugin_names()
    if not names:
        raise SystemExit("no weather plugin names in the Bearforj library")
    xml = module_xml(names)
    fomod = root / "fomod"
    fomod.mkdir(exist_ok=True)
    (fomod / "ModuleConfig.xml").write_text(xml, encoding="utf-8")
    stop = fomod / "stop"
    stop.mkdir(exist_ok=True)
    (stop / "Bearforj_FOMOD_STOP.txt").write_text(
        "Bearforj Shaders\n\n"
        "RU: Красивый путь не установлен. Нужны ровно одна погода, True Light, Light Placer 5.0.0,\n"
        "Window Shadows Ultimate, Embers XD и Helios, когда игра от 1.6.1170 и ниже 1.7.0.\n"
        "Номера версий модов XML не проверяет: у fileDependency нет атрибута версии.\n"
        "Lux не требуется. Профиль Prism не добавлен: имя плагина неизвестно.\n"
        "Поставьте моды и запустите установщик снова. Этот файл — единственное, что ставится без проверки.\n\n"
        "EN: The beauty path was not installed. It needs exactly one weather, True Light, Light Placer,\n"
        "Window Shadows Ultimate, Embers XD, and Helios when the game is at least 1.6.1170 and below 1.7.0.\n"
        "Mod version numbers are not checked: fileDependency has no version attribute.\n"
        "Lux is not required. No Prism profile was added because the plugin filename is unknown.\n"
        "Install the mods and run the installer again. This file is the only thing installed when a check fails.\n",
        encoding="utf-8",
    )
    return xml


def validate(xml):
    from lxml import etree
    schema = etree.XMLSchema(etree.parse(str(SCHEMA)))
    document = etree.fromstring(xml.encode("utf-8"))
    if not schema.validate(document):
        raise SystemExit(f"FOMOD schema failed:\n{schema.error_log}")


if __name__ == "__main__":
    text = write_checked_in()
    validate(text)
    profiles = weather_profiles()
    print(
        f"schema valid, {len(profiles)} weather profiles, {len(weather_plugin_names())} plugin names, "
        "exactly one profile, Prism not included, mod versions not readable by fileDependency"
    )
