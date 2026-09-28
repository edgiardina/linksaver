# Linksaver

A Windows screensaver where Link explores Hyrule on his own.

It runs the real *A Link to the Past* game logic, so the maps, collision,
movement and scrolling are the game's own. An autopilot plays the controller,
picking places to go, walking there, hopping down ledges, swimming across lakes,
crossing into new areas and now and then using the Magic Mirror to slip between
the Dark World and the Light World. There are no enemies by default, just Link,
the scenery and some harmless locals: cuccos, birds, villagers.

You need your own copy of the US ROM. No Nintendo data is included in this
repository.

## Getting started

### 1. Prerequisites

- **Python 3** with Pillow and PyYAML: `python -m pip install -r requirements.txt`
- **TCC**: download [tcc_20221020.zip](https://github.com/FitzRoyX/tinycc/releases/download/tcc_20221020/tcc_20221020.zip) and extract it so that `third_party\tcc\tcc.exe` exists
- **SDL2**: download [SDL2-devel-2.26.3-VC.zip](https://github.com/libsdl-org/SDL/releases/download/release-2.26.3/SDL2-devel-2.26.3-VC.zip) and extract it into `third_party\` (giving `third_party\SDL2-2.26.3\`)

### 2. Extract the game data

Put your US ROM in the project folder as `zelda3.sfc` and run `extract_assets.bat`.
This creates `zelda3_assets.dat`; the ROM isn't needed after that.

### 3. Build

Run `build_linksaver.bat`. It produces:

| File | What it is |
|---|---|
| `dist\Linksaver.scr` | The screensaver as a single file (about 12 MB). This is the one to install. |
| `linksaver.exe` | The same program unpacked, for development. With no arguments it's the normal, playable game. |

### 4. Install

Copy `dist\Linksaver.scr` somewhere permanent, right-click it and choose
**Install**. It appears in Windows' Screen Saver Settings, where **Settings…**
opens its options.

On first run it unpacks itself to `%LOCALAPPDATA%\Linksaver\` and keeps your
settings in `%APPDATA%\Linksaver\linksaver.ini`.

`dist\Linksaver.scr` contains data extracted from your ROM. Keep it on your own
machines and don't publish it.

To watch without installing: `linksaver.exe --linksaver` runs it in a window.

## Settings

The Settings dialog covers:

- **Start points:** where Link starts: Link's House and the Sanctuary (the Light World spawn points from the game's reload menu, after which he walks out the door), outside Eastern Palace, or one of the Dark World save points
- **Light World:** whether he mirrors over to the Light World
- **Wide screens:** whether to show more of the world on wide monitors (up to about 448×240 game pixels)
- **Sound:** music and sound on or off
- **Characters:** harmless animals and townsfolk, and enemies, each on or off
- **Debug overlay:** draws the autopilot's collision map and planned route
- **New start point:** how often Link jumps to a different start point

**Advanced…** opens the ini file itself, including zelda3's own options.

## How it works

The engine is zelda3's reimplementation of the game in C. Linksaver adds an
autopilot that produces controller input each frame, plus a few small hooks in
the game code.

**Seeing the world.** The autopilot reads each tile's collision attribute through
the same function Link's collision uses. A spot counts as walkable when Link's
whole 16×16 collision box (`x+0..15`, `y+8..23`) would stand on walkable tiles.

**Getting around:**
- **Routes:** a breadth-first search over those spots, 8 pixels apart.
- **Ledges:** one-way jumps. Attribute `28` jumps north, `29` south, `2a` west and `2b` east; each direction was verified by probing the engine.
- **Water:** with the Flippers, Link swims through it. Without them, he never jumps in, and if he ends up in water anyway he heads for the nearest shore.
- **Steering:** Link isn't forced onto exact pixels. The game nudges him around corners and onto stairs itself, and fighting that makes him stall.

**Choosing where to go:**
- **Exploring:** in each new area Link wanders at least once before leaving.
- **Exits:** he prefers edges leading to screens he's visited least, and only turns back the way he came when there's no other way out.

**Recovering when stuck:**
- **Stuck:** a short random nudge, then a new plan.
- **Invisible walls:** getting stuck twice in the same spot marks the tiles ahead as blocked.
- **Dead ends:** long stretches without progress, or a small pocket with no way out, send Link to a new start point.

**Game hooks:**
- **Characters:** when enemies are off, only whitelisted harmless sprite types can spawn.
- **Doors:** entrances are blocked, so Link stays outdoors.

| Piece | Where |
|---|---|
| Autopilot | `src/linksaver/autopilot.c` |
| Start points, resets, settings, sprite whitelist, debug overlay | `src/linksaver/linksaver.c` |
| Screensaver modes (`/s`, `/p`, `/c`), DPI awareness, multi-monitor | `src/linksaver/screensaver.c` |
| Single-file launcher and Settings dialog | `src/launcher/` |
| Packer | `tools/pack_linksaver.py` |
| Engine hooks | `src/sprite.c`, `src/overworld.c`, `src/main.c`, `src/config.c` |

## Testing

- **Offline simulation**, no ROM needed: `third_party\tcc\tcc.exe -I. -Isrc -run tests/autopilot_sim.c` runs the autopilot on a synthetic map with a simple movement model.
- **Headless runs:** `linksaver.exe --linksaver-test <frames> <screenshot every N frames> [start point]` runs the real game with the autopilot at full speed. It logs area changes, jumps, stuck events and resets, and writes screenshots and per-area tile dumps to `linksaver_test/`. `LINKSAVER_SEED=n` makes a run repeatable.
- **Engine probes:** `LINKSAVER_PROBE=x,y,buttons:frames,...` (with `--linksaver-test 1 0 <start point>`) places Link, plays a sequence of inputs, and logs his position, state and the surrounding tiles every frame.

## Not done yet

- **Lifting:** Link can't lift rocks or bushes yet (the Desert Palace start needs it).
- **Diagonal ledges:** not taken yet.
- **Interiors:** Link stays outdoors.
- **Distribution:** there's no ROM-free release that extracts assets on first run.

## Built on zelda3

Linksaver is a fork of [**zelda3**](https://github.com/snesrev/zelda3) by snesrev
and contributors: a reverse-engineered C reimplementation of *A Link to the Past*,
with graphics and audio from [LakeSnes](https://github.com/elzo-d/LakeSnes).
Everything that makes the game *the game* comes from there. For the engine
itself, playing it, other platforms and its many options, see the zelda3
repository.

MIT licensed, like zelda3 (see `LICENSE.txt`).
