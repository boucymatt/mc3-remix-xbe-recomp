# Renderer profiles (MC3-0008)

The D3D12 backend has one code path and a profile fixed at boot. A profile is a
policy over that path, not a second renderer: D3D8/NV2A translation, Bink, Flash,
targets and present are shared, so a compatibility fix reaches all three.

| Profile | What it draws |
| --- | --- |
| `legacy` | The minimal console path and regression reference, with the approved lighter `motion-blur` baseline. |
| `enhanced` | The approved package, including the lighter `motion-blur` baseline. |
| `hybrid` | The console path plus features named one by one; `light-shadow` and `motion-blur` are its defaults. |

Changing the profile needs a restart; the log says so on every boot.

## Selecting

Command line wins over `settings.toml`, which wins over `legacy`.

```text
--mc3-render-profile=legacy|enhanced|hybrid
--mc3-render-feature=post,materials,contact-shadow,vehicle-shadow,sun-shadow   (also --mc3-render-features; repeatable)
--mc3-render-disable=<feature list>
--mc3-render-capture-ab=<tag>     F3 captures go to gpu_captures/<time>_<tag>_<profile>, and timing is on
--mc3-render-timing               CPU/GPU frame time avg/P95/P99 every 600 presents
--mc3-gpu-index=<n>               adapter in high-performance order; the boot log lists them
```

`settings.toml` keys: `render_profile = "hybrid"`, `render_features = "post"`,
`gpu_index = -1` (-1 is the first high-performance adapter, as before). Feature
names also accept underscores (`contact_shadow`).

`legacy` ignores requested additions outside its shared motion-blur baseline; `enhanced` ignores features outside its package
(use `hybrid` to test one). A feature asked for before its code lands is logged
as `pending` and stays off. `--mc3-render-disable` wins over everything.

## Features

| Feature | State | Notes |
| --- | --- | --- |
| `post` | in `enhanced`, and in `hybrid` on request | Gate 2 of MC3-0008, promoted 18/10. |
| `contact-shadow` | in `enhanced`, and in `hybrid` on request | Gate 4 of MC3-0008, promoted 18/10. |
| `vehicle-shadow` | in `enhanced`, and in `hybrid` on request | Gate 5 of MC3-0008, promoted 19/10. |
| `sun-shadow` | implemented, hybrid only | Gate 6 of MC3-0008, two cascades, under test (19/10). |
| `reflections` | in `enhanced`, and in `hybrid` on request | The title's car paint, glass and wet road reflections sharper and shaped (19/10). |
| `light-shadow` | approved, enhanced and on by default in hybrid (20/10) | MC3-0012: MCLA style headlight beams with shadows for the cars near the camera; they replace MC3's headlight cone, which only legacy keeps. Street lamp shadows optional, off by default. `--mc3-render-disable=light-shadow` turns it off. |
| `motion-blur` | default in all profiles (23/10) | MC3-0011: calibrated depth/reprojection streak pass inspired by LARecomp's documented MCLA blur path. A validated sidecar and a narrow draw signature suppress MC3's original 480p radial blur. `--mc3-render-disable=motion-blur` restores the old effect. |
| `materials` | pending | Accepted by the parser, logged as pending, no effect. |

### `post`

One full screen pass on the 3D scene, once a frame, before the overlay:

1. the first depth-tested draw (z function other than always, or z written) into the
   guest sized colour target marks it as the frame's scene;
2. the first draw after that into the same target that tests nothing (the game's glow
   composite, HUD, Flash, text) starts the overlay: the pass runs right before it;
3. a frame with 3D and no overlay gets the pass at present; a frame with no 3D (Bink,
   legal, 2D menus) never gets it.

So HUD text, minimap, gauges, Flash and video are never filtered. Alpha passes
through untouched. A target an extension keeps in its own range (`range != 1`) is
skipped and counted.

The pass is an edge antialiasing filter (own implementation: luma of the four
corners gives the edge direction, two taps at a sixth and two at half the span,
the wide average dropped when it leaves the neighbourhood's luma range) followed
by exposure, shoulder, contrast, saturation and gamma.

```text
--mc3-post=fxaa=1,threshold=0.0625,exposure=1,gamma=1,contrast=1,saturation=1,shoulder=0,split=0
```

or `render_post = "..."` in `settings.toml`. Every grading value is neutral by
default; `fxaa=0` with the defaults makes the pass an exact copy. `split=1` leaves
the left half untouched for an A/B inside one image. With an F3 capture the
scene is saved before and after the pass (`NNNN_post_pre_<why>.bmp`,
`NNNN_post_<why>.bmp`) and `frame.txt` gets a `post` line. Every 600 presents the
log says how many frames had the pass.

With timing on, the log also gives the pass's own GPU cost (the copy it reads
plus the pass): `MC3RENDER timing post_pass_ms avg=... p95=... p99=...`.

#### Measured (18/10, RTX 4060, guest 720x480, window 1920x1080)

Pre/post pairs come from the same frame of one F3 capture (`render_ab/post_pairs.py`):

