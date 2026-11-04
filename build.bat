@echo off
rem Builds dinput8.dll from pip.cpp. Needs Visual Studio 2019 or 2022 (or the Build Tools) with "Desktop development with C++".
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto found
echo vswhere.exe not found - install Visual Studio or the Build Tools
exit /b 1
:found
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if defined VSDIR goto vs
echo No Visual Studio with the C++ tools found
exit /b 1
:vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /LD pip.cpp /Fe:dinput8.dll /link /DEF:dinput8.def d3d11.lib dxgi.lib user32.lib hid.lib setupapi.lib || exit /b 1
echo Built %~dp0dinput8.dll
