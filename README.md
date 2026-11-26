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
| L3 / R3 | left / right stick click |
| right stick | right stick |

The menu line "controls" (`controls=std|alt` in `settings.txt`, default `std`) switches to an alternative scheme at once. It applies to full pads (handheld, Pro Controller, a Joy-Con pair), not to one Joy-Con held sideways.

| Game input (Xbox name) | standard | alternative |
| --- | --- | --- |
| left trigger | ZL | right stick up |
| left shoulder | L | L or right stick down |
| right stick click | R3 | R3, ZL or L3 |
| left stick click | L3 | not available |
| right stick up/down | right stick | not available |
| right stick left/right | right stick | right stick |

All other buttons are the same in both schemes. The stick acts as a button from half of its way and releases at about a third.

| Switch | Action |
| --- | --- |
| **+** and **−** together | Open the menu: resume, fps counter, sound, overlay, hatching, frame skip, controls, start level, go to level, cheats (a submenu), mods (a submenu). Saved in `sdmc:/switch/gh/settings.txt` |
| **D-pad up/down** | Move in the menu |
| **D-pad left/right** | Change the value of the line: left = previous, right = next (wraps around; held, it repeats after 0.4 s). The A button works as "next" and opens the `cheats` and `mods` lines. The B button goes back |
| **+** | Start |
| **−** | Back (skips cut scenes) |

The version `0.<count>.<sha>` shows at the bottom-left of the screen and in the app list. Logs are in
`sdmc:/switch/gh/gh_log.txt` and `gh_rex_log.txt`. The menu item "overlay" hides the version; the fps counter is a separate item and still shows (alone) when the overlay is off; an empty file `sdmc:/switch/gh/overlay_off` does the same.

The menu line "hatching" (`hatching=0|1` in `settings.txt`, default 1) switches the pencil hatching on the character sprites off or on. It works at once.

