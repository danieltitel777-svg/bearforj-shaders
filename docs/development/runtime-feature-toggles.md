# Runtime feature toggles

Feature availability (`loaded`) and the live rendering switch (`IsEnabled()`) are
separate. Features named in `package/Shaders/Common/RuntimeFeatureList.hlsli`
stay loaded when their saved switch is off, so their shader defines remain in
the cache. `SetEnabled` changes `runtimeEnabled`. Shader `#if` guards stay.
`ShaderCache` still keys defines off `loaded`, not `IsEnabled()`.

The shared feature buffer ends with `uint4 RuntimeFeatureFlags`, immediately
after `rainRenderingSettings`. C++ fills that mask from the same ordered list.
Render dispatches and shader branches that were wrapped consult
`RuntimeFeatures::IsEnabled`.

Light Limit Fix, Horizon Fix, Unified Water, and True PBR require a restart.
Their hooks are skipped when the latched switch is off. Other listed features
can change during play. Features that are not in the 51-name list keep the
existing boot switch: off means `Load()` is skipped.

`HasReleasableResources()` stays false, so a live disable does not free the
feature's textures and buffers. The switch gates use. Per-feature
`OnRuntimeEnabled` history resets from Open Shaders were not copied.

Devbench command registration is not part of this tree. The menu checkbox
persists `Disable at Boot` and, for a listed feature, also calls `SetEnabled`.
