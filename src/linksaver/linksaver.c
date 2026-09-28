// Host glue between main.c and the autopilot: config, choosing start points,
// periodic resets, and the engine hooks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"
#include "../variables.h"
#include "../zelda_rtl.h"
#include "../sprite.h"
#include "../util.h"
#include "snes/ppu.h"
#include "linksaver.h"

LinksaverConfig g_linksaver_config = {
  .enable_audio = false,
  .no_enemies = true,
  .ambient_life = true,
  .enter_buildings = false,
  .reset_minutes = 20,
  // Where Link can roam freely (see zelda3.ini for the others).
  .start_saves = { kStart_LinksHouse, kStart_Sanctuary, 2, 5, 6, 7, 9 },
  .num_start_saves = 7,
  .show_debug = false,
  .widescreen = true,
  .light_world_percent = 50,
};

bool g_linksaver_active;

enum {
  kNumReferenceSaves = 13,
  // Replay key log grows while running; trim it every few minutes.
  kClearKeyLogFrames = 60 * 60 * 3,
};

static uint32 g_rng;
static uint32 g_frames_since_start;
static uint32 g_frames_since_keylog_clear;

bool Linksaver_SuppressSprites() {
  return g_linksaver_active && g_linksaver_config.no_enemies;
}

// Overworld sprites that don't hurt Link, chase him, or start events.
// Left out on purpose: ravens (swoop at Link), the snitch ladies (call the
// guards), Kiki the monkey (walks up and asks for rupees).
static bool IsHarmlessSprite(uint8 type) {
  switch (type) {
  case 0x0B:  // cucco
  case 0x1A:  // smithy
  case 0x25:  // talking tree
  case 0x28:  // Dark World hint folk
  case 0x29:  // villagers
  case 0x2A:  // sweeping lady
  case 0x2B:  // hobo
  case 0x2C:  // lumberjacks
  case 0x2E:  // flute boy
  case 0x59:  // Lost Woods bird
  case 0x5A:  // Lost Woods squirrel
  case 0x74:  // running man
  case 0x75:  // bottle vendor
  case 0x78:  // Sahasrahla's wife
  case 0x9E:  // Haunted Grove ostrich
  case 0x9F:  // Haunted Grove rabbit
  case 0xA0:  // Haunted Grove bird
  case 0xAD:  // old man
  case 0xE3:  // fairy
    return true;
  }
  return false;
}

bool Linksaver_SuppressSpriteType(uint8 type) {
  if (!Linksaver_SuppressSprites())
    return false;
  return !(g_linksaver_config.ambient_life && IsHarmlessSprite(type));
}

bool Linksaver_BlockEntrances() {
  return g_linksaver_active && !g_linksaver_config.enter_buildings;
}

bool Linksaver_ParseConfigKey(const char *key, char *value) {
  if (StringEqualsNoCase(key, "Audio")) {
    return ParseBool(value, &g_linksaver_config.enable_audio);
  } else if (StringEqualsNoCase(key, "NoEnemies")) {
    return ParseBool(value, &g_linksaver_config.no_enemies);
  } else if (StringEqualsNoCase(key, "AmbientLife")) {
    return ParseBool(value, &g_linksaver_config.ambient_life);
  } else if (StringEqualsNoCase(key, "EnterBuildings")) {
    return ParseBool(value, &g_linksaver_config.enter_buildings);
  } else if (StringEqualsNoCase(key, "LightWorldPercent")) {
    g_linksaver_config.light_world_percent = (uint8)IntMin(IntMax(strtol(value, NULL, 10), 0), 100);
    return true;
  } else if (StringEqualsNoCase(key, "Widescreen")) {
    return ParseBool(value, &g_linksaver_config.widescreen);
  } else if (StringEqualsNoCase(key, "ShowDebug")) {
    return ParseBool(value, &g_linksaver_config.show_debug);
  } else if (StringEqualsNoCase(key, "ResetMinutes")) {
    g_linksaver_config.reset_minutes = (uint16)strtol(value, NULL, 10);
    return true;
  } else if (StringEqualsNoCase(key, "StartSaves")) {
    uint8 n = 0;
    // Chapter numbers, or "house" / "sanctuary" for the Light World spawns.
    char *item;
    while ((item = NextDelim(&value, ',')) != NULL && n < kLinksaverMaxStartSaves) {
      while (*item == ' ')
        item++;
      if (StringStartsWithNoCase(item, "house")) {
        g_linksaver_config.start_saves[n++] = kStart_LinksHouse;
      } else if (StringStartsWithNoCase(item, "sanctuary")) {
        g_linksaver_config.start_saves[n++] = kStart_Sanctuary;
      } else {
        long v = strtol(item, NULL, 10);
        if (v >= 1 && v <= kNumReferenceSaves)
          g_linksaver_config.start_saves[n++] = (uint8)v;
      }
    }
    if (n == 0)
      return false;
    g_linksaver_config.num_start_saves = n;
    return true;
  }
  return false;
}

