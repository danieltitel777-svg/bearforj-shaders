# Bearforj Shaders changelog

## 0.6.6

### English

- Removed Screen Space Physical Particles, including its rain, snow, sprite atlas, settings and menu entries. That simulation was the stutter.
- Removed Footstep Kick Particles, the old Footstep Particles entry, Native Particle Physics, Physical Blood, Physical Leaves and Precipitation Physics.
- Kept GPU Footstep Particles. Physical Effect Particles is its own small spark, magic and optional fire-ember pool (a few thousand particles, not the old 160k simulation).
- Removed any remaining Unified Water and Water Cache package traces.
- New installs and Restore Defaults use Linear Lighting with ACEScg, ambient 0.32 and saturation 0.92. Skylighting stays on the maximum probe grid. Weather color adaptation is unchanged.
- The shader window title "Bearforj Shaders" sits beside the Mish Mish logo.

### Русский

- Убраны Screen Space Physical Particles: дождь, снег, атлас спрайтов, настройки и пункты меню. Эта симуляция дёргала кадр.
- Убраны Footstep Kick Particles, старые Footstep Particles, Native Particle Physics, Physical Blood, Physical Leaves и Precipitation Physics.
- GPU Footstep Particles остались. Physical Effect Particles — отдельный маленький пул искр, магии и по желанию искр от костра (несколько тысяч частиц, не старые 160 тысяч).
- Следы Unified Water и Water Cache из пакета убраны.
- Новая установка и «сброс по умолчанию»: линейный свет с ACEScg, ambient 0.32, насыщенность 0.92. Skylighting на максимальной сетке проб. Подстройка цветов под погоду не тронута.
- В окне настроек шейдеров рядом с логотипом Миш Миш написано «Bearforj Shaders».
