@echo off
setlocal DisableDelayedExpansion
rem Build only with an already installed x64 MSVC toolchain and Windows SDK.
rem No downloads, registration, model access, UI or administrator elevation.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio Installer discovery tool not found. Install VS C++ Build Tools first.
  exit /b 1
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
if not "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 1
set "OUT=%~dp0..\build\provider"
set "TESTOUT=%~dp0..\build\tests"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%TESTOUT%" mkdir "%TESTOUT%"
if errorlevel 1 exit /b 1
pushd "%~dp0..\src"
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /EHsc /W4 /WX /permissive- /sdl /MT /O2 /guard:cf /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /LD /Fo"%OUT%\Provider.obj" Provider.cpp /link /OUT:"%OUT%\ZBrushThumbnailProvider.dll" /IMPLIB:"%OUT%\ZBrushThumbnailProvider.lib" /DEF:Provider.def ole32.lib gdi32.lib uuid.lib /DYNAMICBASE /NXCOMPAT /GUARD:CF
if errorlevel 1 goto failed
cl.exe /nologo /std:c++17 /EHsc /W4 /WX /permissive- /sdl /MT /O2 /guard:cf /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo"%TESTOUT%\ProviderTests.obj" /Fe"%TESTOUT%\ProviderTests.exe" ..\tests\ProviderTests.cpp /link ole32.lib gdi32.lib uuid.lib /DYNAMICBASE /NXCOMPAT /GUARD:CF
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
