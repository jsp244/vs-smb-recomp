VS. Super Mario Bros. native port - Android (handhelds and phones)
==================================================================

There are two APKs. Pick the one for your device:

- vssmb-android-handheld.apk: for gaming handhelds with built-in controls (AYANEO,
  Retroid, AYN Odin and the like). The controls are the device's own buttons; nothing is
  drawn over the picture.
- vssmb-android-phone.apk: for phones and tablets. A translucent on-screen controller
  (d-pad, B, A, COIN, START, MENU) is drawn over the picture. It installs as its own app,
  "VS. Super Mario Bros. Touch", so it can sit next to the handheld one; their saves are
  separate.

Both run on Android 7 or newer (arm64; they also carry an x86_64 build for emulators and
Chromebooks). They contain no game data: you need your own dump of the VS. Super Mario
Bros. board (a .nes file, or MAME's suprmrio.zip).

Install
-------
Copy the APK to the device and open it (allow installing from that app when Android asks),
or from a PC: `adb install vssmb-android-handheld.apk` (or -phone.apk).

First start
-----------
The menu opens on the ROM tab. Choose "Choose ROM file..." and pick your .nes file or
suprmrio.zip in the system file picker. The app copies it into its own storage, checks its
hash and remembers it, so this happens once.
Alternatively put the file in Android/data/org.vssmb.nativeport/files/ on the shared
storage (org.vssmb.nativeport.touch for the phone app); the app finds it there by itself.

The picture fills the screen in the shape that matches it: 16:9 widescreen on a 1920x1080
screen like the AYANEO Pocket EVO's, 4:3 on the Retroid Pocket Nova's 1280x960. During play
the level the game keeps beyond the screen's edges fills the widescreen sides. Both can be
changed on the Graphics tab.

The cabinet
-----------
VS. Super Mario Bros. is a coin-op. The Cabinet tab sets the board's DIP switches: free play
(the default: Start begins a game) or coins per credit, lives, coins for an extra life, the
timer speed and the lives after a continue. Insert coin is the Select button (COIN on the
phone's screen); a coin goes in once per press.

Controls on a phone
-------------------
The on-screen controller: d-pad at the bottom left (slide your thumb around it; the
diagonals are narrower than the four directions), B (run, fireball) and A (jump) at the
bottom right (a thumb between them presses both, and it can slide from one to the other),
COIN and START at the bottom, MENU at the top for this port's menu (the Back gesture opens
it too). In the menu, tap a tab or a row; tap the arrows of a row to change it. A
controller can be used as well: the on-screen controls step aside while you play with it
and come back when you touch the screen.

Controls on a handheld
----------------------
The built-in controller works as a gamepad. Default buttons (Xbox labels; on a Nintendo-
style layout such as the Retroid's, change them on the Controls tab if they feel wrong):
A = jump, X = run / fireball, Select = insert coin, Start = 1P start. LT + RT (L2 + R2)
together opens the menu and closes it again; Start + Select together and the Android back
button open it too. R1 (hold) rewinds, R2 (hold) fast-forwards; within 1.5 s after a rewind
it goes forward again to where the rewind began. 2P start has no button by default: give
it one on the Controls tab.

Notes
-----
- Save states (9 slots) and settings live in the app's private storage; Android's app
  backup includes them. Uninstalling the app deletes them.
- The app never goes online (it has no internet permission).
- A log of the last run is written to Android/data/org.vssmb.nativeport/files/
  vssmb-log.txt (.touch for the phone app; handy with `adb pull` when something goes
  wrong).
- Smooth motion and latency: on a 120 Hz screen (Pocket EVO) the display runs at 120 Hz
  while the app is in front, and every frame is shown for exactly two refreshes, locked
  to the compositor's deadlines (Android 13 and newer report them).
- Building the APK yourself: see the README (android/build_apk.py).
