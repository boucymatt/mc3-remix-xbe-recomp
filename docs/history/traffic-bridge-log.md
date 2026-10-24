> Engineering log of the Cruise traffic bridge, kept as an audit trail of what was tried and measured.
> The `runs/<stamp>/...` folders it cites were written by a test harness that is not part of this
> repository, and the captures in them are never committed. Task ids (MC3-00xx) are the maintainers' own.
> For the current flags and limits read `docs/TRAFFIC_POOL.md` first.

# MC3-0015: independent Cruise pool - next implementation boundary

This is research for the next stage. As requested on 2026-10-22, MC3-0015 is
paused and the shipped executable uses the original ambient traffic in every
render profile by default. The race-template bridge and the independent Cruise
pilot remain in source for later work, gated by explicit opt-in flags.

## Paused checkpoint (2026-10-22)

The final investigation isolated two separate native boundaries:

* Ordinary Cruise can Open both Jetta and Scion `_o` files through 0x20D6F0,
  and typed load 0x2105F0 returns a valid Scion resource. The isolated probe
  returned to gameplay (`202103`, `202418`); missing Dave archive content is
  not the explanation.
* An independent zero-count owner pool (six 0x43c slots, native 0x108 config,
  0x88 Start, explicit manager+8 lifetime) reached `aiOpponent::Init`. Direct
  G500, Jetta and Scion `_o` loads then reached shader PageIn and asserted in
  0xDFB40: first shader word `0x000000AA`, masked type 42 where the retail
  factory accepts 0..3. G500 run `20261021-205656` and instrumented run
  `20261021-210050` preserve this failure. The relocation was mathematically
  consistent: G500 entry 0x828EE0C0 + delta 0x00D43300 = 0x836313C0. The
  read-only Dave probe `inspect_xbck.py` confirmed `0xAA` in the source XBCK
  (Jetta `_o` at file offset +0x5CE4). Do not replace type 42 blindly.
* Native asynchronous preloader 0x105CA0 has 15 free cache slots and resolves
  `vp_jetta_03` to registry index 1, but its registration via 0x210620/
  0x20DC00 fails with `-1` for
  `resources\\vehicle\\vp_jetta_03\\vp_jetta_03_o.xbck` (run `205255`).
  The earlier `204941` trial accidentally selected the `_g` suffix because
  the garage's fifth argument was copied; argument six selects `_o`.
* Enhanced Arcade Race remains functional with the existing opponent-template
  bridge: `203914` and `205935` both built six full vehicles. Its `_o` loads
  occur during city setup, whereas the synthetic Cruise pilot attempted its
  new load after gameplay reached state 2. This timing difference is a lead,
  not yet a proven root cause.

Resume by comparing 0x105A74 PageIn inputs and the 0x20DC00/0x1A2000 file
index route during city setup versus state 2; test early resource preparation
at 0x1E4561 before changing shader parsing. The opt-in runner is
`probe_arcade_traffic.py --arcade-mode cruise --cruise-pilot --resource-probe`;
expect an assertion until the resource path is fixed. `--traffic-vp-bridge`
explicitly enables the existing race/Career bridge, while ordinary launches
keep native traffic. The source and failed run logs are retained for review.

The paused build was verified after the default switch: enhanced Arcade
Cruise `runs/20261022-042224/result.json` and Arcade Race
`runs/20261022-042416/result.json` both PASS, built zero extra owners and
returned to frontend state 7. An explicit bridge run
`runs/20261022-042604/result.json` PASS built six complete vehicles, made six
native removals and returned to frontend. The shipped EXE SHA-256 is
`5976F833EFA518892D97AC09850BE174F1D1AAAC55B38021A50FD892DBC60AC7`.

## Correction: Cruise can open and load an arbitrary `_o` resource