| Scene | Result |
| --- | --- |
| Bink intro, legal, title video | no pass (no scene); 0 `post` lines in `frame.txt` |
| main menu 3D | pass at the overlay boundary; FXAA changes 3-17 % of pixels |
| MC2 LA race, night, HUD/minimap/gauges | pass at the HUD start (draw ~700-1100); minimap drawn after it; FXAA changes 4-6 % at 2.25x, 11-24 % at native 713x475 |
| `fxaa=0`, neutral grade | 0 pixels changed in all 9 captures (menu and race) |
| `split=1` | changes start at x=360 only |
| grade (exposure 1.1, contrast 1.08, saturation 1.2, gamma 1.05) | scene graded, HUD colours unchanged |
| cost, race at 2.25x | `post_pass_ms` avg 0.072, P99 0.079 of a ~2.2 ms GPU frame (about 3 %); CPU unchanged |
| soak, 662 s race | 81001 presents, the pass on 599-600 of every 600 frames, no device errors |

Tested since, and part of why both features went into `enhanced`: Paris, Tokyo and
the GTX 1660 (MC2 career races), the 619 Customs garage and San Diego free roam
(native MC3, `run_render_native.py`).

The boundary rule came from the captures: an earlier version that let XYZRHW draws
mark or end the scene filtered the Bink frame and, in races, ran at draw 13 (the
game's own glow composite in the middle of its 3D).

### `motion-blur` (MC3-0011)

LARecomp's Xbox 360 path documents `StreakMotionBlur`: scene depth is reprojected
between cameras, while stencil vehicle IDs select each vehicle's previous matrix
so the car does not smear like static scenery. This MC3 D3D12 experiment implements
that principle with its own shader. MC3's NV2A path has no equivalent vehicle-ID
stencil map here, so it tests reconstructed depth points against the four nearest
car body boxes and uses their current/previous Matrix34. It is not an exact port
of the Xbox 360 pixel shader. The pass samples the current 3D scene along the
motion vector, with depth discontinuity rejection, before the HUD, Flash and
video overlays. Since 23/10 the calibrated version is the shared default in
`legacy`, `enhanced` and `hybrid`.

```text
--mc3-render-profile=legacy|enhanced|hybrid
--mc3-blur=strength=0.30,max_pixels=10,samples=6,split=0
```

`split=1` leaves the left half on MC3's original image for a same-frame A/B.
`render_blur = "strength=0.30,max_pixels=10,samples=6,split=0"` is also accepted
in `settings.toml`. Use `--mc3-render-disable=motion-blur` for an explicit
comparison with MC3's original high-speed radial effect.

To avoid stacking MC3's original speed blur on this pass, build the tiny sidecar
from the user's own, unmodified Remix Xbox archive:

```text
cd templates/game
python tools/generate_blur_override.py assets/assets_x.dat
```

The output `assets/mc3_motion_blur_override.bin` contains the compressed original
`tune/vehicle/default.mcsenseofspeed` entry and a same-sized variant with its blur
alpha/scale zero and speed thresholds out of range. The game file API substitutes
those bytes only when `motion-blur` is effective, after validating the sidecar
against the archive. Remix still submits one half-resolution radial-blur composite
at high speed, so the D3D12 backend also suppresses that precisely identified draw
before applying the replacement. The archive on disk is never changed. The log
reports both substitution and suppressed-composite counts; without a valid sidecar
the MC3 speed blur stays on and the experimental pass stays off.

An F3 capture saves `NNNN_blur_pre_overlay.bmp` and `NNNN_blur_overlay.bmp`; its
`frame.txt` says how many car matrices corrected the pass. A camera cut or missing
depth skips the effect for that frame. On 22/10 the hybrid Arcade Race automated route ran 16 s in gameplay with
throttle, two F3 frames and no crash. The last timed 598-pass window measured
0.191 ms GPU average, 0.488 ms P95 for this pass at 1620 x 1080. The matching
enhanced run (23 s) kept the original tune and passed. On 23/10 a delayed F3
capture finally reached high speed and proved that MC3's half-resolution radial
composite still survived the tune override. After suppressing that exact draw and
calibrating the replacement to `0.30/10/6`, a 58 s hybrid run produced a sharp
pre-pass scene and a light post-pass streak; a 20 s Enhanced regression also passed.
The user approved that result as the default for all three profiles. A trial with eight car
boxes coincided with two guest XACTENG exceptions; it was reverted to four and
the separate audio investigation is tracked as MC3-0017.

### `contact-shadow`

A soft dark ellipse on the ground under the player's car, made from the game's own
depth (Gate 4 of MC3-0008). Each pixel's depth goes back to a world position and
into the car's frame; inside the ellipse and near the contact plane it is darkened,
the car's body (more than `top` above the plane) and a road far below (a jump) are
left alone. It darkens by multiplying, so it is as strong as the light under the car
allows, day, night or tunnel; it fades out between `near` and `far` metres from the
camera. Alpha is never written.

When: before the first blended, depth tested, not depth written draw that follows
the opaque world in the scene target, which in MC3 is the game's own blob shadows
(a black alpha 64x32 texture, capture 201454 draw 0600). The ground and the car are
in the depth then; smoke, glass, glows and the HUD come after it.

Which car: `src/game/render_car.cpp` sends every car (vtable `0x46C750`, Matrix34)
with each frame's draws in `State::host`; the backend takes the one nearest the main
camera (within 30 m), follows it by position from frame to frame, and switches when
it stays more than 20 m from the camera for a second while another car is within
12 m (the race intro camera picks an opponent first). The title also sets views of
its own around every car (reflection maps) and one about 860 m away, so the choice
cannot be made from the views the title sets.

```text
--mc3-shadow=strength=0.6,width=1.25,length=2.65,ground=0,top=0.22,fade=1.2,soft=0.45,near=35,far=70,debug=0
```

or `render_shadow = "..."`. `ground=0`: MC3 puts the car's origin on the contact
plane (calibrated with `debug=1`, which paints 0.25 m height bands and marks the
plane within 5 cm; the road came out at 0.95 there). Car axes: x right, y up, the
car faces -z. With an F3 capture: `NNNN_shadow_pre_<why>.bmp` / `NNNN_shadow_<why>.bmp`
and a `shadow` line in `frame.txt` with where the car's origin and axes land on
screen. `render_ab/shadow_views.py <run>` measures a run.

The F7 freecam cannot judge the car against its shadow: it moves only what is drawn
with the renderer's view (the MC2 city, fixed function draws); MC3's vertex program
draws, the car among them, stay on the game's camera. Under freecam the shadow sits
where the car really is and the car is drawn where the chase camera would see it.

### `vehicle-shadow`

The player's car rendered from a directional light into a depth map, and the ground
near it compared against the map (Gate 5 of MC3-0008). Hybrid only:
`--mc3-render-profile=hybrid --mc3-render-feature=vehicle-shadow`.

How the car gets into the map without knowing MC3's vertex programs: every
depth-tested triangle draw into the scene target with the full viewport that writes
depth or does not blend runs a stream-output variant of its own pipeline, which keeps
the clip-space positions the translated vertex shader made (the root signature gets
`ALLOW_STREAM_OUTPUT` only when the feature is on). At the shadow's point of the next
frame (the same point as `contact-shadow`) those positions go back to the world with
that frame's inverse view x projection, into the car's frame with that frame's car
matrix and out again with this frame's, so a rigid car casts where it is now even in
San Diego, where the title draws the car after that point. Only what lies inside the
car's box (`boxw`, `boxl`, `boxt`, from 2 cm above the contact plane) casts; the road
does not, so there is no acne on it. Then an orthographic light box of `radius` round
the car, moved in whole texels so the edges do not swim; receivers are the pixels
near the car below `top` in its frame (the ground, not the car), with constant and
slope bias and (2 pcf + 1)^2 taps, faded at the edge of the radius and with camera
distance.

```text
--mc3-vshadow=strength=0.5,size=1024,game=1,lumref=0.8,floor=0,elev=55,azim=35,radius=4.5,bias=0.03,slope=1.5,pcf=2,top=0.3,near=35,far=70,debug=0
```

or `render_vshadow = "..."`. `size` is 256, 512, 1024 or 2048. `debug=1` shows the
mask unfaded, `debug=2` also paints the depth map in the top left corner (anything
in it solid). The light is the title's own environment light (`game=1`): MC3 sets it as fixed
function directional light 2 — in San Diego at night a grey light 30 degrees up
(moonlight, diffuse 0.38, 0.19 in places), at dusk in Paris an orange one 10
degrees up (0.41 0.23 0.14), in the MC2 cities' night races the same direction as
San Diego but black. Slot 0 is a white 45 degree default and slot 1 carries local
lights (street lamps) that change from object to object; neither is used. The same
direction reaches the car programs as `c14`. Its brightness over `lumref` (eased
over half a second) scales the strength, never below `floor`: moonlight and a low
dusk sun give a fainter shadow than a day sun, and a black light (MC2 night races)
gives none — the log counts those frames as `no key light`. A low light casts long, so the light box grows to hold a
1.6 m car's shadow (up to 12 m). Without such a light in the last half second, or
with `game=0`, `elev`/`azim` and the full strength are used; the log says which
(`MC3RENDER vehicle-shadow: light from ...`) and the capture line carries it.

Each draw streams into its own part of the buffer with its own filled-size slot: with
one shared counter, consecutive draws raced on the GPU and kept 18-41k of 133k
vertices (a car of outlines in the map). In an F3 capture `frame.txt` marks the
streamed draws (`so N`) and lists any draw whose GPU count differs from the CPU's
(`so-check ...`, expected 0); `render_ab/vshadow_views.py <run> <prefix>` shows a
run's pairs, maps and a zoom on the ground behind the car.

### `sun-shadow`

Directional shadow for the whole scene from the title's environment light, one
cascade over the first `range` metres of the view and a coarser one out to `far`
(Gate 6 of MC3-0008). Hybrid only:
`--mc3-render-profile=hybrid --mc3-render-feature=sun-shadow`.

It shares the stream output of `vehicle-shadow` (either feature turns it on): the
previous frame's opaque draws go back to the world with that frame's camera and are
drawn from the light into one depth map that holds the cascades side by side (2 x
2048 by 2048). Each cascade is the sphere round its slice of the view, the near one
0..`range`, the far one 0.8 `range`..`far`, so neither swims when the camera turns;
each light box reaches 200 m further toward the light for what stands between the
light and the slice, and moves in whole texels. At the defaults the near cascade's
sphere has a radius of about 50 m and 5 cm texels, the far one's about 160 m and 16
cm texels; between 0.8 `range` and `range` the receiver blends from the near to the far
cascade, and the far cascade's constant bias grows with its texel. The player's car is moved to where it is
now; with `vehicle-shadow` on it is left out, since that pass draws the car's shadow
sharper. Receivers are the pixels within `far` of the camera (`range` with one
cascade) whose face (from the depth's derivatives) is turned to the light, with
constant and slope bias and (2 pcf + 1)^2 taps; the last fifth of the reach fades
out. Strength follows the
light's brightness exactly as for `vehicle-shadow` (`lumref`, `floor`), with its own
easing.

MC3 leaves the alpha test on for nearly every draw, at ALWAYS or GEQUAL 0; a draw
whose test can discard (a fence, a tree) does not cast, since it would cast as a
solid card. Everything else that is opaque casts.

```text
--mc3-sun=strength=0.55,size=2048,cascades=2,range=60,far=200,bias=0.08,slope=2,pcf=1,game=1,lumref=0.8,floor=0,elev=55,azim=35,debug=0
```

or `render_sun = "..."`. `size` (each cascade's side) is 512, 1024, 2048 or 4096,
`cascades` 1 or 2, `range` 10-300 m, `far` 20-1000 m (no further than `range` + 1:
one cascade). `debug=1` shows the mask unfaded, `debug=2` also paints the maps in the
top left corner, `debug=3` paints the cascades as bands instead of the shadow (near
light, far dark). The log has `MC3RENDER sun-shadow: light from ...`, a first-pass line, and
every 600 frames how many had the pass (`no previous frame`, `no target`,
`no key light`); the capture line is `NNNN sunshadow ...`, with `NNNN_sun_pre_*` and
`NNNN_sun_*` images. `render_ab/sun_views.py <capture dir substring> <out.png>` puts
them side by side with the difference.

Known limits, by construction:

- what the title culled from last frame's view casts nothing: a building behind the
  camera or beside it, with the light behind it, gives no shadow, and its shadow
  appears when it comes into view;
- moving cars other than the player's cast where they were a frame ago (about 0.5 m
  at 100 km/h at 60 fps);
- where the car's shadow (from `vehicle-shadow` or `contact-shadow`) falls inside
  a building's shadow, both darken it;
- under a roof (the 619 Customs garage, a tunnel) everything is in shadow, which is
  right for a light outside but darkens lamp-lit interiors; in the garage the title's
  key light is black, so the pass is off there unless `floor` is raised.

#### Measured (19/10, RTX 4060, guest 720x480, window 1920x1080)

San Diego free roam at night (`run_render_native.py`, the same 30 s drive each time,
one build):

| Profile | GPU frame avg | GPU frame p99 | The passes themselves |
| --- | --- | --- | --- |
| `legacy` | 1.48 ms | 2.12 ms | - |
| `hybrid` `vehicle-shadow` | 1.74 ms | 1.84 ms | vehicle 0.09 ms |
| `hybrid` `sun-shadow`, `cascades=1` | 2.04 ms | 2.15 ms | sun 0.15 ms |
| `hybrid` `sun-shadow` (two cascades) | 2.32 ms | 2.43 ms | sun 0.16 ms |
| `hybrid` all four (10 minute soak) | 1.83-2.11 ms | 2.17-2.32 ms | post 0.07, contact 0.02, vehicle 0.03, sun 0.11 ms |

The frame times move by 0.3-0.8 ms between runs of the same drive (traffic, where
the car ends up: the vehicle-only run measured 2.24 ms on an earlier build), so the
reliable figure is the pass's own stamps: the second cascade (a second caster draw
of the same vertices and a second set of taps for the far pixels) adds about
0.01-0.04 ms; the sun pass is 0.10-0.19 ms with two cascades in San Diego and
0.07-0.19 ms in Paris. Most of the step from `legacy` is the stream output of every
opaque draw (0.3-0.8 ms here, where MC3 draws the whole city itself; far less in the
MC2 cities, whose city the host draws), which `vehicle-shadow` already pays. Casters:
100-250k streamed vertices a frame in San Diego, 27-61k in Paris.

Two soaks of 10 minutes with all four features in San Diego (one cascade: 114 timing
windows, 68 400 presents; two cascades: 130 windows, 78 000 presents) ended with no
exception and no `no previous frame` after the first frames. On the second drive the
pass was off for whole windows with `no key light`: parts of San Diego's free roam set
the environment light dark, and the shadow goes with it.

What it shows: with the moonlight of San Diego (brightness 0.34, strength about 0.23)
the near cascade gives building shadows on the sidewalks, lamp posts' and traffic
lights' thin shadows and the car's long shadow ahead of it; the far cascade adds the
shadows on the facades and streets past 50 m that one cascade left lit. Paris at dusk:
strength about 0.18, the low sun's long shadows. Not seen yet: a day light, Los
Angeles and Tokyo (their night light is black, so the pass is off), a race in San
Diego.

An unrelated fault seen while measuring: 5 of 68 MC2 career runs (one of them
`legacy`) died 2-5 s after the race loaded, with a guest null read or write from
0x408BBF/0x408FD9/0x409185 on object 0x5A1FC0, under every profile.
`run_render_ingame.py` now fails a run whose log has a guest exception or whose F3
captures came back empty (it used to report such a run as passed).

## Reflections

### The fix every profile gets (19/10)

The car paint had no reflection in any profile, `legacy` included: the body drew flat
paint. MC3 draws its cars with D3DX effects (`$/shaderlibx/cars/*.fx`).
`carpaint.fx` blends the game's environment map in with its own pixel shader:

```text
lrp     r0.rgb, t1.a, t1, t0     ; decal over the base paint
mul_x2  r0.rgb, r0, v0
mov     r1.rgb, t2               ; the environment map
+mul_x2 r1.a, t2.a, v0.a         ; its alpha times the per vertex Fresnel
lrp     r0.rgb, r1.a, r1, r0
```

The first time an effect pass runs, its states are recorded into an XDK state block;
after that the block is applied. `EndStateBlock` (0x4169D0) reads the pixel shader to
record from the device (`CDevice + 0x784`). The runtime's `SetPixelShader` replaced the
XDK's and never wrote that field, so every pass recorded pixel shader 0 and replayed
it. The car paint, the car glass (`car_window.fx`) and the garage panels fell back to
the fixed function defaults: stage 1 times TFACTOR, stage 2 (the environment map)
disabled. `runtime/xdk/d3d8.cpp` now:

- stores the handle in the device as the XDK does;
- makes pixel shader objects shaped like the XDK's (reference count at +0, the
  definition at +0xC with its pointer at +8);
