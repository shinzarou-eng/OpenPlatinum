@echo off
REM build.bat — configure + compile OpenPlatinum (w64devkit + meson + ninja)
setlocal
set ROOT=%~dp0
set PATH=%ROOT%tools\w64devkit\bin;%LOCALAPPDATA%\Programs\Python\Python312;%LOCALAPPDATA%\Programs\Python\Python312\Scripts;%PATH%

if not exist builddir (
    meson setup builddir || exit /b 1
)
ninja -C builddir || exit /b 1

REM DLL SDL3 + donnees a cote de l'exe
copy /y "%ROOT%tools\SDL3-3.4.18\x86_64-w64-mingw32\bin\SDL3.dll" builddir\ >nul
if not exist builddir\data mkdir builddir\data
copy /y "%ROOT%data\headers.bin" builddir\data\ >nul
echo OK — builddir\OpenPlatinum.exe