The earlier inference that Cruise lacks `_o` resource registration was wrong.
At 0x2126E8 the *same* static asset manager, vtable `0x46D768`, Open target
`0x20D6F0` and extension context `0x453F68` appear in Arcade Race and Cruise.
In ordinary Cruise, the one-shot opt-in probe `runs/20261021-202103` opened
both `vp_jetta_03_o` and `vp_d_scion_tc_05_o` through that native Open method;
both returned stream `0x4BF500`. The stronger `runs/20261021-202418` also
called native typed load 0x2105F0 for Scion `_o` *without an opponent* and got
resource `0x832B0400`. At 0x21270C its header was read successfully (0x80
bytes, version word 1, type 0x38D030, body length 0x5F770). The probe freed
the returned typed buffer and restored the player's original path; Cruise
continued into gameplay and `result.json` reports PASS.

Thus the synthetic aiOpponent Init assertion in runs `144332`/`144514` is
**context-specific to that pilot's construction/invocation**, not proof of a
missing archive entry, mount, resource registration, or `_o` preload. The next
experiment must compare the old pilot's call stack, Start/config fields,
manager and loader return/header at 0x2126C0 while `g_constructing=1`.

## Career free roam: native hookmen are a working template source

The user's Career screenshot showed enhanced traffic repeating the hookman cars,
while Arcade Race repeated its opponents. The dedicated Career run
`runs/20261021-155408/result.json` confirms the underlying initialization:
Continue Career entered San Diego free roam, native `aiOpponentManager::Init`
reported `race_count=3`, and `aiOpponent::Init` requested the `_o` resources for
`vp_montecarlo_78`, `vp_gs430_04`, and `vp_eclipse_04`. The bridge captured
those three live configs/starts and built six complete physical traffic cars
from them. All six followed the ambient route oracle during the 12-second run;
the result recorded six owners, 8 route samples each, and at most 18.47 m
tracking error. The player requested the unsuffixed `vp_jetta_03` resource.
The stricter `runs/20261021-155808/result.json` passed with the resource
probe, bridge and route requirements enabled: 15 seconds of gameplay, six
complete owners, six native physical removals, and a clean return to frontend
state 7. The Career pause menu has five rows, so the runner now selects its
actual `Go to Menus` row instead of the Arcade row.

This establishes a **template/ownership dependency**, not an inherent need
for race AI to drive the traffic cars. The enhanced cars use their own native
vehicle/physics objects and the ambient lane oracle; the existing opponents
currently provide valid configs/starts and owner storage. Career
hookmen happen to provide that context, so the visible selection follows the
hookman pool. Arcade Cruise has no opponent manager/init at all; its earlier
synthetic Init still returned null at 0x2105F0, although the isolated typed
load now succeeds. To select arbitrary `vp_*` models, create an independently
owned, lifecycle-safe config/start pool and isolate why the old nested Init
changed the resource load outcome. Adding a dummy race AI
would be a diagnostic shortcut, not the desired final design.

## Cruise probe and failed synthetic pool

`runs/20261021-142538/result.json`: the new `--arcade-mode cruise` harness
entered actual Cruise, simulated 12 seconds and returned through its native
pause/Go to Menus path with 8 seconds of settled frontend observation. There
were zero opponent init captures, zero complete traffic actors, and no
`aiOpponentManager::Init` reserve/ready hook. Cruise pause has Go to Menus at
visible row 5; race has it at row 6. The harness now handles both.

`runs/20261021-142744/result.json` is a deliberately preserved FAIL from a
temporary independent pool. It allocated six owner slots separately,
constructed three 0x108-byte mcCarConfig values with native constructor
0x271D20 and native type/profile setters 0x270AC0/0x270A90, and initialized
0x88-byte aiOpponentStart values with native Clear 0x1CA210. The pool became
ready and began the first native aiOpponent::Init. At 0x105B69 the game's own
resource load asserted because 0x2105F0 returned null; the call was reached
from 0x105B80/0x105AA0 after the car-name lookup 0x26A6A0. No full vehicle
was built. The assertion text at Xbox VA 0x455230 says that retail cannot
make a car resource dynamically; the attempted path format at 0x4552A0 is
`$/resources/vehicle/%s/%s`. Dave archive `assets_x.dat_2` **does** contain
`resources/vehicle/vp_d_scion_tc_05/vp_d_scion_tc_05.xbck`, so file absence
is not established. The actual formatted path and requested resource type
still need capture at 0x105B36/0x2126E5. The pool source and crashing EXE
were removed; the race-validated behavior was rebuilt and regression-tested
in `runs/20261021-143117/result.json`. Merely constructing config and Start
objects does **not** prepare the same car-resource context as Arcade race.

