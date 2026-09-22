@echo off
rem Compiles one source of the Direct3D 11 plugin's library on its own (no link), for checking a
rem file while the rest is still being written. Objects go to %TEMP%.
rem   src\plugins\d3d11\compile_check.cmd src\plugins\d3d11\src\capture.cpp [more.cpp ...]
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul
set ROOT=%~dp0..\..\..
cl /nologo /c /std:c++20 /EHsc /W4 /wd4100 /wd4505 /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%\src\sdk\include" /I"%ROOT%\third_party\minhook\include" ^
   /Fo"%TEMP%\\" %*
