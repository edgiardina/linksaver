// Autopilot: explores the overworld by producing joypad input each frame.
//
// It never moves Link directly. It reads the same tile attribute data the
// engine uses for collision (Overworld_GetTileAttributeAtLocation), plans a
// path over Link-sized positions on an 8px grid, and presses the D-pad to
// follow it. The game's own movement code does the rest, including sliding
// around corners and scrolling between areas.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"
#include "../variables.h"
#include "../zelda_rtl.h"
#include "../tile_detect.h"
#include "../hud.h"
#include "linksaver.h"

enum {
  kBtnB = 1 << 0,
  kBtnY = 1 << 1,
  kBtnUp = 1 << 4,
  kBtnDown = 1 << 5,
  kBtnLeft = 1 << 6,
  kBtnRight = 1 << 7,
};

enum {
  kMaxTiles = 128,  // A large area is 1024px = 128 tiles of 8px
  kNodeStride = kMaxTiles,
  kNumNodes = kMaxTiles * kMaxTiles,
  kMaxWaypoints = 512,
};

enum {
  // How far off the path on the cross axis Link can be before we correct.
  // The engine's corner nudging covers at least this much.
  kCornerAssistSlop = 3,
  kMajorSlop = 1,
  kArriveDist = 2,
  kStuckFrames = 45,
};

enum {
  kSide_None = -1,
  kSide_North,
  kSide_South,
  kSide_West,
  kSide_East,
};

static const uint16 kSideButtons[4] = { kBtnUp, kBtnDown, kBtnLeft, kBtnRight };
static const int8 kOppositeSide[4] = { kSide_South, kSide_North, kSide_East, kSide_West };

typedef enum Phase {
  kPhase_Plan,
  kPhase_Walk,
  kPhase_Exit,
  kPhase_Idle,
} Phase;

int g_autopilot_verbose;
bool g_autopilot_allow_hazard_jumps;
#define LOG(...) do { if (g_autopilot_verbose) { printf("[%6u] ", ap.frame); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct {
  uint32 rng;
  uint32 frame;
  Phase phase;
  int timer;

  // Area the grid was built for
  uint16 area;
  int ax0, ay0, tiles;
  bool grid_valid;
  uint8 tile_attr[kMaxTiles][kMaxTiles];  // [row][col], raw 8x8 tile attribute
  uint8 tile_ok[kMaxTiles][kMaxTiles];    // [row][col], 8x8 tile walkable
  uint8 node_ok[kNumNodes];             // Link standing at node's top-left is clear
  int16 dist[kNumNodes];
  int16 parent[kNumNodes];
  int16 queue[kNumNodes];  // BFS visit order, closest first
  int queue_len;

  // Current route, as world pixel coordinates of Link's position
  int wp_x[kMaxWaypoints], wp_y[kMaxWaypoints];
  int wp_count, wp_pos;
  int goal_side;  // kSide_None for a wander goal
  uint16 nudge_buttons;  // held during kPhase_Idle to shake loose when stuck

  int came_from_side;
  int wanders_in_area;
  uint8 failed_sides;  // bitmask of sides that didn't work in this area

  // Stuck detection: frames without getting closer to the current waypoint
  int best_dist;
  int frames_no_closer;
  uint16 prev_area;  // area before the current one, to spot back-and-forth
  int stuck_count;
  int frames_since_progress;
  int frames_off_overworld;
  bool want_reset;

  // World position of the current goal, so we can re-plan to it after a jump.
  int goal_x, goal_y;
  // Getting to the Light World from a Dark World start, via the Magic Mirror.
  bool want_light_world;
  bool try_mirror;     // at a stop; try the mirror before moving on
  int mirror_tries;
  int mirror_press;    // frames left holding Y
  int frames_in_light_world;
  int saved_item;      // item equipped before we switched to the mirror (-1: none)

  bool airborne;  // Link was in a non-steerable state (e.g. jumping) last frame
  bool swimming;  // grid was built with deep water passable
  uint8 last_handler;
} ap;

static uint32 Rand() {
  // xorshift32
  uint32 x = ap.rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return ap.rng = x;
}

static int RandRange(int lo, int hi) {  // inclusive
  return lo + (int)(Rand() % (uint32)(hi - lo + 1));
}

