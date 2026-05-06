:: build_win32_heap_threadctx_503_icx.bat
@echo off
call loadenv.bat

set PATH=%llvm_path%;%PATH%

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
