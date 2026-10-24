@echo off
rem Enhanced profile, Arcade > Cruise in San Diego with traffic made entirely of complete vp_* cars: 100 cars from 30
rem models drawn at random among the validated cars of src\game\traffic_vp_pool.h (no bikes, no police), each car with
rem one of its .carcfg (plain or a numbered preset: paint, rims, kit) also at random. A new draw at every city load.
rem What it takes, all opt-in; docs\TRAFFIC_POOL.md explains each:
rem   --mc3-guest-memory=128                game heap +64 MB (a development kit's memory)
rem   --mc3-car-cache=64                    car resource cache 16 -> 64 and the per-car lists that overran past 48 cars
rem   --mc3-traffic-ambient-scale=3         San Diego's traffic pool 48 -> 144, which capped the live lanes at 30
rem   --mc3-traffic-cruise-late-models      models past the 18 spawn points load during the race, with their parts
rem Optional seed to repeat a draw:  play_traffic_vp_random.cmd 20261024
rem The seed of each draw is in the log: "MC3TRAFFIC cruise pool random seed=...".
set "SEED="
if not "%~1"=="" set "SEED==%~1"
call "%~dp0_start.cmd" --mc3-render-profile=enhanced --mc3-guest-memory=128 --mc3-car-cache=64 --mc3-traffic-ambient-scale=3 --mc3-traffic-cruise-late-models --mc3-traffic-vp-bridge --mc3-traffic-cruise-pilot --mc3-traffic-cruise-count=100 --mc3-traffic-cruise-random-models=30 --mc3-traffic-density-scale=25 --mc3-traffic-cruise-random%SEED%
