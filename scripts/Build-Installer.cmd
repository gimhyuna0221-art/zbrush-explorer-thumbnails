@echo off
setlocal DisableDelayedExpansion
rem Build only. Never executes the installer or writes user registration.
set "ROOT=%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio 2022 C++ tools and a Windows SDK are required to build from source.
  exit /b 1
)
set "VSROOT="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" exit /b 1
if not exist "%ROOT%\src\Package.h" exit /b 1
set "OUT=%ROOT%\build\installer"
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
pushd "%ROOT%\src"
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /EHsc /W4 /WX /permissive- /sdl /MT /O2 /guard:cf /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 /Fo"%OUT%\Installer.obj" /Fe"%OUT%\ZBrushThumbnailInstaller.exe" Installer.cpp /link /SUBSYSTEM:WINDOWS /MANIFESTUAC:"level='asInvoker' uiAccess='false'" user32.lib gdi32.lib advapi32.lib bcrypt.lib shell32.lib shlwapi.lib ole32.lib uuid.lib /DYNAMICBASE /NXCOMPAT /GUARD:CF /Brepro
set "BUILD_RESULT=%errorlevel%"
popd
exit /b %BUILD_RESULT%