// Tile attributes Link can stand on outdoors. Mirrors the cases in
// TileDetect_ExecuteInner that don't block or hurt Link, minus anything that
// triggers an event (warps, stairs into interiors, ledges which are one-way).
static bool TileAttrWalkable(uint8 t) {
  switch (t) {
  // TileBehavior_NothingOW
  case 0x00: case 0x05: case 0x06: case 0x07: case 0x14: case 0x15: case 0x16: case 0x17:
  case 0x21: case 0x23: case 0x24: case 0x25: case 0x38: case 0x39: case 0x3a: case 0x3b:
  case 0x3c: case 0x41: case 0x45: case 0x47: case 0x49: case 0x5e: case 0x5f: case 0x61:
  case 0x62: case 0x64: case 0x65: case 0x66: case 0xa6: case 0xa7: case 0xbe: case 0xbf:
    return true;
  case 0x04:  // thick grass (outdoors)
  case 0x09:  // shallow water
  case 0x0a:  // short water ladder
  case 0x22:  // outdoor stairs
  case 0x40:  // thick grass
  case 0x48: case 0x4a:  // diggable ground
  case 0x60:  // rupee tile (outdoors: normal)
  case 0x6c: case 0x6d: case 0x6e: case 0x6f:  // normal outdoors
    return true;
  default:
    return t >= 0xd0 && t <= 0xef;
  }
}

// Tiles that swallow Link: deep water (he swims if he has flippers, otherwise
// he's pulled out) and pits.
static bool TileAttrHazard(uint8 t) {
  return t == 0x08 || t == 0x0b || t == 0x20 || (t >= 0xb0 && t <= 0xbd);
}

enum {
  kHandler_Ground = 0,
  kHandler_Swimming = 4,
  kHandler_OnIce = 5,
  kHandler_Bunny = 23,
};

// States where the D-pad walks (or swims) Link around, so we should steer.
static bool LinkIsSteerable(uint8 handler) {
  return handler == kHandler_Ground || handler == kHandler_Swimming ||
         handler == kHandler_OnIce || handler == kHandler_Bunny;
}

static inline int NodeIndex(int cx, int cy) {
  return cy * kNodeStride + cx;
}

// Nodes are Link positions aligned to 8px. Link's collision box covers
// x+0..x+15 and y+8..y+23 (see kDetectTiles_tab* in tile_detect.c), so a
// node needs the 2x2 tiles at columns cx..cx+1 and rows cy+1..cy+2.
static inline int NodesWide() { return ap.tiles - 1; }
static inline int NodesHigh() { return ap.tiles - 2; }

static void BuildGrid(bool water_ok) {
  // Use the same origin/size the engine uses for its tile lookups, which also
  // covers special areas like the Master Sword grove.
  ap.area = overworld_area_index;
  ap.ax0 = overworld_offset_base_x << 3;
  ap.ay0 = overworld_offset_base_y;
  ap.tiles = (overworld_offset_mask_y + 16) >> 3;
  if (ap.tiles > kMaxTiles)
    ap.tiles = kMaxTiles;

  // While swimming, deep water is passable; Plan() then heads for the shore.
  ap.swimming = water_ok || link_player_handler_state == kHandler_Swimming;
  int tx0 = ap.ax0 >> 3;
  for (int r = 0; r < ap.tiles; r++) {
    for (int c = 0; c < ap.tiles; c++) {
      uint8 attr = Overworld_GetTileAttributeAtLocation(tx0 + c, ap.ay0 + r * 8);
      ap.tile_attr[r][c] = attr;
      ap.tile_ok[r][c] = TileAttrWalkable(attr) || (ap.swimming && (attr == 0x08 || attr == 0x0b));
    }
  }
  if (g_autopilot_verbose) {
    // Dump the raw attributes once per area so collision can be inspected offline.
    static uint8 dumped[256];
    if (!dumped[ap.area & 0xff]) {
      dumped[ap.area & 0xff] = 1;
      char name[64];
      snprintf(name, sizeof(name), "linksaver_test/area_%02x.txt", ap.area & 0xff);
      FILE *f = fopen(name, "w");
      if (f) {
        fprintf(f, "area %02x origin %d,%d tiles %d\n", ap.area, ap.ax0, ap.ay0, ap.tiles);
        for (int r = 0; r < ap.tiles; r++) {
          for (int c = 0; c < ap.tiles; c++)
            fprintf(f, "%02x", ap.tile_attr[r][c]);
          fprintf(f, "\n");
        }
        fclose(f);
      }
    }
  }
  memset(ap.node_ok, 0, sizeof(ap.node_ok));
  for (int cy = 0; cy < NodesHigh(); cy++) {
    for (int cx = 0; cx < NodesWide(); cx++) {
      ap.node_ok[NodeIndex(cx, cy)] =
          ap.tile_ok[cy + 1][cx] && ap.tile_ok[cy + 1][cx + 1] &&
          ap.tile_ok[cy + 2][cx] && ap.tile_ok[cy + 2][cx + 1];
    }
  }
  ap.grid_valid = true;
}