A read-only, opt-in hook at native 0x105B36 (`--resource-probe` in the Arcade
runner) now records the exact path immediately before 0x2105F0. In the race
run `143744`, native opponents requested `_o` variants; the player requested
the unsuffixed Jetta. Normal Cruise run `144145` requested only the unsuffixed
Jetta and returned to the frontend. Two later *opt-in* synthetic Cruise probes
then established the exact missing operation:

* `144332` attempted a Jetta opponent. Native Init requested
  `$/resources/vehicle/vp_jetta_03/vp_jetta_03_o`; 0x2105F0 returned null and
  the retail build asserted at 0x105B69. The player's unsuffixed Jetta had
  loaded successfully one second earlier.
* `144514` attempted a Scion opponent. It requested
  `$/resources/vehicle/vp_d_scion_tc_05/vp_d_scion_tc_05_o` and hit the same
  assertion.

Both `_o.xbck` files are present in `assets_x.dat_2`, and their header type is
0x38D030, the exact type passed to 0x2105F0 in the failing call. Thus the
problem is not explained by a missing archive entry, bad file header, or
using the player's unsuffixed resource path. The later isolated native typed
load proves Cruise can read Scion `_o` outside that failed Init. The pilot code was again
removed after these captures; only the read-only path probe remains installed.
The final executable passed enhanced race with six complete owners and menu
return in `runs/20261021-144659/result.json`, then ordinary Cruise and menu
return in `runs/20261021-144842/result.json`. Its SHA-256 is
`ECFD1C747EEF3F2F245B935D06D4F69D13D05E22419B7D5255D5980129D0BFCE`.

## Native routines found during the long soak

| Xbox VA | Identified behavior | Evidence |
|---|---|---|
| 0x271D20 | mcCarConfig constructor, vtable 0x4483DC | matches alpha constructor structure; defaults at +E3/+F9 |
| 0x26FE80 | mcCarConfig default/clear portion | writes blank strings, appearance/performance defaults |
| 0x270AC0 | vehicle type name setter | bounded 0x16-byte format to config+E3 |
| 0x270A90 | vehicle profile name setter | bounded 0x0D-byte copy to config+F9 |
| 0x270AE0 | vehicle type lookup from name | passes config+E3 to 0x26A6A0 |
| 0x1CA210 | aiOpponentStart::Clear | matches alpha default bytes, pose +38, config +64, ends +84 |

The config allocation size is exactly 0x108: native `sub_00150F3D` pushes
0x108 into `operator new` at 0x1B3B90 and immediately calls the constructor
at 0x271D20; it does this twice. `sub_00148CF0` also reserves a 0x108-byte
stack config. The profile setter terminates at +105. Do NOT infer the size
from the 0x450 spacing between config pointers in a race load: adjacent heap
allocations explain that spacing, not object size.

`runs/20261021-051524/checkpoint_templates/` contains five native starts
(0x88 each) and configs (0x108 each), plus their live pointers/model names.
These dumps are evidence to compare default fields, not pointer-bearing
blobs to deserialize into a different process.

## Remaining implementation

1. Verify default Start ownership and the manager's zero-count allocation path.
2. Compare the failed nested synthetic aiOpponent::Init with the successful
   isolated Cruise 0x2105F0 call: the 0x2126E8 Open result, 0x21270C header,
   type/output pointers, Start/config fields and live manager. Identify the
   *actual* difference before changing the loader. Then build a whitelist
   using native config construction/name/profile setters with explicit
   lifetime through city teardown.
3. Inspect aiOpponent::Init argument 3 and its Cruise brain branches against
   native race data before choosing synthetic defaults. Do not transplant
   process pointers or assume all racing defaults are valid in Cruise.
4. When normal opponent count is zero, reserve collision-resolvable owner
   storage at manager+8 without adding race standings. The ordinary manager
   global destructor skips that array when its count is zero, so explicit
   release/clearing is needed for any independently allocated backing storage.