- frees one only on its last release, since a state block holds a reference.

The paint now reflects the garage and the street lights in `legacy` as the game
intended. It also changes the glass, the tyres' tread shader, the menu panels drawn
over the car in the garage, and any other effect pass that had lost its shader.
Checked with no exception in: the garage, San Diego free roam, the MC2 Los Angeles and
Paris races, and the boot screens and menus.

How it was found, for the next time a material looks wrong: with `MC3PS_NOTES=1` in
the environment, an F3 capture's `frame.txt` lists the title's `SetTexture` and
`SetPixelShader` calls between the draws, with the guest address that made each.
`render_ab/xbe_dis.py <va> --func` disassembles the XBE around an address.

### `reflections`

On top of that fix, three changes to the title's own reflections. A real cube map or
screen space reflections are out of reach: the title draws the city only from its own
cameras, and a car mostly reflects what is behind the camera.

- The reflection maps are rendered at `scale` times the render scale (2: 1152 x 1152
  at 1080p instead of 576). These are the square power-of-two targets up to 512 (the
  256 x 256 environment map, its 64 x 64 blur, their depth).
- The combiner stage that blends a reflection in by `r1.a` is found by its inputs
  (A = r1.a, B = r1, t1 or t2, C = r0, D = 1 - r1.a). Before it, the reflection share
  is raised to `fresnel` and scaled by `strength`, and what is reflected is
  brightened by `boost`. The frame buffer alpha keeps the title's own share. This
  matches the car paint, the car glass and the city's wet road.

