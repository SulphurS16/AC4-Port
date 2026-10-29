# Changelog

## 0.6.0 (2026-10-29)

Two mouse modes, keys under every key type, the mouse in the garage, movies on two-core PCs, and
fixes for the SIMULATOR and ELIMINATION (GitHub issues #1 and #2). Most of it follows the ACFA
port's 0.7.3.

Updating from 0.5.x: unzip the new release and start its launcher, then click **Set up**: it
rebuilds what changed (this time also a part of the game's code), which takes a few minutes.

Mouse and keyboard
- **MOUSE MODE: STICK**, the new default (PC SETTINGS > KEYBOARD / MOUSE): the mouse works like a
  precise right stick. The AC turns exactly as far as the mouse moves, as fast as it can turn, and
  a quick flick turns the whole way (up to half a turn) instead of a few degrees; looking up and
  down works the same way.
- **MOUSE MODE: CAMERA**: the earlier free mouse camera. Now only the camera turns freely:
  lock-on, firing, the radar and movement stay with the AC's front as in the game, so it no longer
  locks on to or shoots at targets beside or behind it.
- Fixed: in the SIMULATOR the mouse did not turn or look (GitHub issue #1). Both modes work there.
- Keys keep their actions under every key type: with TYPE B or CUSTOM A / B the keys still fire
  the weapon, boost and so on that they do with TYPE A. PC SETTINGS shows the button each key
  presses in missions.
- New keys: **PURGE RIGHT / LEFT / SHOULDER WEAPON** (both weapon switches and the weapon's button
  together, as on the controller). No keys until you choose them in PC SETTINGS.
- Keys and mouse buttons pressed and released within one frame are no longer lost.

Menus
- The mouse in the garage: ASSEMBLY and WEAPON (click a slot to pick it, the part shown to equip
  it, the picker's ◀ ▶ or the wheel over it to go through the parts, the < > at the top for the
  next page), the ◀ ▶ arrows of option rows (CONTROL OPTIONS, the COPY dialog's CATEGORY, ...),
  PAINT's menus and the SIMULATOR's lists. Still by keys: PAINT's color sliders, the tiles of the
  postgame MISSION LIST, STABILIZER.
- Fixed: right after loading a save, the postgame MISSION LIST was taken for a screen without a
  menu, so Enter or a click pressed START (the KEY GUIDE) instead of choosing. Menus are now also
  recognised while they open or have nothing highlighted yet.

Performance
- Fixed: severe frame drops in ELIMINATION (Hard) whenever the Normals' missiles explode (GitHub
  issue #2). The port copied the depth buffer for every explosion effect, up to 433 times in a
  frame; now once per frame (the explosions look the same).
- Movies (the opening, briefings) play at full speed with working sound on PCs with two processor
  cores: there the opening ran at 20 to 30 frames per second with broken sound; now 60.

PC SETTINGS
- Long captions (MOUSE SENSITIVITY X / Y) and the render scale's resolution are no longer cut off.

## 0.5.1 (2026-10-17)

- **License notices.** The mingw-w64 runtime built into the launcher now has its notice in the release (in `licenses/` and at the end of the zip's README.txt), and `LICENSE` is the standard MIT text, so GitHub shows the project as MIT.
- **README.** The zip's README.txt follows the README on GitHub.
- No changes to the game. Updating from 0.5.0: start the new launcher and click **Set up**; it rebuilds a few files.

## 0.5.0 (2026-10-16)

PlayStation controllers, and a quieter launcher.

Updating from 0.4.0: unzip the new release (into a new folder or over the old one) and start its
launcher; it finds your install, then click **Set up**. This update builds the whole game again,
once (a few minutes); later updates rebuild only what changed (before, a new version number alone
made every update rebuild everything).

Controllers
- PlayStation controllers work directly: DUALSHOCK 4, DualSense and DualSense Edge, over USB or
  Bluetooth, also when connected while the game runs (before, only Xbox and other XInput
  controllers were read, so a PlayStation controller needed DS4Windows or Steam). Buttons as on
  the PS3 controller; Share / Create or the touchpad click is SELECT. No vibration on them yet.

Launcher
- The window says only what helps: the next step, or what is wrong. No more "Ready." or
  "Ready to play." (PLAY becomes clickable when the game is ready), no repeated size and folder
  under the Game data row, no notes after the shortcuts change.

## 0.4.0 (2026-10-16)

The launcher's install flow: from the download to playing, updating and removing.

Updating from 0.3.0: unzip the new release into a new folder (or over the old one) and start its
launcher; it finds your install (**Use it**), then click **Set up**: it rebuilds what changed.

Launcher
- A first start is guided step by step: the game folder, the decrypted files (Prepare files, or
  your own), the game data (a new row: where it goes, the space it needs, **Change...**) and Set up.
- Before anything goes into your Downloads folder (Windows' cleanup can delete old files there) or
  a folder OneDrive syncs (gigabytes to upload; files can become online-only), the launcher proposes
  your Local AppData folder or one you choose, showing the space needed.
- A new release unzipped into a new folder finds your earlier install and offers to use it, or to
  move its game data beside the new launcher (same drive): no Prepare files and no download of the
  build tools again.
- An update rebuilds only what changed (before, Set up rebuilt the whole game after each update).
- After the first Set up, the launcher asks which shortcuts to add: a desktop shortcut and a
  Start menu entry (two check boxes, both ticked; **More...** adds or removes them later); a newer
  launcher started from another folder makes them open itself.
- **More... > Remove game data...** deletes what the launcher and the game put on the disk, listed
  with sizes; your saves and settings only when you tick that box, and they are then kept as a
  backup. **More... > Install folder** became the Game data row's **Change...**.

Release
- The README explains Windows SmartScreen's warning (the launcher is not code-signed) and how to
  check the download against the SHA-256 on the release page.

## 0.3.0 (2026-10-16)

Mouse camera and new default keys.

- The mouse moves the camera, not the AC: the camera turns at once and exactly as far as the
  mouse moves (raw input, no acceleration, smoothing or dead zone), and the AC turns after it with
  its own turning, as with the stick, until it faces where the camera looks. The game still
  places the camera (distance, framing, walls); reset view and the game's scripted views bring it
  back behind the AC. Looking up and down is unchanged. The same at every frame rate (checked at
  60 and 120 fps).
- New default keys: left / right click left / right arm weapon, middle click lock on, Q / E switch
  left / right arm weapon, Space / Shift / Ctrl boost / quick boost / over boost, C shoulder
  weapon, V reset view, Tab or Escape pause, Enter SELECT, arrow keys the D-pad, and I J K L or
  numpad 8 4 5 6 turn and look (the numpad works with Num Lock on or off). Bindings still at the
  earlier defaults change to these when the game starts; bindings you changed stay (PC SETTINGS >
  KEYBOARD / MOUSE can reset them to the defaults).
- Tutorial: "Turn" and "Adjust view" count mouse movements of any speed.

## 0.2.1 (2026-10-16)

- Fixed: after the system's save or load list, its questions or its text entry (for example
  LOAD GAME from the main menu), the menu highlight moved by itself (in the hub it ran through
  the missions and the buttons) until a click or a key. While such a dialog holds the
  controller, the game clears its own copy of the controller; the port now sends the full
  controller state again as soon as the dialog closes (it used to report "no change", which left
  the game's cleared copy, read as sticks pushed up and left, in place).
- The launcher and the game window have the port's icon.

## 0.2.0 (2026-10-16)

Fixes from play-testing 0.1.0, and a smaller download.

Release
- The release zip holds two files: `AC4-Launcher.exe` and `README.txt`. The launcher carries the
  port's sources and unpacks them into its install folder (`local\source`) when it needs them;
  the launcher's file properties show its version. Updating: unzip over the old release, start
  the launcher, click **Set up** (it rebuilds what changed; tools, files, settings and saves
  stay). 0.1.0's source files beside the launcher are no longer used (README: Setup).

Game files
- **Prepare files**: RPCS3's questions are yours to answer. When RPCS3 asks "Install firmware:
  ...PS3UPDAT.PUP?", the launcher brings the question to the front, says what it asks and waits
  for you; **No**, or closing the question, stops Prepare files with nothing left behind. Any
  other window RPCS3 shows is handled the same way. Cancel now says "Cancelled" (it was reported
  as a failure). Decrypting the files yourself in RPCS3 works as before.

Mouse and keyboard
- Mouse look: the AC turns and the view pitches by exactly the angle the mouse moved, with no
  smoothing, acceleration or dead zone; a movement faster than the AC can turn finishes at the
  AC's full turning speed instead of being cut short, at every frame rate.
- Tutorial: looking up and down with the mouse counts for the "Adjust view" step; the whole
  tutorial can be played with keyboard and mouse.
- Menu cursor: in every menu it comes back as soon as the mouse moves (after PC SETTINGS, a
  mission's end, the pause menu, AC TEST, the tutorial and movies); menu keys hide it. Cutscenes,
  movies, the intro and other scenes without a menu show no cursor, even when the mouse moves.
  Outside missions the game no longer holds the mouse.

Display
- The system's save and load lists, questions and text entry are drawn inside the game's
  picture (they were Windows dialogs): the window keeps its mode, size and position during and
  after them, in fullscreen, borderless and windowed. Typed text replaces the suggested name.

## 0.1.0 (2026-10-15)

First release of the ARMORED CORE 4 port.

Game files
- No keys: `EBOOT.BIN` and 10 system modules of PS3 system software 4.93 are decrypted by your
  RPCS3. **Prepare files** in the launcher does it in one step with your `rpcs3.exe` and the
  update file `PS3UPDAT.PUP` (a private copy of RPCS3; your RPCS3 is not changed), or you choose a
  folder with files you decrypted yourself. Each file is checked by its SHA-256; the port
  decrypts nothing and downloads no system software. The disc is read in place.

Launcher
- Folders (game folder, Prepare files or decrypted files; each line turns green when its files
  are right), **Set up** (build tools download, import, build; resumable; space needed and free;
  CPU check), display mode, resolution and frame rate, **PLAY** (only the game window).
- **More...**: render scale, monitor, presets (standard, modest PC, handheld / low power, high
  refresh), install folder, open / back up / restore saves, reset all settings (keeps a .bak),
  safe mode, the setup log, the files the port needs.

Display
- The game renders at your window's or screen's resolution, with a render scale from 50 % to
  200 %; the 16:9 picture keeps its shape with borders; windowed, borderless fullscreen (the
  default) and exclusive fullscreen; monitor choice; resizable window; Alt+Enter and F11.
- Frame rates: 30, 60, 120, 144, the display's refresh rate and uncapped in missions, the
  tutorial and AC TEST (menus and movies at 60); the game's speed does not depend on the rate.

Mouse and keyboard
- The whole game without a controller. Mouse look in missions, the tutorial and AC TEST (raw
  input, no acceleration, separate X and Y sensitivity and inversion); the game takes the mouse
  when gameplay starts, F1 releases it.
- Menus with the mouse: the item under the cursor is highlighted, click chooses, right click
  goes back, the wheel moves; list rows (missions, schematics), the hub's two panels.
- Menus with the keyboard: arrows or W A S D, Enter or Space, Escape or Backspace, Tab (START),
  Q / E (pages), R / F / Z / X (Triangle, Square, L2, SELECT).
- Every key can be changed, for missions and for menus, two keys per action, mouse buttons and
  the wheel included.

In the game
- **SYSTEM > PC SETTINGS**: a row the port adds under QUIT GAME (the game's entries are
  unchanged), with DISPLAY, KEYBOARD / MOUSE and MENU KEYS pages; changes apply at once and are
  kept in the same settings file as the launcher's.

Saves and logs
- Settings and saves in `Saved Games\ARMORED CORE 4`, with backups and restore.
- Logs contain no personal data.