5. Keep native vehAutoMgr capacity hook (128 in enhanced), unique IDs,
   pause gate and complete native destruction. Repeating only the old factory
   constructor experiment loses reset/activation and is not full physics.
6. Add a visible Arcade/Cruise probe. Require actual native initialization,
   motion, lane tracking, respawn/reuse, cleanup and no heap corruption.

Also still pending: replace address-based model hashing with a verified
logical ambient-slot identity plus explicit seed, nearest-traffic pool reuse,
impact/turn quality checks across more models, and repeated city loads.

## 2026-10-23: frontend traffic boundary and working Cruise route bind

The frontend has its own moving `aiAmbientVehicle` population and reaches the
same respawn/render hooks as the city.  This explained why the first successful
early Cruise construction selected stale object `0x8300AA38`: game state 7 is
shared by frontend and city loading, so it is not an epoch boundary.

The stable city boundary is the asynchronous lookup of
`D:\\flash\\loadscreen_arcade_*.xbck`.  The opt-in Cruise pilot now clears all
frontend ambient observations at that point.  The following city respawns are
retained and one complete opponent vehicle is constructed at the synchronous
player resource acquisition (`0x105B80`) while state 7 is still active.  This
matches the successful native race/police resource window and avoids the late
state-2 shader type-42 crash.

The respawn routine at `0x6D7D0` deliberately writes route state 1 after every
pool slot Init (`0x6D859`).  Therefore the slot used for early construction is
only a spawn pose; it is not guaranteed to become a live lane.  In state 2 the
bridge now finds the nearest active slot with state 0 and a valid `+0xFC` lane,
transfers the actor/oracle association, sets the new pose through native
`mcRacer` routine `0x2C8860`, and invokes native reset `0x2C8690`.  Only then is
the active ambient collider removed and its normal lane controller used.

Canonical strict result:
`runs/20261023-police-cruise-native-rebind-r16/result.json` is PASS after 20 s.
The complete `vp_impalass_cop_s_96` actor moved 136.96 m, its native ambient
oracle moved 122.11 m, and maximum tracking error was 11.23 m.  No shader
invalid type, KeBugCheck, or heap failure occurred.  Lifecycle run
`20261023-police-cruise-native-rebind-exit-r17` removed the physical car,
released one extra owner and returned to settled frontend state 7; its overall
runner result is FAIL only because a red light limited oracle path length to
6.04 m, below the optional 10 m route threshold.  Default Enhanced Cruise
`20261023-cruise-original-traffic-regression-r18` is PASS with `built: 0`,
confirming the original traffic remains the default without both experimental
flags.  The dedicated lifecycle rerun
`20261023-police-cruise-native-rebind-exit-r19/result.json` is PASS: one native
physical removal, one extra owner released, zero redundant ambient removals,
and a settled active frontend after eight seconds.

Next work: repeat city loads, validate respawn/reuse after the player moves far
enough to recycle lanes, and expand the independent whitelist beyond the one
police model before considering any default promotion.

## 2026-10-23: independent mixed pool, repeated loads and six-car movement

The independent Cruise pool now allocates a separate native `mcCarConfig`
(0x108), `aiOpponentStart` (0x88) and name buffer for every requested actor.
`--mc3-traffic-cruise-mixed` cycles the validated list
`vp_impalass_cop_s_96`, `vp_g500_03`, `vp_jetta_03` and
`vp_d_scion_tc_05`; `--mc3-traffic-cruise-count=N` selects one through six
complete actors.  Every construction consumes its own template rather than
reusing template zero.  The Scion profile appearing as `d_scion_tc_0` is the
expected native representation: setter 0x270A90 stores at most 0x0D bytes,
including the terminator.

Canonical mixed result `runs/20261023-cruise-four-mixed-r23/result.json` is
PASS.  Four different full actors followed four native lanes for 20 seconds;
actor path lengths were 130.06, 357.93, 309.68 and 131.26 m, with maximum
tracking errors 10.29-15.29 m.  Native city exit removed four physical cars,
released four owners and reported zero redundant ambient removals.