// Link's current node, or the nearest walkable one if he's partially
// overlapping something (the game lets him stand closer to walls than 8px).
static int FindStartNode() {
  int cx = (link_x_coord - ap.ax0 + 4) >> 3;
  int cy = (link_y_coord - ap.ay0 + 4) >> 3;
  for (int radius = 0; radius <= 3; radius++) {
    for (int dy = -radius; dy <= radius; dy++) {
      for (int dx = -radius; dx <= radius; dx++) {
        if (abs(dx) != radius && abs(dy) != radius)
          continue;
        int x = cx + dx, y = cy + dy;
        if (x >= 0 && y >= 0 && x < NodesWide() && y < NodesHigh() && ap.node_ok[NodeIndex(x, y)])
          return NodeIndex(x, y);
      }
    }
  }
  return -1;
}

// Ledges are one-way: a ledge tile sits on the high side of a cliff, and
// walking onto it towards the cliff makes Link jump over. Verified against the
// engine: 0x28 jumps north, 0x29 south, 0x2a west, 0x2b east. (Diagonal ledges
// 0x2c-0x2f aren't used yet.)
static const uint8 kLedgeAttr[4] = { 0x28, 0x29, 0x2a, 0x2b };  // by kSide_*
static const int8 kSideDx[4] = { 0, 0, -1, 1 };
static const int8 kSideDy[4] = { -1, 1, 0, 0 };

// If Link standing at node (x, y) can jump a ledge towards |side|, returns the
// node where he'll roughly land, else -1. Side jumps also drop him a little,
// so the landing search allows for that. The exact spot doesn't matter much
// since we re-plan from wherever he actually lands.
static int JumpLanding(int x, int y, int side) {
  uint8 a = kLedgeAttr[side];
  bool takeoff;
  switch (side) {
  case kSide_North: takeoff = ap.tile_attr[y][x] == a && ap.tile_attr[y][x + 1] == a; break;
  case kSide_South: takeoff = y + 3 < ap.tiles && ap.tile_attr[y + 3][x] == a && ap.tile_attr[y + 3][x + 1] == a; break;
  case kSide_West: takeoff = x >= 1 && ap.tile_attr[y + 1][x - 1] == a && ap.tile_attr[y + 2][x - 1] == a; break;
  default: takeoff = x + 2 < ap.tiles && ap.tile_attr[y + 1][x + 2] == a && ap.tile_attr[y + 2][x + 2] == a; break;
  }
  if (!takeoff)
    return -1;
  int w = NodesWide(), h = NodesHigh();
  for (int k = 2; k < 48; k++) {
    for (int drop = 0; drop <= (kSideDx[side] ? 4 : 0); drop++) {
      int nx = x + kSideDx[side] * k, ny = y + kSideDy[side] * k + drop;
      if (nx < 0 || ny < 0 || nx >= w || ny >= h)
        return -1;
      if (ap.node_ok[NodeIndex(nx, ny)])
        return NodeIndex(nx, ny);
      // The game drops Link at the first non-cliff spot; if that's water or
      // a pit, don't take this jump. (Tests can allow it, to exercise the
      // swim-to-shore recovery.)
      if (!g_autopilot_allow_hazard_jumps && TileAttrHazard(ap.tile_attr[ny + 1][nx]) || TileAttrHazard(ap.tile_attr[ny + 1][nx + 1]) ||
          TileAttrHazard(ap.tile_attr[ny + 2][nx]) || TileAttrHazard(ap.tile_attr[ny + 2][nx + 1]))
        return -1;
    }
  }
  return -1;
}

