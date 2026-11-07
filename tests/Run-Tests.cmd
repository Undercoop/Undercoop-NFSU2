@echo off
rem Copyright (C) 2026 batuhan. GNU GPL version 3 only.
rem Additional permission in LICENSE-7ZIP-EXCEPTION.txt.
setlocal
cd /d "%~dp0.."
if not defined VSCMD_ARG_TGT_ARCH (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%I\VC\Auxiliary\Build\vcvars32.bat" >nul
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 1
if not exist UndercoopTransport.lib (
    echo Build the source first to create UndercoopTransport.lib.
    exit /b 1
)
if not exist output mkdir output
for %%T in (CoopPeerPartVisibilityTests CoopConnectionHudTests CoopConnectionTransportTests) do (
    cl /nologo /O2 /std:c++17 /utf-8 /EHsc tests\%%T.cpp /Fo:output\%%T.obj /Fe:output\%%T.exe UndercoopTransport.lib ws2_32.lib winmm.lib
    if errorlevel 1 exit /b 1
    output\%%T.exe
    if errorlevel 1 exit /b 1
)
rc /nologo /i src /fo output\ConnectionHud.res src\NFSU2Coop.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /std:c++17 /utf-8 /EHsc tests\CoopConnectionHudRenderTests.cpp output\ConnectionHud.res /Fo:output\CoopConnectionHudRenderTests.obj /Fe:output\CoopConnectionHudRenderTests.exe user32.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b 1
output\CoopConnectionHudRenderTests.exe
if errorlevel 1 exit /b 1
exit /b 0
