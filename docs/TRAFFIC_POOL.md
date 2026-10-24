# Cruise traffic pool

An opt-in experiment that replaces San Diego's ambient traffic in **Arcade > Cruise** with complete `vp_*` vehicles:
real cars with their own model, paint, kit, wheels and `.carcfg`, driven along the lanes the game's own traffic uses.
It is off unless asked for, and the game's original traffic is what runs by default in every render profile.

Everything here is from the Xbox build of Midnight Club 3: DUB Edition (see `config/supported_xbe.txt`). Addresses are
guest virtual addresses in that build. The engineering log with the dead ends is `docs/history/traffic-bridge-log.md`.

## Turning it on

The bridge needs the `enhanced` render profile **and** `--mc3-traffic-vp-bridge`. `launchers\play_traffic_vp_random.cmd`
is the full 100-car setup; the minimum is:

```text
--mc3-render-profile=enhanced --mc3-traffic-vp-bridge --mc3-traffic-cruise-pilot --mc3-traffic-cruise-count=18
```

| Flag | What it does |
| --- | --- |
| `--mc3-traffic-vp-bridge` | Enables the bridge at all (also needs the `enhanced` profile). |
| `--mc3-traffic-cruise-pilot` | Builds the Cruise pool: one complete car per ambient lane. |
| `--mc3-traffic-cruise-count=N` | How many cars, up to 128. San Diego's own lanes give 18 until the pool is enlarged (below). |
| `--mc3-traffic-cruise-model=<vp_name>` | The pilot's original single model setting: only a few built-in models are accepted, and `vp_g500_03` is the default. Prefer `-models`. |
| `--mc3-traffic-cruise-mixed` | A short built-in list of cars, used in turn. |
| `--mc3-traffic-cruise-models=<a,b,c>` | A comma separated list, used in turn. Any car of `src/game/traffic_vp_pool.h`; a name that is not one is logged and skipped. |
| `--mc3-traffic-cruise-random[=seed]` | Draws distinct ordinary cars at random from `src/game/traffic_vp_pool.h` (no bikes, no police), as many as there are cars. The seed of each draw is in the log as `MC3TRAFFIC cruise pool random seed=...`. Each car also loads one of its `.carcfg` at random: the plain `vp_x` or a numbered preset `vp_x_N`. |
| `--mc3-traffic-cruise-random-models=K` | With `random`: draw only K distinct models and repeat them to fill the count. |
| `--mc3-traffic-density-scale=X` | Multiplies the traffic density (hook on `0x2B9670`). San Diego's Cruise runs at 0.04. |
| `--mc3-traffic-cruise-late-models` | Lets models past the spawn points load during the race (see below). |
| `--mc3-traffic-ambient-scale=X` | Multiplies the traffic pool (up to 16). |
| `--mc3-car-cache=N` | Enlarges the car resource cache and the per-car lists. N is rounded up to a multiple of 16 and limited to 256; 64 is what the 100-car setup uses. |
| `--mc3-guest-memory=128` | 128 MB of guest memory instead of 64 (a development kit's): the game's heap grows by 64 MB. |
| `--mc3-traffic-follow[=seconds]` | Debug camera that follows each pool car in turn. `--mc3-traffic-follow-from=N` picks the first (F9 = next). |
| `--mc3-write-watch=0xVA` | Diagnostic: a hardware write watchpoint on one guest address; logs each new writer by name and the guest stack. |
| `--mc3-trace-resources` | Diagnostic: logs every resource the game loads by name, geometry setups and part loads. |

## What stops a big pool, and what lifts it

None of these was a lack of memory: the game's heap still had about 44 MB free with 100 cars. Each was a fixed-size
table in the original code that nothing checks, and the symptom was a crash far from the cause. All are lifted by
`--mc3-car-cache` or the flag named, all are inert without it.

| Limit | Where | Symptom | Lifted by |
| --- | --- | --- | --- |
| Car resource cache: 16 entries | `[0x50A08C]`, built by `0x103B40`; `0x105B80`, `0x105CA0`, `0x103B80`, `0x1057B0`, `0x105720` | The 17th distinct model dereferenced null. | `--mc3-car-cache`: the entries live in N/16 windows of 16 and the game's own methods run on the right window (`src/game/car_resource_cache.cpp`). A null acquire is retried on the next window, because the player's acquire is nested around the cruise build. |
| Per-car lists | `[0x53890C]` (188 slots), `[0x536704]` (100), `[0x4D6308]` (256, inline) | Heap corruption at the 48th, 100th and 60th car; each list is appended to with no bound. | `--mc3-car-cache`: the first two are allocated N/16 times larger; the inline one gets extra windows. |
| Physics level | moving-object records, word `+0x10` of the level (`0x2AC730` sums the capacities) | From about the 92nd car a car found no record, stayed out of the physics at 0,0,0 and left a dead record that crashed Go to Menus. | `--mc3-car-cache`: the capacity is grown by N/16 before the level is built. |
| Traffic pool of 48 | `aiAmbientVehicleInstances` (`[+0x130]` objects of `0x21C` bytes, word `[+0x12C]` count) | Only 30 live lanes besides the 18 spawn cars, whatever the density. | `--mc3-traffic-ambient-scale`: the pool restored from the city's traffic resource is rebuilt larger before any car is active (`src/game/traffic_ambient_pool.cpp`). |
| Models past the 18 spawn points | the shader factory `[0x5077B8]` and the mesh loader `[0x507668]` | Stops on `Invalid type %d in rmcShaderFactoryStandard::ResourcePageIn`; a car that did load came without bumpers, kit and wheels. | `--mc3-traffic-cruise-late-models` (`src/game/car_late_load.cpp`), below. |
| Contact with a removed object | `0x2A5E10` | Null read when a contact names an object that left the physics level. | A guard that skips such a contact, with `--mc3-car-cache`. |

### Late models

While a city loads, the shader factory the car resources page in through is the cars' own and the mesh loader is the car
part loader (`[0x5319E4]`). During the race both are the world's. A car acquired then read its shader records as the
standard kind (type 42, the fatal error above), and its LODs asked the wrong loader for their kit and wheel `.mesh` files,
so none loaded. With `--mc3-traffic-cruise-late-models` the acquire swaps the shader factory to the cars' for its
duration, and the geometry setup (`0x109D40`) swaps the mesh loader to the part loader. Note that `0x109ED0` and `0x109F40`,
which look like a load bracket, are the per-frame bracket of the car draws: they set the draw phase, and bracketing a load
with them leaves the phase wrong.

## Measured

On the maintainer's machine, 128 MB, `--mc3-car-cache=64 --mc3-traffic-ambient-scale=3 --mc3-traffic-cruise-late-models`,
100 cars from 30 models, density x25: 90 seconds of Cruise, then Go to Menus, repeated with four different seeds, all
clean, every car on the road and every car removed at exit, at about 25 frames per second. Without the flags, Cruise is
unchanged.

Not covered: other cities, long soaks, impact and damage per model, audio per model, any render profile but `enhanced`
(the bridge needs it), and route quality at 100 cars (the strict route check flagged cars that stood still before the
physics fix and was not rerun after it).

## Code

| File | What it holds |
| --- | --- |
| `src/game/traffic_vehicle_bridge.cpp` | The bridge: builds, routes and removes the complete cars. The largest file; keep changes small. |
| `src/game/traffic_vp_pool.h` | The models the random draw may take, with their classes, preset counts and whether they passed the probe sweep. |
| `src/game/car_resource_cache.cpp` | The cache, the per-car lists, the physics level capacity and the two guards. |
| `src/game/traffic_ambient_pool.cpp` | The traffic pool rebuild. |
| `src/game/car_late_load.cpp` | The late model fixes and the resource trace. |
| `src/game/debug_tools.cpp` | The free camera, the AI spectator and the traffic follow camera. |
| `src/game/write_watch.cpp` | The write watchpoint. |

## Habits that found these

* A crash with a host return address in a recompiled function: resolve `__imp__sub_XXXXXXXX+off` with the executable's
  PDB and read the generated code at that line; then disassemble the guest address.
* Heap corruption: `--mc3-write-watch` on the damaged heap header names the function that writes it. Look for an array
  appended to with no bound next to it.
* A fixed table is usually `{pointer, capacity word, count word}` built by a constructor with a literal capacity: search
  the recompiled code for the `push <capacity>` before the constructor call.