// Breadth-first search over 8-connected nodes. Link moves at the same speed
// per axis when walking diagonally, so a diagonal step costs the same as a
// straight one. Diagonals require both orthogonal neighbours to be clear.
static void RunBfs(int start) {
  static const int8 kDx[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
  static const int8 kDy[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
  int w = NodesWide(), h = NodesHigh();
  for (int i = 0; i < kNumNodes; i++)
    ap.dist[i] = -1;
  int head = 0, tail = 0;
  ap.dist[start] = 0;
  ap.parent[start] = -1;
  ap.queue[tail++] = start;
  while (head < tail) {
    int n = ap.queue[head++];
    int x = n % kNodeStride, y = n / kNodeStride;
    for (int d = 0; d < 8; d++) {
      int nx = x + kDx[d], ny = y + kDy[d];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h)
        continue;
      int m = NodeIndex(nx, ny);
      if (!ap.node_ok[m] || ap.dist[m] >= 0)
        continue;
      if (d >= 4 && (!ap.node_ok[NodeIndex(nx, y)] || !ap.node_ok[NodeIndex(x, ny)]))
        continue;
      ap.dist[m] = ap.dist[n] + 1;
      ap.parent[m] = n;
      ap.queue[tail++] = m;
    }
    for (int side = 0; side < 4; side++) {
      int m = JumpLanding(x, y, side);
      if (m < 0 || ap.dist[m] >= 0)
        continue;
      ap.dist[m] = ap.dist[n] + 1;
      ap.parent[m] = n;
      ap.queue[tail++] = m;
    }
  }
  ap.queue_len = tail;
}

// Turns the BFS parent chain into waypoints, keeping only the corners.
static bool BuildRoute(int goal) {
  static int16 chain[kNumNodes];
  int len = 0;
  for (int n = goal; n >= 0 && len < kNumNodes; n = ap.parent[n])
    chain[len++] = n;
  if (len == 0)
    return false;

  ap.wp_count = 0;
  int prev_dx = 99, prev_dy = 99;
  for (int i = len - 1; i >= 0; i--) {
    int n = chain[i];
    int x = n % kNodeStride, y = n / kNodeStride;
    if (i > 0) {
      int m = chain[i - 1];
      int dx = m % kNodeStride - x, dy = m / kNodeStride - y;
      if (dx == prev_dx && dy == prev_dy)
        continue;  // same heading, not a corner
      prev_dx = dx, prev_dy = dy;
    }
    if (ap.wp_count >= kMaxWaypoints)
      break;
    ap.wp_x[ap.wp_count] = ap.ax0 + x * 8;
    ap.wp_y[ap.wp_count] = ap.ay0 + y * 8;
    ap.wp_count++;
  }
  // The last waypoint must be the goal itself.
  int gx = ap.ax0 + (goal % kNodeStride) * 8, gy = ap.ay0 + (goal / kNodeStride) * 8;
  if (ap.wp_x[ap.wp_count - 1] != gx || ap.wp_y[ap.wp_count - 1] != gy) {
    if (ap.wp_count < kMaxWaypoints)
      ap.wp_count++;
    ap.wp_x[ap.wp_count - 1] = gx;
    ap.wp_y[ap.wp_count - 1] = gy;
  }
  ap.wp_pos = 0;
  return true;
}

static bool SideLeadsSomewhere(int side) {
  if (ap.area >= 0x80)  // special areas (grove, Zora's domain) only exit south
    return side == kSide_South;
  switch (side) {
  case kSide_North: return (ap.ay0 & 0xfff) != 0;
  case kSide_South: return ((ap.ay0 + ap.tiles * 8) & 0xfff) != 0;
  case kSide_West: return (ap.ax0 & 0xfff) != 0;
  case kSide_East: return ((ap.ax0 + ap.tiles * 8) & 0xfff) != 0;
  }
  return false;
}

// Reachable nodes on the given edge that Link can walk out of.
static bool IsExitNode(int side, int cx, int cy) {
  if (!ap.node_ok[NodeIndex(cx, cy)] || ap.dist[NodeIndex(cx, cy)] < 0)
    return false;
  switch (side) {
  case kSide_North: return cy == 0 && ap.tile_ok[0][cx] && ap.tile_ok[0][cx + 1];
  case kSide_South: return cy == NodesHigh() - 1;
  case kSide_West: return cx == 0;
  case kSide_East: return cx == NodesWide() - 1;
  }
  return false;
}

// Picks the middle of a random opening along the given edge. Returns -1 if none.
static int PickExitNode(int side) {
  int n = (side == kSide_North || side == kSide_South) ? NodesWide() : NodesHigh();
  int fixed = side == kSide_South ? NodesHigh() - 1 : side == kSide_East ? NodesWide() - 1 : 0;
  int run_start[64], run_len[64], runs = 0, total = 0;
  int cur = -1;
  for (int i = 0; i <= n; i++) {
    bool ok = false;
    if (i < n) {
      int cx = (side == kSide_North || side == kSide_South) ? i : fixed;
      int cy = (side == kSide_North || side == kSide_South) ? fixed : i;
      ok = IsExitNode(side, cx, cy);
    }
    if (ok && cur < 0) {
      cur = i;
    } else if (!ok && cur >= 0) {
      // Skip slivers narrower than Link; they're usually map seams.
      if (i - cur >= 2 && runs < 64) {
        run_start[runs] = cur, run_len[runs] = i - cur, total += i - cur;
        runs++;
      }
      cur = -1;
    }
  }
  if (runs == 0)
    return -1;
  int pick = RandRange(0, total - 1), r = 0;
  while (pick >= run_len[r])
    pick -= run_len[r++];
  int mid = run_start[r] + run_len[r] / 2;
  int cx = (side == kSide_North || side == kSide_South) ? mid : fixed;
  int cy = (side == kSide_North || side == kSide_South) ? fixed : mid;
  return NodeIndex(cx, cy);
}

static int PickWanderNode(int start) {
  int max_dist = 0;
  for (int i = 0; i < kNumNodes; i++)
    if (ap.dist[i] > max_dist)
      max_dist = ap.dist[i];
  if (max_dist < 4)
    return -1;
  // Prefer somewhere in the far half of what's reachable.
  int min_dist = max_dist / 2;
  int count = 0;
  for (int i = 0; i < kNumNodes; i++)
    if (ap.dist[i] >= min_dist)
      count++;
  int pick = RandRange(0, count - 1);
  for (int i = 0; i < kNumNodes; i++) {
    if (ap.dist[i] >= min_dist && pick-- == 0)
      return i;
  }
  return -1;
}

static void Plan(bool keep_goal) {
  BuildGrid(false);
  int start = FindStartNode();
  if (start < 0 && !ap.swimming) {
    // Standing in water (or somewhere else odd): plan through the water to shore.
    BuildGrid(true);
    start = FindStartNode();
  }
  if (start < 0) {
    LOG("no start node at %d,%d", link_x_coord, link_y_coord);
    // Nowhere sane to stand; wiggle and try again soon.
    ap.phase = kPhase_Idle;
    ap.timer = 20;
    ap.stuck_count++;
    return;
  }
  RunBfs(start);

  // Decide between leaving the area and wandering inside it. Wander a
  // couple of times per area, then move on.
  int goal = -1;
  if (keep_goal) {
    int gx = (ap.goal_x - ap.ax0) >> 3, gy = (ap.goal_y - ap.ay0) >> 3;
    if (gx >= 0 && gy >= 0 && gx < NodesWide() && gy < NodesHigh() && ap.dist[NodeIndex(gx, gy)] >= 0 &&
        BuildRoute(NodeIndex(gx, gy))) {
      ap.phase = kPhase_Walk;
      ap.best_dist = 0x7fffffff;
      ap.frames_no_closer = 0;
      return;
    }
  }
  ap.goal_side = kSide_None;
  if (ap.swimming) {
    // Swim to the nearest dry spot. BFS order means the first dry node found
    // in the queue is the closest.
    for (int i = 0; i < ap.queue_len && goal < 0; i++) {
      int n = ap.queue[i];
      int x = n % kNodeStride, y = n / kNodeStride;
      if (TileAttrWalkable(ap.tile_attr[y + 1][x]) && TileAttrWalkable(ap.tile_attr[y + 1][x + 1]) &&
          TileAttrWalkable(ap.tile_attr[y + 2][x]) && TileAttrWalkable(ap.tile_attr[y + 2][x + 1]))
        goal = n;
    }
    LOG("swimming at %d,%d, heading for shore", link_x_coord, link_y_coord);
  }
  // No shore in this area: swim out through an edge and look again there.
  bool want_exit = ap.swimming ? goal < 0 : (ap.wanders_in_area >= 2 || RandRange(0, 99) < 45);
  if (want_exit) {
    int weights[4], total = 0;
    for (int s = 0; s < 4; s++) {
      weights[s] = 0;
      if (SideLeadsSomewhere(s) && !(ap.failed_sides & (1 << s)))
        weights[s] = (s == ap.came_from_side) ? 1 : 4;
      total += weights[s];
    }
    // Try sides in weighted random order until one has an opening.
    while (total > 0 && goal < 0) {
      int pick = RandRange(0, total - 1), s = 0;
      while (pick >= weights[s])
        pick -= weights[s++];
      goal = PickExitNode(s);
      if (goal >= 0)
        ap.goal_side = s;
      total -= weights[s];
      weights[s] = 0;
    }
  }
  if (goal < 0 && !ap.swimming)
    goal = PickWanderNode(start);
  if (goal < 0 || !BuildRoute(goal)) {
    LOG("no goal from %d,%d", link_x_coord, link_y_coord);
    ap.phase = kPhase_Idle;
    ap.timer = 60;
    ap.stuck_count++;
    return;
  }
  ap.goal_x = ap.ax0 + (goal % kNodeStride) * 8;
  ap.goal_y = ap.ay0 + (goal / kNodeStride) * 8;
  ap.phase = kPhase_Walk;
  ap.best_dist = 0x7fffffff;
  ap.frames_no_closer = 0;
}

// Steers along the main axis and leaves small cross-axis offsets alone: the
// game nudges Link around corners and onto stairs when he's within a few
// pixels, and pressing sideways fights that (and stops him on stairs).
static uint16 SteerTowards(int tx, int ty) {
  int dx = tx - link_x_coord, dy = ty - link_y_coord;
  bool x_major = abs(dx) >= abs(dy);
  int x_slop = x_major ? kMajorSlop : kCornerAssistSlop;
  int y_slop = x_major ? kCornerAssistSlop : kMajorSlop;
  uint16 b = 0;
  if (dx > x_slop) b |= kBtnRight;
  else if (dx < -x_slop) b |= kBtnLeft;
  if (dy > y_slop) b |= kBtnDown;
  else if (dy < -y_slop) b |= kBtnUp;
  return b;
}

static void OnAreaChanged() {
  LOG("area %02x -> %02x at %d,%d", ap.area, overworld_area_index, link_x_coord, link_y_coord);
  if (ap.phase == kPhase_Exit && ap.goal_side != kSide_None)
    ap.came_from_side = kOppositeSide[ap.goal_side];
  else
    ap.came_from_side = kSide_None;
  ap.wanders_in_area = 0;
  ap.failed_sides = 0;
  ap.stuck_count = 0;
  // Walking straight back where we came from doesn't count as progress, so
  // bouncing between two areas eventually trips the no-progress reset.
  if (overworld_area_index != ap.prev_area)
    ap.frames_since_progress = 0;
  ap.prev_area = ap.area;
  ap.grid_valid = false;
  ap.phase = kPhase_Plan;
}

static void OnStuck() {
  LOG("stuck at %d,%d heading for %d,%d (goal side %d, count %d)", link_x_coord, link_y_coord,
      ap.wp_x[ap.wp_pos], ap.wp_y[ap.wp_pos], ap.goal_side, ap.stuck_count + 1);
  ap.frames_no_closer = 0;
  if (++ap.stuck_count >= 3) {
    // This goal isn't working out. If it was an exit, don't try that side again.
    if (ap.goal_side != kSide_None)
      ap.failed_sides |= 1 << ap.goal_side;
    ap.stuck_count = 0;
  }
  // Step in a random direction for a moment before replanning, so we don't
  // retry the exact same blocked move.
  ap.phase = kPhase_Idle;
  ap.timer = RandRange(6, 16);
  ap.nudge_buttons = kSideButtons[RandRange(0, 3)];
}

enum {
  kHudItem_Mirror = 20,
  kMaxMirrorTries = 6,
  // The game bounces Link back if his Light World spot is blocked; it
  // decides within a few seconds of arriving.
  kLightWorldSettledFrames = 60 * 8,
};

static void RestoreEquippedItem() {
  if (ap.saved_item >= 0) {
    hud_cur_item = ap.saved_item;
    Hud_UpdateEquippedItem();
    ap.saved_item = -1;
  }
}

void Autopilot_RequestLightWorld() {
  ap.want_light_world = true;
}

// Called every overworld frame while we want the Light World.
static void TrackLightWorldArrival() {
  if (savegame_is_darkworld) {
    ap.frames_in_light_world = 0;
    if (ap.mirror_tries >= kMaxMirrorTries) {
      LOG("giving up on the Light World after %d mirror tries", ap.mirror_tries);
      ap.want_light_world = false;
      RestoreEquippedItem();
    }
  } else if (++ap.frames_in_light_world >= kLightWorldSettledFrames) {
    LOG("arrived in the Light World at %d,%d", link_x_coord, link_y_coord);
    ap.want_light_world = false;
    RestoreEquippedItem();
  }
}

// Equips the mirror (through the game's own HUD code, so the item box
// shows it) and starts pressing Y.
static void StartMirror() {
  if (ap.saved_item < 0)
    ap.saved_item = hud_cur_item;
  hud_cur_item = kHudItem_Mirror;
  Hud_UpdateEquippedItem();
  ap.mirror_press = 3;
  ap.mirror_tries++;
  LOG("using the mirror at %d,%d (try %d)", link_x_coord, link_y_coord, ap.mirror_tries);
}

void Autopilot_Reset(uint32 seed) {
  memset(&ap, 0, sizeof(ap));
  ap.saved_item = -1;
  ap.rng = seed ? seed : 0x12345678;
  ap.came_from_side = kSide_None;
  ap.goal_side = kSide_None;
  ap.prev_area = 0xffff;
  ap.phase = kPhase_Plan;
}

bool Autopilot_WantsReset() {
  return ap.want_reset;
}

uint16 Autopilot_RunFrame() {
  ap.frame++;
  if (g_autopilot_verbose >= 2 && ap.frame % 60 == 0)
    LOG("trace module=%d/%d area=%02x pos=%d,%d handler=%d phase=%d wp=%d/%d tgt=%d,%d no_closer=%d",
        main_module_index, submodule_index, overworld_area_index, link_x_coord, link_y_coord,
        link_player_handler_state, ap.phase, ap.wp_pos, ap.wp_count, ap.wp_x[ap.wp_pos], ap.wp_y[ap.wp_pos],
        ap.frames_no_closer);

  // Text boxes: tap B to advance.
  if (main_module_index == 14)
    return (ap.frame & 8) ? kBtnB : 0;

  if (main_module_index != 9) {
    // Interiors aren't handled yet; if we end up somewhere odd, start over.
    if (++ap.frames_off_overworld > 60 * 10 && !ap.want_reset) {
      LOG("stuck off the overworld (module %d)", main_module_index);
      ap.want_reset = true;
    }
    return 0;
  }
  ap.frames_off_overworld = 0;

  // Area transitions and other overworld sequences move Link by themselves.
  if (submodule_index != 0)
    return 0;

  if (++ap.frames_since_progress > 60 * 90 && !ap.want_reset) {
    LOG("no progress for 90s at %d,%d", link_x_coord, link_y_coord);
    ap.want_reset = true;
  }

  if (overworld_area_index != ap.area && ap.grid_valid)
    OnAreaChanged();

  if (ap.want_light_world)
    TrackLightWorldArrival();

  // Only steer when the D-pad moves Link (not while jumping off a ledge,
  // recoiling, etc.).
  uint8 handler = link_player_handler_state;
  if (!LinkIsSteerable(handler)) {
    ap.airborne = true;
    return 0;
  }
  if (ap.airborne || handler != ap.last_handler) {
    // Just landed, or started/stopped swimming. Re-plan from where we are,
    // keeping the goal if it's still reachable.
    bool entered_water = handler == kHandler_Swimming && ap.last_handler != kHandler_Swimming;
    bool left_water = handler != kHandler_Swimming && ap.last_handler == kHandler_Swimming;
    ap.airborne = false;
    ap.last_handler = handler;
    if (ap.phase == kPhase_Walk || entered_water || left_water) {
      LOG("landed at %d,%d (handler %d)", link_x_coord, link_y_coord, handler);
      Plan(!entered_water && !left_water);
    }
  }


  switch (ap.phase) {
  case kPhase_Plan:
    Plan(false);
    return 0;

  case kPhase_Idle:
    if (ap.mirror_press > 0) {
      // Hold Y for a few frames, then release (the game wants a fresh press).
      return --ap.mirror_press > 0 ? kBtnY : 0;
    }
    if (ap.try_mirror) {
      ap.try_mirror = false;
      if (ap.want_light_world && savegame_is_darkworld && link_item_mirror >= 2) {
        StartMirror();
        return kBtnY;
      }
    }
    if (--ap.timer <= 0) {
      ap.phase = kPhase_Plan;
      ap.nudge_buttons = 0;
    }
    return ap.nudge_buttons;

  case kPhase_Walk: {
    // Advance past waypoints we've reached.
    while (ap.wp_pos < ap.wp_count &&
           abs(ap.wp_x[ap.wp_pos] - link_x_coord) <= kArriveDist &&
           abs(ap.wp_y[ap.wp_pos] - link_y_coord) <= kArriveDist) {
      ap.wp_pos++;
      ap.best_dist = 0x7fffffff;
      ap.frames_no_closer = 0;
    }
    if (ap.wp_pos >= ap.wp_count) {
      ap.stuck_count = 0;
      ap.frames_since_progress = 0;
      if (ap.goal_side != kSide_None) {
        ap.phase = kPhase_Exit;
        ap.timer = 90;
      } else {
        ap.wanders_in_area++;
        ap.phase = kPhase_Idle;
        ap.timer = RandRange(20, 150);
        ap.try_mirror = ap.want_light_world;
      }
      return 0;
    }
    uint16 b = SteerTowards(ap.wp_x[ap.wp_pos], ap.wp_y[ap.wp_pos]);
    int dist = abs(ap.wp_x[ap.wp_pos] - link_x_coord) + abs(ap.wp_y[ap.wp_pos] - link_y_coord);
    if (dist < ap.best_dist) {
      ap.best_dist = dist;
      ap.frames_no_closer = 0;
    } else if (++ap.frames_no_closer > kStuckFrames) {
      OnStuck();
    }
    return b;
  }

  case kPhase_Exit:
    // Push off the edge; OnAreaChanged fires once the scroll completes.
    if (--ap.timer <= 0) {
      LOG("exit side %d failed at %d,%d", ap.goal_side, link_x_coord, link_y_coord);
      ap.failed_sides |= 1 << ap.goal_side;
      ap.phase = kPhase_Plan;
      return 0;
    }
    return kSideButtons[ap.goal_side];
  }
  return 0;
}

static void BlendPixel(uint8 *pixels, int pitch, int x, int y, uint32 color) {
  uint32 *p = (uint32 *)(pixels + y * pitch) + x;
  *p = ((*p >> 1) & 0x7f7f7f) + ((color >> 1) & 0x7f7f7f);
}

static void FillRect(uint8 *pixels, int pitch, int width, int height,
                     int x, int y, int w, int h, uint32 color) {
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > width) x1 = width;
  if (y1 > height) y1 = height;
  for (int yy = y; yy < y1; yy++)
    for (int xx = x; xx < x1; xx++)
      BlendPixel(pixels, pitch, xx, yy, color);
}

