// Linksaver: a Windows screensaver built on top of the zelda3 reimplementation.
// The game engine runs unmodified except for a couple of small hooks; Link is
// driven by an autopilot that produces joypad input every frame, so movement,
// collision, and scrolling all go through the original game logic.
#ifndef ZELDA3_LINKSAVER_LINKSAVER_H_
#define ZELDA3_LINKSAVER_LINKSAVER_H_

#include "../types.h"
#include "screensaver.h"

enum {
  kLinksaverMaxStartSaves = 16,
  // Start points beyond the numbered reference saves: the game's own
  // Light World spawn points, chosen from its spawn menu.
  kStart_LinksHouse = 101,
  kStart_Sanctuary = 102,
};

typedef struct LinksaverConfig {
  bool enable_audio;
  bool no_enemies;
  // With no_enemies: still show harmless animals and townsfolk
  bool ambient_life;
  bool enter_buildings;
  // Reload a random start point after this many minutes (0 = never).
  uint16 reset_minutes;
  // Reference saves (1-based chapter numbers from saves/ref) or kStart_*
  // spawn points to start from.
  uint8 start_saves[kLinksaverMaxStartSaves];
  uint8 num_start_saves;
  bool show_debug;
  // Show more of the world to fill wide monitors
  bool widescreen;
  // Percent of Dark World starts that mirror over to the Light World
  uint8 light_world_percent;
} LinksaverConfig;

extern LinksaverConfig g_linksaver_config;

// True while the autopilot owns the game. The engine hooks below only apply then.
extern bool g_linksaver_active;

// Engine hooks, checked from sprite.c and overworld.c.
bool Linksaver_SuppressSprites();
// Like Linksaver_SuppressSprites, but lets harmless ambient life through
// when that's enabled.
bool Linksaver_SuppressSpriteType(uint8 type);
bool Linksaver_BlockEntrances();

// Called from config.c for keys in the [Linksaver] section.
bool Linksaver_ParseConfigKey(const char *key, char *value);

// Host glue (linksaver.c)
void Linksaver_Start(uint32 seed);
uint16 Linksaver_RunFrame();

// Draws the autopilot's view of collision and its current path over the frame.
void Linksaver_DrawDebugOverlay(uint8 *pixels, int pitch, int width, int height, int render_scale);

// Autopilot (autopilot.c)
extern bool g_autopilot_allow_hazard_jumps;  // testing only
extern int g_autopilot_verbose;  // 1 = events, 2 = per-second trace
void Autopilot_Reset(uint32 seed);
// Makes the autopilot mirror to the Light World from a Dark World start.
void Autopilot_RequestLightWorld();
// Answers the game's spawn menu with this choice (0 = Link's House,
// 1 = Sanctuary), then walks Link out of the building.
void Autopilot_RequestSpawn(int menu_choice);
uint16 Autopilot_RunFrame();
bool Autopilot_WantsReset();
void Autopilot_DrawDebug(uint8 *pixels, int pitch, int width, int height, int scale, int screen_x, int screen_y);

#endif  // ZELDA3_LINKSAVER_LINKSAVER_H_
