@echo off
setlocal
cd /d "%~dp0.."
if not defined VSCMD_ARG_TGT_ARCH (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%I\VC\Auxiliary\Build\vcvars32.bat" >nul
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 1
if not exist output mkdir output
cl /nologo /O2 /arch:IA32 /std:c++17 /utf-8 /EHsc /W4 tests\CoopModConsentTests.cpp /Fo:output\CoopModConsentTests.obj /Fe:output\CoopModConsentTests.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
output\CoopModConsentTests.exe
exit /b %errorlevel%
