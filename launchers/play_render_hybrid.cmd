@echo off
rem Renderer profile hybrid: the console path plus the features you name one by one. See docs\RENDER_PROFILES.md.
rem Extra arguments pass through, e.g. --mc3-render-feature=post,contact-shadow
call "%~dp0_start.cmd" --mc3-render-profile=hybrid %*
