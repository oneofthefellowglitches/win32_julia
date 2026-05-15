TODO: switch from GDI to DXGI

/**GDI
== CPU rasterization

== Immediate mode

== State-heavy, fragile

== Single-threaded by design

== No real backbuffer unless you build one

✅ Dead simple

✅ Zero setup

✅ Perfect for:

❌ Flicker unless double-buffered

❌ No vsync control

❌ DPI / scaling weirdness

❌ Slow at scale

❌ Undefined behavior if GDI objects are mishandled

❌ Not compatible with Vulkan/modern GPU paths

Window ownership | Backbuffer presentation | HUD drawing (one to keep)
*/



/**DXGI:

owns backbuffers (GPU textures) | handles vsync | handles tearing | handles fullscreen | handles resizing | is required for D3D11 / D3D12 / Vulkan-on-Windows

✅ Tear-free

✅ VSync control

✅ Flip model (modern)

✅ Zero flicker

✅ GPU-native

✅ Plays perfectly with Vulkan

✅ No GDI object lifetime madness
*/

TODO: introduce a general lock-free job system
