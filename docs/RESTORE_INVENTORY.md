# Bearforj Shaders 0.6 — RESTORE INVENTORY (шаг 0)

База: alandtse/open-shaders тег v2.19.0, коммит 7848c83d107c02cbfce8cd2e4aac680705ef50d4, ветка `bearforj`.
Источник восстановления: /workspace/bearforge/pkg/Bearforj Shaders (642 файла): 510 совпадают с апстримом, 14 изменены, 118 новых (из них 15 Textures/meshes, 19 redist DLL апстрима не импортировались).
Импорт: коммит 0ec4327cf. Новые фичи -> `features/<Имя>/Shaders/...`, изменённые общие шейдеры -> на их места в `package/`, прочие ассеты -> `features/Bearforj Assets/`.
Сырьё: dll_ascii.txt (strings), dll_srcfiles.txt (219 путей исходников из DLL), missing_cpp.txt (22 .cpp, которых нет в апстриме), diff_mod.txt, diff_new.txt.

## Важные находки
- DLL в pkg собрана ДО нового UI: строк "SYS.01 // CHAR-ONLINE" и т.п. в ней нет. Новый UI восстанавливать нечем, пишется с нуля.
- Шейдеров тесселяции (hull/domain) для Terrain Deformation в pkg нет. Есть только TerrainDeformation.hlsli + ветка TERRAIN_DEFORMATION в Lighting.hlsl. Тесселяцию надо писать с нуля, это объясняет баг «деформация нигде не работает».
- В /workspace/jiaye лежит только собранный пакет, без .cpp/.h. Physical Sky C++ (PhysicalSky.cpp, PhysicalSky/Ndf.cpp, PhysicalSky/VolumetricClouds.cpp) придётся писать по шейдерам.
- Hi-Z: HiZPyramid.cpp и GrassHiZCS уже есть в апстриме (Grass Optimizations). Своего кода Hi-Z не было.
- SkySync, WetnessEffects, CharacterRainSurfaces, LandscapeSeamsFix уже есть в апстриме 2.19.
- В DLL есть ссылка на Data/SKSE/Plugins/DynamicWetness.dll (детект конфликта).
- Изменённые общие файлы: Lighting.hlsl (+214), SharedData.hlsli (+96: PhysSkyData, PseudoSunBounceSettings, HairBacklightingSettings), Sky.hlsl (+86), Particle.hlsl (+35), Effect/DistantTree/RunGrass/Water.hlsl (PHYSICAL_SKY), DeferredCompositeCS, LightingEval.hlsli (HAIR_BACKLIGHTING), VolumetricFogLightScatteringCS, Themes/Default.json, Translations en.json/ru.json (ru.json уже есть!).

