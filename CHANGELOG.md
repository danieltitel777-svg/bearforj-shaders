# Bearforj Shaders changelog

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
