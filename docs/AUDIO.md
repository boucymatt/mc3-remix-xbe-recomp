# Audio

The Xbox plays sound on the MCPX audio processor. The recomp has none: `sdk/runtime/xdk/dsound.cpp` takes over the
DirectSound library (XDK 5849) and mixes every buffer and stream in software, and `sdk/runtime/audio/xaudio2.cpp`
sends the stereo mix to the PC through XAudio2. XACT, which Midnight Club 3 uses for most of its sounds, is not
replaced: its own code (the `XACTENG` section) runs as recompiled and calls the DirectSound functions the host
provides.

The DirectSound functions are found by address. `config/game.toml` names them under `[symbols]`; a name that is `0`
there, or missing, runs the console's own code, which reaches for MCPX voice state the host objects do not have.

## Credit

The DirectSound and XACT functions behind the fixes below were located with **Thiekus**'s Ghidra project of the
Midnight Club 3 XBE, which names far more of the XDK libraries than the SDK's signature set. Thiekus gave permission
to use it. The XDK 5849.17 documentation and headers are the reference for what each function does.

## What the mixer does

* **Pitch and frequency.** `SetFrequency` and `SetPitch` (1/4096 octave from 48 kHz, held to
  `DSBPITCH_MIN..DSBPITCH_MAX`) set the same rate and override each other, as the XDK documents. XACT sets the pitch of
  the sounds it plays, and keeps setting it: 54 561 calls in a three-minute Cruise run. Before, every one of them
  failed and the sound kept the rate of its file.
* **Amplitude envelope.** `SetEG` with `DSEG_AMPLITUDE` is played: delay, attack, hold, decay and sustain in blocks of
  512 frames of the mix. `StopEx` with `DSBSTOPEX_ENVELOPE` plays the release, and with `DSBSTOPEX_RELEASEWAVEFORM` the
  wave leaves its loop meanwhile; the buffer reports itself playing until the release ends. XACT stops its sounds this
  way and gives most of them a release of 32 blocks, about a third of a second.
* **3D voice data.** XACT works out 3D itself with the DirectSound 3D calculator and hands the result to
  `Set3DVoiceData`. The terms are summed per mix bin as the 5849 voice client sums them while `Use3DVoiceData` is on,
  and the doppler term is added to the pitch.
* Mix bins, mix bin volumes, volume, headroom, play and loop regions, PCM and Xbox ADPCM, stream packets and their
  callbacks, as before.

Not reproduced: filters, LFOs, the multi-function envelope (pitch and filter), envelopes on streams, HRTF filtering,
the DSP effects image (reverb and the other sends are dropped when the voice also reaches speakers), and time stamps
on `StopEx`/`PauseEx`.

## Timer DPCs and XACT

XACT's sequencer runs from a kernel timer DPC (`XACT::CEngine::Dispatch`) and walks its event queue with no lock of
its own: on the console a DPC runs at `DISPATCH_LEVEL`, so no thread that raised its IRQL runs at the same time, and
`XACTEnterCriticalSection` takes no lock above `PASSIVE_LEVEL`. The runtime now runs every DPC at `DISPATCH_LEVEL`
(`RunDpcs` in `sdk/runtime/kernel/objects.cpp`), which takes the same host lock as a thread that raises to it. Before,
the DPC ran at `PASSIVE_LEVEL` beside the title's raised queue updates, and the queue broke: an access violation in
`XACT::CPriorityQueue::Pop2` at `0x408BBF` (task MC3-0017).

## Diagnostics

Both are off by default.

| Variable | Effect |
| --- | --- |
| `XBE_AUDIO_TRACE=1` | Every 10 s, `logs\mc3_xbe.log` gets how many buffers and streams exist and play, their rate range, and how often each DirectSound function ran. |
| `XBE_AUDIO_DUMP=<file>` | Writes the mix, as sent to the PC, to a 16-bit 48 kHz stereo WAV file. It is a recording of the game: keep it out of the repository. |