## По модулям
| Модуль | Шейдеры/INI в дереве | C++ переписать (пути из DLL) |
|---|---|---|
| Wetness/Submersion | features/Submersion Wetness: SubmersionWetness.hlsli, ini 1-x; Lighting.hlsl SUBMERSION_WETNESS | src/Features/SubmersionWetness.cpp(.h); апстрим WetnessEffects + CharacterRainSurfaces уже есть |
| SSPP (частицы) | features/Screen Space Physical Particles: Core/* (20 CS), коллизии, HeightField, OrthographicHeightCapture; Physical Leaves, Physical Blood (+dds), Physical Effect Particles (ini) | ScreenSpacePhysicalParticles.cpp, Core/ParticleRuntime.cpp, Collision/PlayerCollisionProvider.cpp, OrthographicHeightField.cpp, ScreenSpaceCollisionDepth.cpp, StressTest.cpp, PhysicalLeaves/PhysicalLeaves.cpp, PhysicalBlood/{PhysicalBlood,Hooks,Profiles}.cpp, PhysicalEffectParticles.cpp, Utils/BodyCapsules.cpp, Utils/HiggsInterface.cpp |
| Физика осадков | features/Precipitation Physics: PrecipitationPhysics.hlsli; Particle.hlsl PRECIPITATION_PHYSICS | PrecipitationPhysics.cpp; апстрим CSEditor/Weather/PrecipitationWidget |
| Terrain deformation + тесселяция | TerrainDeformation.hlsli, Lighting.hlsl TERRAIN_DEFORMATION; HS/DS нет | TerrainDeformation.cpp (строка "TerrainDeformation::Prints"), новые HS/DS шейдеры |
| GPU footstep particles | только FootstepParticles.ini (сим через SSPP Core) | FootstepParticles.cpp (BSTEventSink<BGSFootstepEvent>) |
| Hi-Z | апстрим GrassOptimizations/HiZPyramid + GrassHiZCS | нет своего, только интеграция |
| Hair Backlighting | HairBacklighting.hlsli, LightingEval.hlsli, SharedData HairBacklightingSettings | HairBacklighting (struct в DLL, .cpp-путь не найден, вероятно header-only) |
| Physical Sky (Jiaye) | features/Physical Sky: 26 файлов (облака Nubis, NDF, LUT, cirrus, volumetrics); текстуры Data/Textures/PhysicalSky/* | PhysicalSky.cpp, PhysicalSky/Ndf.cpp, PhysicalSky/VolumetricClouds.cpp |
| Fur Shells (Bottled) | FurShells.hlsli, Lighting.hlsl FUR_SHELLS | FurShells.cpp, по решению 0.6 удаляется |
| Pseudo Sun Bounce | sunbounce.hlsli, SharedData | PseudoSunBounce (struct) |
| Новый UI | Themes/Default.json, ru.json; кода нет | Menu/* с нуля (категории, лого, Миш Миш, Save/Reset/Cache, фикс monoFont) |

## Конфликты
Общие точки: Lighting.hlsl (Fur, Submersion, TerrainDeformation, SunBounce, PhysicalSky, WetnessEffects), SharedData.hlsli (cbuffer FeatureData, порядок полей должен совпадать с C++), Sky.hlsl, Particle.hlsl (Precipitation против SSPP), слоты t55–t126 в PS.
- Open fur против Bottled fur: оба рисуют оболочки в Lighting.hlsl и хукают отрисовку геометрии. Варианты: (а) Open #964–978: апстрим, поддерживается, нет C++ для восстановления; (б) Bottled: наш код утерян. Рекомендация (а).
- Surface Rain Flow (#2912) против нашего wetness: два источника влажности в Lighting.hlsl. Рекомендация: одна система, наш Submersion + WetnessEffects как владелец маски влажности, SRF как слой потоков поверх неё.
- SkySync, physical sun и Jiaye Physical Sky: три владельца направления/цвета солнца. Рекомендация: SkySync единственный владелец, Jiaye только облака/стиль.
- Grass v2 против grass/wind оптимизаций: обе меняют RunGrass.hlsl и инстансинг. Рекомендация: v2 + Open Wind, конфликтующие старые оптимизации убрать, Hi-Z оставить.
- Terrain deformation против травы и тесселяции: смещение земли без смещения травы даёт висящую траву. Рекомендация: деформацию выключать (soft-disable) на траве, тесселяцию только на ландшафте около игрока.
- Precipitation Physics против SSPP: два GPU-сима частиц. Рекомендация: осадки как профиль SSPP Core.
- Scene Manager (CS 2.0): исключён.

## Решения 0.6 (утверждено)
ОСТАВЛЯЕМ: Open fur (#964–978); наш wetness + Surface Rain Flow слоем поверх; SkySync единственный владелец солнца/неба (Jiaye Physical Sky только как стиль облаков); grass v2 + Open wind; terrain deformation с тесселяцией (soft-disable на траве); Hi-Z и все оптимизации апстрима.
УБИРАЕМ: Bottled fur, второе физическое солнце, старые оптимизации травы, конфликтующие с v2, Scene Manager CS 2.0.
Вид по умолчанию (без пресета, совместим с NAT III CS/NAT): днём мягкие тени + отражённый свет, Foliage SSS, клочья тумана, мокрые поверхности после дождя, тёплый ACES-подобный тонмап (Crimson Desert); ночью поднятая база лунного света (светлее, не серо), аврора подсвечивает снег, Hair Backlighting, чуть сильнее объёмный свет от костров/факелов. Воду пока не трогаем.


## Решение: физика осадков → профиль SSPP (запланировано, не реализовано в 0.6-step1/2)
- Дождь/снег/искры переводятся в профили одной общей GPU-симуляции частиц SSPP (без отдельных систем).
- Требования: дождь и снег физические и хорошо заметные; искры живут дольше.
- Surface Rain Flow (PR #2912) и Submersion Wetness остаются поверхностными эффектами и читают интенсивность осадков из WetnessEffects.

## Linear Lighting (ILL) — аудит 0.6
Апстрим 2.19: состояние LL = `SharedData::linearLightingSettings.enableLinearLighting` (`ENABLE_LL`), ACEScg = `ENABLE_ACEScg`; C++ `globals::features::linearLighting.GetCommonBufferData()` / `IsLinearLightingActive()`. Во время отрисовки мира kMAIN хранится в «scene gamma» (флаг `GammaRenderTarget`, кодирование `Color::SceneLinearToGamma` = pow(1/1.6)+гамут), в `OnWorldRenderEnd` декодируется `SceneGammaDecodeCS`; после этого (пост-процесс) цель линейная. Внутри шейдеров с LL цвета линейные (рабочий гамут), без LL — авторская гамма; конверсия только через `Color::*`.

| Модуль | Находка | Исправление |
|---|---|---|
| Default look: тёплый ACES-тонмап, ночная база лунного света, аврора→снег | **Кода нет.** В 0.6 есть только утверждённое описание (раздел «Решения»), ни шейдеров, ни C++ не написано. Проверять нечего. | Требование при реализации: тонмап только в пост-процессе (после декода, цель линейна в обоих режимах → один путь); ночную базу и подсветку снега задавать как авторский цвет через `Color::Light/Ambient` (LL сам масштабирует ×π/ambientMult), не как сырые линейные множители. |
| SSPP (дождь/снег/листья/искры/кровь/пыль) | Рисуется в `OnBeforePostProcessing`, т.е. ПОСЛЕ декода LL. Цвета частиц и `LightColor` (ambient+sun из `Sky::skyColor`) — авторская гамма. С LL они писались как есть в линейную цель → частицы с LL светлее/белёсые, искры пересвечены (то самое «не работает с ILL»). | `SSPPDraw.hlsl`: при LL цвет декодируется ровно как SceneGammaDecodeCS (pow 1.6, + sRGB→AP1 при ACEScg). Флаги LL/ACEScg передаются в `DustP.w`/`BloodP.w` из C++. |
| SubmersionWetness + Surface Rain Flow | Только маски/нормали/шероховатость; затемнение мокрого альбедо апстримовое `pow(base, 1+k)` — степень инвариантна к гамма-домену. SRF (#2912) живёт в WetnessEffects апстрима. | Не требуется. |
| Hair Backlighting | Свет через `context.lightColor` (уже `Color::Light`), порог через `Color::Ambient`, нормировка `Color::VanillaNormalization()` (1/π с LL, компенсирует ×π dirlight). Поглощение `pow(baseColor, a)` доменно-инвариантно. | Не требуется. |
| Terrain deformation (затемнение следа) | `baseColor *= shade` после `Color::Albedo` → с LL множитель применялся к линейному альбедо, т.е. след был светлее, чем без LL (s против s^2.2). | `TerrainDeformation.hlsli`: при LL `shade = pow(shade, 2.2)`. |
| Jiaye Physical Sky (стиль облаков) | Шим `ColorManagement.hlsli` верен внутри Sky/Lighting (до финального `SceneLinearToGamma`). Но (1) купол неба из Sky-View LUT (линейный sRGB) смешивался с psout без конверсии → без LL небо было «двойная гамма» (темнее/насыщеннее), с ACEScg — не тот гамут; (2) трансмиттанс солнца умножался на `dirLightColor`, который без LL в гамме. | `Sky.hlsl`: `ColorManagement::LinearSrgbToScene(skyColor)`; `Lighting.hlsl`: трансмиттанс через `SceneColor::ScaleByLinear`. Остаток: композит объёмных облаков/AP добавляет линейный sRGB без гамут-трансформа при ACEScg (мелкий сдвиг оттенка, только с ACEScg). |

Проверка: fxc OK — SSPPDraw VS/PS, Sky PS (PHYSICAL_SKY DITHER), Lighting PS (LANDSCAPE TERRAIN_DEFORMATION PHYSICAL_SKY DEFERRED; WETNESS SUBMERSION). В игре не проверено.

## Новый UI 0.6 (шаг 2)
- Макеты: /workspace/bearforj_ui/mockup_ru_v2.png, mockup_en_v2.png (скрипт mk2.py, копия в docs/ui/).
- Код: src/Menu/BearforjMenu.{h,cpp} (окно целиком: шапка с лого и EN/RU, 8 вкладок, слева фичи с головой Миш Миша и тумблером, справа DrawSettings фичи, футер Save / Reset to defaults / Clear shader cache + прогресс компиляции). Menu::DrawSettings вызывает BearforjMenu::Render вместо старой шапки и FeatureListRenderer. SYSTEM → General = апстримный DrawGeneralSettings (Shaders/Keybindings/Interface → Behavior → Language).
- Тумблер фичи = State::SetFeatureBootEnabled (сохраняется сразу, применяется после перезапуска, в списке пометка «нужен перезапуск»). Feature::IsDisabledByDefault: все фичи ON, кроме SceneManager и FurShells (решения 0.6).
- Шрифты: ImGui 1.92 грузит глифы динамически, кириллица есть во всех шрифтах кроме Crimson Pro и Sanguis → в них мерджится IBM Plex Serif (только кириллица). Моно IBM Plex Mono добавляется в атлас после каждого Clear() под именем BearforjMono и ищется в живом атласе каждый кадр (ThemeManager::FindMonoFont), указатели не кешируются — фикс бага monoFont из 0.5.
- Картинки: package/Interface/CommunityShaders/Bearforj/ (logo, head_happy, head_grumpy, emotions/*, emotes/*), читаются с диска, можно заменить без пересборки. head_* — заглушки (вырезаны из happy/thinking), ждём Дизайнера.
- ru.json/en.json: +188 RU ключей (все строки Submersion Wetness, Terrain Deformation, SSPP, Surface Rain Flow/WetnessEffects, новый UI).