```text
--mc3-refl=scale=2,strength=1.3,fresnel=0.8,boost=1.2,debug=0
```

or `render_refl = "..."`. `debug=1` paints the reflection share in grey, `debug=2`
the reflection alone. Not done: mipmaps for the reflection maps (they are sampled with
the title's filter, linear without mips), and a per pixel Fresnel (the title's comes
per vertex, and the paint's pixel shader has no normal to work one out from).

Measured 19/10 (RTX 4060, the same 30 s San Diego drive): `legacy` 1.47 ms GPU a frame (with the fix), `hybrid reflections` 1.88 ms, `enhanced` (post, both car shadows, reflections) 2.25 ms. The step is mostly the larger reflection maps, and the drive's own spread is 0.3-0.8 ms. A 12 minute `enhanced` soak in San Diego (129 timing windows, 77 400 presents) and an MC2 Los Angeles race under `enhanced` ran with no exception. `--mc3-render-disable=reflections` takes it out of `enhanced`; `scale=1` keeps the maps at the render scale.

## `light-shadow` (MC3-0012)

Shadows from the night's own lights, starting with the player's headlights. Hybrid
only: `--mc3-render-profile=hybrid --mc3-render-feature=light-shadow`.

MC3's headlights are additive passes (`mcSpotlight`): the geometry near the light is
drawn again with a fixed function vertex format (XYZ, diffuse, one texture
coordinate), the 64 x 64 cone texture and `SRC_ALPHA + ONE`. Traffic headlights use
the same kind of pass. At the shadow point the backend renders a perspective depth map
from the front of the player's car (`fwd` ahead of its origin, `height` above it,
tilted by `pitch`, `fov` wide, `near`..`far`). The casters are the previous frame's
streamed scene without the player's car. Every headlight pass after that point then
reads the map (texture slot 3, constants in `c[180..191]`) and multiplies its own
light by the visibility. A shadow only takes away headlight light, never the rest of
the scene.