`runs/20261023-cruise-four-mixed-reload-r25/result.json` is PASS for two city
loads in one process.  Both loads built all four models, used distinct owner
backing arrays, then removed all eight physical cars and returned to a settled
frontend.  This closes the repeated-load and frontend-traffic contamination
items.  Run r24 is retained as a harness false negative: one oracle remained
at a red light during the 12-second window while three others moved and its
actor stayed within 4.08 m.  `--require-route` now accepts such a bounded
stopped pair only when at least one other actor/oracle pair moves.

Capacity/movement result `runs/20261023-cruise-six-mixed-drive-r26/result.json`
is PASS after 65 seconds with 60 seconds of player acceleration.  Six complete
actors covered 433.54-1299.55 m, all stayed within 15.27 m of their lane
oracles, and all six native removals completed before frontend observation.
No fatal, assertion, exception, heap failure or redundant removal appeared.

The run produced exactly six initial lane binds and no `retired owner` event.
Thus it validates movement and six-owner pressure but does not prove lane
retirement/reuse: the ambient director did not recycle those six particular
slots.  Keep original traffic as the default.  Remaining work is a controlled
retirement/respawn trigger, partial-Init rollback, impact/turn/audio quality,
logical-slot model seeding and a final ten-minute mixed soak.

The missing rearm was then found in the retirement hook: the Cruise pilot set
`g_manager_ready=false` after filling the pool, and Deactivate reduced the live
count without reopening construction.  The hook now rearms one construction
slot after destroying a pilot owner.  Opt-in flag
`--mc3-traffic-cruise-recycle-test` waits for a settled pool, calls the game's
own `aiAmbientVehicle::Deactivate` on one active lane, and requires the native
director/bridge pair to replace it.

`runs/20261023-cruise-native-recycle-r27/result.json` is PASS.  Owner
`B016FA74` was removed from ambient `8300AA38`, logged as retired, then reused
by a newly constructed complete G500 on live ambient `8300A600`.  At city exit
the four live owners were removed once.  The result records five native
removals total (one retirement plus four exit removals), zero redundant
ambient removals and a settled frontend.  Retirement and owner-slot reuse are
therefore implemented and reproducible through the native lifecycle.  Natural
director retirement still needs observation during a longer moving soak.

Final no-flag regression
`runs/20261023-cruise-original-traffic-final-r28/result.json` is PASS with
`built: 0`, no pilot log, native Cruise gameplay and a settled frontend after
Go to Menus.  The mixed/recycle implementation remains opt-in.  Installed EXE
SHA-256: `2C21E177C172102C9211E63519ED39380F3D76D2B689468F131E7704A5C760F4`.

## 2026-10-23: the whole vp_* car pool (helper session, MC3-0014)

`vp_inventory.py` (read-only) lists every `resources/vehicle/vp_*` in the Dave
archives with its variants, `_o` header, class/group from
`tune/vehicle/vehicle.lst`, `RiderIdx` from its `.carcfg`, the numbered
appearance presets and the opponent audio `vo_*.vehaudio`. All 94 have `base`,
`_o` (type 0x38D030, 0.16-0.48 MB) and `_g`, all in `assets_x.dat_2`. 15 are
bikes (SPORTBIKE, CHOPPER, COPBIKE; RiderIdx > 0), 5 police, 75 ordinary cars;
`vp_d_cls55_05` and `vp_mclaren_f1_95` have no `vo_` audio file.

`gen_vp_pool.py` writes `templates/game/src/game/traffic_vp_pool.h`: the 75
cars plus the 4 COPCARs (native type defaults, never drawn at random). The
Cruise pilot takes two new opt-in flags in front of `--mc3-traffic-cruise-mixed`:

* `--mc3-traffic-cruise-models=vp_a,vp_b,...` any car of the table, in turn;
* `--mc3-traffic-cruise-random[=seed]` `--mc3-traffic-cruise-count` distinct
  non-police cars, validated ones only; the seed is logged
  (`MC3TRAFFIC cruise pool random seed=...`).

