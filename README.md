# Guardian Heroes (XBLA) recompilation

Static recompilation of **Guardian Heroes** (2011, Xbox 360 Live Arcade, Treasure/Sega, title ID `58410B2B`)
for **Nintendo Switch** and **PC (Linux)**. It uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0.

**This project contains no game data.** You need your own copy of the game. The release files hold only the
recompiled program and the renderer. They do not hold the game's graphics, sound or music.

## What works

| Platform | Renderer | State |
| --- | --- | --- |
| Nintendo Switch | deko3d | Story mode plays at 60 fps on hardware. **No sound yet.** |
| Linux x86-64 | Vulkan | Plays. Keyboard and gamepad. In-game settings window. Sound uses SDL audio (not yet checked by ear). |
| Windows x86-64 | Vulkan | Plays (title and story checked, keyboard works). Build: MSVC headers and libraries with clang. Gamepad not yet checked on Windows. |

Known gaps: Xbox Live features, the avatar items, and the camera are not supported. There is no texture replacement yet
(see `docs/PC-FEATURES.md` for the plan).

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

Buttons follow the Xbox layout by position (Switch B = Xbox A). The on-screen button icons show Switch letters.

| Switch | Action |
| --- | --- |
| **+** and **−** together | Open the small settings menu: resume, fps counter on or off |
| **+** | Start |
| **−** | Back (skips cut scenes) |

The version `0.<count>.<sha>` shows at the bottom-left of the screen and in the app list. Logs are in
`sdmc:/switch/gh/gh_log.txt` and `gh_rex_log.txt`. Create an empty file `sdmc:/switch/gh/overlay_off` to hide the strip.

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