// The save states come with their own sprites already spawned; keep only
// the ones we'd have let spawn.
static void RemoveUnwantedSprites() {
  for (int k = 0; k < 16; k++) {
    if (sprite_state[k] && Linksaver_SuppressSpriteType(sprite_type[k]))
      sprite_state[k] = 0;
  }
  for (int k = 0; k < 8; k++)
    overlord_type[k] = 0;
}

// The Light World spawns start from a later save, for a fuller inventory
// (sword, mirror, flippers), then go through the game's own file load.
static const uint8 kSpawnBaseChapters[] = { 5, 6, 7, 9 };

static void LoadRandomStart() {
  g_rng = g_rng * 1103515245 + 12345;
  int start = g_linksaver_config.start_saves[(g_rng >> 16) % g_linksaver_config.num_start_saves];
  int spawn_choice = -1;
  int chapter = start;
  if (start == kStart_LinksHouse || start == kStart_Sanctuary) {
    spawn_choice = start == kStart_LinksHouse ? 0 : 1;
    g_rng = g_rng * 1103515245 + 12345;
    chapter = kSpawnBaseChapters[(g_rng >> 16) % countof(kSpawnBaseChapters)];
  }

  if (g_autopilot_verbose)
    printf("--- loading start point: %s (chapter %d)\n",
           spawn_choice == 0 ? "Link's House" : spawn_choice == 1 ? "Sanctuary" : "save", chapter);
  ZeldaApuLock();
  SaveLoadSlot(kSaveLoad_Load, 256 + chapter - 1);
  ZeldaApuUnlock();
  // Some reference saves resume a recorded replay; we want our own input.
  PatchCommand('l');
  if (g_linksaver_config.no_enemies)
    RemoveUnwantedSprites();

  Autopilot_Reset(g_rng ^ 0x9e3779b9);
  if (spawn_choice >= 0) {
    // Reload the save the way the game does after Agahnim: in the Light
    // World, from the spawn menu (death_var4 tells the loader to use the
    // chosen spawn point rather than the last entrance).
    savegame_is_darkworld = 0;
    death_var4 = 1;
    main_module_index = 5;
    submodule_index = 0;
    Autopilot_RequestSpawn(spawn_choice);
    g_frames_since_start = 0;
    return;
  }
  // Balance the worlds by having some Dark World starts mirror across.
  g_rng = g_rng * 1103515245 + 12345;
  if (savegame_is_darkworld && link_item_mirror >= 2 &&
      (int)((g_rng >> 16) % 100) < g_linksaver_config.light_world_percent)
    Autopilot_RequestLightWorld();
  g_frames_since_start = 0;
}

void Linksaver_Start(uint32 seed) {
  g_linksaver_active = true;
  g_rng = seed;
  LoadRandomStart();
}

uint16 Linksaver_RunFrame() {
  g_frames_since_start++;
  bool timed_out = g_linksaver_config.reset_minutes != 0 &&
                   g_frames_since_start > (uint32)g_linksaver_config.reset_minutes * 60 * 60;
  // Only cut away at a calm moment: on the overworld, not mid-transition.
  bool calm = main_module_index == 9 && submodule_index == 0;
  if (Autopilot_WantsReset() || (timed_out && calm))
    LoadRandomStart();

  if (++g_frames_since_keylog_clear >= kClearKeyLogFrames) {
    g_frames_since_keylog_clear = 0;
    PatchCommand('k');
  }
  return Autopilot_RunFrame();
}

void Linksaver_DrawDebugOverlay(uint8 *pixels, int pitch, int width, int height, int render_scale) {
  if (!g_linksaver_active || !g_linksaver_config.show_debug)
    return;
  int extra = g_zenv.ppu->extraLeftRight;
  Autopilot_DrawDebug(pixels, pitch, width, height, render_scale,
                      BG2HOFS_copy2 - extra, BG2VOFS_copy2);
}