// screen_x/screen_y are the world coordinates at the top-left of the frame.
void Autopilot_DrawDebug(uint8 *pixels, int pitch, int width, int height, int scale,
                         int screen_x, int screen_y) {
  if (!ap.grid_valid || main_module_index != 9)
    return;
  for (int r = 0; r < ap.tiles; r++) {
    int sy = (ap.ay0 + r * 8 - screen_y) * scale;
    if (sy <= -8 * scale || sy >= height)
      continue;
    for (int c = 0; c < ap.tiles; c++) {
      if (ap.tile_ok[r][c])
        continue;
      int sx = (ap.ax0 + c * 8 - screen_x) * scale;
      if (sx <= -8 * scale || sx >= width)
        continue;
      FillRect(pixels, pitch, width, height, sx, sy, 8 * scale, 8 * scale, 0xff0000);
    }
  }
  if (ap.phase == kPhase_Walk || ap.phase == kPhase_Exit) {
    for (int i = ap.wp_pos; i < ap.wp_count; i++) {
      uint32 color = (i == ap.wp_count - 1) ? 0x00ff00 : 0xffff00;
      int sx = (ap.wp_x[i] + 4 - screen_x) * scale;
      int sy = (ap.wp_y[i] + 12 - screen_y) * scale;
      FillRect(pixels, pitch, width, height, sx, sy, 8 * scale, 8 * scale, color);
    }
  }
}
