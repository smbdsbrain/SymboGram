@echo off
setlocal EnableExtensions
::=====================================================================
:: SymboGram - desktop build (Qt 4.8.7 / MinGW 4.8.2)
::
:: This is the AUTONOMOUS TEST TARGET, not a shipping build. It exists so
:: the app can be run and driven on the PC instead of requiring a SIS
:: install and manual tapping on the phone for every change.
::
:: Qt 4.x specifically, NOT Qt 5/6: all 23 QML files use
:: `import QtQuick 1.0`, and QtQuick 1 (QtDeclarative) was removed in
:: Qt 5.6. Qt 4.8.7 still ships it, so the QML runs unmodified and stays
:: identical to what the Symbian build uses.
::
:: What this target does NOT cover, and still needs the device:
::   - the Q_OS_SYMBIAN branch of src/platformutils.cpp
::   - Pigler notifications, SIS packaging, capabilities
::   - real memory behaviour under EPOCHEAPSIZE
:: Device behaviour is authoritative wherever the two disagree.
::
:: Setup this expects (see docs/building.md):
::   Qt     C:\Qt\4.8.7          qt-opensource-windows-x86-mingw482-4.8.7.exe
::   MinGW  C:\mingw482\mingw32  i686-4.8.2-release-posix-dwarf-rt_v3-rev3.7z
::
:: NOTE: if Qt 4.8.7 was installed silently (/S), its installer skips the
:: edition step and leaves src\corelib\global\qconfig.h containing
::     #define QT_EDITION QT_EDITION_
:: an unfinished placeholder. Every compile then dies inside Qt's own
:: headers with "QtValidLicenseForCoreModule does not name a type", which
:: looks like a toolchain mismatch but is not. Fix by completing it to
:: QT_EDITION_OPENSOURCE.
::=====================================================================

set "QTDIR=C:\Qt\4.8.7"
set "MINGW=C:\mingw482\mingw32"
for %%i in ("%~dp0..") do set "PROJ=%%~fi"
set "BUILD=%PROJ%\build-desktop"

if not exist "%QTDIR%\bin\qmake.exe" (echo FAILED: no qmake at %QTDIR%\bin & exit /b 1)
if not exist "%MINGW%\bin\g++.exe"   (echo FAILED: no g++ at %MINGW%\bin & exit /b 1)

set "PATH=%QTDIR%\bin;%MINGW%\bin;%PATH%"

:: credentials -> libkg\apisecrets.h (listed in libkg.pri HEADERS)
pwsh -NoProfile -ExecutionPolicy Bypass -File "%PROJ%\tools\write-apisecrets.ps1" || exit /b 1

if /i "%~1"=="clean" rmdir /s /q "%BUILD%" 2>nul
if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%"

echo [1/2] qmake
qmake.exe "%PROJ%\symbogram.pro" -r -spec win32-g++ "CONFIG+=release" || exit /b 1

echo [2/2] mingw32-make
mingw32-make -j8 || exit /b 1

echo.
echo Built: %BUILD%\release\SymboGram.exe
echo Run with %QTDIR%\bin and %MINGW%\bin on PATH.
endlocal