```text
--mc3-lights=strength=0.9,size=1024,fov=70,near=0.3,far=50,fwd=2,height=0.65,pitch=-4,bias=0.06,slope=0.015,pcf=1,debug=0
```

or `render_lights = "..."`. `debug=1` paints the headlight passes red where shadowed
and green where lit. `debug=2` also shows blue inside the light's frustum and green
outside it (other cars' lights).

Measured 19/10 in San Diego: the map costs 0.02-0.03 ms, and about 11 headlight passes
a frame read it. A pole or a traffic light casts its shadow on the facade ahead, and a
parked car shadows the wall beside it. The visible change is small, though. The
game's headlight light is faint and falls on the facades ahead, not on the road, and
the shadows cover about 0.3% of the frame. The street lamps have no projected light to
mask: their light is baked into the city's vertex colours, with additive glows on top.

### The headlight beams, after MCLA (20/10)

Seen in LARecomp (MCLA for the Xbox 360, built and run here on 20/10, see below): at
night every car near the camera throws a warm pool of light on the road ahead, the
light falls on the other cars too, and a car standing in a beam shades it. The street
lamps cast no shadow. MC3's own headlights are too faint for this: they only light the
facades far ahead. So the beams are this feature's main part:

- **The cars.** Every car MC3 draws goes through one of its four car vertex programs
  (handles B0101FC1, FD1, FE1, FF1, the same in every run and city seen: the player's
  paint, glass and parts, and the traffic's). Those programs get the car's world x view
  x projection in c100-c103, so with the draw's own view x projection taken back out
  the car's world matrix is left: player, opponents and traffic alike. A car's parts
  come with matrices of their own; within 3 m only the most upright one is kept. Only the draws into the main scene count (a car also draws the others into its reflection map from a camera of its own), and a car must be at least 2 m across, measured on its vertices. Each beam follows a filtered car placement, and a car that had a beam keeps it against one up to a fifth nearer.
- **Following them.** Each car is a track: where it was, and its speed in metres a
  frame. A frame's car goes to the track that would be nearest it at that speed, one car
  a track. The beam follows the track's filtered place, predicted at the car's speed and
  pulled half way towards where it was drawn, and a frame further on (the cars are the
  previous frame's, the scene it lights is this one). A miss over 4 m (a respawn) is
  taken at once. Until 20/10 the place was only eased (35 % a frame) and a miss over
  2 m snapped it: a racer at speed trailed by more than that, so the beams of the
  opponents jumped forward and fell back (seen by the user). The log says how far the
  cars were from their prediction on average and how many snapped; the MC2 Los Angeles
  race: 4-8 cm, none. The track still identifies and prioritizes a car, but a
  recognized model's beam now uses its live vehCar Matrix34 directly.
- **Parked cutscene cars (22/10).** The first San Diego Ordered Race keeps its intro
  cars still while the camera passes them. F3 exposed paint matrices from one car
  entering several tracks: two lamps landed 1.8 m apart, and at times two landed on
  the same spot. The four-beam budget now removes tracks in the same 2.5 m footprint,
  compares normalized facing vectors, and prefers the lower body matrix over a part.
  A track that stays inside a 2 m window for 60 frames holds its averaged pose and
  zero speed until it leaves that window. This prevents velocity prediction from
  amplifying static part jitter. The F3 `beams` line now includes `overlaps` (candidates
  discarded) and `stationary` (selected held poses). Six retest captures in this
  Ordered Race had no overlapping selected beams; five of six showed all four held
  poses, with one newly seen car still settling. Interactive video retest remains
  useful because F3 samples cannot prove the absence of every frame-to-frame flash.
- **Which have their lights on.** A car is followed from frame to frame. Its lights
  are on while it moves, or while one of MC3's glows sits at its front, for 5 s after
  either. The player's are always on. A parked car neither moves nor glows and stays
  dark.
- **Model anchors (22/10).** All 94 main `vp_*.xbck` models in the Remix Xbox archive
  have one `headlight_cone`, `hl_cone`, or `hlight_cone` skeleton node. Its local
  translation is generated into `src/game/headlight_anchors.h` by
  `python tools/generate_headlight_anchors.py assets/assets_x.dat_2`. For each live
  vehCar, the game hook validates the mcCar owner and reads its `vp_*` model name.
  The renderer matches that car to the drawn body and transforms the model's
  local cone anchor by the live Matrix34. This fixes the origin and orientation
  to the car even when a paint part's draw matrix wanders. Unknown/custom models
  and unmatched cars retain the filtered `beam_fwd`/`beam_height` fallback.
  F3's `beams` line reports `anchored=N` as well as overlaps/stationary. A garage
  test with `vp_d_scion_tc_05` produced `anchored=1` and a matching beam origin.
  An Arcade Race in San Diego (22/10, 25 s) reached gameplay and three F3
  captures reported `anchored=4`, `anchored=4`, and `anchored=2`, matching the
  number of beams actually drawn in each captured frame.
- **The beams.** The `cars` lit cars nearest the camera each get a spot light at
  their model's cone anchor, or just ahead of the front bumper when there is no
  match (`beam_fwd`, `beam_height`, tilted by `beam_pitch`, `beam_fov` wide,
  reaching `beam_range`), with a perspective depth map (a 2 x 2 atlas
  of `beam_size`) from the previous frame's streamed scene, the other cars included. A
  beam's own car is left out of its map by a box at the place the car was drawn that
  frame (the vehicle shadow's box, a little larger, as long as the car's measured
  length): with it in, the car's body a frame back stood in front of its own lamps while
  it moved, and the beams flickered (seen by the user on 20/10). One full screen pass per beam
  then adds `beam_strength` x colour x cone x distance falloff x shadow, multiplied into
  what is already there (target + target x light). A headlight meets the road at a few
  degrees, so the light there follows the cone rather than the cosine.

```text
--mc3-lights=cars=4,beam_strength=2,beam_range=32,beam_fov=90,beam_inner=0.45,beam_pitch=-10,beam_r=1,beam_g=0.86,beam_b=0.66,beam_size=1024,beam_bias=0.06,beam_slope=0.01,beam_debug=0,beam_fwd=2.4,beam_height=0.7,beam_cone=0
```

`beam_debug=1` paints the beams green where they light and red where something in the
way shades them. `cars=0` turns the beams off. The log says every 600 frames how many
car bodies were taken, how many frames had beams, and how many car frames were left
dark (parked); F3 captures have a `NNNN beams` line with each beam's place and
direction, and `NNNN_beam_pre_*` / `NNNN_beam_*` images.

The beams take the place of MC3's own headlight cone (the 64 x 64 mcSpotlight passes):
with `cars` above 0 those passes are not drawn, and the player's headlight shadow, which
only masked them, is not made. `legacy` keeps the cone, the feature being off there;
`beam_cone=1` keeps it beside the beams. The log counts the cones left out.

Known limits: a car drawn after the shadow point (the player's in San Diego) is not lit
by the others' beams that frame. Unmatched cars still follow the previous frame's
tracked placement, about 0.5 m behind at 100 km/h. Matching a glow to a car's front
is by distance only. The model anchor is the central cone node; independent left and
right emitters are not implemented.

Measured 20/10 (RTX 4060, guest 720x480 in a 1080p window). The 30 s San Diego drive: `legacy` 1.57 ms GPU a frame, `hybrid light-shadow` (beams and the player's headlight shadow, lamps off) 2.69 ms. That includes the stream output every shadow feature shares (0.3-0.8 ms here), up to four beam maps and four full screen passes. A 12 minute soak with `post`, both car shadows, `reflections` and `light-shadow` (75 600 presents, 126 timing windows) ran with no exception, 1.85 ms GPU at the end. San Diego free roam finds 5-12 cars a frame (player and traffic); parked cars stayed dark. The MC2 Los Angeles race lights the player and the opponents up the road. Shadows of one car in another's beam were not caught in a capture yet (the runners do not stage one car behind another).

### The street lamps (optional, off by default)

`lamps=0` by default since 20/10, so the default look follows MCLA, whose lamps cast
no shadow; `--mc3-lights=lamps=4` turns them on.

MCLA, as far as this project's material on it goes (the renderer study, LARecomp's
notes, the PSP headlight reports), has the moon's cascades and the same headlight
cone as MC3, and nothing on shadows from lamps. So this part is MC3's own.

- **Where the lamps are.** MC3 draws a glow at every lamp head as a screen space card
  (XYZRHW: four corners in guest pixels and the lamp's depth, the cards strung into
  one strip). Each card's centre and depth go back to the world with that frame's
  camera. A point `lamp_min_h`..`lamp_max_h` above the player's car goes into a cache
  of the session, and it counts as a lamp once it has been seen at the same place on
  `lamp_hits` frames. Lights that move never get there. Lamps that left the screen
  stay known.
- **The shadow.** The `lamps` lamps nearest the camera (within `lamp_range`) each get
  a downward perspective depth map (`lamp_fov`, a 2 x 2 atlas of `lamp_size`). The
  casters are the previous frame's streamed scene, with the player's car moved to
  where it is now. Then, one full screen pass per lamp takes the baked light down by
  `lamp_strength`. The amount falls off linearly to nothing at `lamp_radius` and is
  scaled by how squarely the surface faces the lamp. The pole, a parked car, a
  pedestrian and the player's own car shade the ground around the lamp.

```text
--mc3-lights=lamps=4,lamp_range=45,lamp_radius=14,lamp_strength=0.5,lamp_fov=140,lamp_size=1024,lamp_far=25,lamp_min_h=2.5,lamp_max_h=14,lamp_drop=0.3,lamp_hits=6,lamp_bias=0.08,lamp_slope=0.01
```

`debug=3` paints the lamps' shadows black, without the falloff. `lamps=0` keeps only
the headlights. The log says every 600 frames how many lamps are cached, how many
glows were taken and how many lamps cast on average; F3 captures have a
`NNNN lampshadow` line with the lamps used, and `NNNN_lamp_pre_*` / `NNNN_lamp_*`
images.

Known limits: a lit window drawn with the same glow at lamp height counts as a lamp too.
The baked light is taken down whether or not that lamp is what lit the spot, which
reads as a shadow but is not a relighting. The maps are coarse near the edge of the
140 degree view.

Measured 19/10 (RTX 4060, San Diego night): 4 lamps a frame from a cache of about 800 points (nearly all confirmed), 0.18 ms GPU for the four maps and passes (0.25 ms average on the first run with 512 maps, peaks to 1.9 ms while the cache fills), headlights 0.02 ms. Same 30 s drive: `legacy` 1.70 ms GPU, `hybrid light-shadow` 2.17 ms. A 12 minute soak with `post`, both car shadows, `reflections` and `light-shadow` (64 200 presents) and the MC2 Los Angeles race ran with no exception. In the MC2 cities no lamp is found (0 cached): their lamps are not drawn as MC3's glows, so only the headlights cast there.

## Promotion to `enhanced` and what is not tested yet

`post` and `contact-shadow` (18/10) and `vehicle-shadow` (19/10) are in `enhanced`
with their defaults. `legacy` stays the default profile and takes none of them;
`--mc3-render-disable=post`, `--mc3-render-disable=contact-shadow` or
`--mc3-render-disable=vehicle-shadow` takes one out of `enhanced`.

### Where `vehicle-shadow` is off or faint

It follows the title's environment light, so in much of MC3 it does little, by
design:

| Where | What it does | Why |
| --- | --- | --- |
| MC2 career night races (Los Angeles, Tokyo) | off | the environment light is black (`no key light` in the log) |
| the 619 Customs garage | off | the garage sets a black key light; `contact-shadow` still darkens under the car |
| San Diego free roam at night | faint (about 0.12-0.24 of full) | moonlight 30 degrees up, diffuse 0.19-0.38 against `lumref` 0.8 |
| Paris at dusk (MC2 career) | faint and long (about 0.16 of full) | an orange sun 10 degrees up; the light box grows to 11 m |
| before the title sets any environment light (boot, the first frames of a session) | full, from `elev`/`azim` | the configured fallback |
| a day race | not seen yet | should give the full strength if the title sets a bright light |

`--mc3-vshadow=floor=0.4` keeps a faint shadow where the light is black,
`lumref=0.4` makes night shadows twice as strong, `game=0` uses `elev`/`azim` at full
strength everywhere.

Covered: Bink intro, legal, title, main menu (no pass where there is no 3D scene);
main menu 3D, 619 Customs garage, San Diego free roam at night (native MC3); MC2
career races in Los Angeles, Paris and Tokyo, night and bright interiors, with HUD,
minimap, gauges and headlights; tyre smoke over the shadow; 640x480-like internal
resolution in a 1080p window; RTX 4060 and GTX 1660; four 10 minute soaks (two with
all three features, in Los Angeles and in Paris where the key light is on). Cost on
the 4060: post 0.08-0.14 ms, the contact shadow 0.03-0.05 ms, the vehicle shadow
0.06-0.13 ms plus its stream output; all three together 2.33 ms against 2.15 ms of
legacy in the same race (+8 %), CPU unchanged.

Not tested yet; check these first if `enhanced` looks wrong:

- a day race or day free roam in a native MC3 city (only night San Diego and the
  MC2 cities so far);
- rain;
- tunnels, bridges, ramps and jumps under the car: the shadow is built to fade when
  the road drops away, but nobody has watched it happen; the F7 freecam cannot show
  the car against its shadow (MC3-0009);
- Atlanta and Detroit;
- opponents' and traffic cars (both shadows are the player's only, by design);
- the vehicle shadow with a full-strength day light;
- networking, split screen, the race editor, replays.

## What the boot log says

```text
MC3RENDER profile=hybrid features=none (profile from command line; restart to change, the profile is fixed at boot)
MC3RENDER requested=post disabled=none ignored=none pending=post
MC3RENDER adapter[0] NVIDIA GeForce RTX 4060 vram_mb=...
MC3RENDER adapter_index=0 name="..." luid=... vram_mb=... driver=a.b.c.d vendor=10DE device=... (index from default)
MC3RENDER guest=640x480 output=1920x1080 scale=2.250 vsync=0 tearing=1 fps_cap=0
MC3RENDER pso_cache=memory shader_cache=memory config_hash=... timing=1 capture_ab=...
MC3RENDER effective config written to ...\logs\render_effective_config.txt
MC3RENDER timing frames=600 cpu_ms avg=... p95=... p99=... gpu_ms avg=... | psos=... shaders=... textures=... targets=...
```

The profile lines come before the device is made, so before any target or PSO.
`logs/render_effective_config.txt` repeats all of it plus the EXE's SHA-256 and the
command line; an F3 capture copies it next to `frame.txt` with `capture_meta.txt`.

Shader and PSO caches are in memory only. `cache_identity` (vendor, device,
subsystem, revision, driver, profile hash; never the LUID, which changes across
boots) is the header any future persistent cache must match before it is used.

## Backout

`--mc3-render-profile=legacy`, or `render_profile = "legacy"` in `settings.toml`,
or delete the key. The pre-profile binary is kept as
`MC3RemixXBE.before_render_profiles.exe` (SHA-256 4D5623BE...C9D1).

## A/B

The A/B runner used during development (a script of the research tree, not part of this
repository) boots each profile to the title and the main menu, takes F3 captures and
PrintWindow shots, and compares them against `legacy`, with a second `legacy` run as the
control for the scenes' own nondeterminism. Its results went to `runs/<stamp>/result.json`
there; captures of the game are never committed to this repository.

`render_ab/run_render_native.py --profile <p> [--feature ...]` covers native MC3
without the MC2 career (no `MC3_MC2_DIR`): it loads profile Player 1 (a San Diego
career; the highlighted row is found by its colour, whatever profile was used last),
opens Career > Go to Garage (619 Customs) for two captures, leaves the garage,
continues the career into San Diego and captures standing and driving.
`render_ab/native_summary.py <tag>` prints where each pass ran and what it changed.
`render_ab/pilot.py` drives one game process a command at a time for mapping new
menu paths.

Code: `runtime/gpu/render_profile.{h,cpp}`, the adapter/timing/capture parts of
`runtime/gpu/d3d12.cpp`, the three keys in `runtime/app/settings.cpp`.
