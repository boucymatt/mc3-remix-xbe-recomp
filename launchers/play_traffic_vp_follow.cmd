@echo off
rem The 100-car Cruise of play_traffic_vp_random.cmd with the game's own camera going from one pool car to the next for
rem as long as the race lasts: each car for 6 s, in build order, back to the first after the last. F9 jumps to the next
rem car. Go to Arcade > Cruise in San Diego as usual; the camera starts once the race does and gives itself back to
rem the player when it ends.
rem   --mc3-traffic-follow=6       seconds per car (src\game\debug_tools.cpp)
rem   --mc3-traffic-follow-from=0  first car shown (18 = the first model loaded during the race)
rem Optional seed to repeat a draw:  play_traffic_vp_follow.cmd 20261024
set "SEED="
if not "%~1"=="" set "SEED==%~1"
call "%~dp0_start.cmd" --mc3-render-profile=enhanced --mc3-guest-memory=128 --mc3-car-cache=64 --mc3-traffic-ambient-scale=3 --mc3-traffic-cruise-late-models --mc3-traffic-vp-bridge --mc3-traffic-cruise-pilot --mc3-traffic-cruise-count=100 --mc3-traffic-cruise-random-models=30 --mc3-traffic-density-scale=25 --mc3-traffic-follow=6 --mc3-traffic-follow-from=0 --mc3-traffic-cruise-random%SEED%
