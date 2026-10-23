# Bearforj Shaders changelog

## 0.6.7

### English

- Runtime FSR4 can be turned on for Radeon RX 6000 (Navi 21, 22, 23 and 24, including the RX 6950 XT). The switch is marked experimental. RX 7000 and RX 9000 keep the existing switch. RX 6000 is not turned on by itself.
- AMD announced the INT8 FSR Upscaling 4.1 model for RX 6000 in Adrenalin 26.10.1 on 2026-10-20. The public FidelityFX SDK is still v2.3.0 (`60f4ea8`, 2026-06-24). That release documents RX 7000 and RX 9000 only, so the shipped signed DLLs were not replaced.
- If the runtime provider does not start, that session uses FSR 3.1.
- Ryzen integrated graphics and other series stay hidden. A bare "RDNA 2" name is not treated as an RX 6000 card.

### Русский

- Runtime FSR4 можно включить на Radeon RX 6000 (Navi 21, 22, 23 и 24, включая RX 6950 XT). Переключатель помечен как эксперимент. На RX 7000 и RX 9000 остаётся прежний пункт. На RX 6000 он сам не включается.
- AMD объявила INT8-модель FSR Upscaling 4.1 для RX 6000 в Adrenalin 26.10.1 от 2026-10-20. Публичный FidelityFX SDK по-прежнему v2.3.0 (`60f4ea8`, 2026-06-24). В том выпуске описаны только RX 7000 и RX 9000, поэтому подписанные DLL в пакете не менялись.
- Если провайдер не создаётся, эта сессия использует FSR 3.1.
- Встроенная графика Ryzen и другие серии скрыты. Одно имя «RDNA 2» не считается картой RX 6000.

## 0.6.6

### English

- Removed Screen Space Physical Particles, including its rain, snow, sprite atlas, settings and menu entries. That simulation was the stutter.
- Removed Footstep Kick Particles, the old Footstep Particles entry, Native Particle Physics, Physical Blood, Physical Leaves and Precipitation Physics.
- Kept GPU Footstep Particles. Physical Effect Particles is its own small spark, magic and optional fire-ember pool (a few thousand particles, not the old 160k simulation).
- Removed any remaining Unified Water and Water Cache package traces.
- New installs and Restore Defaults use Linear Lighting with ACEScg, ambient 0.32 and saturation 0.92. Skylighting stays on the maximum probe grid. Weather color adaptation is unchanged.
- The shader window title "Bearforj Shaders" sits beside the Mish Mish logo.
- Rain refraction and the raindrop ripple math already matched the 2026-10-22 AIO build (Raindrops.png and optimized-ggx.hlsli are byte-identical). The AIO Wetness Effects file is an older subset, so it was not copied over the current shader.
- Raindrop ripples and splashes show on wet ground again. Flat puddles are still stronger, but a slope no longer turns the impact strength to zero.
- Rain on a character starts as droplets at the head and moves down. The wet area darkens after the droplets. In water, only the part under the surface darkens as he wades. The whole body soaks at once only when it is completely under water. Droplets stay off the submerged part, then run down and thin out as he dries. Hair uses the same rule. Actor darkening follows the stronger 0.13.2 curve (porosity times wetness squared, times 2.4, then a 0.78 absorption).
- During snowfall a light coat settles on upward surfaces of the player, NPCs and creatures, and melts off when the snow stops. It stays a dusting, not a heavy blanket. Roofs block it.
- Cold weather shows breath from Wet and Cold's meshes (isparks, CC BY-NC-SA) on NPC Head [Head], and on a few creature nodes when those bones exist.
- Water keeps the shallow-color normalization that stops orange shallows, the existing shore wetness on sand, and a soft foam tint where the water thins out at the beach. Extra sheets of waves and foam under the surface were not added.
- Snow, mud and sand stand up (60, 18 and 10 units by default). A foot sinks about 65% of that layer, and the old footprints stay when the raise is zero. The raise fades out before the tessellation range so it meets the flat far ground. Shallow sand hollows near water are lowered. Legs standing in snow pick up a coat up to the drift surface.
- The existing water mesh is displaced by nine Gerstner waves. Rivers and waterfalls (flowmap) are not moved. The wave height fades to a flat plane between 4000 and 7000 units. Shore depth is one land-height grid shared by the mesh, the camera and actors, so the waterline does not swim with the camera. The last 4 units of depth at the shore stay on that plane. Fog, refraction and caustics use the displaced surface.

### Русский

- Убраны Screen Space Physical Particles: дождь, снег, атлас спрайтов, настройки и пункты меню. Эта симуляция дёргала кадр.
- Убраны Footstep Kick Particles, старые Footstep Particles, Native Particle Physics, Physical Blood, Physical Leaves и Precipitation Physics.
- GPU Footstep Particles остались. Physical Effect Particles — отдельный маленький пул искр, магии и по желанию искр от костра (несколько тысяч частиц, не старые 160 тысяч).
- Следы Unified Water и Water Cache из пакета убраны.
- Новая установка и «сброс по умолчанию»: линейный свет с ACEScg, ambient 0.32, насыщенность 0.92. Skylighting на максимальной сетке проб. Подстройка цветов под погоду не тронута.
- В окне настроек шейдеров рядом с логотипом Миш Миш написано «Bearforj Shaders».
- Преломление дождя и математика кругов от капель уже совпадали со сборкой AIO от 2026-10-22 (Raindrops.png и optimized-ggx.hlsli побайтно одинаковые). Файл Wetness Effects из AIO старше нашего, поэтому поверх текущего шейдера он не копировался.
- Круги и брызги от капель на мокрой земле снова видны. На плоских лужах они сильнее, но наклон больше не обнуляет силу удара.
- Дождь на персонаже начинается каплями с головы и спускается вниз. Потемнение приходит следом. В воде темнеет только то, что под поверхностью, по мере захода в воду. Весь персонаж промокает сразу только когда он полностью под водой. Капель нет на затопленной части; после выхода они стекают, редеют и пропадают по мере высыхания. Волосы по тем же правилам. Потемнение — кривая 0.13.2: пористость на квадрат мокроты, на 2.4, затем поглощение 0.78.
- В снегопад на верхние поверхности игрока, NPC и существ ложится лёгкий слой и тает, когда снег кончается. Это пыль, не тяжёлый сугроб. Под крышей снег не садится.
- На холоде видно дыхание. Меши Wet and Cold (isparks, CC BY-NC-SA) крепятся к NPC Head [Head], а у части существ — к их костям, если такие есть.
- У воды остаётся нормализация мелкого цвета, из‑за которой мель не оранжевая, прежняя мокрота песка у берега и мягкая пена там, где вода сходит на нет. Отдельные слои волн и пены под поверхностью не добавлялись.
- Снег, грязь и песок приподнимаются (по умолчанию 60, 18 и 10 единиц). Нога проваливается примерно на 65% слоя, а старые следы без подъёма остаются как были. Подъём сходит на нет до границы тесселяции и стыкуется с плоской дальней землёй. Мелкие впадины песка у воды понижаются. Ноги в снегу покрываются до поверхности сугроба.
- Обычная сетка воды смещается девятью волнами Герстнера. Реки и водопады (flowmap) не двигаются. Высота сходит в плоскую плоскость между 4000 и 7000 единицами. Глубина у берега — одна сетка высоты земли, общая для сетки воды, камеры и актёров, поэтому линия берега не плывёт за камерой. Последние 4 единицы глубины у берега остаются на этой плоскости. Туман, преломление и каустика считаются от смещённой поверхности.
