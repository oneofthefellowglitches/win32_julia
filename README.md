== win32_heap_threadctx_503.c: 
main application source file

== crt.h & minicrt.h: 
minimal C runtime headers, allowing without linking against the standard C library

== system.h: 
a platform-abstraction header, type definitions, SIMD includes, and compiler-specific macros

== q1616.h: 
implementation of the Q16.16 fixed-point math library

== palette.h: 
color palette system for the fractal renderer

== ocl.h & ocl_stub.h: 
headers related to future OpenCL support for GPU acceleration

== example.env & .env: 
configuration files for setting up build environment variables

== build_*.bat: 
scripts to compile the project using different compilers (Clang, MSVC, Intel C++ Compiler)

== loadenv.bat: 
script to load the environment configuration



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
