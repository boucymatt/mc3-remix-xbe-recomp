@echo off
rem Renderer profile enhanced: the approved package of extra rendering. See docs\RENDER_PROFILES.md.
rem Extra arguments pass through, e.g. --mc3-render-disable=light-shadow
call "%~dp0_start.cmd" --mc3-render-profile=enhanced %*
