@echo off
rem Renderer profile legacy: the console path and the regression reference. See docs\RENDER_PROFILES.md.
rem Extra arguments pass through, e.g. --mc3-render-feature=post
call "%~dp0_start.cmd" --mc3-render-profile=legacy %*
