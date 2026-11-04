# Input latency

Where the time goes between a button press and the pixels, and what the port
does about each part. `vssmb --play --stats out.txt` logs the measured values
once a second (add `--quit-after 600` for a timed run).

| stage | arcade board + CRT | Mesen 2, default settings | this port, PC |
|---|---|---|---|
| controller to program | ~0 (polled by the game) | USB pad 1-4 ms, Bluetooth 8-15 ms | USB pad 1-4 ms, Bluetooth 8-15 ms |
| press until the pad is read | 0-16.7 ms | 0-16.7 ms (latched once a frame) | 0-16.7 ms |
| the game's own reaction (1 frame) | 17 ms | 17 ms (run-ahead off) | 0 (run-ahead 1) |
| pad read until the frame is handed over | - | ~1 ms | ~2 ms (frame delay) |
| handed over until on screen | 0 (the beam) | vsync off: shown at the next scan-out | 0-1 ms: handed over just before the vblank |
| scan-out to mid-screen | 8 ms | 8 ms | 8 ms (60 Hz), 2 ms (240 Hz) |
| **average, press to mid-screen** | **~33 ms** | **~33 ms**, more in a desktop window | **~18 ms** at 60 Hz |
| sound effects | 0 | ~60 ms (audio latency 60) | ~25-45 ms (Low / Normal) |

## What the frontend does

**Frame delay.** On Windows each loop iteration first waits for the display's
vertical blank (`IDXGIOutput::WaitForVBlank` on the monitor the window is on,
which also works on hybrid-GPU laptops), then sleeps until `deadline - work -
margin`, where `work` is the measured time to emulate and draw a frame and
`margin` starts at 2 ms and grows if a deadline is missed. Only then are events
polled, the pad read, the frame emulated and presented. Input is therefore
sampled about 2 ms before the frame is handed to the display. On displays whose
refresh is not a multiple of ~60 Hz (75, 144, 165 Hz) the loop falls back to
timer pacing with the same frame delay, so latency varies by up to one refresh.
Presentation runs without vsync; a one-frame DXGI queue (`SetMaximumFrameLatency(1)`)
means a frame can never sit behind older ones.

**Run-ahead.** VS. Super Mario Bros. reads the controllers in its vblank handler
and moves Mario in the same handler; the sprites it builds there are copied to the
PPU at the next vblank, so a press shows one frame later: measured with the
headless harness (A pressed for frame 800 of a game, the first changed picture is
frame 801, Mario's jump pose). With run-ahead the loop emulates the real frame,
saves the machine state, emulates one more frame with the same input, shows *that*
picture, and restores the state. The player sees the game's reaction on the very
frame of the press, which the arcade board cannot do. Audio comes from the real
frame only. The default of 1 matches the game's lag exactly; a higher value would
show movement that gets taken back when a button is released. The self-test
(`--selftest N out.png`) verifies that a save / look-ahead / restore cycle leaves
the machine state bit-identical.

**Audio.** Dynamic rate control nudges the APU's output rate so the queue sits
at the preset target (Low 16 ms, Normal 32 ms, Safe 70 ms) plus a small device
buffer: a proportional part of up to 1.2% reacts to the queue, and a slow
integral part learns a steady clock difference (the board's 60.0988 Hz against
the display's rate), so the queue settles at the target instead of above it.

**Exclusive fullscreen** bypasses the desktop compositor, which otherwise adds
about one frame in a window; it is the recommended mode for play on Windows.

## Compared with Mesen 2 and RetroArch

Mesen 2's defaults, from its source (`Core/Shared/SettingTypes.h`): run-ahead 0,
vertical sync off, audio latency 60 ms, and the controller state is updated at
scanline 241, once a frame. RetroArch's (`config.def.h`, `configuration.c`):
run-ahead off, frame delay 0, vsync on with up to three swap-chain images, audio
latency 64 ms.

* At those defaults the port shows the game's reaction to a press a frame (17 ms)
  sooner: run-ahead 1 removes the game's own delay.
* With Mesen's run-ahead also set to 1 and vsync off, Mesen presents each frame as
  soon as it is emulated; the port hands its frame over about 2 ms before the
  vblank. The two are within a few milliseconds on average.
* With vsync on (Mesen's option, RetroArch's default), the emulator waits 0-16.7 ms
  (about 8 ms on average) for the vblank and can hold finished frames in the swap
  chain; the port is ahead by that.

## Measured on the development laptop (240 Hz panel)

```
pacing: vblank phase-lock, display 240 Hz, 4 vblank(s) per frame, run-ahead 1
frame 16.668 ms (60.00 fps)  input->present 1.79 ms  work 1.63 ms  audio queued 47 ms  missed deadlines 0
```

Input to present under 2.5 ms, plus up to one 4.2 ms refresh for scan-out, with
the game's own frame removed by run-ahead: faster than the arcade board on a CRT.

## Tuning

* Graphics > Low-latency sync: Off switches to plain timer pacing (for displays
  or drivers where the vblank wait misbehaves; look at `--stats` for frames
  far from 16.6 ms).
* Graphics > Run-ahead: 0-3 frames.
* Audio > Latency: Low / Normal / Safe.
* Prefer a wired controller; Bluetooth adds roughly 10 ms before SDL sees the press.
