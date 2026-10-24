@echo off
rem Starts the built game with the arguments it was given. Used by the other launchers in this folder.
rem The run folder is the exe's own, so settings.toml, logs and saves stay inside out\build\release.
set "RUNDIR=%~dp0..\out\build\release"
if not exist "%RUNDIR%\MC3RemixXBE.exe" goto :notbuilt
pushd "%RUNDIR%"
start "" "MC3RemixXBE.exe" %*
popd
exit /b 0

:notbuilt
echo The game is not built yet. Put your game files in assets\ and run build.bat in the repository root.
pause
exit /b 1
