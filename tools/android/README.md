# Android device test tools

For testing the APK on a handheld over adb without touching it. Paths below are on the device.

* `vpad.c` - a virtual copy of the AYANEO Pocket EVO's controller (uinput: same keys, axes,
  ranges and USB ids), driven by text commands through a FIFO. Writing events into the real
  pad with `sendevent` does not work for the d-pad and triggers: the kernel delivers an
  injected press and its release in one report, so Android only sees the final state.
  Build and start it (the adb shell user may use /dev/uinput):

  ```
  clang --target=aarch64-linux-android24 -O2 -o vpad vpad.c    # the NDK's clang
  adb push vpad vp.sh /data/local/tmp/
  adb shell 'chmod 755 /data/local/tmp/vpad; mkfifo /data/local/tmp/vpad.fifo'
  adb shell /data/local/tmp/vpad /data/local/tmp/vpad.fifo        # -t: digital L2 / R2 only
  ```

  On a Steam Deck, build it with `gcc -O2 -static` and run it with `-x`: it is then an Xbox 360
  pad, which Steam Input takes over the way it does a real one, so the game reads it only
  through Steam's virtual pad, like the Deck's own controls (the deck user may use /dev/uinput).

* `vp.sh` - sends commands and waits until they have run, for example
  `adb shell 'sh /data/local/tmp/vp.sh "press down" "chord l2 r2 40 250" "wait 500"'`.
  Buttons: a b x y l1 r1 l2 r2 (l2a / r2a axis only, l2k / r2k key only) select start mode
  l3 r3 up down left right. `quit` removes the virtual pad.

* `sflat.py` - summarizes the compositor's timestamps for the game's surface: how many
  refreshes each frame stayed on screen, and the time from queueing a frame to its display.

  ```
  adb shell 'dumpsys SurfaceFlinger --latency "$(dumpsys SurfaceFlinger --list | grep "^SurfaceView\[org.vssmb.*BLAST")"' > lat.txt
  python sflat.py lat.txt
  ```

The app's own timing log: put `--stats` and an absolute path (one per line) in
`/sdcard/Android/data/org.vssmb.nativeport/files/args.txt` and restart the app.
