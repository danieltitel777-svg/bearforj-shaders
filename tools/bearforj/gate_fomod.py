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


def weather_plugin_names(library=LIBRARY):
    names = []
    seen = set()
    for path in sorted(library.glob("weather_*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        for name in data.get("plugins") or []:
            if name and name not in seen:
                seen.add(name)
                names.append(name)
    return names


def _file_deps(names, state="Active", operator="Or"):
    inner = "".join(f'<fileDependency file="{escape(name)}" state="{state}"/>' for name in names)
    return f'<dependencies operator="{operator}">{inner}</dependencies>'


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


def _stop_found(group, stop_name, found_name, description, flag, dependencies, stop_folder=STOP):
    stop = _plugin(
        stop_name,
        description,
        flag,
        "no",
        _typed("Required", [_pattern(dependencies, "NotUsable")]),
        stop_folder,
    )
    found = _plugin(
        found_name,
        description,
        flag,
        "yes",
        _typed("NotUsable", [_pattern(dependencies, "Required")]),
    )
    return (
        f'<group name="{escape(group)}" type="SelectExactlyOne">'
        f"<plugins>{stop}{found}</plugins></group>"
    )


def module_xml(weather_files, core_source=None, stop_source=STOP, preset_source=PRESET):
    true_light = _file_deps(["True Light.esp", "TrueLight.esp"])
    placer = _file_deps(["SKSE/Plugins/po3_LightPlacer.dll"], operator="And")
    weather = _file_deps(weather_files)
    windows = _file_deps(["Window Shadows Ultimate Supplement.esp"], operator="And")
    embers = _file_deps(["Embers XD.esp"], operator="And")
    helios_files = _file_deps(["Helios.esp", "Helios.esl", "Helios.esm"])
    game_17 = "<dependencies><gameDependency version=\"1.7.0\"/></dependencies>"
    lux_files = ["Lux.esp", "Lux - Master plugin.esm", "Lux CS.esp", "Lux Orbis.esp", "Lux Via.esp"]

    light = """RU: Красивый путь ждёт True Light 3.6.5 (Bottle, ведёт SkrubbySkrubInAShrub).
Lux этот пункт не закрывает и по умолчанию выключен.
https://www.nexusmods.com/skyrimspecialedition/mods/135488
Нужны ещё Light Placer 5.0.0, CS Light, Luma Utility, ATLM и powerofthree's Tweaks.
Имена файлов Tweaks, Luma, ATLM, XEMI и MMSF на открытых страницах не напечатаны, поэтому здесь их нет.
Отмените установщик, поставьте моды, запустите снова.
EN: The beauty path needs True Light 3.6.5. Lux does not count and stays off.
Also install Light Placer 5.0.0, CS Light, Luma Utility, ATLM and powerofthree's Tweaks.
Cancel, install the mods, run this installer again."""
    placer_text = """RU: Нужен Light Placer 5.0.0, powerofthree.
https://www.nexusmods.com/skyrimspecialedition/mods/127557
Файл: SKSE/Plugins/po3_LightPlacer.dll и его требование powerofthree's Tweaks.
EN: Install Light Placer 5.0.0 and powerofthree's Tweaks, then run this again."""
    weather_text = """RU: Нужна ровно одна погода под Community Shaders. Две сразу ставить не надо.
Установщик видит, что активен хотя бы один плагин из профилей Bearforj. Два активных файла он отличить не может.
Профиль Prism не добавлен: имя его плагина неизвестно.
Подойдут NAT, Cathedral, Azurite, Obsidian, Vanilla CS, Vespro, Astralite, Faithful, Mythical Ages, Nirn Whispers, Real Weathers.
EN: Install one Community Shaders weather. The check passes when any listed plugin is active.
It cannot tell that two weather mods are active together. Do not install two.
No Prism profile: the plugin filename is not known."""
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
    helios_text = """RU: На 1.6.1170 нужен Helios 1.01, isoprovophlex, плюс Luma 1.7, MMSF 1.1 и XEMI.
https://www.nexusmods.com/skyrimspecialedition/mods/181533
Luma 2.0 с Helios 1.01 ломает дом. На Skyrim 1.7 этот шаг пропускается: Helios 2.0 ещё нет, 1.01 не ставить.
EN: On 1.6.1170 install Helios 1.01 with Luma 1.7 and MMSF 1.1. On 1.7 this step is skipped."""
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
        _stop_found("Погода / Weather", "Стоп: нет CS-погоды / Stop: no CS weather", "Погода найдена / Weather found", weather_text, "gate_weather", weather, stop_source),
        _stop_found("Окна / Windows", "Стоп: нет Window Shadows Ultimate", "Window Shadows Ultimate найден", windows_text, "gate_wsu", windows, stop_source),
        _stop_found("Огонь / Fire", "Стоп: нет Embers XD", "Embers XD найден", embers_text, "gate_embers", embers, stop_source),
    ]
    helios_stop = _plugin(
        "Стоп: нет Helios на 1.6 / Stop: Helios missing on 1.6",
        helios_text,
        "gate_helios",
        "no",
        _typed("Required", [_pattern(game_17, "NotUsable"), _pattern(helios_files, "NotUsable")]),
        stop_source,
    )
    helios_found = _plugin(
        "Helios найден / Helios found",
        helios_text,
        "gate_helios",
        "yes",
        _typed("NotUsable", [_pattern(game_17, "NotUsable"), _pattern(helios_files, "Required")]),
    )
    helios_skip = _plugin(
        "Skyrim 1.7: Helios пропущен / Skyrim 1.7: Helios skipped",
        helios_text,
        "gate_helios",
        "yes",
        _typed("NotUsable", [_pattern(game_17, "Required")]),
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
        "RU: Красивый путь не установлен. Нужны True Light 3.6.5, Light Placer 5.0.0, одна CS-погода,\n"
        "Window Shadows Ultimate 1.4.6, Embers XD 3.3.0 и, только на Skyrim 1.6.1170, Helios 1.01.\n"
        "Lux не требуется. Профиль Prism не добавлен: имя плагина неизвестно.\n"
        "Поставьте моды и запустите установщик снова. Этот файл — единственное, что ставится без проверки.\n\n"
        "EN: The beauty path was not installed. It needs True Light 3.6.5, Light Placer 5.0.0, one CS weather,\n"
        "Window Shadows Ultimate 1.4.6, Embers XD 3.3.0, and Helios 1.01 only on Skyrim 1.6.1170.\n"
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
    print(f"schema valid, {len(weather_plugin_names())} weather plugin names, Prism not included")
