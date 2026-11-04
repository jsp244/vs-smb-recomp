VS. Super Mario Bros. native port - Steam Deck edition
=======================================================

This build starts in fullscreen at the Deck's own 1280x800 with the 16:10 widescreen
view (during play the level the game keeps beyond the screen's edges fills the sides),
and the menus are driven with the Deck's controls. It contains no game data: you need
your own dump of the VS. Super Mario Bros. board (a .nes file, or MAME's suprmrio.zip).

Install (Desktop Mode)
----------------------
1. Extract this folder somewhere in your home folder, for example
   ~/Games/VSSuperMarioBros.
2. Copy your dump (the .nes file or suprmrio.zip) into that same folder. The program
   finds it there by itself the first time (it checks the file's hash and remembers it).
3. Open Konsole in that folder (Dolphin: right-click > Open Terminal Here) and run
   `python3 add-to-steam.py`. It closes Steam, adds VS. Super Mario Bros. as a non-Steam
   game with its library artwork from SteamGridDB (portrait and wide capsules, hero,
   logo, icon), and starts Steam again. Running it again replaces the shortcut;
   `python3 add-to-steam.py --remove` takes it out again.
   (By hand instead: Steam > Games > Add a Non-Steam Game to My Library > Browse, switch
   the file type filter to "All Files", pick `vssmb`, Add Selected Programs.)
4. Return to Game Mode and start VS. Super Mario Bros. from your library.

Controls (Steam's default gamepad layout)
-----------------------------------------
D-pad or left stick   move
A                     jump
X                     run / fireball
View (two squares)    insert coin
Menu (three lines)    1P start
L2 + R2 together      this port's menu (graphics, audio, controls, cabinet switches,
                      save states); again to close it. Menu + View together works
                      too, and Steam's own menu as usual.
R1 (hold)             rewind
R2 (hold)             fast-forward; within 1.5 s after a rewind it goes forward
                      again to where the rewind began
2P start has no button by default; give it one on the menu's Controls tab, where every
button can be changed.

The cabinet
-----------
The menu's Cabinet tab sets the board's DIP switches: free play (the default: 1P start
begins a game) or coins per credit, lives, coins for an extra life, the timer speed and
the lives after a continue. The board reads them when it starts, so during a game a
change applies at the next reset (Play > Reset game).

Updates
-------
New versions are installed from the menu, in Game Mode, without reinstalling: L2 + R2,
then the Updates tab. Check for updates asks GitHub for the newest release (the Deck
needs a network connection only for this and the download). When there is a newer
version, Update now downloads the Steam Deck package, checks it against the SHA-256
checksums published with the release, and replaces the vssmb program in the game folder.
Your settings, save states, the ROM and the Steam shortcut with its artwork stay as they
are; the previous version is kept as vssmb.old. Restart now starts the new version in
the same Steam session, so the game simply comes back up. Nothing is downloaded or
installed until you press those buttons.

Tips
----
- Graphics > Scaling: Fit (the default) fills the 1280x800 screen with the 16:10 picture.
  Integer keeps exact pixels at 3x, the level across the whole width.
- Deck OLED: for perfectly even motion set the refresh rate to 60 Hz for this game in
  the quick access menu (Performance). The arcade board runs at 60 Hz.
- Latency: Graphics > Low-latency sync (on by default) hands each frame to the screen
  without vsync, as Mesen does by default, so no frame waits in a queue; with run-ahead
  the game reacts a frame sooner than in Mesen at its defaults. Switch it off for
  vsync: the smoothest scrolling, at up to one frame more latency.
- The 9 save state slots, settings and screenshots are kept in the game folder.
- The full player notes are in README.txt.
