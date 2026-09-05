@echo off
setlocal EnableExtensions
::=====================================================================
:: SymboGram - Symbian^1 / Qt 4.7.3 build
::
:: Upstream kutegram/quick ships no build script at all; the archived
:: kutegram/client had buildSymbian.bat, which this is modelled on.
::
:: Must run from cmd.exe, NOT Git Bash or PowerShell: the Symbian perl
:: scripts shell out to find/sort/make, and Git-for-Windows' POSIX
:: versions shadow the SDK ones and fail in ways that look unrelated.
::
:: Usage:  tools\build-symbian.cmd [clean]
::=====================================================================

:: abld invokes the ABLD.BAT it generates in the project root as a bare command.
:: If NoDefaultCurrentDirectoryInExePath is set (it is, on some Windows setups
:: and in some CI images), cmd refuses to resolve executables from the current
:: directory and the build dies with "'ABLD.BAT' is not recognized" immediately
:: after bldmake reported success. Clear it for this process only.
set "NoDefaultCurrentDirectoryInExePath="

:: --- locate project and its parent -----------------------------------
set "SCRIPTDIR=%~dp0"
for %%i in ("%SCRIPTDIR%..") do set "PROJ=%%~fi"
for %%i in ("%PROJ%")        do set "PROJNAME=%%~nxi"
for %%i in ("%PROJ%\..")     do set "PARENT=%%~fi"

set "PROFILE=kutegramquick.pro"
if exist "%PROJ%\symbogram.pro" set "PROFILE=symbogram.pro"

:: core.hooksPath is local config and is NOT carried by git clone, so a fresh
:: clone has no pre-commit or pre-push audit. Arm it here too. Idempotent.
git -C "%PROJ%" config core.hooksPath .githooks >nul 2>&1

:: --- short drive ------------------------------------------------------
:: abld builds under epoc32\build\<mangled-full-source-path>\ and blows past
:: MAX_PATH; the 2011 binaries are not long-path aware. qtenvS1.bat also
:: derives EPOCROOT by stripping the drive letter (%SDKPREFIX:~2%), so the
:: SDK and the project must live on the SAME logical drive.
:: Test for the mapping we actually want rather than probing `subst` itself --
:: its query form does not report "already mapped" through errorlevel, so
:: checking that instead makes the script fail on every run after the first.
set "SUBSTDRV=S:"
set "WORK=%SUBSTDRV%\%PROJNAME%"
set "SDK=%WORK%\Symbian1Qt473"
if not exist "%WORK%\%PROFILE%" subst %SUBSTDRV% "%PARENT%" >nul 2>&1
if not exist "%WORK%\%PROFILE%" (
    echo FAILED: %WORK%\%PROFILE% not found.
    echo   %SUBSTDRV% may be mapped elsewhere; free it with: subst %SUBSTDRV% /D
    exit /b 1
)

:: --- Telegram credentials -> libkg\apisecrets.h -----------------------
:: libkg.pri lists apisecrets.h in HEADERS, so the build cannot link without it.
pwsh -NoProfile -ExecutionPolicy Bypass -File "%PROJ%\tools\write-apisecrets.ps1" || (
    echo FAILED: could not generate libkg\apisecrets.h & exit /b 1
)

:: --- SDK --------------------------------------------------------------
if not exist "%SDK%\bin\qtenvS1.bat" (
    echo Fetching Symbian toolchain...
    git clone --depth 1 --branch Symbian1Qt473 -c core.autocrlf=false ^
        https://github.com/smbdsbrain/kutegram-compilers-mirror.git "%SDK%" || exit /b 1
)

:: patch.qmake.paths.bat renames .qmake.cache -> .PREV, but the repo already
:: SHIPS .PREV and .bak files, so the rename fails on a fresh clone. Harmless
:: (the following copy overwrites anyway) but it makes the script non-idempotent
:: and the log noisy. Clear them first.
del /q "%SDK%\.qmake.cache.PREV" "%SDK%\.qmake.cache.bak" >nul 2>&1
del /q "%SDK%\bin\qt.conf.PREV" "%SDK%\bin\qt.conf.bak" >nul 2>&1
del /q "%SDK%\mkspecs\default\qmake.conf.PREV" "%SDK%\mkspecs\default\qmake.conf.bak" >nul 2>&1

%SUBSTDRV%
cd "%SDK%"
call "%SDK%\patch.qmake.paths.bat" >nul 2>&1
cd "%SDK%"
call bin\qtenvS1.bat >nul 2>&1
cd "%WORK%"

:: --- clean ------------------------------------------------------------
if /i "%~1"=="clean" (
    if exist ABLD.BAT call ABLD.BAT reallyclean >nul 2>&1
    del /q Makefile bld.inf ABLD.BAT *.mmp *.pkg *.loc *.rss >nul 2>&1
)

:: --- generate -----------------------------------------------------------
echo [1/4] qmake
qmake.exe "%PROFILE%" -r -spec symbian-abld "CONFIG+=release" ^
    -after "OBJECTS_DIR=obj" "MOC_DIR=moc" "UI_DIR=ui" "RCC_DIR=rcc" || exit /b 1

