@echo off
setlocal EnableExtensions
set "GAME_TITLE=Midnight Club 3 DUB Edition Remix"
set "GAME_XBE=default.xbe"
set "GAME_EXE=MC3RemixXBE"
title %GAME_TITLE% - Build
cd /d "%~dp0"
echo.
echo   %GAME_TITLE% - build
echo   ================================================
echo.
rem   build.bat            check the prerequisites, then build
rem   build.bat check      check the prerequisites only, build nothing
rem   CI=1                 no prompts: installs nothing, does not offer to play
rem   XBE_BUILD_JOBS=N     parallel compile jobs, the CPU count by default; lower it if the compiler runs out of memory

set "PF86=%ProgramFiles(x86)%"
set "VSINST=%PF86%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINST%\vswhere.exe"
set "OUT=%CD%\out\build\release"
if not defined XBE_BUILD_JOBS set "XBE_BUILD_JOBS=%NUMBER_OF_PROCESSORS%"
if not defined XBE_BUILD_JOBS set "XBE_BUILD_JOBS=1"

rem ---------------------------------------------------------------------------------------------------------------
echo [1/5] Game files
if not exist assets\%GAME_XBE% goto :nogame
set "WANT_SHA1="
set "HAVE_SHA1="
if exist config\supported_xbe.txt for /f "usebackq tokens=1,* delims==" %%a in ("config\supported_xbe.txt") do if /i "%%a"=="sha1" set "WANT_SHA1=%%b"
for /f "usebackq delims=" %%h in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA1 -LiteralPath 'assets\%GAME_XBE%').Hash.ToLower()"`) do set "HAVE_SHA1=%%h"
if not defined HAVE_SHA1 goto :nohash
if /i not "%HAVE_SHA1%"=="%WANT_SHA1%" goto :badxbe
echo       assets\%GAME_XBE% is the supported build, SHA-1 %HAVE_SHA1%

rem ---------------------------------------------------------------------------------------------------------------
rem Python only writes two things from the user's own game files, never into the repository's tracked files:
rem   src\game\headlight_anchors.h          headlight positions read from the car models, git-ignored
rem   assets\mc3_motion_blur_override.bin   optional run time sidecar for the motion blur tune, inside assets\
echo [2/5] Python and data read from your game files
set "PY="
python -c "import sys" >nul 2>nul && set "PY=python"
if not defined PY py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if not defined PY goto :nopython
%PY% -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" || goto :oldpython
for /f "tokens=*" %%v in ('%PY% --version') do echo       %%v
if exist src\game\headlight_anchors.h goto :anchorsdone
if not exist assets\assets_x.dat_2 goto :noarchive
echo       generating src\game\headlight_anchors.h from assets\assets_x.dat_2
%PY% tools\generate_headlight_anchors.py assets\assets_x.dat_2 || goto :fail
:anchorsdone
echo       src\game\headlight_anchors.h is there

rem ---------------------------------------------------------------------------------------------------------------
echo [3/5] Toolchain
rem vswhere is called through PowerShell on purpose: Program Files (x86) has parentheses, and cmd cannot run a quoted
rem path like that from inside a for /f (it drops the outer quotes, or the ")" ends the for early).
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "& '%VSWHERE%' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"
rem Without vswhere, look for Visual Studio 2022 where it installs.
if not defined VSDIR for %%e in (Community Professional Enterprise BuildTools Preview) do if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\%%e"
if not defined VSDIR (
    echo       Visual Studio with the C++ workload was not found.
    echo       Install Visual Studio Community and tick "Desktop development with C++".
    goto :fail
)
call :setuptools
rem CI is set on GitHub Actions and by anything that wants no prompts.
if not defined TOOLS_OK if defined CI goto :byhand
if not defined TOOLS_OK (
    choice /c YN /n /m "      Let Visual Studio install the missing components? [Y/N] "
    if errorlevel 2 goto :byhand
    echo       Close Visual Studio if it is open, it will be shut down anyway.
    echo       Installing. Accept the admin prompt, then wait for the installer.
    powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%VSINST%\setup.exe' -ArgumentList 'modify','--installPath','%VSDIR%','--add','Microsoft.VisualStudio.Component.VC.Llvm.Clang','--add','Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset','--add','Microsoft.VisualStudio.Component.VC.CMake.Project','--passive','--norestart','--force'"
    echo       Press a key once the Visual Studio Installer window has closed.
    pause >nul
    call :setuptools
    if not defined TOOLS_OK goto :byhand
)
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v
for /f "tokens=*" %%v in ('cmake --version ^| findstr /i version') do echo       %%v
if /i "%~1"=="check" goto :checked

