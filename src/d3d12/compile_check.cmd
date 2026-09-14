@echo off
rem Compiles one source of the D3D12 capture library on its own (no link), for checking a file
rem while the rest of the library is still being written. Objects go to %TEMP%.
rem   src\d3d12\compile_check.cmd src\d3d12\src\capture.cpp [more.cpp ...]
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul
set ROOT=%~dp0..\..
cl /nologo /c /std:c++20 /EHsc /W3 /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I"%ROOT%\src\d3d12\src" /I"%ROOT%\src\d3d12\gen" /I"%ROOT%\src\vulkan\src" /I"%ROOT%\third_party\minhook\include" ^
   /Fo"%TEMP%\\" %*
