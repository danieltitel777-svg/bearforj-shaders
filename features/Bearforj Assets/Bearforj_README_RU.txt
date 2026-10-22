Bearforj Shaders — сборка 0.2 (21.10.2026)
Основа: Open Shaders 2.19.0 для Skyrim SE 1.6.1170.
DLL называется CommunityShaders.dll, папки, ключи настроек, Overrides и SceneSettings не изменены —
пресеты NAT.CS III и Dlizzio подхватываются как в Open.
Установка: заменить в MO2 мод Open/Bottled Shaders этим (не включать вместе с ними).
ВНИМАНИЕ: сборка в игре не проверялась.

Меню: клавиша END -> Bearforj Shaders. Новые функции по умолчанию ВЫКЛЮЧЕНЫ (Beta) —
включаются в списке функций, после чего появляются в своём разделе.

Добавленные функции и где их найти:
 Персонаж:
  - Fur Shells (мех) — из dev-версии Open Shaders.
  - Hair Backlighting — свет просвечивает сквозь волосы (по образцу Bottled Shaders). Выкл.
  - Submersion Wetness — намокание персонажа при погружении в воду.
 Небо:
  - Physical Sky — физическое небо, атмосфера, объёмные облака, дымка (порт Jiaye),
    действует на небо, воду, объекты, траву, дальние деревья, эффекты, отложенный
    рендер и объёмный туман; опция «солнце во всех интерьерах». Выкл.
 Освещение:
  - Pseudo Sun Bounce — отражённый от земли солнечный свет (из Jiaye). Выкл.
 Ландшафт и текстуры:
  - Terrain Deformation — следы в снегу, грязи и песке (на уровне шейдера). Выкл.
  - Footstep Particles — снег/грязь/песок/брызги из-под ног, лужи и мелкая вода.
 Прочее:
  - Screen Space Physical Particles — экранная физика частиц. Выкл.
  - Physical Effect Particles — физика для всех частиц игры, 9 категорий. Выкл.
  - Precipitation Physics — дождь и снег: ветер, столкновение с землёй и крышами,
    брызги, оседание снега. Выкл.
  - Physical Blood, Physical Leaves — физика крови и листьев. Выкл.
 Также:
  - Inverse Square Lighting — уже есть в основе (флаг света + Light Editor).
  - Снята жёсткая блокировка DTD (NMN_DeformableTerrain.dll).

Пока НЕ входит в сборку:
  - Hi-Z Occlusion (отсечение невидимых объектов);
  - Snow Deformation с настоящей тесселяцией (HS/DS/CS);
  - GPU Footstep Particles как в Bottled;
  - OIT (в Bottled нет исходников).
