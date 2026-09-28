// Offline test for the autopilot planner. Runs autopilot.c against a synthetic
// overworld area and a simplified movement model (1-2px per axis per frame,
// blocked by the same collision box the real game uses), so path planning,
// steering, exits, and stuck handling can be checked without the ROM.
//
// Build & run (from repo root):
//   third_party\tcc\tcc.exe -I. -Isrc -run tests/autopilot_sim.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/types.h"
uint8 g_ram[0x20000];
#include "src/variables.h"

enum { kTiles = 64, kAreaX = 512, kAreaY = 512 };

// '#' wall, '.' ground, '~' deep water. One char per 8x8 tile.
static char g_map[kTiles][kTiles + 1];

static void BuildMap() {
  for (int r = 0; r < kTiles; r++) {
    for (int c = 0; c < kTiles; c++) {
      bool border = r < 2 || c < 2 || r >= kTiles - 2 || c >= kTiles - 2;
      g_map[r][c] = border ? '#' : '.';
    }
    g_map[r][kTiles] = 0;
  }
  // North opening, 6 tiles wide
  for (int r = 0; r < 2; r++)
    for (int c = 30; c < 36; c++)
      g_map[r][c] = '.';
  // East opening, 6 tiles tall
  for (int r = 20; r < 26; r++)
    for (int c = kTiles - 2; c < kTiles; c++)
      g_map[r][c] = '.';
  // A horizontal wall across the middle with a 4-tile gap
  for (int c = 2; c < kTiles - 2; c++)
    if (c < 40 || c >= 44)
      g_map[32][c] = g_map[33][c] = '#';
  // A pond
  for (int r = 40; r < 50; r++)
    for (int c = 10; c < 22; c++)
      g_map[r][c] = '~';
  // Scattered rocks
  for (int i = 0; i < 25; i++) {
    int r = 4 + (i * 7) % 54, c = 4 + (i * 13) % 54;
    if (r != 32 && r != 33)
      g_map[r][c] = '#';
  }
}

static char MapAt(int wx, int wy) {
  int c = (wx - kAreaX) >> 3, r = (wy - kAreaY) >> 3;
  if (c < 0 || r < 0 || c >= kTiles || r >= kTiles)
    return '.';  // outside the area: the neighbouring area
  return g_map[r][c];
}

// Stub for the engine function: x is in 8px tiles, y in pixels.
uint8 Overworld_GetTileAttributeAtLocation(uint16 x, uint16 y) {
  char ch = MapAt(x * 8, y);
  return ch == '#' ? 0x01 : ch == '~' ? 0x08 : 0x00;
}

#include "src/linksaver/autopilot.c"

static bool BodyClear(int x, int y) {
  // Link's collision box: x+0..x+15, y+8..y+23
  for (int yy = y + 8; yy <= y + 23; yy += 1)
    for (int xx = x; xx <= x + 15; xx += 1)
      if (MapAt(xx, yy) != '.')
        return false;
  return true;
}

// Approximates the engine's corner assist: when a straight move is blocked
// but would be clear with the other axis shifted by up to 4px, Link slides
// 1px towards that opening. (Verified against the real engine on stairs.)
static int CornerNudge(int x, int y, bool shift_x) {
  for (int d = 1; d <= 4; d++) {
    for (int sign = -1; sign <= 1; sign += 2) {
      int nx = shift_x ? x + sign * d : x, ny = shift_x ? y : y + sign * d;
      if (BodyClear(nx, ny))
        return sign;
    }
  }
  return 0;
}

int main() {
  BuildMap();
  main_module_index = 9;
  submodule_index = 0;
  overworld_area_index = 0x09;
  overworld_offset_base_x = kAreaX >> 3;
  overworld_offset_base_y = kAreaY;
  overworld_offset_mask_y = 0x1f0;
  link_x_coord = kAreaX + 100;
  link_y_coord = kAreaY + 120;

  Autopilot_Reset(1234);
  int exits[4] = { 0 }, stuck_events = 0, frames_moving = 0;
  int last_stuck = 0;
  const int kFrames = 60 * 60 * 20;  // 20 minutes
  for (int f = 0; f < kFrames; f++) {
    uint16 b = Autopilot_RunFrame();
    if (Autopilot_WantsReset()) {
      printf("FAIL: autopilot requested reset at frame %d (Link at %d,%d)\n",
             f, link_x_coord - kAreaX, link_y_coord - kAreaY);
      return 1;
    }
    int dx = (b & kBtnRight) ? 1 : (b & kBtnLeft) ? -1 : 0;
    int dy = (b & kBtnDown) ? 1 : (b & kBtnUp) ? -1 : 0;
    if (dx || dy)
      frames_moving++;
    // Link's walking speed works out to 1 or 2 pixels per frame.
    int steps = (f % 2) ? 2 : 1;
    for (int i = 0; i < steps; i++) {
      if (dx) {
        if (BodyClear(link_x_coord + dx, link_y_coord))
          link_x_coord += dx;
        else if (!dy)
          link_y_coord += CornerNudge(link_x_coord + dx, link_y_coord, false);
      }
      if (dy) {
        if (BodyClear(link_x_coord, link_y_coord + dy))
          link_y_coord += dy;
        else if (!dx)
          link_x_coord += CornerNudge(link_x_coord, link_y_coord + dy, true);
      }
    }

    // Leaving the area: count it, then re-enter from the opposite side like
    // the game would after a transition (reusing the same map).
    int side = -1;
    if (link_y_coord < kAreaY - 8) side = kSide_North;
    else if (link_y_coord > kAreaY + kTiles * 8 - 16) side = kSide_South;
    else if (link_x_coord < kAreaX - 8) side = kSide_West;
    else if (link_x_coord > kAreaX + kTiles * 8 - 8) side = kSide_East;
    if (side >= 0) {
      exits[side]++;
      overworld_area_index ^= 1;  // looks like a new area to the autopilot
      // Arrive somewhere open, on the far side of the middle wall from the exit used.
      link_x_coord = kAreaX + 30 * 8;
      link_y_coord = kAreaY + (side == kSide_North ? 55 : 22) * 8;
    }
    if (ap.stuck_count > last_stuck)
      stuck_events++;
    last_stuck = ap.stuck_count;
  }
  printf("frames=%d moving=%d%% exits N=%d S=%d W=%d E=%d stuck_events=%d\n",
         kFrames, frames_moving * 100 / kFrames, exits[0], exits[1], exits[2], exits[3], stuck_events);
  if (exits[kSide_South] || exits[kSide_West]) {
    printf("FAIL: left through a wall\n");
    return 1;
  }
  if (exits[kSide_North] == 0 || exits[kSide_East] == 0) {
    printf("FAIL: never used one of the openings\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