Level select (Switch): in the menu, the line `start level` steps (left or right) through off, 1-1, 1-2, 1-3, 2-1, ... 18-2 (the game's own stage names).
The chosen level is used when you pick `Story > Continue` or start a new story game: the story-entry code reads the stage byte, and the game replaces it in memory by `start_level` (log: `[gh] continue: stage A -> B`). The save file on disk is not changed, but a later autosave may store the chosen stage, so Continue then plays that stage. Set the level to `off` to play normally.
The line `go to level` (A button) closes the menu and jumps to the chosen stage at once, without leaving the game: it runs the same steps as Story > New game (clear the game state, store the stage, request the game's mode state 8) from the game thread. This is experimental: it resets the story progress like a new game, the screen you get may be the stage-intro menu, and it has not been proven for every game state. The log shows `[gh] apply ...` and `[gh] mode state A -> B`. If it fails, quit and use Story > Continue or New game (the start level is applied there).
It is saved as `start_level=<0..40>` in `settings.txt` (0 = normal flow) and applies when a new story game starts.
The log shows `[gh] new game: stage byte A -> B` and `[gh] stage load: index N`.
The start level applies once per visit of the title screen (log: `[gh] start level consumed`). After that stage the story goes on to the next stage. Back at the title it is armed again.

Performance log: `gh_log.txt` has one `[dk] perf:` line a second: fps, then the command processor's time per frame in ms (`draw`, `tex` inside it, `clear`, `copy`, `swap` with `acquire` and `fence`), the texture uploads, `gpu wait` (the wait for values the game writes, by kind), `guest poll` (the share of the second the game's thread waits for the GPU side) and the calls of the 4P hooks.
`gpu_poll_us=<50..1000>` in `settings.txt` (default 1000) is the sleep between two polls of such a wait.
The menu line "frame skip" (`fps_cap=60|30` in `settings.txt`, default 60 = off) draws every second frame of the game. The game and the sound keep their speed; the picture moves at 30 fps. The perf line shows `skip on`, the dropped packets a second and `new` (copies into a texture that was never drawn: if it is not 0, a one-time picture may be missing). An empty file `sdmc:/switch/gh/dk_diag` turns the per-draw diagnostic log lines on again (they cost time).
Exit from the game's menu ends the process without a teardown and returns to the HOME menu (not to the homebrew menu).

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

### Mods (Switch)

A mod is a folder on the SD card with pictures that replace the game's own (character sprites, portraits).

```
sdmc:/switch/gh/mods/
  hd-heroes/            one folder per mod; the folder name is the mod's id
    mod.toml            name, version, author, ...
    sprites/            PNG files for the 8 bit sprites
    textures/           PNG files for true-colour textures (portraits)
  sprites/              the flat folder of older versions: still works, shown as "legacy sprites"
  textures/             the same for textures: "legacy textures"
```

```toml
id = "hd-heroes"          # the same as the folder name
name = "HD Heroes"        # shown in the menu
version = "1.0"
author = "your name"
description = "Upscaled hero sprites"   # shown under the mod list in the menu
kind = "sprites"          # sprites or textures (information only)
license = "CC-BY-4.0"
order = 0                 # optional: a lower number wins a conflict
```

- The menu line `mods` opens the list of all mods found, each under its `name`. D-pad left/right or A switches the mod under the cursor on or off. The switch works at once, without a restart. The bottom of the page shows the `description`, the version and the author of that mod.
- In `settings.txt` the keys are `mod.<id>=0|1`. A new mod starts on.
- The menu font has small Latin letters, digits and `. , ! ? ' ( ) / & + - _ :`. Another character shows as `?`. A long name scrolls on the cursor line. A description has two lines of 31 characters; the rest is cut.
- Two mods with the same picture: the mod with the lower `order` wins, then the id that comes first in the alphabet. The legacy folders come last. The log (`gh_log.txt`) has one line `mods: conflict: ...` per picture. When the winner is switched off, the next mod's picture is drawn.
- The game lists the folders once at the start. Add or remove a mod, then restart the game. At most 16 mods are loaded.
- `tools/make_sprite_mod.py <png folder> <out folder> --id <id> --name "<name>"` makes a mod folder from a folder of PNG files.
- How to make the pictures: [docs/SPRITE-REPLACEMENT.md](docs/SPRITE-REPLACEMENT.md). The file name ends with `__<id>.png`; the id comes from `tools/spritemod.py export-all` (it reads your own game files).

The game's own menus are not changed: a new native item needs a study of the game's menu code, and that study is not done.

## Cheats

Four cheats, all off by default: `cheat_invincible` (player health stays full), `cheat_mp` (magic stays full),
`cheat_continues` (99 continues) and `cheat_attributes` (all six attributes at the game's cap).

- PC (Linux, Windows): tick them in the settings window, tab **Cheats**, and press "Save to gh.toml". They also work as
  `cheat_invincible = true` in `gh.toml` or as `--cheat_invincible=true` on the command line.
- Switch: press + and - together to open the settings menu and pick `cheats`. The submenu has `invincible`,
  `magic`, `continues`, `attributes` and `back`. Move with D-pad up/down, switch with D-pad left/right or the A button (the right
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

## Four-player story mode

The story mode takes up to four players. This is the work of [RedDude](https://github.com/RedDude/guardian-heroes-recomp-4p-story).
The game starts with one or two players, as the original. With player 2 in the game, a free controller joins as player 3 or 4: press **Start** (Switch: **+**) while no dialogue runs. The game shows its own "player joins" screens (hero and parameters), then restarts the stage.
The levels of all four heroes are saved (`gh4p_party.bin` in the user data folder). Players 3 and 4 do not take part in cut scenes. The credits are the original, finite ones (`GH_4P_CREDITS=1` on Linux and Windows makes them infinite).

- Switch: the console in handheld mode comes first, then controllers 1-4 in the order of the system's Controllers screen. The first four are the game's pads 1-4. A controller is a Pro Controller, a Joy-Con pair or one Joy-Con held sideways. The menu (**+** and **−**) belongs to the first one. There is no on-screen "press START" hint on the Switch.
- One Joy-Con held sideways (the rail up) is a whole pad: the stick moves (it is also the D-pad); the four buttons act by their place as on a full pad (right = A, bottom = B, left = Y, top = X); **SL** / **SR** = L / R; the two buttons at the end = ZL and ZR; **−** on a left Joy-Con or **+** on a right one = Start (join); the stick click = Back. If the directions are turned by a quarter, put `single_joycon=0` in `sdmc:/switch/gh/settings.txt` (the system then turns the Joy-Con).
- The file `sdmc:/switch/gh/gh_log.txt` has `[nx] pads:` lines: what the console reports for each controller, at the start and at every change.
- Off switch (Switch): the line **4 players (RedDude)** on the **Mods** page of the menu (**+** and **−** together). It is saved as `mod.4players=0|1` in `sdmc:/switch/gh/settings.txt` and acts after a restart of the game. The older key `four_players=0` is still read when `mod.4players` is absent. With the mod off, every hook of the mod calls the game's own function.
- Off switch (Linux, Windows): the environment variable `GH_4P=0`. It is read at the start.

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
