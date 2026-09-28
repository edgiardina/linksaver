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
  .enter_buildings = false,
  .reset_minutes = 20,
  // Chapters where Link can roam freely (see zelda3.ini for the others).
  .start_saves = { 2, 5, 6, 7, 9 },
  .num_start_saves = 5,
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

bool Linksaver_BlockEntrances() {
  return g_linksaver_active && !g_linksaver_config.enter_buildings;
}

bool Linksaver_ParseConfigKey(const char *key, char *value) {
  if (StringEqualsNoCase(key, "Audio")) {
    return ParseBool(value, &g_linksaver_config.enable_audio);
  } else if (StringEqualsNoCase(key, "NoEnemies")) {
    return ParseBool(value, &g_linksaver_config.no_enemies);
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
    for (char *s = value; *s && n < kLinksaverMaxStartSaves;) {
      char *end;
      long v = strtol(s, &end, 10);
      if (end == s)
        return false;
      if (v >= 1 && v <= kNumReferenceSaves)
        g_linksaver_config.start_saves[n++] = (uint8)v;
      s = end;
      while (*s == ',' || *s == ' ')
        s++;
    }
    if (n == 0)
      return false;
    g_linksaver_config.num_start_saves = n;
    return true;
  }
  return false;
}

static void LoadRandomStart() {
  g_rng = g_rng * 1103515245 + 12345;
  int chapter = g_linksaver_config.start_saves[(g_rng >> 16) % g_linksaver_config.num_start_saves];

  if (g_autopilot_verbose)
    printf("--- loading start point: chapter %d\n", chapter);
  ZeldaApuLock();
  SaveLoadSlot(kSaveLoad_Load, 256 + chapter - 1);
  ZeldaApuUnlock();
  // Some reference saves resume a recorded replay; we want our own input.
  PatchCommand('l');
  if (g_linksaver_config.no_enemies)
    Sprite_DisableAll();

  Autopilot_Reset(g_rng ^ 0x9e3779b9);
  // Only one Light World start point roams well, so balance the worlds by
  // having some Dark World starts mirror across.
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
