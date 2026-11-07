@echo off
rem Copyright (C) 2026 batuhan.
rem GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 exit /b 1
set "TRANSPORT_OBJECTS=%TEMP%\UndercoopTransport613\release"
if not exist "%TRANSPORT_OBJECTS%" mkdir "%TRANSPORT_OBJECTS%"
for %%S in (callbacks compress host list packet peer protocol win32) do (
    cl.exe /nologo /O2 /W3 /c /TC /I"src\third_party\enet\include" "src\third_party\enet\%%S.c" /Fo"%TRANSPORT_OBJECTS%\%%S.obj"
    if errorlevel 1 exit /b 1
)
cl.exe /nologo /O2 /W4 /c /std:c++17 /EHsc /I"src\third_party\enet\include" src\CoopTransportWin.cpp /Fo"%TRANSPORT_OBJECTS%\CoopTransportWin.obj"
if errorlevel 1 exit /b 1
lib.exe /nologo /OUT:UndercoopTransport.lib "%TRANSPORT_OBJECTS%\CoopTransportWin.obj" "%TRANSPORT_OBJECTS%\callbacks.obj" "%TRANSPORT_OBJECTS%\compress.obj" "%TRANSPORT_OBJECTS%\host.obj" "%TRANSPORT_OBJECTS%\list.obj" "%TRANSPORT_OBJECTS%\packet.obj" "%TRANSPORT_OBJECTS%\peer.obj" "%TRANSPORT_OBJECTS%\protocol.obj" "%TRANSPORT_OBJECTS%\win32.obj"
exit /b %errorlevel%
