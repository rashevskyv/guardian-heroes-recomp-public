# Guardian Heroes (XBLA) recompilation

Static recompilation of **Guardian Heroes** (2011, Xbox 360 Live Arcade, Treasure/Sega, title ID `58410B2B`)
for **Nintendo Switch** and **PC (Linux)**. It uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0.

**This project contains no game data.** You need your own copy of the game. The release files hold only the
recompiled program and the renderer. They do not hold the game's graphics, sound or music.

## What works

| Platform | Renderer | State |
| --- | --- | --- |
| Nintendo Switch | deko3d | Plays at 60 fps on hardware, with sound (libnx audout). Both graphics modes (Remix and Original) work. |
| Linux x86-64 | Vulkan | Plays. Keyboard and gamepad. In-game settings window. Sound uses SDL audio (not yet checked by ear). |
| Windows x86-64 | Vulkan | Plays (title and story checked, keyboard works). Build: MSVC headers and libraries with clang. Gamepad not yet checked on Windows. |

Known gaps: Xbox Live features, the avatar items, and the camera are not supported. Sprite replacement (mods) works on Switch only
(see "Sprite mods" below). Switch applet mode (launch from the album) is not tested.

## Get your game files

You need the XBLA package of the full game (a file with the `LIVE` or `CON` header, title ID `58410B2B`).

```
python3 -I tools/stfs_extract.py "Guardian Heroes.bin" game/
```

The `game/` folder must hold `default.xex` and the `media/` folder.
The recompiled code matches the retail `default.xex` of the 2011 release only (3,178,496 bytes).

## Switch

1. Unzip `gh-<version>-switch.zip` to the root of the SD card. It creates `switch/gh/`.
2. Copy your `game/` folder to `sdmc:/switch/gh/game/` (so `sdmc:/switch/gh/game/default.xex` exists).
3. Start **Guardian Heroes** from the homebrew menu. Use title takeover: hold **R** while you start any game.
   Applet mode is not tested.

Buttons follow the Switch letters: Switch A = the game's accept (Xbox A), Switch B = cancel (Xbox B). Y and X stay positional (Switch Y, west = Xbox X; Switch X, north = Xbox Y). The on-screen icons show the same letters: A and B as the game draws them, X and Y swapped. In the menu: A = ok, B = back.

| Switch | Game action (Xbox name) |
| --- | --- |
| A (east) | A: accept / attack |
| B (south) | B: cancel / special |
| Y (west) | X |
| X (north) | Y |
| L / R | left / right shoulder |
| ZL / ZR | triggers |

| Switch | Action |
| --- | --- |
| **L3 and R3** together (hold 0.15 s, any order) or **+** and **−** together | Open the small settings menu: resume, fps counter, sound, overlay, hatching, sprite mods, cheats (a submenu; saved in `sdmc:/switch/gh/settings.txt`) |
| **D-pad up/down** | Move in the menu |
| **D-pad left/right** | Change the value of the line: left = previous, right = next (wraps around; held, it repeats after 0.4 s). The A button works as "next" and opens the `cheats` line. The B button goes back |
| **+** | Start |
| **−** | Back (skips cut scenes) |

The version `0.<count>.<sha>` shows at the bottom-left of the screen and in the app list. Logs are in
`sdmc:/switch/gh/gh_log.txt` and `gh_rex_log.txt`. The menu item "overlay" hides the version; the fps counter is a separate item and still shows (alone) when the overlay is off; an empty file `sdmc:/switch/gh/overlay_off` does the same.

The menu item "hatching" (`hatching=0|1` in `settings.txt`, default 1) switches the pencil hatching on the character sprites off or on.
The item "sprite mods" (`sprite_mods=0|1`, default 1) draws the PNG replacements from `sdmc:/switch/gh/mods/sprites/`, or the game's own sprites when off. Both work at once, without a restart.

