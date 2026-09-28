# Linksaver

A Windows screensaver where Link wanders Hyrule on his own. It's a fork of
[zelda3](https://github.com/snesrev/zelda3), so the maps, collision, movement,
and scrolling are the game's own code. An autopilot presses the D-pad; it never
moves Link directly.

## Setup

1. Put your US ROM at `zelda3.sfc` in this folder, then extract the assets:
   `python -m pip install -r requirements.txt` and `extract_assets.bat`.
   This creates `zelda3_assets.dat`; the ROM isn't needed after that.
2. Build: `build_linksaver.bat` (TCC and SDL2 are expected in `third_party/`,
   see README.md). This produces `linksaver.exe` and `linksaver.scr`.
3. Try it in a window: `linksaver.exe --linksaver`
4. Install: right-click `linksaver.scr` → **Install**. It stays in this folder
   and reads `zelda3_assets.dat`, `zelda3.ini`, and `saves/ref/` from here.

`linksaver.exe` with no arguments is still the normal, playable game.

### Single-file build

`build_linksaver.bat` also produces `dist\Linksaver.scr` (about 12 MB): a small
launcher (`src/launcher/launcher.c`) with the exe, SDL2, assets, reference saves
and default settings appended by `tools/pack_linksaver.py`. Copy it anywhere and
right-click → **Install**. On first run it unpacks to `%LOCALAPPDATA%\Linksaver\<id>\`;
settings live in `%APPDATA%\Linksaver\linksaver.ini`, which the screensaver
settings button opens.

This file contains data extracted from your ROM, so keep it for your own
machines and don't publish it.

## Settings

The `[Linksaver]` section of `zelda3.ini`: start points, audio, reset interval,
enemies on or off, and `ShowDebug`, which draws the autopilot's collision map
and planned route over the game.

## How it works

| Piece | File |
|---|---|
| Screensaver shell (`/s`, `/p <hwnd>`, `/c`, multi-monitor blanking, exit on input) | `src/linksaver/screensaver.c` |
| Start points, periodic resets, config, debug overlay | `src/linksaver/linksaver.c` |
| Autopilot: collision grid, BFS pathing, steering, area exits, stuck recovery | `src/linksaver/autopilot.c` |
| Engine hooks: no sprite spawns, no entering buildings | `src/sprite.c`, `src/overworld.c` |

The autopilot reads tile attributes through `Overworld_GetTileAttributeAtLocation`,
the same lookup Link's collision uses, and treats a position as walkable when
Link's 16×16 collision box (`x+0..15`, `y+8..23`) touches only walkable tiles.
It alternates between wandering to far points in the current area and walking
out through an edge opening into the next one. Ledges are one-way jump edges:
`28` jumps north, `29` south, `2a` west, `2b` east (verified against the engine).
Steering leaves small offsets to the game's own corner nudging.

Start points are the reference save states in `saves/ref/` (chapters 2, 5, 6,
7 and 9 by default: the ones where Link can roam freely).

## Tests

- `third_party\tcc\tcc.exe -I. -Isrc -run tests/autopilot_sim.c` runs the autopilot
  against a synthetic map with a simple movement model; no ROM needed.
- `linksaver.exe --linksaver-test <frames> <screenshot every N frames> [chapter]`
  runs the real game plus autopilot headless at full speed. It logs area changes,
  jumps, stuck events and resets, and writes debug screenshots and per-area tile
  attribute dumps to `linksaver_test/`.
- `LINKSAVER_PROBE=x,y,buttons:frames,...` (with `--linksaver-test 1 0 <chapter>`)
  places Link, plays a hex input sequence, and logs his position, state and
  surrounding tile attributes each frame. This is how the ledge rules were verified.

## Not done yet

- Lifting rocks and bushes: pressing A starts the game's lift state, but the
  object is never picked up. The Desert Palace start (chapter 3) needs this.
- Swimming and diagonal ledges (`2c`–`2f`).
- Interiors: entering buildings is blocked, and if Link ends up indoors the
  autopilot reloads a start point.
- No harmless ambient sprites (birds, NPCs); `NoEnemies` removes all sprites.