`probe_arcade_traffic.py` passes them as `--cruise-models` / `--cruise-random`.
`vp_pool_sweep.py` ran all 79 through the strict r23 check in batches of six
(complete construction, lane tracking, Go to Menus cleanup): 79/79 PASS, kept in
`vp_pool_validation.json`, which `gen_vp_pool.py` turns into `validated`.
A seeded draw `runs/20261023-vp-pool-random-seed` PASS (six new models built,
routed and removed); default Cruise `runs/20261023-vp-pool-default-regression`
PASS with `built: 0`.

What the sweep does not cover: 20 s per batch at the start of Cruise only, no
reload, recycle or long soak per model; impact, damage and audio quality per
model; the two cars without `vo_` audio; cities other than San Diego.

## 2026-10-23: density and random .carcfg (helper session)

* **Capacity.** The Cruise pool has its own capacity, `kCruiseCapacity = 24`
  (`g_extra_capacity` per mode; the race bridge keeps `kTrafficCapacity = 6`),
  and `g_templates` holds 32. San Diego's Cruise `.rac` controls 18 ambient
  lanes (`numControlledAmbients 18`), so 18 complete cars replace all of its
  traffic; a car needs a lane, so more cars than lanes are not built.
* **Appearance.** Each pool car loads one of its `.carcfg` at random, the plain
  `vp_x` or a numbered preset `vp_x_N` (10, some 20), through the game's own
  `mcCarConfig::Load` 0x2733B0, as aiOpponent setup 0x1CADB0 does for race
  opponents (`<race>/<NN>_<type>`, else the type). Police keep their type's
  defaults. The pick uses its own stream of the draw's seed and is logged
  (`MC3TRAFFIC cruise pilot carcfg template=N 'vp_x_K' loaded`).
* **The limit is the heap.** 0x4C9BE0, a fixed ~41 MB region (0x80CFA000, size
  0x28E4A90). 18 distinct models overran ("Heap %s overrun", 0x1B4740 ->
  0x1A28F0 -> int3 at 0x325AE5) while loading the 15th. A repeated model shares
  its car type (`refs` > 1), so `--mc3-traffic-cruise-random-models=K` draws K
  models and repeats them: free heap measured 2.83 MB before the first car,
  about 0.57 MB per new model and 0.15-0.2 MB per repeat (one sub-heap of
  22.8 MB is counted as used, so this is a lower bound). 18 cars: K=2, 3
  (seeds 20261024, 777, 4242) PASS; K=4 PASS with 20261024 and 31337, overrun
  with 777; K=6 and K=8 overrun after all 18 were built (the rest of the city
  load). `play_traffic_vp_random.cmd` uses 18 cars x 3 models.
  Run `runs/20261023-vp-18x3-s4242`: 18 cars, 185-618 m each, max tracking
  error 17.9 m, 18 removals at Go to Menus.
* Going further needs more guest memory (the recomp maps 64 MB:
  `kPhysicalSize` in runtime/kernel/memory.h) or more ambient lanes.

## 2026-10-24: car resource cache 16 -> 64, per-car list 188 -> 752 (helper session)

* **Car resource cache** ([0x50A08C], ctor 0x103B40, 16 entries of 0x10: type,
  resource, variant, refs, pending, request). `--mc3-car-cache=64`
  (`src/game/car_resource_cache.cpp`) keeps the entries in a 64-entry table
  seen as 4 windows of 16 and runs the game's own acquire 0x105B80, preload
  0x105CA0, release 0x103B80 / 0x1057B0 and unload-all 0x105720 on the right
  window. Catch found on the way: the player's acquire looks its name up and
  the cruise build runs nested inside that call, so its window can be full
  when it resumes; a null acquire is retried on the next free window (logged
  `MC3CARCACHE acquire ... attempt=1`). 18 distinct traffic models + player +
  police now load (the 17th used to dereference null in `sub_002CBDE0`).
* **Per-car list** [0x53890C] ({array, capacity, count}, 0x21F120 with 188 from
  0x44398; 0x21F2C0 appends without a bound, ~4 per car). Found with
  `--mc3-write-watch=0x828AFFD0` (`src/game/write_watch.cpp`, DR0 data
  breakpoint, logs each new writer by PDB name): this was the "heap BAD" at the
  48th car. The same flag scales it by N/16.