:: bldmake must be run explicitly. The generated Makefile has a
::     $(ABLD): bld.inf
::             bldmake bldfiles
:: rule, but make runs every recipe line in its own shell and bldmake is
:: itself a .bat, so ABLD.BAT is not visible to the next line and
:: `make release-gcce` dies with "'ABLD.BAT' is not recognized". Running it
:: here sidesteps that entirely.
:: NOTE the `call`. bldmake resolves to bldmake.BAT, and invoking a batch file
:: from a batch file without `call` transfers control and never returns -- the
:: script would end here silently, with a success exit code and no SIS.
echo [2/4] bldmake bldfiles
call bldmake bldfiles || exit /b 1

:: --- compile ------------------------------------------------------------
:: abld is a Perl wrapper that shells out to make and does NOT propagate make's
:: exit code: the link can fail with "Error 1" and abld still returns 0, so the
:: `||` below never fires on its own. Left unchecked the build carries on with
:: whatever .exe is already on disk and packages that instead.
::
:: So do not ask abld whether it succeeded. Delete the target first and ask the
:: filesystem afterwards.
set "TARGETEXE=%SDK%\epoc32\release\gcce\urel\SymboGram.exe"
if exist "%TARGETEXE%" del /q "%TARGETEXE%"
echo [3/4] abld build gcce urel
call ABLD.BAT build gcce urel || exit /b 1
if not exist "%TARGETEXE%" (
    echo.
    echo FAILED: abld reported success but produced no SymboGram.exe.
    echo Scroll up for "Error 1" or an arm-none-symbianelf-ld message; abld
    echo swallows both. A section overlap here means the image outgrew the
    echo 4 MB code region -- see docs\building.md.
    exit /b 1
)

:: Scan the UNCOMPRESSED linker output, not the packaged artifacts.
::
:: Two layers of compression sit between here and the installer, and a substring
:: search sees through neither. A SIS deflate-compresses its payload, so
:: searching dist\*.sis finds nothing on a binary that carries the string. Less
:: obviously, abld runs the linker output through elftran and the E32 image in
:: epoc32\release is byte-pair compressed too: compression UID 0x102822AA at
:: offset 0x1C of the E32ImageHeader, 1.9 MB from a 4.0 MB ELF.
::
:: epoc32\BUILD\...\urel\SymboGram.exe is the raw ELF, before elftran. Same
:: bytes, uncompressed, and the only form a substring search can read.
::
:: scan-artifact.ps1 expects the api_hash to be present and warns when it is
:: not. That warning means the scan is aimed at the wrong file, or that
:: apisecrets.h never reached the link. Do not silence it.
set "LINKEDELF=%SDK%\epoc32\BUILD\%PROJNAME%\SYMBOGRAM_EXE\GCCE\urel\SymboGram.exe"
if not exist "%LINKEDELF%" (
    echo FAILED: no uncompressed ELF at %LINKEDELF%
    echo The leak scan cannot run against the compressed E32 image; refusing
    echo to package a binary that has not been scanned.
    exit /b 1
)
pwsh -NoProfile -ExecutionPolicy Bypass -File "%PROJ%\tools\scan-artifact.ps1" ^
     -Path "%LINKEDELF%" || exit /b 1

:: --- package ------------------------------------------------------------
:: Always pass an explicit certificate. The SDK's bundled
:: src\s60installs\selfsigned.cer dates from ~2011 and has long expired;
:: Symbian validates against the DEVICE clock, so letting createpackage fall
:: back to it produces an "expired certificate" install failure on the phone
:: that looks like a signing bug but is not.
echo [4/4] make sis
set "CERT=%PROJ%\secrets\symbogram.cer"
set "KEY=%PROJ%\secrets\symbogram.key"
if not exist "%CERT%" (
    echo FAILED: %CERT% missing. Generate with:
    echo   openssl req -x509 -newkey rsa:2048 -sha256 -days 7300 -nodes ^
-keyout secrets/symbogram.key -out secrets/symbogram.cer -subj "/CN=SymboGram/O=SymboGram/C=UA"
    echo   openssl rsa -in secrets/symbogram.key -out secrets/symbogram.key -traditional
    exit /b 1
)
make.exe sis QT_SIS_CERTIFICATE="%CERT%" QT_SIS_KEY="%KEY%" || exit /b 1

:: --- collect ------------------------------------------------------------
if not exist "%PROJ%\dist" mkdir "%PROJ%\dist"
for /f %%v in ('git -C "%PROJ%" rev-parse --short HEAD') do set "SHA=%%v"
for %%f in ("%WORK%\*.sis") do (
    copy /y "%%f" "%PROJ%\dist\%%~nf-symbian1-%SHA%.sis" >nul
    echo   dist\%%~nf-symbian1-%SHA%.sis
)
echo.
echo This SIS is a LOCAL build. SymboGram publishes no prebuilt binaries --
echo see docs\security.md. dist\ is gitignored and tools\audit-public.ps1
echo rejects any binary that reaches the publication set.
echo Done.
endlocal
