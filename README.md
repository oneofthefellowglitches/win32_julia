win32_heap_threadctx_503.c: The main application source file.

crt.h & minicrt.h: Custom, minimal C runtime headers, allowing the program to run without linking against the standard C library.

system.h: A platform-abstraction header that handles type definitions, SIMD includes, and compiler-specific macros.

q1616.h: The implementation of the Q16.16 fixed-point math library.

palette.h: The color palette system for the fractal renderer.

ocl.h & ocl_stub.h: Headers related to future OpenCL support for GPU acceleration.

example.env & .env: Configuration files for setting up build environment variables.

build_*.bat: A collection of Batch scripts to compile the project using different compilers (Clang, MSVC, Intel C++ Compiler).

loadenv.bat: A helper script to load the environment configuration



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