* **Result** `runs/20261024-012307`, `-012520` (128 MB, cache 64, seed 20261024,
  18 models, density x5.5 / x25): 48 cars, no BAD, no crash, ~47-49 fps.
* **What still stops it:**
  - More distinct models than spawn points (18): a model first loaded during
    state 2 stops the game on "Invalid type %d in
    rmcShaderFactoryStandard::ResourcePageIn" (0x4537D8, KeBugCheck at
    0x2DB02B), same as before. Opt-in `--mc3-traffic-cruise-late-models` lets
    the live lanes try it (probe `--cruise-late-models`); not usable yet.
  - 48 = 18 spawns + 30 live lanes: the native traffic director opens 30 live
    ambients even at density 1.0; not traced yet.
* Probe options added: `--car-cache N`, `--write-watch VA`,
  `--cruise-late-models`. `play_traffic_vp_random.cmd`: 128 MB, cache 64,
  48 cars x 18 models, density x25.

## 2026-10-24: 100 cars from 30 models (helper session)

Run `runs/20261024-030905`: 128 MB, `--car-cache 64 --ambient-scale 3 --cruise-late-models`, seed 20261024,
100 cars x 30 models, density x25, 90 s soak, Go to Menus clean; ~24.5 fps. Default Cruise without the flags
`runs/20261024-031221` clean. What stood in the way, in order:

* **30 live lanes** = San Diego's ambient pool of 48 (`aiAmbientVehicleInstances`: [+0x130] array of 0x21C-byte
  cars, word [+0x12C] count; update 0x6CC90). In Cruise it is restored with the traffic resource, not built by
  0x6C840 from the city list (that path is scaled too, at 0x807A0). `src/game/traffic_ambient_pool.cpp`
  (`--mc3-traffic-ambient-scale=X`) rebuilds it before any car is active: originals moved, copies of each type's own
  idle cars (all 48 are identical fresh Inits, no pointers between them), the four per-type member lists
  ({ptr, count, capacity} in each type) and +0x130/+0x12C repointed. A 128 MB scan found no other pointer to a car.
* **60 cars**: the list at [0x4D6308] (0x408 bytes: flag, 256 inline item pointers, count at +0x404; add 0x33A00 /
  0x339A0 / 0x33980, remove 0x33910, draw 0x337D0, update 0x34340, delete 0x342F0) overran its count. Inline, so not
  growable: with `--mc3-car-cache` it gets windows of 256 (car_resource_cache.cpp).
* **100 cars**: [0x536704], same layout as [0x53890C] (init 0x21E0B0 with 100 from 0x44381, append 0x21E190 without
  bound) -> x4 with the cache flag.
* **Models past the 18 spawns**: car loads are bracketed by the car resource handler (vtable 0x4554EC) Begin 0x109ED0
  / End 0x109F40, which swap the shader factory [0x5077B8] to the cars' own (vtable 0x468B68). A live-lane acquire
  in state 2 had no bracket, so its shaders hit rmcShaderFactoryStandard::ResourcePageIn (0xDFB40) -> type 42.
  `src/game/car_late_load.cpp` brackets such an acquire (state 2, outside any bracket) with the game's own Begin/End.
* Intermittent, seen once each at 100 cars over 90 s: a contact whose object left the physics level (0x2A5E10, word
  +8 = 0xFFFF -> null) -> skipped with the cache flag; and once a null read in `sub_000ED8A0` (esi = phLevel) right
  after Go to Menus, not reproduced in three more runs, not fixed.
* Not checked: how the late-loaded models look up close (no capture near one), route quality at 100 cars (the strict
  check failed on 2 of 100 actors that stood still), other cities, longer soaks.

## 2026-10-24: the crash at Go to Menus was the physics level's 96 moving records

