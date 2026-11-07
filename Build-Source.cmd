@echo off
rem Copyright (C) 2026 batuhan.
rem GNU GPL version 3 only, with the permission in LICENSE-7ZIP-EXCEPTION.txt.
setlocal
cd /d "%~dp0"
if not defined VSCMD_ARG_TGT_ARCH (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%I\VC\Auxiliary\Build\vcvars32.bat"
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
    echo Use an x86 developer prompt.
    exit /b 1
)
if not exist output mkdir output
for %%S in (callbacks compress host list packet peer protocol win32) do (
    cl /nologo /O2 /c /TC /I"src\third_party\enet\include" "src\third_party\enet\%%S.c" /Fo"output\%%S.obj"
    if errorlevel 1 exit /b 1
)
cl /nologo /O2 /c /std:c++17 /EHsc /I"src\third_party\enet\include" src\CoopTransportWin.cpp /Fo"output\CoopTransportWin.obj"
if errorlevel 1 exit /b 1
lib /nologo /OUT:UndercoopTransport.lib output\CoopTransportWin.obj output\callbacks.obj output\compress.obj output\host.obj output\list.obj output\packet.obj output\peer.obj output\protocol.obj output\win32.obj
if errorlevel 1 exit /b 1
rc /nologo /d UNDERCOOP_MOD_MANAGER /i src /fo output\NFSU2Coop.res src\NFSU2Coop.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /arch:IA32 /LD /std:c++17 /utf-8 /EHsc /DUNDERCOOP_MOD_MANAGER src\NFSU2Coop.cpp output\NFSU2Coop.res /Fo:output\NFSU2Coop.obj /Fe:output\NFSU2Coop.asi ws2_32.lib user32.lib shell32.lib /link /INCREMENTAL:NO
exit /b %errorlevel%
