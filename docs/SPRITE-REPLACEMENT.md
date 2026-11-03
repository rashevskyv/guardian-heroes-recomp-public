# How to replace sprites (Switch)

This guide tells you how to draw your own character sprites and see them in the game.
It covers the Remix (HD) graphics mode on the Switch. The Original mode keeps the game's own art.
Background: `docs/SPRITES-RESEARCH.md`. Nothing here needs a change to the game program.

## What you can replace

- Every character sprite frame of the Remix mode. One frame is one PNG.
- Any size. A 2x or 4x picture of the same frame works. The game scales it to the frame's place.
- Many colours. A true-colour PNG is drawn with its own 255-colour palette.
- Not covered: backgrounds, portraits, UI textures, and the Original mode.

## What you need

| Item | Where |
| --- | --- |
| Your own game files | `game/pkg/media/chara.mrg` and `chara_ss.mrg` (see README, "Get your game files") |
| Python 3 with `numpy` and `Pillow` | `pip install numpy pillow` |
| The tools | `tools/spritemod.py`, `tools/palettize.py` (run them from the repo root) |
| A Switch build of this project | 0.74 or later (the first build with the sprite hook is 0.73) |

Game files and exported pictures never go into git. The `mods/` folder is git-ignored.

## The file formats

- In the game: `chara.mrg` holds 55 character sets. A set holds up to 126 frames.
- A frame is an 8 bit indexed texture. Index 0 is transparent. The colours come from a 256-entry palette.
- Each set has 64 palettes. A costume palette recolours a few entries of palette 0.
- For you: PNG files. Two kinds work:
  - **Indexed PNG** (mode P) with the game's palette. Its indices go straight into the game's palette. Costumes keep working.
  - **Any other PNG** (RGB, RGBA, grey). The game reduces it to 255 colours and uses that palette. Costumes are lost.

## Step 1: export the frames

```bash
python3 -I tools/spritemod.py export-all mods
```

This writes:

| Path | Content |
| --- | --- |
| `mods/sprites/c<entry>_f<frame>__<id>.png` | Remix frames, RGBA, palette 0. These are the files you repaint. |
| `mods/sprites_ss/` | The Saturn-size frames, for reference only. |
| `mods/manifest.txt` | One line per frame: id, entry, frame, size, pivot. |
| `mods/shared.txt` | Frames that are byte-identical in both modes. They are not exported, so a mod never reaches the Original mode. |

The `<id>` is `<width>x<height>_<hash>`. The game computes the same id from the texture bytes at load time.
The id is the link between your file and the sprite. Keep it.

## Step 2: repaint or upscale

- Open a file from `mods/sprites/`. Repaint it, or run it through an upscaler.
- Keep the whole frame. Do not crop. Do not move the picture. Any size is fine, the frame's shape must stay.
- Keep the part after `__` in the file name. The part before `__` is free.
- Save as PNG without interlace. The game reads interlaced PNGs as an error.

## Step 3: give the game's palette back (for characters with costumes)

A true-colour PNG gets its own palette. The game's costume palettes recolour palette indices.
A true-colour sprite therefore shows the palette 0 costume in every costume. To keep costumes:

```bash
python3 -I tools/palettize.py <folder with your PNGs> mods/out
```

For every pixel, the tool takes the closest colour of the original palette, among the indices around that place in the original frame.
The output is an indexed PNG with the original palette, named with the frame id. It goes straight into the mod folder.
The tool needs the game files (`GH_MEDIA`, default `game/pkg/media`) and the original file names from step 1.

Skip this step for a sprite that has one palette only, or if you accept the costume loss.

## Step 4: put the files on the Switch

Copy your PNG files to `sdmc:/switch/gh/mods/sprites/`. The folder is flat. The game lists it once at start.
Add or remove a file, then restart the game.

## Step 5: test

1. Eden first: `powershell -File switch/eden-deko3d.ps1 -Seconds 60 -Keys "15:A,25:A,33:A,41:A,49:A,57:A,65:A"`.
   The key list reaches the Story intro scene (the tavern) in about 40 s. The Eden SD card is `E:\Switch\Eden-gh\user\sdmc\switch\gh\`.
2. Read `gh_log.txt`:
   - `sprite mod: N replacement file(s) in .../mods/sprites/`: the folder was found and N files are listed.
   - `sprite mod: <id> replaced (WxH -> WxH)`: that frame was drawn with your file.
   - `sprite mod: <error>`: a file did not load (bad PNG, interlaced, truncated).
3. To see which ids a scene uses, create the empty file `sdmc:/switch/gh/dk_dump_tex`. The log then prints `sprite id <id>` for every 8 bit texture.
   Look the ids up in `mods/manifest.txt`. Remove the marker after the test, it writes every texture to `tex/` and slows the game.
4. Then the console. Same files, same folder on the console's SD card.

A worked check with pictures: `docs/SPRITES-RESEARCH.md`, section 8, and `docs/sprite-hook-eden.png`.

## Pitfalls

- **Wrong costume colours.** The PNG was true-colour. Use step 3.
- **Nothing changes.** The id in the name does not match. Export again from your own game files; another release of the game has other bytes.
  Or the file is in a subfolder. The folder is flat.
- **Nothing changes in the Original mode.** By design. The mod is for the Remix mode only.
- **A frame is missing in the export.** It is in `shared.txt`: the same texture is used by both modes, so it is skipped.
- **Only 255 colours.** A true-colour PNG with more colours is reduced (median cut). Alpha is on or off; soft edges become hard.
- **A short pause at first use.** A big PNG is decoded and quantised when the sprite is first drawn. Not measured yet.
- **PC.** The Linux and Windows builds have no sprite hook yet. The guide is Switch only.

## Another way: patch the game file

`tools/spritemod.py export <set> <entry>` and `apply <set> <entry>` write a new `.mrg` with your frames inside.
The frame size is fixed and the colours are limited to the palette. It works on every platform, since the game loads the file.
The tool keeps the original as `<set>.mrg.orig`; `restore <set>` puts it back. Details: `tools/spritemod.py` (the docstring).
