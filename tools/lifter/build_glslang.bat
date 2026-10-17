@echo off
rem Builds build\bin\smo_glslang.dll (runtime\vk_glslang_shim.c + glslang static libraries) with MSVC.
rem Adapted from ssbu-recomp runtime\build_glslang.bat (GPL-3.0-or-later). GLSLANG_ROOT = a folder with include\ and lib\
rem (or Include\ and Lib\): the glslang release package, or the Vulkan SDK (default: the newest C:\VulkanSDK\*).
rem Why a DLL: glslang is MSVC C++ built against the DLL C runtime (/MD); the game exe is C linked by clang.
setlocal
set HERE=%~dp0
set REPO=%HERE%..\..
if "%GLSLANG_ROOT%"=="" for /d %%d in (C:\VulkanSDK\*) do set GLSLANG_ROOT=%%d
if "%GLSLANG_ROOT%"=="" ( echo build_glslang: no GLSLANG_ROOT and no C:\VulkanSDK & exit /b 1 )
set GL=%GLSLANG_ROOT%
set VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
if exist "%VSW%" "%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\smo_vsdir.txt"
if exist "%TEMP%\smo_vsdir.txt" set /p VSDIR=<"%TEMP%\smo_vsdir.txt"
if "%VSDIR%"=="" ( echo build_glslang: no Visual Studio / Build Tools with the C++ x64 tools found & exit /b 1 )
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%REPO%\build\obj\glslang" mkdir "%REPO%\build\obj\glslang"
if not exist "%REPO%\build\bin" mkdir "%REPO%\build\bin"
echo build_glslang: GLSLANG_ROOT=%GL%
cl /nologo /MD /O2 /W3 /LD /I "%GL%\include" "%REPO%\runtime\vk_glslang_shim.c" /Fo:"%REPO%\build\obj\glslang\vk_glslang_shim.obj" /Fe:"%REPO%\build\bin\smo_glslang.dll" /link /LIBPATH:"%GL%\lib" glslang.lib glslang-default-resource-limits.lib SPIRV-Tools-opt.lib SPIRV-Tools.lib
exit /b %errorlevel%