San Diego's phLevel has four capacities (ctor 0x2A8D50, summed into +0xE by Init 0x2AC730): +0x2A 1030, +0x10 96
moving-object records (0x27C bytes at +0x40), +0x44 4066 active nodes, +0x54 0. From about the 92nd car of a
100-car pool a car found no moving record: its instance stayed out of the level (id 0xFFFF, so the bridge never
removed it), the car sat at 0,0,0 with the others that failed (the actors the strict route check flagged), their
contacts named no object (0x2A5E10), and the level clear at Go to Menus (0xED8A0) found a record left with no
instance ([rec+8], or [rec+0x10] when [rec+8] is rec+0xC). With `--mc3-car-cache` Init now grows +0x10 by N/16
(96 -> 384). Seeds 20261027 (7 cars at the origin before), 20261031-33: 100 built, 0 at the origin, 100 removed at
Go to Menus, no guard fired; default Cruise clean (`runs/20261024-072449`, `-072807`, `-073110`, `-073415`,
`-073718`). The two guards (contact skip, unchaining a dead record before the clear) stay as a safety net.
Doubling +0x44 changed nothing (tried, reverted).

## 2026-10-24: camera on the Cruise pool's cars (`--mc3-traffic-follow`)

`--mc3-traffic-follow[=seconds]` (debug_tools.cpp; start car `--mc3-traffic-follow-from=N`, default 18 = the first
late-loaded model; F9 next) turns the game's own player cameras onto each pool car in build order, so the city,
culling and LOD follow it. The cameras (classes 0x476DE8, 0x476BEC — the active chase is one of its six —
0x476AFC, 0x46CC00) keep the target's Matrix34 pointer at +0x6C; found once a race by their pointer to the player's
Matrix34. Their collision test (CollideType +0x15C) ignores only the player's car and pulled the camera into the
followed one (the user's catch: it came closer near walls), so it is 0 while following; both are restored when
the race ends or the car goes. Dead ends: replacing only the D3D view (cars and city drawn from two cameras), and
retargeting the 0x4754D4 object too (it is the player's target for HUD and input: speedometer and view went wrong).
The bridge keeps {vehCar, name} per build (`Mc3TrafficBuiltCar`). Probe: `--traffic-follow S`,
`--traffic-follow-from N`, `--soak-captures N`, `--soak-capture-spacing S`, `--game-arg ARG`.
Run `runs/20261024-104223` (30 models): cars #19-#29 shown, all with paint, textures, plates and lights
(`late_models_sheet.png`; its labels are approximate, matched by time).

## 2026-10-24: late models without bumpers, kit and wheels

A car's kit and wheels are separate .mesh files its LODs load at geometry setup (0x109D40: part loader [0x5319E4],
vtable 0x467D58, given the car's folder and kit mask; LODs via 0xE6D00 -> 0xE9260 -> LOD::Load 0xE90D0, which asks the
current mesh loader [0x507668]->vtbl[1]; the part loader, 0x1BD8F0, loads a part when its mask bit is set). While a city
loads, [0x507668] is the part loader; during the race it is another (the world's), which loads no car part: a late
car loaded its body (`_o`) and nothing else. Traced with `--mc3-trace-resources` (car_late_load.cpp; every load by
name through the 0x2105F0 hook in mc2_city.cpp, geometry setups, LoadPart calls and mask bits).
* Fix (car_late_load.cpp, `--mc3-traffic-cruise-late-models`): during the race 0x109D40 runs with the part loader in
  [0x507668], given back after. A late EXT now loads 14 files, a late Esprit 7 (body only before);
  `runs/20261024-112823/late_fixed_sheet.png`.
* The shader fix changed with it: Begin/End of the car handler (0x109ED0/0x109F40) turned out to be the per-frame
  car draw bracket (from 0x92A68/0x92A78), not a load bracket; a city-load acquire runs at phase 3 with the cars'
  shader factory and nothing else changed. The late acquire now only swaps [0x5077B8] to the cars' factory.
* `play_traffic_vp_follow.cmd`: the 100 x 30 Cruise with `--mc3-traffic-follow=6 --mc3-traffic-follow-from=0`, the
  camera going round the pool for as long as the race lasts (wrap checked, `runs/20261024-113134`).
* Probe: `--follow-captures` (one capture per followed car, 2 s after the switch, named after it; the camera blend
  can still show the previous car).
