# Host performance

How the recompiled game spends the host's CPU, what has been done about it, and the rules that keep it fast. Task
MC3-0018.

## Two hot threads

The game runs on two busy host threads:

* the **title thread** runs the recompiled game code and records every Direct3D call for the render thread
  (`sdk/runtime/gpu/render_thread.cpp`);
* the **render thread** replays those calls into Direct3D 12 (`sdk/runtime/gpu/d3d12.cpp`).

A frame takes as long as the slower of the two. Before parallelising either, find out which one is limiting:

* `XBE_PERF=1` logs, once a second, `title thread per frame: recording X ms, waiting Y ms`. When `waiting` stays near zero,
  the title thread limits the frame. When it grows, the title is waiting for the render thread.
* A thread sampler shows which functions take the time. Any sampler that resolves symbols through
  `MC3RemixXBE.pdb` will do.

## What was done (MC3-0018, 25/10/2026)

The scene: San Diego Cruise with 100 traffic cars from 30 models, the enhanced profile and 128 MB of guest memory.
Before these changes the render thread was idle about half of the time, and the title thread set the frame time.

| Change | Where | Effect |
| --- | --- | --- |
| Debug keys F7 and F8 are read at most every 8 ms | `src/game/debug_tools.cpp` `Down()` | `GetAsyncKeyState` ran on every view transform, many times a frame: 22% of the title thread, about 9 ms of a 44 ms frame |
| Dirty bits for the 8 KB of `simple` states and the 3 KB of vertex shader constants | `State::MarkSimple`/`MarkVsc`, `render_thread.cpp` `SendState` | recording for the render thread halved; `SendState` went from 17% to 6% of the title thread |
| Draw state cache on the command list | `d3d12.cpp` `SetViewport`, `SetScissor`, `SetBlendFactor`, `SetStencilRef`, `SetTopology`, `SetPipeline` | about 4,800 Direct3D calls fewer per dense frame; 97% of draws repeat the viewport, 70% the pipeline |
| Car hooks check each car once a call and keep its headlight anchor | `src/game/render_car.cpp` | from 6.7% to 3.0% of the title thread |
| The previous draw's four texture views are reused when they repeat | `d3d12.cpp` `SetTextureViewsAndSamplers` | 40 to 70% of draws need no new views; driver time on the render thread from 15% to 9% |

The scene is not deterministic, so a run is compared with another by frame time at equal draws per frame, with two
runs of each build:

| draws/frame | before | after |
| --- | ---: | ---: |
| 2000-2499 | 33.2 ms | 17.9 ms |
| 2500-2999 | 35.7 ms | 18.9 ms |
| 3000-3499 | 36.6 ms | 18.0 ms |
| 3500-3999 | 35.1 ms | 18.8 ms |

In practice that is about 28 FPS before and 53 FPS after. Every change is plain behaviour, with no flag. None of them changes what is drawn. They
were checked in:

* the no-flag legacy Cruise;
* six bridge cars with a same-process reload;
* Midnight Club 2 Los Angeles under legacy and enhanced;
* the 100-car scene twice, through pause and Go to Menus.

After these changes the title thread waits about 2 ms a frame for the render thread in dense scenes. Only now would
recording command lists on several threads pay off. Moving single per-draw jobs, such as texture hashes, to workers
has been measured to cost more than it saves.

## Rules

* **Every write to `State::simple` or `State::vsc` marks its piece.** Use `g_state.MarkSimple(index)` or
  `g_state.MarkVsc(register)` next to the write; a function that writes into a `State&` marks it too
  (`ApplyPixelConstants`). An unmarked write is never sent, and the render thread draws with the old value. Run once
  with `MC3_STATE_VERIFY=1`: it compares the whole 11 KB after each send and logs
  `render thread: state at +N changed without its dirty bit` for a write that missed its mark.
* **Viewport, scissor, blend factor, stencil reference, topology and pipeline go through the backend's setters.** Do
  not call `RSSetViewports`, `RSSetScissorRects`, `OMSetBlendFactor`, `OMSetStencilRef`, `IASetPrimitiveTopology` or
  `SetPipelineState` on `cl_` directly: the cache would skip a later call that it believes is already set. An
  extension that records on the list it gets from `Context::List()` is free to set anything; the cache stops while
  it holds the list and is forgotten after its call.
* **The texture view reuse relies on released resources staying alive until the frame's fence.** Release a texture
  or target through `CurrentFrame().garbage`, never by dropping its `ComPtr` while the frame is being recorded.

## Measuring

`XBE_PERF=1` writes per-second lines to `logs\mc3_xbe.log`. `tools\perf_windows.py` groups them by draws per frame so
that two runs with different scenes compare:

```bat
python tools\perf_windows.py before=run1\mc3_xbe.log after=run2\mc3_xbe.log
```

Pool at least two runs per build. Read the median as well as the mean, because a single second with shader
compilation moves a bucket's mean by tens of milliseconds.