Level select (Switch): in the submenu `cheats`, the item `start level` steps (left or right) through off, 1-1, 1-2, 1-3, 2-1, ... 18-2 (the game's own stage names).
The chosen level is used when you pick `Story > Continue` or start a new story game: the story-entry code reads the stage byte, and the game replaces it in memory by `start_level` (log: `[gh] continue: stage A -> B`). The save file on disk is not changed, but a later autosave may store the chosen stage, so Continue then plays that stage. Set the level to `off` to play normally.
The line `apply` (A button) closes the menu and jumps to the chosen stage at once, without leaving the game: it runs the same steps as Story > New game (clear the game state, store the stage, request the game's mode state 8) from the game thread. This is experimental: it resets the story progress like a new game, the screen you get may be the stage-intro menu, and it has not been proven for every game state. The log shows `[gh] apply ...` and `[gh] mode state A -> B`. If it fails, quit and use Story > Continue or New game (the start level is applied there).
It is saved as `start_level=<0..40>` in `settings.txt` (0 = normal flow) and applies when a new story game starts.
The log shows `[gh] new game: stage byte A -> B` and `[gh] stage load: index N`.

Create an empty file `sdmc:/switch/gh/mute` to start with the sound off.

### Automatic clocks (Switch)

The game raises the CPU and GPU clocks by itself and lowers them again when it has headroom (a governor, adapted from the Hard Corps: Uprising port). Keys in `sdmc:/switch/gh/settings.txt` (MHz; the pad menu keeps them when it saves):

| Key | Default | Meaning |
| --- | --- | --- |
| `auto_clock` | `1` | `1` = governor; `0` = fixed at the maximum (the old behaviour) |
| `clock_max_cpu` | `1785` | CPU limit; a smaller value snaps down to a table rate (1020 ... 1785) |
| `clock_max_gpu` | `768` | GPU limit (307.2 ... 768) |
| `clock_max_mem` | `1600` | Memory clock, set once (1331 or 1600) |

The limits can only go down: a larger value is cut to the defaults, which are the old fixed clocks. The governor counts the game's frames every 0.1 s over the last second. It goes up one rung at once at 3 or more lost frames (two rungs at 6 or more). It goes down one rung after 5 checks of 2 s with under 1 lost frame. A failed step down doubles that wait (up to 64). A second with fewer than 30 frames (a load, a menu) keeps the rung. There is one ladder for handheld (CPU 1224 / GPU 384 up to 1785 / 768) and one for docked (CPU 1428 up to 1785, GPU 768). The ladders are not measured on the console yet. If sys-clk or Horizon OC runs, the game leaves the clocks alone. The old clocks come back on exit and after a crash. Log lines start with `[clock]`.

### Sprite mods (Switch)

Put PNG files in `sdmc:/switch/gh/mods/sprites/` to replace the character sprites of the Remix mode. The file name ends with
`__<id>.png`; the id comes from `tools/spritemod.py export-all` (it reads your own game files). An indexed PNG that keeps the
game's palette also keeps the costume colours. Tools: `tools/spritemod.py`, `tools/palettize.py`. Step by step: [docs/SPRITE-REPLACEMENT.md](docs/SPRITE-REPLACEMENT.md).

## Cheats

Four cheats, all off by default: `cheat_invincible` (player health stays full), `cheat_mp` (magic stays full),
`cheat_continues` (99 continues) and `cheat_attributes` (all six attributes at the game's cap).

- PC (Linux, Windows): tick them in the settings window, tab **Cheats**, and press "Save to gh.toml". They also work as
  `cheat_invincible = true` in `gh.toml` or as `--cheat_invincible=true` on the command line.
- Switch: press + and - together to open the settings menu and pick `cheats`. The submenu has `invincible`,
  `magic`, `continues`, `attributes`, `start level`, `apply` and `back`. Move with D-pad up/down, switch with D-pad left/right or the A button (the right
  button). They work at once and are saved in `sdmc:/switch/gh/settings.txt` with the other menu items.
  You can also write the keys by hand, in `settings.txt` or in `sdmc:/switch/gh/cheats.txt` (read first; `settings.txt`
  wins), then start the game:

```
invincible=1
mp=1
continues=1
attributes=1
```

The cheats act on human players only.

## Linux

1. Unpack `gh-<version>-linux-x86_64.tar.gz`.
2. Put your `game/` folder next to the `gh` file, or pass `--game_data_root=<folder>`.
3. Run `./gh.sh`. You need a Vulkan driver (Mesa or the NVIDIA driver).

Keyboard defaults: move **WASD**, A button **Space** or **;**, B button **Backspace** or **'**, X **L**, Y **P**,
Start **Enter**, Back **Tab**. A gamepad works without setup.

Press **F1**, **Esc**, or **Start and Back together** on a gamepad to open the settings window. The game sees a neutral pad
while the window is open. You can change the display, the sound and the keys there. The window saves to `gh.toml`.
The option `menu_open = start_back | hold_back | esc` selects how the gamepad opens it.

## Windows

1. Unzip `gh-<version>-windows-x86_64.zip`. It holds the program, the Microsoft runtime DLLs (no install needed) and `gh.bat`.
2. Put your `game/` folder next to `gh.exe`, or pass `--game_data_root=<folder>`.
3. Run `gh.bat` (or `gh.exe`). You need a Vulkan driver (every current graphics driver has one).

The keys, the settings window and `gh.toml` work as on Linux.

## Build it yourself

The build uses the ReXGlue SDK v0.10.0 plus small edits. Order of work:

1. Install the SDK and the CLI. Apply `patches/sdk/*` and run `switch/sdk-edits.py <sdk>` and `patches/sdk-edits-gh.py <sdk>`.
2. Make the code: `rexglue codegen gh_manifest.toml` (needs `game/default.xex`). The output goes to `generated/default/`
   and is **not** part of this repository.
3. Linux: `tools/wsl-linux.sh` (WSL or Linux, clang 20). Windows: `tools/win-msvc-build.cmd` (VS 2022 Build Tools for the headers and libraries, clang with the MSVC target; Vulkan only).
   Switch: `switch/build.sh` (devkitPro with devkitA64, deko3d, uam).

`docs/` holds design notes.

## Legal

This is an unofficial fan project. It is not linked to Sega, Treasure or Microsoft. Guardian Heroes is a trademark of its owner.
Do not ask for game files. Use only a copy that you own.
The source code uses the BSD 3-clause licence (see `LICENSE`). It includes parts of the ReXGlue SDK and of Xenia.