rem ---------------------------------------------------------------------------------------------------------------
rem config\game.toml is in the repository with its [symbols] already written, so this is normally skipped
echo [4/5] XDK symbols
cmake --preset release >nul || goto :fail
findstr /b /l /c:"[symbols]" config\game.toml >nul 2>nul && echo       config\game.toml already has the symbols && goto :game
cmake --build out\build\release --target xbe_symbols >nul || goto :fail
if not exist out\build\release\sdk_dir.txt echo       out\build\release\sdk_dir.txt was not written, update the SDK. && goto :fail
set /p SDK=<out\build\release\sdk_dir.txt
if not exist config mkdir config
"%OUT%\xbe_symbols.exe" assets\%GAME_XBE% "%SDK%\xdk" --out config\game.toml || goto :fail

rem ---------------------------------------------------------------------------------------------------------------
:game
echo [5/5] Game code
echo       the first build recompiles %GAME_XBE% into C++ on this machine and compiles it, a few minutes on a fast CPU and up to an hour or more on a slow one
echo       the recompiled code is made from your game file: it stays in generated\ and is never committed
echo       parallel compile jobs: %XBE_BUILD_JOBS%
cmake --build out\build\release --parallel %XBE_BUILD_JOBS% || goto :fail

rem optional, and a failure here does not fail the build
if exist assets\mc3_motion_blur_override.bin goto :blurdone
if not exist assets\assets_x.dat goto :blurdone
echo       writing assets\mc3_motion_blur_override.bin, the motion blur tune sidecar
%PY% tools\generate_blur_override.py assets\assets_x.dat || echo       warning: no motion blur sidecar was written, the game runs without it
:blurdone

echo.
echo   %GAME_TITLE% built.
echo   out\build\release\%GAME_EXE%.exe
echo.
echo   The executable holds your game's code translated to C++. Keep it to yourself: do not share it or commit it.
echo.
if defined CI exit /b 0
choice /c YN /n /m "  Play now? [Y/N] "
if not errorlevel 2 (start "" /d "%OUT%" "%OUT%\%GAME_EXE%.exe") else (pause)
exit /b 0

rem ---------------------------------------------------------------------------------------------------------------
:checked
echo.
echo   All prerequisites are in place. Run build.bat without "check" to build.
echo.
exit /b 0

rem helpers
:setuptools
set TOOLS_OK=
set MISSING=
rem VsDevCmd.bat looks for vswhere.exe on PATH and complains when it is not there
if exist "%VSINST%\vswhere.exe" set "PATH=%VSINST%;%PATH%"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang++ >nul 2>nul || set MISSING=%MISSING% clang++
where cmake   >nul 2>nul || set MISSING=%MISSING% cmake
where ninja   >nul 2>nul || set MISSING=%MISSING% ninja
where git     >nul 2>nul || set MISSING=%MISSING% git
if defined MISSING (
    echo       missing:%MISSING%
    exit /b 0
)
set TOOLS_OK=1
exit /b 0

:byhand
echo.
echo       Install by hand, then run this again:
echo         clang++ / cmake / ninja  -  Visual Studio Installer, Modify, Individual
echo                                     components: "C++ Clang tools for Windows"
echo                                     and "C++ CMake tools for Windows"
echo         git                      -  https://git-scm.com/download/win
echo       The full list is in DEPENDENCIES.md.
goto :fail

:nogame
echo   Your game files are not in place. Put the extracted game folder in:
echo     %CD%\assets
echo   It needs %GAME_XBE% with the game's other files and folders beside it. The repository has none of them.
goto :fail

:nohash
echo   Could not compute the SHA-1 of assets\%GAME_XBE%. PowerShell is needed for that.
goto :fail

:badxbe
echo   assets\%GAME_XBE% is not the build this port was made for.
echo     found:     %HAVE_SHA1%
echo     supported: %WANT_SHA1%
echo   See config\supported_xbe.txt. The title is Midnight Club 3: DUB Edition for the original Xbox.
goto :fail

:nopython
echo   Python 3.9 or newer was not found. It is used to read two small things out of your game files.
echo   Install it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
goto :fail

:oldpython
echo   Python is too old, 3.9 or newer is needed.
goto :fail

:noarchive
echo   assets\assets_x.dat_2 is missing, and src\game\headlight_anchors.h cannot be made without it.
echo   Put the complete extracted game folder in assets.
goto :fail

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
if not defined CI pause
exit /b 1
