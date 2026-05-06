:: build_win32_heap_threadctx_503.bat
@echo off
call loadenv.bat
:: echo %sdl2_lib_path_x86%
:: echo %sdl2_lib_path_x64%
:: echo %sdl2_include_path%
:: echo %msvcl_build_path%
:: echo %msvcl_libs%
:: echo %opencl_lib_path_x64%
:: echo %tcc_winapi_include_path%
:: echo %tcc_lib_path%
:: echo %llvm_path%

set PATH=%llvm_path%;%PATH%

if not exist build\clang_x64 mkdir build\clang_x64 
pushd build\clang_x64
clang-cl -target x86_64-pc-windows-msvc ^
  /I"%opencl_include_path%" ^
  /O2 ^
  /GS- ^
  /arch:AVX2 ^
  /W4 ^
  -Xclang -fno-builtin-memcpy ^
  -Xclang -fno-builtin-memset ^
  -Xclang -std=c99 ^
  /Tc ^
  ..\..\win32_heap_threadctx_503.c ^
  /link ^
  /LIBPATH:"%opencl_lib_path_x64%" ^
  OpenCL.lib kernel32.lib user32.lib gdi32.lib ^
  /MACHINE:X64 ^
  /NODEFAULTLIB ^
  /ENTRY:win32_main_entry ^
  /SUBSYSTEM:WINDOWS ^
  /OUT:win32_heap_threadctx_503.exe
popd

call %msvcl_build_path%\vcvarsall.bat x64
if not exist build\msvcl_x64 mkdir build\msvcl_x64 
pushd build\msvcl_x64
cl ^
  /I"%opencl_include_path%" ^
  /O2 ^
  /GS- ^
  /arch:AVX2 ^
  /W4 /permissive- /Tc ^
  ..\..\win32_heap_threadctx_503.c ^
  /link ^
  /LIBPATH:"%opencl_lib_path_x64%" ^
  OpenCL.lib kernel32.lib user32.lib gdi32.lib ^
  /MACHINE:X64 ^
  /NODEFAULTLIB ^
  /ENTRY:win32_main_entry ^
  /SUBSYSTEM:WINDOWS ^
  /OUT:win32_heap_threadctx_503.exe
popd

call "C:\Program Files (x86)\Intel\oneAPI\setvars.bat" intel64 vs2022
if not exist build\intelcx_x64 mkdir build\intelcx_x64 
pushd build\intelcx_x64
icx-cl -target x86_64-pc-windows-msvc ^
  /I"%opencl_include_path%" ^
  /O2 ^
  /GS- ^
  /arch:AVX2 ^
  /W4 ^
  -Xclang -fno-builtin-memcpy ^
  -Xclang -fno-builtin-memset ^
  -Xclang -std=c89 ^
  /Tc ^
  ..\..\win32_heap_threadctx_503.c ^
  /link ^
  /LIBPATH:"%opencl_lib_path_x64%" ^
  OpenCL.lib kernel32.lib user32.lib gdi32.lib ^
  /MACHINE:X64 ^
  /NODEFAULTLIB ^
  /ENTRY:win32_main_entry ^
  /SUBSYSTEM:WINDOWS ^
  /OUT:win32_heap_threadctx_503.exe
popd

pause
::/permissive-  Standard conformance mode (Turn this on instead of /Za). It is much smarter and won't break windows.h.
::/GS-  Disables stack security cookies (Required).
::/Oi Enables intrinsics (Helps the compiler replace some CRT calls with CPU instructions).
::/GR-  Disables RTTI (Run-Time Type Information).
::/EHa- Disables C++ Exception Handling.
::/NODEFAULTLIB The "Nuclear Option" that removes the CRT entirely.