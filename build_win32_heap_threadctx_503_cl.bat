:: build_win32_heap_threadctx_503_cl.bat
@echo off
call loadenv.bat

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

pause
::/permissive-  Standard conformance mode (Turn this on instead of /Za). It is much smarter and won't break windows.h.
::/GS-  Disables stack security cookies (Required).
::/Oi Enables intrinsics (Helps the compiler replace some CRT calls with CPU instructions).
::/GR-  Disables RTTI (Run-Time Type Information).
::/EHa- Disables C++ Exception Handling.
::/NODEFAULTLIB The "Nuclear Option" that removes the CRT entirely.