// Co-op game logic: reads the running game's memory once per game frame,
// shares it with the other players and merges what they send back.
//
// The memory map knowledge and the merge rules for the save data come from the
// alttpo project (https://github.com/alttpo/alttpo); the transport, the event
// based sharing (keys, bombs, PvP hits) and the sprite streaming are new.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <zlib.h>
#include "app.h"
#include "emu.h"
#include "snes/snes.h"
#include "names.h"

RoomSettings g_room = {1, 1, 1, 1, {0}};

// ------------------------------------------------------------------ messages
enum {
  M_STATE = 1, M_SRAM = 2,
  M_HELLO = 16, M_TILES, M_PAL, M_BOMBS, M_KEYS, M_KEYREQ, M_KEYSNAP, M_PVP, M_ROMREQ, M_ROMDATA,
  M_TILEMAP, M_TILERESET, M_NOROM,
};

#define MAX_SPR 48
#define SRAM_SIZE 0x500
#define EX_SIZE 0x500           // $7F6018..: pots (0x250), drops (0x250), purchases (0x60)
#define EX_BASE 0x16018         // offset of $7F6018 in WRAM

typedef struct SprRec {
  int16_t wx, wy;   // world position
  uint32_t key;     // tile data hash
  uint8_t flags;    // bit0 16x16, bit1 hflip, bit2 vflip, bits3-4 priority, bits5-7 palette
} SprRec;

typedef struct Hitbox { bool active; int x, y, w, h; } Hitbox;

typedef struct TmRun { uint16_t offs; uint8_t count; bool same, vertical; uint32_t tiles[64]; } TmRun;

typedef struct Remote {
  bool used;
  uint32_t cid;
  char name[13];
  uint8_t color;
  uint32_t romCrc;
  bool hello;
  uint64_t tState;
  // state
  uint8_t flags, module, sub;
  uint32_t location;
  int16_t x, y;
  uint8_t health, maxHealth, armor, swordType, roomLevel;
  Hitbox hit, act;
  uint8_t swordTime, itemUsed;
  int nspr;
  SprRec spr[MAX_SPR];
  uint16_t pal[8][16];
  bool palHave[8];
  // save data mirror
  uint8_t sram[SRAM_SIZE];
  uint8_t sramHave[SRAM_SIZE / 16];
  uint8_t ex[EX_SIZE];
  uint8_t exHave[EX_SIZE / 16];
  // what we already sent to this player
  uint32_t sentKeys[4096];
  int sentCount;
  uint16_t sentPal[8][16];
  bool sentPalValid[8];
  bool sentHello;
  int attackCooldown;
  uint32_t lastSeenLocation;
  // tilemap changes made in their current room
  uint32_t tmLocation, tmStamp;
  TmRun *tmRuns; int tmCount;
  bool wasInGame;
} Remote;

static Remote s_rem[MAX_PLAYERS];
static bool s_online, s_running;
static uint8_t *R;                 // WRAM
#define SR (R + 0xF000)            // save data mirror ($7EF000)

// rom facts
static bool s_isRando, s_isDoor, s_swampMasks;
static uint32_t s_fast;
static uint32_t s_romCrc;

// local state
static struct {
  uint32_t frame;
  uint8_t module, sub, subsub, state;
  bool bad, inGame, wasInGame;
  uint32_t location, actual, lastLocation, lastActual;
  uint16_t owRoom, uwRoom, dungeon;
  int16_t x, y, xoffs, yoffs;
  Hitbox hit, act;
  uint8_t swordTime, swordType, itemUsed, roomLevel;
  int nspr;
  SprRec spr[MAX_SPR];
  uint8_t sprTile[MAX_SPR][128];
  uint16_t pal[8][16];
  bool palUsed[8];
  // bombs / keys
  int bombsLast, bombsPending, bombsOut, bombsOutAge;
  int keysLast[16];
  bool keysInit;
  int keySyncWait;
  // tilemap
  int32_t tm[0x2000];
  uint16_t tmSnapT[0x2000];
  uint8_t tmSnapA[0x2000];
  bool tmPrevSafe, tmDirty;
  uint32_t tmLocation, tmStamp;
  int tmCount;
} L;

// pending events (applied inside the game frame hook)
static int s_evBombs;
static struct { uint8_t damage; int8_t dx, dy, dz; uint8_t swordTime, mode; } s_evPvp[8];
static int s_evPvpCount;

// patch code for this frame
static uint8_t s_code[200];
static int s_codeLen;

// sram chunk bookkeeping
typedef struct Chunk { uint8_t space; uint16_t start, count; uint32_t lastHash; uint64_t lastSent; bool randoOnly, doorOnly; } Chunk;
static Chunk s_chunks[] = {
  {0, 0x340, 0x090, 0, 0, false, false},  // equipment, keys, tracking
  {0, 0x3C0, 0x040, 0, 0, false, false},  // progress
  {0, 0x400, 0x100, 0, 0, true, false},   // randomizer stats and counters
  {0, 0x000, 0x128, 0, 0, false, false},  // rooms (two halves)
  {0, 0x128, 0x128, 0, 0, false, false},
  {0, 0x280, 0x0C0, 0, 0, false, false},  // overworld
  {1, 0x000, 0x250, 0, 0, false, true},   // pots
  {1, 0x250, 0x250, 0, 0, false, true},   // enemy drops
  {1, 0x4A0, 0x060, 0, 0, false, true},   // shop purchases
};
#define NUM_CHUNKS ((int)(sizeof(s_chunks) / sizeof(s_chunks[0])))

// tile cache shared by all remote players (content addressed)
typedef struct TileEnt { uint32_t key; uint8_t px[256]; } TileEnt;
#define TILE_SLOTS 8192
static TileEnt *s_tiles;
static int s_tileCount;
static uint8_t s_extraPx[PPU_MAX_EXTRA][256];
static uint16_t s_extraPal[MAX_PLAYERS][8][16];

// notifications
typedef struct Note { char text[72]; uint64_t t; uint32_t color; } Note;
static Note s_notes[6];

// join / rom transfer
static int s_joinState = JOIN_IDLE;
static int s_joinProgress;
static char s_joinError[96];
static uint8_t *s_joinRom; static int s_joinRomLen; static char s_joinName[64];
static uint32_t s_joinFrom, s_joinCrc;
static uint64_t s_joinT;
static uint8_t *s_hostRom; static int s_hostRomLen; static char s_hostName[64];

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint32_t fnv(const uint8_t *d, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h;
}

// ------------------------------------------------------------------ notifications
void game_notify(const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  memmove(&s_notes[1], &s_notes[0], sizeof(Note) * 5);
  snprintf(s_notes[0].text, sizeof(s_notes[0].text), "%s", buf);
  s_notes[0].t = app_ms();
  s_notes[0].color = COL_WHITE;
  app_log("note: %s", buf);
}

// ------------------------------------------------------------------ helpers
static Remote *rem_find(uint32_t cid) {
  for (int i = 0; i < MAX_PLAYERS; i++) if (s_rem[i].used && s_rem[i].cid == cid) return &s_rem[i];
  return NULL;
}

static bool rem_alive(const Remote *r) {
  return r->used && r->hello && app_ms() - r->tState < 4000;
}

static bool rem_playing(const Remote *r) {
  return rem_alive(r) && (r->flags & 1) && r->romCrc == s_romCrc;
}

static bool is_bad_time(uint8_t module, uint8_t sub) {
  if (module <= 0x05) return true;
  if (module == 0x14) return true;
  if (module >= 0x1B) return true;
  if (module == 0x0e && sub == 0x07) return true;
  return false;
}

static bool locations_equal(uint32_t a, uint32_t b) {
  if (a == b) return true;
  if ((a & 0x010000) && (b & 0x010000)) return (a & 0xFFFF) == (b & 0xFFFF);
  return false;
}

static bool in_transition(void) {
  if (L.module == 0x09 && L.sub >= 0x01) return true;
  if (L.module == 0x07 && L.sub == 0x02) return true;
  if (L.module == 0x07 && L.sub == 0x19) return true;
  return false;
}

static bool can_see(uint32_t other) {
  if (locations_equal(L.location, other)) return true;
  if (in_transition()) {
    if (locations_equal(L.actual, other)) return true;
    return locations_equal(L.lastLocation, other);
  }
  return false;
}

static bool can_sample_location(void) {
  switch (L.module) {
    case 0x07:
      if (L.sub == 0x0e) return L.subsub > 0x02;
      return true;
    case 0x09:
      if (L.sub >= 0x01 && L.sub <= 0x08) return false;
      if (L.sub == 0x23 || L.sub == 0x2c) { if (L.subsub < 0x03) return false; }
      return true;
    case 0x0e:
      return L.sub != 0x07;
    default:
      return true;
  }
}

static bool hit_intersects(const Hitbox *a, const Hitbox *b) {
  if (a->x + a->w < b->x) return false;
  if (a->x > b->x + b->w) return false;
  if (a->y + a->h < b->y) return false;
  if (a->y > b->y + b->h) return false;
  return true;
}

static void code_jsl(uint32_t a) {
  if (s_codeLen + 4 > (int)sizeof(s_code)) return;
  s_code[s_codeLen++] = 0x22;
  s_code[s_codeLen++] = a & 0xff; s_code[s_codeLen++] = (a >> 8) & 0xff; s_code[s_codeLen++] = (a >> 16) & 0xff;
}

static void code_lda_sta(uint8_t imm, uint16_t addr) {
  if (s_codeLen + 5 > (int)sizeof(s_code)) return;
  s_code[s_codeLen++] = 0xA9; s_code[s_codeLen++] = imm;
  s_code[s_codeLen++] = 0x8D; s_code[s_codeLen++] = addr & 0xff; s_code[s_codeLen++] = addr >> 8;
}

const char *game_location_name(uint32_t loc) {
  static char buf[32];
  const LocName *t; int n;
  if (loc & 0x010000) { t = kUnderworldNames; n = sizeof(kUnderworldNames) / sizeof(LocName); }
  else { t = kOverworldNames; n = sizeof(kOverworldNames) / sizeof(LocName); }
  uint16_t id = loc & 0xFFFF;
  for (int i = 0; i < n; i++) if (t[i].id == id) return t[i].name;
  snprintf(buf, sizeof(buf), (loc & 0x010000) ? "Underworld %03X" : "Overworld %02X", id);
  return buf;
}

// ------------------------------------------------------------------ item tables
enum { T_MAX = 1, T_OR, T_BOTTLE, T_ZERO2NZ, T_FLUTE, T_GLOVES, T_SWORD, T_SHIELD, T_ARMOR, T_WORLD, T_PROG1, T_PROG2, T_RANDOITEMS, T_MAX16 };

typedef struct Sync { uint16_t offs; uint8_t type; uint8_t group; const char *names; } Sync;
// group: 0 items, 1 dungeon items, 2 progress, 3 quiet counters
// names: '|' separated; for T_OR bit 0 first, otherwise value 1 first

static const Sync kSyncBase[] = {
  {0x340, T_MAX, 0, "Bow|Bow|Silver Bow|Silver Bow"},
  {0x341, T_MAX, 0, "Blue Boomerang|Red Boomerang"},
  {0x342, T_MAX, 0, "Hookshot"},
  {0x344, T_MAX, 0, "Mushroom|Magic Powder"},
  {0x345, T_MAX, 0, "Fire Rod"},
  {0x346, T_MAX, 0, "Ice Rod"},
  {0x347, T_MAX, 0, "Bombos Medallion"},
  {0x348, T_MAX, 0, "Ether Medallion"},
  {0x349, T_MAX, 0, "Quake Medallion"},
  {0x34A, T_MAX, 0, "Lamp"},
  {0x34B, T_MAX, 0, "Hammer"},
  {0x34C, T_MAX, 0, "Shovel|Flute|Flute"},
  {0x34D, T_MAX, 0, "Bug Catching Net"},
  {0x34E, T_MAX, 0, "Book of Mudora"},
  {0x350, T_MAX, 0, "Cane of Somaria"},
  {0x351, T_MAX, 0, "Cane of Byrna"},
  {0x352, T_MAX, 0, "Magic Cape"},
  {0x353, T_MAX, 0, "Magic Scroll|Magic Mirror"},
  {0x354, T_GLOVES, 0, "Power Glove|Titan's Mitt"},
  {0x355, T_MAX, 0, "Pegasus Boots"},
  {0x356, T_MAX, 0, "Flippers"},
  {0x357, T_MAX, 0, "Moon Pearl"},
  {0x359, T_SWORD, 0, "Fighter's Sword|Master Sword|Tempered Sword|Golden Sword"},
  {0x35A, T_SHIELD, 0, "Fighter's Shield|Red Shield|Mirror Shield"},
  {0x35B, T_ARMOR, 0, "Blue Mail|Red Mail"},
  {0x35C, T_BOTTLE, 0, "a Bottle"},
  {0x35D, T_BOTTLE, 0, "a Bottle"},
  {0x35E, T_BOTTLE, 0, "a Bottle"},
  {0x35F, T_BOTTLE, 0, "a Bottle"},
  {0x364, T_OR, 1, "||Ganon's Tower Compass|Turtle Rock Compass|Thieves' Town Compass|Tower of Hera Compass|Ice Palace Compass|Skull Woods Compass"},
  {0x365, T_OR, 1, "Misery Mire Compass|Palace of Darkness Compass|Swamp Palace Compass|Agahnim's Tower Compass|Desert Palace Compass|Eastern Palace Compass|Hyrule Castle Compass|Sewers Compass"},
  {0x366, T_OR, 1, "||Ganon's Tower Big Key|Turtle Rock Big Key|Thieves' Town Big Key|Tower of Hera Big Key|Ice Palace Big Key|Skull Woods Big Key"},
  {0x367, T_OR, 1, "Misery Mire Big Key|Palace of Darkness Big Key|Swamp Palace Big Key|Agahnim's Tower Big Key|Desert Palace Big Key|Eastern Palace Big Key|Hyrule Castle Big Key|Sewers Big Key"},
  {0x368, T_OR, 1, "||Ganon's Tower Map|Turtle Rock Map|Thieves' Town Map|Tower of Hera Map|Ice Palace Map|Skull Woods Map"},
  {0x369, T_OR, 1, "Misery Mire Map|Palace of Darkness Map|Swamp Palace Map|Agahnim's Tower Map|Desert Palace Map|Eastern Palace Map|Hyrule Castle Map|Sewers Map"},
  {0x370, T_MAX, 3, NULL},  // bomb capacity
  {0x371, T_MAX, 3, NULL},  // arrow capacity
  {0x374, T_OR, 2, "the Pendant of Power|the Pendant of Wisdom|the Pendant of Courage"},
  {0x379, T_OR, 3, NULL},   // abilities
  {0x37A, T_OR, 2, "Crystal 6|Crystal 1|Crystal 5|Crystal 7|Crystal 2|Crystal 4|Crystal 3"},
  {0x37B, T_MAX, 0, "Half Magic|Quarter Magic"},
  {0x3C5, T_WORLD, 3, NULL},
  {0x3C6, T_PROG1, 3, NULL},
  {0x3C7, T_MAX, 3, NULL},
  {0x3C9, T_PROG2, 3, NULL},
};

static const Sync kSyncRando[] = {
  {0x38C, T_RANDOITEMS, 0, "Flute|Flute|Shovel||Magic Powder|Mushroom|Red Boomerang|Blue Boomerang"},
  {0x38E, T_OR, 0, "||||||Silver Arrows|Bow"},
  {0x410, T_OR, 3, NULL}, {0x411, T_OR, 3, NULL},
  {0x417, T_MAX, 3, NULL}, {0x422, T_MAX, 3, NULL}, {0x46E, T_MAX, 3, NULL},
  {0x418, T_MAX16, 2, "a Triforce Piece"},
  {0x429, T_MAX, 3, NULL}, {0x476, T_MAX, 3, NULL},
  {0x472, T_OR, 3, NULL}, {0x473, T_OR, 3, NULL},
  {0x423, T_MAX16, 3, NULL},
  {0x403, T_OR, 3, NULL}, {0x404, T_OR, 3, NULL},
  {0x414, T_OR, 3, NULL}, {0x415, T_OR, 3, NULL},
  {0x474, T_OR, 3, NULL}, {0x475, T_OR, 3, NULL},
};

static Sync s_sync[256];
static int s_syncCount;

static void add_sync(const Sync *s) { if (s_syncCount < 256) s_sync[s_syncCount++] = *s; }

static void build_sync_table(void) {
  s_syncCount = 0;
  for (int i = 0; i < (int)(sizeof(kSyncBase) / sizeof(Sync)); i++) {
    Sync s = kSyncBase[i];
    if (s_isRando) {
      // the randomizer tracks these through its own inventory bytes
      if (s.offs == 0x340 || s.offs == 0x341) { s.type = T_ZERO2NZ; s.names = NULL; }
      if (s.offs == 0x344) continue;
      if (s.offs == 0x34C) { s.type = T_FLUTE; s.names = NULL; }
    }
    add_sync(&s);
  }
  if (s_isRando) {
    for (int i = 0; i < (int)(sizeof(kSyncRando) / sizeof(Sync)); i++) add_sync(&kSyncRando[i]);
    for (int i = 0x390; i < 0x3A0; i++) { Sync s = {(uint16_t)i, T_MAX, 3, NULL}; add_sync(&s); }
    if (s_isDoor) {
      for (int i = 0; i < 14; i++) { Sync s = {(uint16_t)(0x4B0 + i * 2), T_MAX16, 3, NULL}; add_sync(&s); }
      for (int i = 0; i < 14; i++) { Sync s = {(uint16_t)(0x4E0 + i), T_MAX, 3, NULL}; add_sync(&s); }
    } else {
      for (int i = 0x302; i < 0x33D; i++) { Sync s = {(uint16_t)i, T_MAX, 3, NULL}; add_sync(&s); }
      for (int i = 0x434; i <= 0x439; i++) { Sync s = {(uint16_t)i, T_MAX, 3, NULL}; add_sync(&s); }
    }
  }
}

static const char *name_at(const char *names, int idx, char *buf, int size) {
  buf[0] = 0;
  if (!names) return buf;
  const char *p = names;
  for (int i = 0; i < idx; i++) {
    p = strchr(p, '|');
    if (!p) return buf;
    p++;
  }
  int n = 0;
  while (p[n] && p[n] != '|' && n < size - 1) { buf[n] = p[n]; n++; }
  buf[n] = 0;
  return buf;
}

static bool sram_have(const Remote *r, int offs, int size) {
  for (int i = offs; i < offs + size; i++) if (!(r->sramHave[i >> 4])) return false;
  return true;
}

// ------------------------------------------------------------------ merging save data
static void merge_items(void) {
  if (L.bad || R[0x02E4] != 0) return;
  for (int k = 0; k < s_syncCount; k++) {
    const Sync *s = &s_sync[k];
    int size = s->type == T_MAX16 ? 2 : 1;
    int oldV = size == 2 ? rd16(SR + s->offs) : SR[s->offs];
    int newV = oldV;
    const Remote *from = NULL;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Remote *r = &s_rem[i];
      if (!rem_playing(r) || !sram_have(r, s->offs, size)) continue;
      int rv = size == 2 ? rd16(r->sram + s->offs) : r->sram[s->offs];
      int cand = newV;
      switch (s->type) {
        case T_MAX: case T_MAX16: case T_GLOVES: case T_ARMOR: case T_WORLD:
          if (rv > newV) cand = rv;
          break;
        case T_SWORD:
          if (rv >= 1 && rv <= 4 && rv > newV) cand = rv;
          break;
        case T_SHIELD:
          if (rv > newV && rv <= 3) cand = rv;
          break;
        case T_OR:
          cand = newV | rv;
          break;
        case T_BOTTLE: case T_ZERO2NZ:
          if (newV == 0 && rv != 0) cand = rv;
          break;
        case T_FLUTE:
          if (newV == 0 && rv != 0) cand = rv;
          else if (newV == 2 && rv == 3) cand = rv;
          break;
        case T_PROG1: {
          int v = rv;
          if (!(newV & 0x10)) v &= ~0x10;
          cand = newV | v;
          break;
        }
        case T_PROG2:
          cand = newV | rv;
          break;
        case T_RANDOITEMS:
          cand = newV | rv;
          break;
      }
      if (cand != newV) { newV = cand; from = r; }
    }
    if (newV == oldV) continue;
    // side effects
    switch (s->type) {
      case T_SWORD:
        code_jsl(s_fast + 0x00D308); // decompress sword graphics
        code_jsl(s_fast + 0x1BED03); // sword palette
        break;
      case T_SHIELD:
        code_jsl(s_fast + 0x00D348);
        code_jsl(s_fast + 0x1BED29);
        break;
      case T_GLOVES: case T_ARMOR:
        code_jsl(s_fast + 0x1BEDF9);
        break;
      case T_WORLD:
        if (newV >= 2 && oldV < 2) {
          // the rain stops: reload sprite graphics properties, and finish like a mirror warp does
          code_jsl(s_fast + 0x00FC62);
          if (L.module == 0x09 && L.sub == 0x00) {
            R[0x1D] = 0; R[0x8C] = 0;
            code_jsl(s_fast + 0x02B186);
            code_lda_sta(0x05, 0x012D);
          }
        }
        break;
      case T_PROG1:
        if ((newV & 0x01) && SR[0x3CC] == 0x05) SR[0x3CC] = 0;
        break;
      case T_PROG2:
        if ((newV & 0x20) && (SR[0x3CC] == 0x07 || SR[0x3CC] == 0x08)) SR[0x3CC] = 0;
        if ((newV & 0x10) && SR[0x3CC] == 0x0C) SR[0x3CC] = 0;
        break;
      case T_RANDOITEMS:
        if (!(oldV & 0x10) && (newV & 0x10) && SR[0x344] == 0) SR[0x344] = 2;
        if (!(oldV & 0x20) && (newV & 0x20) && SR[0x344] == 0) SR[0x344] = 1;
        break;
    }
    if (size == 2) wr16(SR + s->offs, (uint16_t)newV); else SR[s->offs] = (uint8_t)newV;
    // tell the player
    if (s->names && from) {
      char nm[48];
      if (s->type == T_OR || s->type == T_RANDOITEMS) {
        for (int b = 0; b < 8; b++) {
          if (!(oldV & (1 << b)) && (newV & (1 << b))) {
            name_at(s->names, b, nm, sizeof(nm));
            if (nm[0]) game_notify("%s found %s", from->name, nm);
          }
        }
      } else if (s->type == T_BOTTLE || s->type == T_MAX16) {
        game_notify("%s found %s", from->name, s->names);
      } else {
        name_at(s->names, newV - 1, nm, sizeof(nm));
        if (nm[0]) game_notify("%s found the %s", from->name, nm);
      }
    }
  }

  // heart containers and pieces
  if (g_room.shareHearts) {
    int oldV = (SR[0x36C] & ~7) | (SR[0x36B] & 3);
    int newV = oldV;
    const Remote *from = NULL;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Remote *r = &s_rem[i];
      if (!rem_playing(r) || !sram_have(r, 0x36B, 2)) continue;
      int rv = (r->sram[0x36C] & ~7) | (r->sram[0x36B] & 3);
      if (rv > newV && (rv & ~7) <= 0xA0) { newV = rv; from = r; }
    }
    if (newV > oldV) {
      int gained = (newV & ~7) - (oldV & ~7);
      SR[0x36C] = newV & ~7;
      SR[0x36B] = newV & 3;
      if (gained > 0) {
        int fill = SR[0x372] + gained;
        SR[0x372] = fill > 0xA0 ? 0xA0 : fill;
        game_notify("%s found a Heart Container", from->name);
      } else {
        game_notify("%s found a Piece of Heart", from->name);
      }
    }
  }
}

static void merge_rooms(void) {
  if (L.bad) return;
  for (int a = 0; a < 0x128; a++) {
    uint16_t mask = 0xFFFF;
    if (a == 0x035) mask = 0xFF7F;
    if (s_swampMasks) {
      if (a == 0x10B) mask = 0xFF7F;
      if (a == 0x028) mask = 0xFEFF;
    }
    uint16_t oldV = rd16(SR + a * 2), newV = oldV;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Remote *r = &s_rem[i];
      if (!rem_playing(r) || !sram_have(r, a * 2, 2)) continue;
      newV |= rd16(r->sram + a * 2) & mask;
    }
    if (newV == oldV) continue;
    wr16(SR + a * 2, newV);
    // keep the room we are standing in consistent, so an opened chest can not be opened twice
    if (L.module == 0x07 && L.uwRoom == a && R[0x1B]) {
      R[0x0403] |= (newV >> 4) & 0xFF;
    }
  }
}

static void merge_overworld(void) {
  if (L.bad) return;
  for (int a = 0; a < 0x82; a++) {
    uint8_t mask = 0xFF;
    if (s_swampMasks && (a == 0x3B || a == 0x7B)) mask = 0xDF;
    uint8_t oldV = SR[0x280 + a], newV = oldV;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Remote *r = &s_rem[i];
      if (!rem_playing(r) || !sram_have(r, 0x280 + a, 1)) continue;
      newV |= r->sram[0x280 + a] & mask;
    }
    if (newV == oldV) continue;
    SR[0x280 + a] = newV;
    if (a == 0x5B && !(oldV & 0x20) && (newV & 0x20)) {
      if (L.owRoom == 0x5B && !R[0x1B] && L.module == 0x09) code_jsl(s_fast + 0x1BC2A7); // open the pyramid hole now
      game_notify("The Pyramid has been opened");
    }
  }
}

static void merge_extras(void) {
  if (!s_isDoor || L.bad) return;
  uint8_t *ex = R + EX_BASE;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Remote *r = &s_rem[i];
    if (!rem_playing(r)) continue;
    for (int j = 0; j < 0x4A0; j++) if (r->exHave[j >> 4]) ex[j] |= r->ex[j];
    for (int j = 0x4A0; j < 0x500; j++) if (r->exHave[j >> 4] && r->ex[j] > ex[j]) ex[j] = r->ex[j];
  }
}

// ------------------------------------------------------------------ keys and bombs
static int key_eff(int d) {
  // the key count the player really has for dungeon d right now
  if (L.module == 0x07 && R[0x1B] && L.dungeon != 0xFF && L.dungeon < 0x20) {
    int cur = L.dungeon >> 1;
    bool same = cur == d || (cur < 2 && d < 2);
    if (same && SR[0x36F] != 0xFF) return SR[0x36F];
  }
  return SR[0x37C + d];
}

static void key_set(int d, int v) {
  if (v < 0) v = 0;
  if (v > 99) v = 99;
  SR[0x37C + d] = (uint8_t)v;
  if (d < 2) { SR[0x37C] = (uint8_t)v; SR[0x37D] = (uint8_t)v; }
  if (L.module == 0x07 && R[0x1B] && L.dungeon != 0xFF && L.dungeon < 0x20) {
    int cur = L.dungeon >> 1;
    if ((cur == d || (cur < 2 && d < 2)) && SR[0x36F] != 0xFF) SR[0x36F] = (uint8_t)v;
  }
}

static bool keys_trackable(void) {
  // key counts move around while rooms load and while the save is being written
  if (L.bad) return false;
  return (L.module == 0x07 || L.module == 0x09 || L.module == 0x0B) && L.sub == 0x00;
}

static void track_keys_bombs(void) {
  if (!keys_trackable()) return;
  if (!L.keysInit) {
    for (int d = 0; d < 14; d++) L.keysLast[d] = key_eff(d);
    L.bombsLast = SR[0x343];
    L.bombsPending = 0;
    L.bombsOut = 0;
    L.keysInit = true;
    // adopt the group's key counts
    uint8_t m = M_KEYREQ;
    uint32_t best = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (rem_playing(&s_rem[i]) && (!best || s_rem[i].cid < best)) best = s_rem[i].cid;
    if (best && g_room.shareKeys) net_send_rel(best, &m, 1);
    return;
  }
  if (g_room.shareKeys) {
    for (int d = 0; d < 14; d++) {
      int v = key_eff(d);
      int delta = v - L.keysLast[d];
      if (delta != 0) {
        L.keysLast[d] = v;
        if (d == 1) continue; // sewers and castle move together, reported as dungeon 0
        uint8_t m[3] = {M_KEYS, (uint8_t)d, (uint8_t)(int8_t)delta};
        net_send_rel(0, m, 3);
      }
    }
  }
  int bombs = SR[0x343];
  if (bombs > L.bombsLast) {
    int d = bombs - L.bombsLast;
    int mine = d;
    if (L.bombsPending > 0) {
      int fromRemote = d < L.bombsPending ? d : L.bombsPending;
      L.bombsPending -= fromRemote;
      mine -= fromRemote;
    }
    if (mine > 0) { L.bombsOut += mine; L.bombsOutAge = 0; }
  }
  if (L.bombsOut > 0 && (SR[0x375] == 0 || ++L.bombsOutAge > 30)) {
    if (g_room.shareBombs) {
      uint8_t m[2] = {M_BOMBS, (uint8_t)L.bombsOut};
      net_send_rel(0, m, 2);
    }
    L.bombsOut = 0;
  }
  if (SR[0x375] == 0) L.bombsPending = 0;
  L.bombsLast = bombs;
  // bombs handed over by the others go through the game's own refill counter
  if (s_evBombs > 0) {
    int add = s_evBombs;
    s_evBombs = 0;
    int f = SR[0x375] + add;
    if (f > 99) f = 99;
    L.bombsPending += f - SR[0x375];
    SR[0x375] = (uint8_t)f;
  }
}

// ------------------------------------------------------------------ PvP
static void calc_action_hitbox_pose(int idx, uint32_t tblX) {
  int a = (int8_t)R[0x45] + (int8_t)emu_rom8(tblX + idx);
  L.act.x = (int16_t)(rd16(R + 0x22) + a);
  uint8_t m44 = R[0x44];
  a = (int8_t)m44 + (int8_t)emu_rom8(tblX + 0x82 + idx);
  L.act.y = (int16_t)(rd16(R + 0x20) + a);
  L.act.w = emu_rom8(tblX + 0x41 + idx);
  L.act.h = emu_rom8(tblX + 0xC3 + idx);
  L.act.active = (m44 != 0x80);
}

static void calc_action_hitbox(void) {
  uint32_t tblX = s_fast + 0x06F473, tblToggle = s_fast + 0x06F577, tblDash = s_fast + 0x06F58C;
  L.act.active = false;
  if (R[0x0372] != 0) { // dashing
    int y = R[0x2F] >> 1;
    int offs = (int16_t)(emu_rom8(tblDash + 2 + y) | (emu_rom8(tblDash + 6 + y) << 8));
    L.act.x = (int16_t)(rd16(R + 0x22) + offs);
    offs = (int16_t)(emu_rom8(tblDash + 10 + y) | (emu_rom8(tblDash + y) << 8));
    L.act.y = (int16_t)(rd16(R + 0x20) + offs);
    L.act.w = 16; L.act.h = 16;
    L.act.active = (R[0x44] != 0x80);
    return;
  }
  if ((R[0x0301] & 0x0A) != 0 || (R[0x037A] & 0x10) != 0) { calc_action_hitbox_pose(0, tblX); return; }
  uint8_t m3c = R[0x3C];
  if ((int8_t)m3c < 0) { // spin attack
    L.act.x = (int16_t)(rd16(R + 0x22) - 0x0E);
    L.act.y = (int16_t)(rd16(R + 0x20) - 0x0A);
    L.act.w = 0x2C; L.act.h = 0x2D;
    L.act.active = true;
    return;
  }
  if (emu_rom8(tblToggle + m3c) != 0) return;
  calc_action_hitbox_pose(((R[0x2F] << 3) + m3c + 1) & 0xff, tblX);
}

static void fetch_pvp(void) {
  L.hit.x = L.x + 4; L.hit.y = L.y + 8; L.hit.w = 8; L.hit.h = 8;
  L.hit.active = L.module != 0x12 && (L.module == 0x07 || L.module == 0x09 || L.module == 0x0B);
  L.swordTime = R[0x3C];
  L.swordType = SR[0x359];
  L.itemUsed = R[0x0301];
  L.roomLevel = R[0xEE];
  L.act.active = false;
  if (g_room.pvp && L.hit.active) calc_action_hitbox();
}

static void send_attack(Remote *r, int damage, int dx, int dy, int swordTime, int mode) {
  uint8_t m[8] = {M_PVP, (uint8_t)damage, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)(damage / 2), (uint8_t)swordTime, (uint8_t)mode, 0};
  net_send_rel(r->cid, m, 7);
  r->attackCooldown = 20;
}

static void attack_pvp(void) {
  if (!g_room.pvp || L.bad || !L.hit.active) return;
  int recoilDx = 0, recoilDy = 0, recoilTimer = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Remote *r = &s_rem[i];
    if (r->attackCooldown > 0) r->attackCooldown--;
    if (!rem_playing(r) || !locations_equal(L.actual, r->location)) continue;
    if (r->roomLevel != L.roomLevel && (L.actual & 0x010000)) continue;
    if (L.act.active && r->hit.active && r->attackCooldown == 0 && hit_intersects(&L.act, &r->hit)) {
      int sword = L.swordType;
      if (sword == 0xFF) sword = 0;
      int damage = 0;
      if (L.itemUsed != 0) sword = 1;
      if (sword > 0 && L.swordTime != 0) {
        damage = 4 << (sword - 1);
        if (L.swordTime >= 0x09 && L.swordTime < 0x80) damage >>= 1;       // poke
        else if (L.swordTime == 0x90) damage <<= 1;                          // spin attack
      }
      if (L.itemUsed & 0x02) damage = 10 * 8;                               // hammer
      if (R[0x0372] != 0 && damage == 0) damage = 4;                        // dash bonk
      if (damage == 1) damage = 2;
      if (damage > 0 || R[0x0372] != 0) {
        double dx = (r->hit.x + r->hit.w / 2) - (L.act.x + L.act.w / 2);
        double dy = (r->hit.y + r->hit.h / 2) - (L.act.y + L.act.h / 2);
        double mag = sqrt(dx * dx + dy * dy);
        if (mag == 0) mag = 1;
        send_attack(r, damage, (int)(dx * (16 + damage * 0.25) / mag), (int)(dy * (16 + damage * 0.25) / mag), L.swordTime, 0);
        if (L.swordTime >= 0x09 && L.swordTime < 0x80 && R[0x0372] == 0) {
          // a poke bounces the attacker back a little
          R[0x3C] = 0; R[0x3A] = 0;
          recoilDx -= (int)(dx * 16 / mag);
          recoilDy -= (int)(dy * 16 / mag);
          recoilTimer = 4;
        }
      }
    }
    // swords clashing
    if (L.act.active && r->act.active && hit_intersects(&L.act, &r->act) && R[0x0372] == 0 && L.swordTime != 0) {
      double dx = (L.hit.x + 4) - (r->hit.x + 4), dy = (L.hit.y + 4) - (r->hit.y + 4);
      double mag = sqrt(dx * dx + dy * dy);
      if (mag == 0) mag = 1;
      recoilDx += (int)(dx * 16 / mag);
      recoilDy += (int)(dy * 16 / mag);
      recoilTimer = 4;
      if (R[0x0FAC] == 0) {
        R[0x0FAC] = 0x05;
        R[0x0FAD] = (uint8_t)((L.x & 0xFF) + R[0x45]);
        R[0x0FAE] = (uint8_t)((L.y & 0xFF) + R[0x44]);
        R[0x0B68] = L.roomLevel;
      }
      R[0x012E] = 0x05;
    }
    // projectiles: arrows, sword beams, rods, boomerang, hookshot
    if (r->hit.active && r->attackCooldown == 0) {
      for (int n = 0; n < 10; n++) {
        uint8_t mode = R[0x0BF0 + 9 * 10 + n];
        if (mode != 0x01 && mode != 0x02 && mode != 0x04 && mode != 0x09 && mode != 0x0B && mode != 0x0C && mode != 0x1F && mode != 0x31) continue;
        int px = R[0x0BF0 + 2 * 10 + n] | (R[0x0BF0 + 4 * 10 + n] << 8);
        int py = R[0x0BF0 + 1 * 10 + n] | (R[0x0BF0 + 3 * 10 + n] << 8);
        Hitbox hb = {true, (int16_t)px, (int16_t)py, 8, 8};
        int k = R[0x0BF0 + 13 * 10 + n];
        if (k < 12) {
          if (mode == 0x0C) k |= 0x08;
          uint32_t tbl = s_fast + 0x088E7D;
          hb.x = (int16_t)px + (int8_t)emu_rom8(tbl + k);
          hb.w = emu_rom8(tbl + 12 + k);
          hb.y = (int16_t)py + (int8_t)emu_rom8(tbl + 24 + k);
          hb.h = emu_rom8(tbl + 36 + k);
        }
        if (!hit_intersects(&hb, &r->hit)) continue;
        int damage = 0;
        double dx = (int8_t)R[0x0BF0 + 6 * 10 + n], dy = (int8_t)R[0x0BF0 + 5 * 10 + n];
        switch (mode) {
          case 0x09: damage = SR[0x340] >= 3 ? 20 * 8 : 2 * 8; break;
          case 0x0C: damage = 8; if (L.swordType > 0 && L.swordType < 5) damage <<= (L.swordType - 1); break;
          case 0x0B: case 0x02: damage = 4 * 8; break;
          case 0x01: case 0x1F: damage = 2 * 8; break;
          case 0x31: damage = 2 * 8; dx = px - (L.hit.x + 4); dy = py - (L.hit.y + 4); break;
          default: damage = 0; break;
        }
        if (mode == 0x04) continue;
        double mag = sqrt(dx * dx + dy * dy);
        if (mag == 0) mag = 1;
        if (damage > 255) damage = 255;
        send_attack(r, damage, (int)(dx * (16 + damage * 0.25) / mag), (int)(dy * (16 + damage * 0.25) / mag), 0, mode);
        // the projectile is spent
        switch (mode) {
          case 0x09: R[0x0BF0 + 90 + n] = 0x0A; R[0x03B1 + n] = 1; R[0x0BF0 + 110 + n] = 0; break;
          case 0x1F: R[0x0BF0 + 100 + n] = 1; R[0x0BF0 + 50 + n] = (uint8_t)(-(int8_t)R[0x0BF0 + 50 + n]); R[0x0BF0 + 60 + n] = (uint8_t)(-(int8_t)R[0x0BF0 + 60 + n]); break;
          case 0x0C: R[0x0BF0 + 90 + n] = 0x04; R[0x0BF0 + 120 + n] = 0x07; R[0x0BF0 + 160 + n] = 0x10; break;
          case 0x0B: R[0x0BF0 + 90 + n] = 0x11; R[0x0BF0 + 160 + n] = 0x10; R[0x0BF0 + 110 + n] = 0; R[0x03B1 + n] = 4; break;
          case 0x02: if (R[0x0BF0 + 100 + n] == 0) { R[0x0BF0 + 100 + n] = 1; R[0x0BF0 + 120 + n] = 0x1F; R[0x0BF0 + 160 + n] = 0x08; } break;
          case 0x31: break;
          default: R[0x0BF0 + 90 + n] = 0; break;
        }
        break;
      }
    }
  }
  if (recoilDx != 0 || recoilDy != 0) {
    R[0x46] = (uint8_t)recoilTimer; R[0x02C7] = (uint8_t)recoilTimer;
    R[0x28] = (uint8_t)(int8_t)recoilDx; R[0x27] = (uint8_t)(int8_t)recoilDy;
    R[0x29] = 0; R[0xC7] = 0;
    R[0x24] = 0; R[0x25] = 0;
  }
}

static void apply_pvp(void) {
  if (s_evPvpCount == 0) return;
  int n = s_evPvpCount;
  s_evPvpCount = 0;
  if (!g_room.pvp || L.bad || !L.hit.active) return;
  if (R[0x037B] != 0) return;  // invincible (cape, byrna)
  if (R[0x4D] == 1) return;    // already knocked back
  if (R[0x031F] != 0) return;  // blinking after a hit
  int damage = 0, dx = 0, dy = 0, dz = 0;
  int armor = SR[0x35B];
  if (armor > 2) armor = 2;
  for (int i = 0; i < n; i++) {
    int d = s_evPvp[i].damage;
    if (s_evPvp[i].swordTime != 0 || s_evPvp[i].mode == 0x09 || s_evPvp[i].mode == 0x0C) d >>= armor;
    damage += d;
    dx += s_evPvp[i].dx; dy += s_evPvp[i].dy; dz += s_evPvp[i].dz;
  }
  if (damage > 0xA0) damage = 0xA0;
  if (dx > 127) dx = 127;
  if (dx < -127) dx = -127;
  if (dy > 127) dy = 127;
  if (dy < -127) dy = -127;
  if (dz > 40) dz = 40;
  if (damage) R[0x0373] = (uint8_t)damage;
  R[0x4D] = 0x01;
  R[0x46] = 0x20; R[0x02C7] = 0x20;
  R[0x28] = (uint8_t)(int8_t)dx; R[0x27] = (uint8_t)(int8_t)dy;
  R[0x29] = (uint8_t)dz; R[0xC7] = (uint8_t)dz;
  R[0x24] = 0; R[0x25] = 0;
}

// ------------------------------------------------------------------ sprites
typedef struct Oam { int x, y; uint16_t chr; uint8_t pal, prio; bool hflip, vflip, big, on; } Oam;

static void decode_oam(Oam *o, int i) {
  Ppu *ppu = g_snes->ppu;
  uint16_t w0 = ppu->oam[i * 2], w1 = ppu->oam[i * 2 + 1];
  uint8_t hi = (ppu->highOam[i >> 2] >> ((i & 3) * 2)) & 3;
  o->x = (w0 & 0xff) | ((hi & 1) << 8);
  if (o->x >= 256) o->x -= 512;
  o->y = w0 >> 8;
  o->chr = (w1 & 0xff) | ((w1 >> 8) & 1) << 8;
  uint8_t attr = w1 >> 8;
  o->pal = (attr >> 1) & 7;
  o->prio = (attr >> 4) & 3;
  o->hflip = attr & 0x40;
  o->vflip = attr & 0x80;
  o->big = hi & 2;
  o->on = (o->y != 0xF0);
}

static void read_tile(uint8_t *dst, int chr) {
  Ppu *ppu = g_snes->ppu;
  uint16_t base = (chr & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
  for (int i = 0; i < 16; i++) {
    uint16_t w = ppu->vram[(base + (chr & 0xff) * 16 + i) & 0x7fff];
    dst[i * 2] = w & 0xff; dst[i * 2 + 1] = w >> 8;
  }
}

static void add_sprite(const Oam *o) {
  if (L.nspr >= MAX_SPR) return;
  SprRec *s = &L.spr[L.nspr];
  uint8_t *t = L.sprTile[L.nspr];
  int len = 32;
  read_tile(t, o->chr);
  if (o->big) {
    read_tile(t + 32, (o->chr & 0x100) | ((o->chr + 1) & 0xff));
    read_tile(t + 64, (o->chr & 0x100) | ((o->chr + 0x10) & 0xff));
    read_tile(t + 96, (o->chr & 0x100) | ((o->chr + 0x11) & 0xff));
    len = 128;
  }
  s->key = (fnv(t, len) & 0x7fffffff) | (o->big ? 0x80000000u : 0);
  if ((s->key & 0x7fffffff) == 0) s->key |= 1;
  s->wx = (int16_t)(o->x + L.xoffs);
  int y = o->y;
  if (y >= 0xF0) y -= 256;
  s->wy = (int16_t)(y + L.yoffs);
  s->flags = (o->big ? 1 : 0) | (o->hflip ? 2 : 0) | (o->vflip ? 4 : 0) | (o->prio << 3) | (o->pal << 5);
  L.palUsed[o->pal] = true;
  L.nspr++;
}

// Picks Link's sprites (body, sword, shield, shadow, held items, effects) out of OAM.
static void capture_sprites(void) {
  L.nspr = 0;
  memset(L.palUsed, 0, sizeof(L.palUsed));
  if (L.bad) return;
  static Oam o[128];
  for (int i = 0; i < 128; i++) decode_oam(&o[i], i);
  int start = rd16(R + 0x0352) >> 2;
  for (int j = 0; j < 12; j++) {
    int i = (start + j) & 0x7F;
    if (o[i].on) add_sprite(&o[i]);
  }
  if (L.module != 0x12) {
    for (int i = 0; i < 128; i++) {
      if (i >= start && i < start + 12) continue;
      if (!o[i].on) continue;
      int chr = o[i].chr;
      if (chr >= 0x100) continue;
      bool take = false;
      if (i > 0) {
        int p = o[i - 1].chr;
        if (chr == 0x26 && p == 0x126) continue;
        if (chr == 0x6c && (p == 0x46 || p == 0x44 || p == 0x42)) take = true;   // shadow under a lifted object
      }
      if (!take && ((chr >= 0xc8 && chr <= 0xca) || (chr >= 0xd8 && chr <= 0xda))) {
        if (i > 0 && i <= 0x7D) {
          int p = o[i - 1].chr, n1 = o[i + 1].chr, n2 = o[i + 2].chr;
          if ((chr == n1 && (n2 == 0x22 || n2 == 0x20)) || (chr == p && (n1 == 0x22 || n1 == 0x20))) take = true;
        }
        if (!take) continue;
      }
      if (!take && (L.state == 0x08 || L.state == 0x09 || L.state == 0x0A)) {
        if ((chr >= 0x40 && chr <= 0x4f) || (chr >= 0x60 && chr < 0x6c)) take = true;  // medallion effects
      }
      if (!take && L.state == 0x13) {
        if (chr == 0x09 || chr == 0x0a || chr == 0x19) take = true;                    // hookshot
      }
      if (!take) {
        switch (chr) {
          case 0x80: case 0x83: case 0xb7: case 0x81: case 0x82: case 0x8c: case 0x92: case 0x93: case 0xd6: case 0xd7:
          case 0x59: case 0xe2: case 0xf2: case 0x58: case 0x48: case 0x26: case 0x09: case 0x0a: case 0x86: case 0xa9:
          case 0x9b: case 0x0c: case 0x4a: case 0x46: case 0x44: case 0x42: case 0x20: case 0x22:
            take = true;
            break;
        }
      }
      if (!take && !(L.module == 0x07 && L.uwRoom == 0x00)) {
        switch (chr) {
          case 0x2a: case 0x2b: case 0x3a: case 0x3b: case 0x2c: case 0x2d: case 0x3c: case 0x3d: case 0x8d: case 0x9c:
          case 0x9d: case 0x8e: case 0xa0: case 0xa2: case 0xa4: case 0xa5: case 0xb6: case 0xcf: case 0xdf: case 0xe3:
          case 0xf3: case 0xb2: case 0xb3: case 0xe9: case 0xc4: case 0xc5: case 0xc6: case 0xd2: case 0xc2: case 0xc3:
          case 0xd3: case 0xd4: case 0xd5:
            take = true;
            break;
        }
      }
      if (take) add_sprite(&o[i]);
    }
  }
  Ppu *ppu = g_snes->ppu;
  for (int p = 0; p < 8; p++)
    if (L.palUsed[p]) memcpy(L.pal[p], &ppu->cgram[128 + p * 16], 32);
}

static bool sent_has(Remote *r, uint32_t key) {
  uint32_t h = (key * 2654435761u) >> 20;
  for (int i = 0; i < 4096; i++) {
    uint32_t k = r->sentKeys[(h + i) & 4095];
    if (k == key) return true;
    if (k == 0) return false;
  }
  return false;
}

static void sent_add(Remote *r, uint32_t key) {
  if (r->sentCount > 3000) { memset(r->sentKeys, 0, sizeof(r->sentKeys)); r->sentCount = 0; }
  uint32_t h = (key * 2654435761u) >> 20;
  for (int i = 0; i < 4096; i++) {
    uint32_t *k = &r->sentKeys[(h + i) & 4095];
    if (*k == 0) { *k = key; r->sentCount++; return; }
  }
}

static TileEnt *tile_find(uint32_t key) {
  uint32_t h = (key * 2654435761u) >> 19;
  for (int i = 0; i < TILE_SLOTS; i++) {
    TileEnt *e = &s_tiles[(h + i) & (TILE_SLOTS - 1)];
    if (e->key == key) return e;
    if (e->key == 0) return NULL;
  }
  return NULL;
}

static void decode_tile8(uint8_t *px, int stride, const uint8_t *t) {
  for (int y = 0; y < 8; y++) {
    uint8_t b0 = t[y * 2], b1 = t[y * 2 + 1], b2 = t[16 + y * 2], b3 = t[16 + y * 2 + 1];
    for (int x = 0; x < 8; x++) {
      int sh = 7 - x;
      px[y * stride + x] = ((b0 >> sh) & 1) | (((b1 >> sh) & 1) << 1) | (((b2 >> sh) & 1) << 2) | (((b3 >> sh) & 1) << 3);
    }
  }
}

static void tile_store(uint32_t key, const uint8_t *data) {
  if (tile_find(key)) return;
  if (s_tileCount > TILE_SLOTS * 3 / 4) {
    // start over and ask everyone to send their tiles again
    memset(s_tiles, 0, sizeof(TileEnt) * TILE_SLOTS);
    s_tileCount = 0;
    uint8_t m = M_TILERESET;
    net_send_rel(0, &m, 1);
  }
  uint32_t h = (key * 2654435761u) >> 19;
  for (int i = 0; i < TILE_SLOTS; i++) {
    TileEnt *e = &s_tiles[(h + i) & (TILE_SLOTS - 1)];
    if (e->key != 0) continue;
    e->key = key;
    if (key & 0x80000000u) {
      decode_tile8(e->px, 16, data);
      decode_tile8(e->px + 8, 16, data + 32);
      decode_tile8(e->px + 128, 16, data + 64);
      decode_tile8(e->px + 136, 16, data + 96);
    } else {
      decode_tile8(e->px, 8, data);
    }
    s_tileCount++;
    return;
  }
}

// ------------------------------------------------------------------ tilemap changes (cut bushes, opened doors, lifted pots...)
static bool tm_safe(void) {
  if (L.bad) return false;
  if (L.module == 0x09) {
    if (L.sub >= 0x01 && L.sub < 0x07) return false;
    if (L.sub >= 0x0d && L.sub < 0x16) return false;
    if (L.sub >= 0x20) return false;
    return true;
  } else if (L.module == 0x0B) {
    return L.sub == 0;
  } else if (L.module == 0x07) {
    switch (L.sub) {
      case 0x01: case 0x02: case 0x06: case 0x07: case 0x08: case 0x0e: case 0x10: case 0x12: case 0x13: case 0x15: case 0x18: case 0x19:
        return false;
    }
    return L.uwRoom != 0;
  }
  return false;
}

static void tm_reset(void) {
  for (int i = 0; i < 0x2000; i++) L.tm[i] = -1;
  L.tmCount = 0;
  L.tmLocation = L.actual;
  L.tmStamp = 0;
  L.tmDirty = false;
  L.tmPrevSafe = false;
}

static void tm_track(void) {
  if (L.actual != L.lastActual) tm_reset();
  bool safe = tm_safe();
  const uint8_t *t = R + 0x2000, *a = R + 0x12000;
  if (safe && L.tmPrevSafe && memcmp(L.tmSnapT, t, 0x4000) != 0) {
    bool uw = L.module == 0x07;
    for (int i = 0; i < 0x2000; i++) {
      uint16_t w = rd16(t + i * 2);
      if (w == L.tmSnapT[i]) continue;
      uint8_t at = uw ? a[i] : 0;
      int32_t v = w | (at << 16);
      if (L.tm[i] == -1) L.tmCount++;
      if (L.tm[i] != v) { L.tm[i] = v; L.tmDirty = true; }
    }
  }
  if (safe && L.tmPrevSafe && L.module == 0x07 && memcmp(L.tmSnapA, a, 0x2000) != 0) {
    for (int i = 0; i < 0x2000; i++) {
      if (a[i] == L.tmSnapA[i]) continue;
      if (L.sub == 0x16 && (a[i] == 0x66 || a[i] == 0x67)) continue; // crystal switch blocks, each game flips its own
      int32_t v = rd16(t + i * 2) | (a[i] << 16);
      if (L.tm[i] == -1) L.tmCount++;
      if (L.tm[i] != v) { L.tm[i] = v; L.tmDirty = true; }
    }
  }
  for (int i = 0; i < 0x2000; i++) L.tmSnapT[i] = rd16(t + i * 2);
  memcpy(L.tmSnapA, a, 0x2000);
  L.tmPrevSafe = safe;
}

static int tm_serialize(uint8_t *out, int cap) {
  // runs of changed tiles, horizontal or vertical, like alttpo's RTDS format
  static int32_t tmp[0x2000];
  memcpy(tmp, L.tm, sizeof(tmp));
  int n = 0, runs = 0;
  int countPos = n; n += 2;
  for (int m = 0; m < 0x2000; m += 0x1000) {
    for (int y = 0; y < 64; y++) {
      for (int x = 0; x < 64; x++) {
        int32_t tile = tmp[m + y * 64 + x];
        if (tile == -1) continue;
        int hc = 1, vc = 1; bool hs = true, vs = true;
        for (int k = x + 1; k < 64 && hc < 64; k++) { int32_t v = tmp[m + y * 64 + k]; if (v == -1) break; if (v != tile) hs = false; hc++; }
        for (int k = y + 1; k < 64 && vc < 64; k++) { int32_t v = tmp[m + k * 64 + x]; if (v == -1) break; if (v != tile) vs = false; vc++; }
        bool vert = vc > hc;
        int cnt = vert ? vc : hc; bool same = vert ? vs : hs;
        if (n + 3 + (same ? 3 : cnt * 3) > cap) goto done;
        uint16_t offs = (uint16_t)(m + y * 64 + x) | (same ? 0x8000 : 0) | (vert ? 0x4000 : 0);
        wr16(out + n, offs); n += 2;
        out[n++] = (uint8_t)cnt;
        for (int k = 0; k < cnt; k++) {
          int idx = vert ? m + (y + k) * 64 + x : m + y * 64 + x + k;
          if (!same || k == 0) { out[n++] = tmp[idx] & 0xff; out[n++] = (tmp[idx] >> 8) & 0xff; out[n++] = (tmp[idx] >> 16) & 0xff; }
          tmp[idx] = -1;
        }
        runs++;
      }
    }
  }
done:
  wr16(out + countPos, (uint16_t)runs);
  return n;
}

static void tm_send(uint32_t cid) {
  static uint8_t buf[40000];
  int n = 0;
  buf[n++] = M_TILEMAP;
  wr32(buf + n, L.tmStamp); n += 4;
  wr32(buf + n, L.tmLocation); n += 4;
  n += tm_serialize(buf + n, sizeof(buf) - n);
  net_send_rel(cid, buf, n);
  app_log("tilemap: sent %d changed tiles (stamp %u, %d bytes) to %08x", L.tmCount, L.tmStamp, n, cid);
}

static void tm_write_vram(int i, int32_t c, int wramTop, int wramLeft, bool overworld) {
  Ppu *ppu = g_snes->ppu;
  uint16_t tile = c & 0xffff;
  if (overworld) {
    int top = (i & 0x0FFF) >> 6, left = i & 0x3f;
    if (top < wramTop || top > wramTop + 16 || left < wramLeft || left > wramLeft + 16) return;
    uint16_t addr = (uint16_t)(i << 1), vaddr = 0;
    if ((addr & 0x003F) >= 0x0020) vaddr = 0x0400;
    if ((addr & 0x0FFF) >= 0x0800) vaddr += 0x0800;
    vaddr += (addr & 0x001F);
    vaddr += (addr & 0x0780) >> 1;
    uint32_t a = s_fast + 0x0F8000 + ((uint32_t)tile << 3);
    for (int k = 0; k < 4; k++) {
      uint16_t w = emu_rom8(a + k * 2) | (emu_rom8(a + k * 2 + 1) << 8);
      ppu->vram[(vaddr + (k & 1) + (k >> 1) * 0x20) & 0x7fff] = w;
    }
  } else {
    int vram = 0;
    int addr = (i & 0x0FFF) << 1;
    if (addr & 0x1000) { vram += 0x1000; addr ^= 0x1000; }
    if (addr & 0x40) { vram += 0x800; addr ^= 0x040; }
    vram += (addr - ((addr & 0xFF80) >> 1));
    vram = (vram >> 1) | (i & 0x1000);
    ppu->vram[vram & 0x7fff] = tile;
  }
}

static void tm_apply(void) {
  if (!tm_safe()) return;
  bool overworld = L.module != 0x07;
  int topleft = overworld ? (rd16(R + 0x84) >> 1) : 0;
  int wramTop = topleft >> 6, wramLeft = topleft & 0x3f;
  bool changed = false;
  for (int p = 0; p < MAX_PLAYERS; p++) {
    Remote *r = &s_rem[p];
    if (!rem_playing(r) || r->tmCount == 0) continue;
    if (!locations_equal(L.actual, r->location) || !locations_equal(L.actual, r->tmLocation)) continue;
    bool adopt = r->tmStamp > L.tmStamp;
    for (int j = 0; j < r->tmCount; j++) {
      TmRun *run = &r->tmRuns[j];
      int stride = run->vertical ? 0x40 : 1;
      int addr = run->offs;
      for (int n = 0; n < run->count; n++, addr += stride) {
        if (addr >= 0x2000) break;
        if (adopt) {
          int32_t c = (int32_t)run->tiles[run->same ? 0 : n];
          if (L.tm[addr] != c) {
            if (L.tm[addr] == -1) L.tmCount++;
            L.tm[addr] = c;
            wr16(R + 0x2000 + addr * 2, c & 0xffff);
            L.tmSnapT[addr] = c & 0xffff;
            if (!overworld) { R[0x12000 + addr] = (c >> 16) & 0xff; L.tmSnapA[addr] = (c >> 16) & 0xff; }
            changed = true;
          }
        }
        if (L.tm[addr] != -1) tm_write_vram(addr, L.tm[addr], wramTop, wramLeft, overworld);
      }
    }
    if (adopt) L.tmStamp = r->tmStamp;
  }
  (void)changed;
}

// ------------------------------------------------------------------ sending
static void send_hello(uint32_t cid) {
  uint8_t m[96];
  int n = 0;
  m[n++] = M_HELLO;
  m[n++] = PROTO_VERSION;
  memset(m + n, 0, 13); snprintf((char *)m + n, 13, "%s", g_cfg.name); n += 13;
  m[n++] = (uint8_t)g_cfg.color;
  wr32(m + n, s_romCrc); n += 4;
  memcpy(m + n, &g_room, sizeof(g_room)); n += sizeof(g_room);
  m[n++] = s_running ? 1 : 0;
  wr32(m + n, (uint32_t)s_hostRomLen); n += 4;
  memset(m + n, 0, 32); snprintf((char *)m + n, 32, "%s", s_hostName); n += 32;
  net_send_rel(cid, m, n);
}

static void send_state(void) {
  uint8_t full[1400], lite[64];
  int n = 0;
  uint8_t flags = (L.inGame ? 1 : 0) | (L.hit.active ? 2 : 0) | (L.act.active ? 4 : 0) | (L.module == 0x12 ? 16 : 0);
  full[n++] = M_STATE;
  full[n++] = flags;
  full[n++] = L.module; full[n++] = L.sub;
  wr32(full + n, L.location); n += 4;
  wr16(full + n, (uint16_t)L.x); n += 2;
  wr16(full + n, (uint16_t)L.y); n += 2;
  full[n++] = L.inGame ? SR[0x36D] : 0;
  full[n++] = L.inGame ? SR[0x36C] : 0;
  full[n++] = L.inGame ? SR[0x35B] : 0;
  full[n++] = L.swordType;
  full[n++] = L.roomLevel;
  wr32(full + n, L.tmStamp); n += 4;
  if (L.act.active) {
    wr16(full + n, (uint16_t)L.act.x); n += 2;
    wr16(full + n, (uint16_t)L.act.y); n += 2;
    full[n++] = (uint8_t)L.act.w; full[n++] = (uint8_t)L.act.h;
    full[n++] = L.swordTime; full[n++] = L.itemUsed;
  }
  int liteLen = n;
  memcpy(lite, full, n);
  lite[liteLen++] = 0;
  full[n++] = (uint8_t)L.nspr;
  for (int i = 0; i < L.nspr; i++) {
    wr16(full + n, (uint16_t)L.spr[i].wx); n += 2;
    wr16(full + n, (uint16_t)L.spr[i].wy); n += 2;
    wr32(full + n, L.spr[i].key); n += 4;
    full[n++] = L.spr[i].flags;
  }
  for (int p = 0; p < MAX_PLAYERS; p++) {
    Remote *r = &s_rem[p];
    if (!r->used || !r->hello) continue;
    bool direct = net_peer_direct(r->cid);
    bool near = L.inGame && rem_playing(r) && (locations_equal(L.location, r->location) || locations_equal(L.actual, r->location));
    if (near) {
      if (!direct && (L.frame % 6) != 0) continue;
      // tiles and palettes this player has not seen yet go first (reliable)
      uint8_t tb[1200];
      int tn = 0;
      for (int i = 0; i < L.nspr; i++) {
        uint32_t key = L.spr[i].key;
        if (sent_has(r, key)) continue;
        int len = (key & 0x80000000u) ? 128 : 32;
        if (tn + 5 + len > (int)sizeof(tb)) { net_send_rel(r->cid, tb, tn); tn = 0; }
        if (tn == 0) tb[tn++] = M_TILES;
        wr32(tb + tn, key); tn += 4;
        memcpy(tb + tn, L.sprTile[i], len); tn += len;
        sent_add(r, key);
      }
      if (tn > 1) net_send_rel(r->cid, tb, tn);
      for (int pal = 0; pal < 8; pal++) {
        if (!L.palUsed[pal]) continue;
        if (r->sentPalValid[pal] && memcmp(r->sentPal[pal], L.pal[pal], 32) == 0) continue;
        uint8_t pm[40];
        pm[0] = M_PAL; pm[1] = (uint8_t)pal;
        memcpy(pm + 2, L.pal[pal], 32);
        net_send_rel(r->cid, pm, 34);
        memcpy(r->sentPal[pal], L.pal[pal], 32);
        r->sentPalValid[pal] = true;
      }
      net_send(r->cid, full, n);
    } else {
      if ((L.frame % (direct ? 6 : 20)) != (uint32_t)(p % 6)) continue;
      net_send(r->cid, lite, liteLen);
    }
  }
}

static void send_sram(void) {
  if (!L.inGame) return;
  uint64_t now = app_ms();
  int slot = L.frame % 8;
  for (int c = 0; c < NUM_CHUNKS; c++) {
    if ((c % 8) != slot) continue;
    Chunk *ch = &s_chunks[c];
    if (ch->randoOnly && !s_isRando) continue;
    if (ch->doorOnly && !s_isDoor) continue;
    const uint8_t *src = ch->space == 0 ? SR + ch->start : R + EX_BASE + ch->start;
    int count = ch->count;
    if (ch->space == 0 && ch->start == 0x128) count = 0x250 - 0x128;
    uint32_t h = fnv(src, count);
    if (h == ch->lastHash && now - ch->lastSent < 2500) continue;
    ch->lastHash = h;
    ch->lastSent = now;
    uint8_t m[1300];
    int n = 0;
    m[n++] = M_SRAM;
    m[n++] = ch->space;
    wr16(m + n, ch->start); n += 2;
    wr16(m + n, (uint16_t)count); n += 2;
    memcpy(m + n, src, count); n += count;
    net_send(0, m, n);
  }
}

// ------------------------------------------------------------------ the per-frame hook
static void fetch_local(void) {
  L.module = R[0x10]; L.sub = R[0x11]; L.subsub = R[0xB0];
  L.bad = is_bad_time(L.module, L.sub);
  L.wasInGame = L.inGame;
  L.inGame = !L.bad;
  L.state = R[0x5D];
  bool dark = R[0x0FFF] != 0, indoors = R[0x1B] != 0;
  L.owRoom = rd16(R + 0x8A);
  L.uwRoom = rd16(R + 0xA0);
  L.dungeon = rd16(R + 0x040C);
  L.lastActual = L.actual;
  L.actual = ((dark ? 1u : 0) << 17) | ((indoors ? 1u : 0) << 16) | (indoors ? L.uwRoom : L.owRoom);
  if (L.bad) return;
  if (can_sample_location() && !in_transition()) {
    L.lastLocation = L.location;
    L.location = L.actual;
  }
  L.x = (int16_t)rd16(R + 0x22);
  L.y = (int16_t)rd16(R + 0x20);
  L.xoffs = (int16_t)(rd16(R + 0xE2) - rd16(R + 0x011A));
  L.yoffs = (int16_t)(rd16(R + 0xE8) - rd16(R + 0x011C));
}

static void on_main(void) {
  if (!s_running) return;
  R = emu_ram();
  L.frame++;
  s_codeLen = 0;
  fetch_local();
  if (!L.inGame) {
    L.keysInit = false;
    if (L.wasInGame) tm_reset();
  }
  if (s_online) {
    if (L.inGame) {
      capture_sprites();
      fetch_pvp();
      tm_track();
      apply_pvp();
      attack_pvp();
      track_keys_bombs();
      if ((L.frame & 15) == 0) merge_items();
      if ((L.frame & 31) == 8) merge_rooms();
      if ((L.frame & 31) == 24) merge_overworld();
      if ((L.frame & 31) == 4) merge_extras();
      tm_apply();
      if (L.tmDirty && (L.frame & 3) == 0) {
        L.tmDirty = false;
        L.tmStamp++;
        for (int p = 0; p < MAX_PLAYERS; p++) {
          Remote *r = &s_rem[p];
          if (rem_playing(r) && r->tmStamp >= L.tmStamp && locations_equal(r->tmLocation, L.tmLocation)) L.tmStamp = r->tmStamp + 1;
        }
        for (int p = 0; p < MAX_PLAYERS; p++) {
          Remote *r = &s_rem[p];
          if (rem_playing(r) && locations_equal(L.actual, r->location)) tm_send(r->cid);
        }
      }
      // a player who walks into our room gets the changes made so far
      for (int p = 0; p < MAX_PLAYERS; p++) {
        Remote *r = &s_rem[p];
        if (!rem_playing(r)) continue;
        bool here = locations_equal(L.actual, r->location);
        bool was = locations_equal(L.actual, r->lastSeenLocation);
        r->lastSeenLocation = r->location;
        if (here && !was && L.tmCount > 0 && L.tmStamp > 0) tm_send(r->cid);
      }
    } else {
      L.nspr = 0;
      L.hit.active = L.act.active = false;
      s_evPvpCount = 0;
    }
    send_state();
    send_sram();
  }
  emu_set_patch(s_code, s_codeLen);
}

// ------------------------------------------------------------------ receiving
static void recv_state(Remote *r, const uint8_t *d, int len) {
  if (len < 21) return;
  int n = 1;
  bool wasPlaying = (r->flags & 1) != 0;
  r->flags = d[n++];
  r->module = d[n++]; r->sub = d[n++];
  r->location = rd32(d + n); n += 4;
  r->x = (int16_t)rd16(d + n); n += 2;
  r->y = (int16_t)rd16(d + n); n += 2;
  r->health = d[n++]; r->maxHealth = d[n++]; r->armor = d[n++]; r->swordType = d[n++]; r->roomLevel = d[n++];
  uint32_t stamp = rd32(d + n); n += 4;
  if (stamp == 0 && r->tmStamp != 0) { r->tmStamp = 0; r->tmCount = 0; }
  r->hit.active = (r->flags & 2) != 0;
  r->hit.x = r->x + 4; r->hit.y = r->y + 8; r->hit.w = 8; r->hit.h = 8;
  r->act.active = false;
  if (r->flags & 4) {
    if (len < n + 8) return;
    r->act.active = true;
    r->act.x = (int16_t)rd16(d + n); n += 2;
    r->act.y = (int16_t)rd16(d + n); n += 2;
    r->act.w = d[n++]; r->act.h = d[n++];
    r->swordTime = d[n++]; r->itemUsed = d[n++];
  }
  if (len < n + 1) return;
  int ns = d[n++];
  if (ns > MAX_SPR || len < n + ns * 9) ns = 0;
  r->nspr = ns;
  for (int i = 0; i < ns; i++) {
    r->spr[i].wx = (int16_t)rd16(d + n); n += 2;
    r->spr[i].wy = (int16_t)rd16(d + n); n += 2;
    r->spr[i].key = rd32(d + n); n += 4;
    r->spr[i].flags = d[n++];
  }
  r->tState = app_ms();
  bool playing = (r->flags & 1) != 0;
  if (playing && !wasPlaying && r->hello && !r->wasInGame) {
    r->wasInGame = true;
    if (s_running) game_notify("%s entered Hyrule", r->name);
  }
}

static void recv_tilemap(Remote *r, const uint8_t *d, int len) {
  if (len < 11) return;
  r->tmStamp = rd32(d + 1);
  r->tmLocation = rd32(d + 5);
  int runs = rd16(d + 9);
  int n = 11;
  free(r->tmRuns);
  r->tmRuns = calloc(runs ? runs : 1, sizeof(TmRun));
  r->tmCount = 0;
  for (int i = 0; i < runs; i++) {
    if (n + 3 > len) break;
    TmRun *run = &r->tmRuns[r->tmCount];
    uint16_t o = rd16(d + n); n += 2;
    run->same = (o & 0x8000) != 0; run->vertical = (o & 0x4000) != 0;
    run->offs = o & 0x3FFF;
    run->count = d[n++];
    if (run->count > 64) break;
    int cnt = run->same ? 1 : run->count;
    if (n + cnt * 3 > len) break;
    for (int k = 0; k < cnt; k++) { run->tiles[k] = d[n] | (d[n + 1] << 8) | (d[n + 2] << 16); n += 3; }
    r->tmCount++;
  }
  app_log("tilemap: %s has %d runs (stamp %u)", r->name, r->tmCount, r->tmStamp);
}

static void serve_rom(uint32_t cid) {
  if (!s_hostRom || !g_baseRom) { uint8_t m = M_NOROM; net_send_rel(cid, &m, 1); return; }
  // the other player owns the same original ROM, so only the difference travels
  uint8_t *x = malloc(s_hostRomLen);
  for (int i = 0; i < s_hostRomLen; i++) x[i] = s_hostRom[i] ^ (i < g_baseRomLen ? g_baseRom[i] : 0);
  uLongf zl = compressBound(s_hostRomLen);
  uint8_t *m = malloc(zl + 64);
  int n = 0;
  m[n++] = M_ROMDATA;
  wr32(m + n, (uint32_t)s_hostRomLen); n += 4;
  wr32(m + n, crc32_buf(s_hostRom, s_hostRomLen)); n += 4;
  memset(m + n, 0, 32); snprintf((char *)m + n, 32, "%s", s_hostName); n += 32;
  compress2(m + n, &zl, x, s_hostRomLen, 9);
  n += (int)zl;
  app_log("sending seed patch to %08x (%d bytes)", cid, n);
  net_send_rel(cid, m, n);
  free(m); free(x);
}

static bool find_seed_by_crc(uint32_t crc, uint8_t **rom, int *len, char *name, int nameSize);

void game_net_recv(uint32_t cid, const uint8_t *d, int len, bool reliable) {
  (void)reliable;
  Remote *r = rem_find(cid);
  if (!r || len < 1) return;
  switch (d[0]) {
    case M_STATE:
      recv_state(r, d, len);
      break;
    case M_SRAM: {
      if (len < 6) break;
      int space = d[1], start = rd16(d + 2), count = rd16(d + 4);
      if (len < 6 + count) break;
      if (space == 0 && start + count <= SRAM_SIZE) {
        memcpy(r->sram + start, d + 6, count);
        for (int i = start; i < start + count; i += 1) r->sramHave[i >> 4] = 1;
      } else if (space == 1 && start + count <= EX_SIZE) {
        memcpy(r->ex + start, d + 6, count);
        for (int i = start; i < start + count; i += 1) r->exHave[i >> 4] = 1;
      }
      break;
    }
    case M_HELLO: {
      if (len < 1 + 1 + 13 + 1 + 4 + (int)sizeof(RoomSettings) + 1 + 4 + 32) break;
      int n = 1;
      int proto = d[n++];
      memcpy(r->name, d + n, 12); r->name[12] = 0; n += 13;
      if (!r->name[0]) strcpy(r->name, "Player");
      r->color = d[n++] % NUM_PLAYER_COLORS;
      r->romCrc = rd32(d + n); n += 4;
      RoomSettings rs;
      memcpy(&rs, d + n, sizeof(rs)); n += sizeof(rs);
      bool running = d[n++] != 0;
      n += 4;
      char seedName[33];
      memcpy(seedName, d + n, 32); seedName[32] = 0;
      bool first = !r->hello;
      r->hello = true;
      r->tState = app_ms();
      if (proto != PROTO_VERSION) {
        if (first) game_notify("%s runs a different version", r->name);
        if (s_joinState == JOIN_WAIT_INFO) { s_joinState = JOIN_ERROR; snprintf(s_joinError, sizeof(s_joinError), "The room uses a different game version."); }
        break;
      }
      if (first && s_running) game_notify("%s joined the room", r->name);
      if (s_joinState == JOIN_WAIT_INFO && running) {
        // this player tells us what the room is playing
        g_room = rs;
        s_joinCrc = r->romCrc;
        snprintf(s_joinName, sizeof(s_joinName), "%s", seedName[0] ? seedName : "seed");
        if (g_baseRom && crc32_buf(g_baseRom, g_baseRomLen) == r->romCrc) {
          s_joinRom = malloc(g_baseRomLen);
          memcpy(s_joinRom, g_baseRom, g_baseRomLen);
          s_joinRomLen = g_baseRomLen;
          snprintf(s_joinName, sizeof(s_joinName), "original");
          s_joinState = JOIN_READY;
        } else if (!getenv("ALTTPO_NO_SEED_LOOKUP") && find_seed_by_crc(r->romCrc, &s_joinRom, &s_joinRomLen, s_joinName, sizeof(s_joinName))) {
          s_joinState = JOIN_READY;
        } else {
          uint8_t m = M_ROMREQ;
          net_send_rel(cid, &m, 1);
          s_joinFrom = cid;
          s_joinState = JOIN_DOWNLOADING;
          s_joinProgress = 0;
          s_joinT = app_ms();
        }
      } else if (s_running && running && r->romCrc != s_romCrc && first) {
        game_notify("%s is playing a different game!", r->name);
      }
      break;
    }
    case M_TILES: {
      int n = 1;
      while (n + 4 <= len) {
        uint32_t key = rd32(d + n); n += 4;
        int tl = (key & 0x80000000u) ? 128 : 32;
        if (n + tl > len) break;
        tile_store(key, d + n);
        n += tl;
      }
      break;
    }
    case M_PAL:
      if (len >= 34 && d[1] < 8) { memcpy(r->pal[d[1]], d + 2, 32); r->palHave[d[1]] = true; }
      break;
    case M_TILERESET:
      memset(r->sentKeys, 0, sizeof(r->sentKeys));
      r->sentCount = 0;
      break;
    case M_BOMBS:
      if (len >= 2 && g_room.shareBombs && s_running && L.inGame && L.keysInit) {
        s_evBombs += d[1];
        game_notify("%s picked up %d bomb%s for everyone", r->name, d[1], d[1] == 1 ? "" : "s");
      }
      break;
    case M_KEYS:
      if (len >= 3 && g_room.shareKeys && s_running && L.inGame && L.keysInit && d[1] < 14) {
        int dgn = d[1], delta = (int8_t)d[2];
        R = emu_ram();
        int v = key_eff(dgn) + delta;
        key_set(dgn, v);
        L.keysLast[dgn] = key_eff(dgn);
        if (dgn < 2) { L.keysLast[0] = key_eff(0); L.keysLast[1] = key_eff(1); }
        if (delta > 0) game_notify("%s found a Small Key", r->name);
      }
      break;
    case M_KEYREQ:
      if (s_running && L.inGame && L.keysInit) {
        uint8_t m[16];
        m[0] = M_KEYSNAP;
        R = emu_ram();
        for (int i = 0; i < 14; i++) m[1 + i] = (uint8_t)key_eff(i);
        net_send_rel(cid, m, 15);
      }
      break;
    case M_KEYSNAP:
      if (len >= 15 && s_running && L.inGame && L.keysInit && g_room.shareKeys) {
        R = emu_ram();
        for (int i = 0; i < 14; i++) { key_set(i, d[1 + i]); }
        for (int i = 0; i < 14; i++) L.keysLast[i] = key_eff(i);
      }
      break;
    case M_PVP:
      if (len >= 7 && s_evPvpCount < 8 && g_room.pvp) {
        s_evPvp[s_evPvpCount].damage = d[1];
        s_evPvp[s_evPvpCount].dx = (int8_t)d[2];
        s_evPvp[s_evPvpCount].dy = (int8_t)d[3];
        s_evPvp[s_evPvpCount].dz = (int8_t)d[4];
        s_evPvp[s_evPvpCount].swordTime = d[5];
        s_evPvp[s_evPvpCount].mode = d[6];
        s_evPvpCount++;
      }
      break;
    case M_TILEMAP:
      recv_tilemap(r, d, len);
      break;
    case M_ROMREQ:
      serve_rom(cid);
      break;
    case M_NOROM:
      if (s_joinState == JOIN_DOWNLOADING) { s_joinState = JOIN_ERROR; snprintf(s_joinError, sizeof(s_joinError), "The host could not send the seed."); }
      break;
    case M_ROMDATA: {
      if (s_joinState != JOIN_DOWNLOADING || len < 41 || !g_baseRom) break;
      uint32_t full = rd32(d + 1), crc = rd32(d + 5);
      if (full < 0x100000 || full > 0x800000) break;
      char nm[33];
      memcpy(nm, d + 9, 32); nm[32] = 0;
      uint8_t *rom = malloc(full);
      uLongf out = full;
      if (uncompress(rom, &out, d + 41, len - 41) != Z_OK || out != full) {
        free(rom);
        s_joinState = JOIN_ERROR; snprintf(s_joinError, sizeof(s_joinError), "The seed data was damaged.");
        break;
      }
      for (uint32_t i = 0; i < full && i < (uint32_t)g_baseRomLen; i++) rom[i] ^= g_baseRom[i];
      if (crc32_buf(rom, (int)full) != crc) {
        free(rom);
        s_joinState = JOIN_ERROR; snprintf(s_joinError, sizeof(s_joinError), "The seed does not match your ROM.");
        break;
      }
      // keep it for next time
      char safe[40]; int k = 0;
      for (int i = 0; nm[i] && k < 39; i++) {
        char c = nm[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') safe[k++] = c;
      }
      safe[k] = 0;
      if (!k) strcpy(safe, "seed");
      char rel[96], path[600];
      snprintf(rel, sizeof(rel), "seeds/%s.sfc", safe);
      app_path(path, sizeof(path), rel);
      file_write(path, rom, (int)full);
      free(s_joinRom);
      s_joinRom = rom; s_joinRomLen = (int)full;
      snprintf(s_joinName, sizeof(s_joinName), "%s", safe);
      s_joinState = JOIN_READY;
      break;
    }
  }
}

void game_net_peer(uint32_t cid, bool joined) {
  if (joined) {
    Remote *r = rem_find(cid);
    if (!r) {
      for (int i = 0; i < MAX_PLAYERS; i++) if (!s_rem[i].used) { r = &s_rem[i]; break; }
      if (!r) return;
      memset(r, 0, sizeof(*r));
      r->used = true;
      r->cid = cid;
      strcpy(r->name, "Player");
    }
    send_hello(cid);
    for (int c = 0; c < NUM_CHUNKS; c++) s_chunks[c].lastSent = 0;
  } else {
    Remote *r = rem_find(cid);
    if (!r) return;
    if (r->hello && s_running) game_notify("%s left the room", r->name);
    if (s_joinState == JOIN_DOWNLOADING && s_joinFrom == cid) { s_joinState = JOIN_WAIT_INFO; }
    free(r->tmRuns);
    memset(r, 0, sizeof(*r));
  }
}

// ------------------------------------------------------------------ joining
#include <dirent.h>
static bool find_seed_by_crc(uint32_t crc, uint8_t **rom, int *len, char *name, int nameSize) {
  char dir[600];
  app_path(dir, sizeof(dir), "seeds/");
  DIR *d = opendir(dir);
  if (!d) return false;
  struct dirent *e;
  bool found = false;
  while (!found && (e = readdir(d)) != NULL) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot || strcasecmp(dot, ".sfc") != 0) continue;
    char p[900];
    snprintf(p, sizeof(p), "%s%s", dir, e->d_name);
    uint8_t *data; int n;
    if (!file_read(p, &data, &n)) continue;
    if (crc32_buf(data, n) == crc) {
      *rom = data; *len = n;
      snprintf(name, nameSize, "%.*s", (int)(dot - e->d_name), e->d_name);
      found = true;
    } else {
      free(data);
    }
  }
  closedir(d);
  return found;
}

void game_join_begin(void) {
  free(s_joinRom);
  s_joinRom = NULL;
  s_joinState = JOIN_WAIT_INFO;
  s_joinProgress = 0;
  s_joinError[0] = 0;
}

int game_join_state(void) { return s_joinState; }
const char *game_join_error(void) { return s_joinError; }

int game_join_progress(void) {
  if (s_joinState != JOIN_DOWNLOADING) return s_joinState == JOIN_READY ? 100 : 0;
  // the patch arrives as one reliable message, estimate from elapsed time
  int p = (int)((app_ms() - s_joinT) / 60);
  return p > 95 ? 95 : p;
}

bool game_join_take_rom(uint8_t **rom, int *len, char *nameOut, int nameSize) {
  if (s_joinState != JOIN_READY || !s_joinRom) return false;
  *rom = s_joinRom; *len = s_joinRomLen;
  snprintf(nameOut, nameSize, "%s", s_joinName);
  s_joinRom = NULL;
  s_joinState = JOIN_IDLE;
  return true;
}

void game_set_hosted(const uint8_t *rom, int len, const char *name) {
  free(s_hostRom);
  s_hostRom = NULL; s_hostRomLen = 0;
  if (rom) {
    s_hostRom = malloc(len);
    memcpy(s_hostRom, rom, len);
    s_hostRomLen = len;
  }
  snprintf(s_hostName, sizeof(s_hostName), "%s", name ? name : "");
}

// ------------------------------------------------------------------ lifecycle
void game_init(void) {
  if (!s_tiles) s_tiles = calloc(TILE_SLOTS, sizeof(TileEnt));
  net_set_callbacks(game_net_recv, game_net_peer);
}

void game_start(bool online) {
  R = emu_ram();
  memset(&L, 0, sizeof(L));
  tm_reset();
  s_online = online;
  s_romCrc = emu_rom_crc();
  s_evBombs = 0; s_evPvpCount = 0;
  memset(s_notes, 0, sizeof(s_notes));
  // what kind of ROM is this?
  char title[22];
  for (int i = 0; i < 21; i++) title[i] = (char)emu_rom8(0x00FFC0 + i);
  title[21] = 0;
  s_isRando = strncmp(title, "ZELDANODENSETSU", 15) != 0 && strncmp(title, "THE LEGEND OF ZELDA", 19) != 0;
  s_isDoor = s_isRando && (strncmp(title, "DR", 2) == 0 || (emu_rom8(0x278000) | emu_rom8(0x278001)) != 0);
  s_fast = (emu_rom8(0x00F82D) & 0x80) ? 0x800000 : 0;
  s_swampMasks = !s_isRando || emu_rom8(0x30803D) == 0x00;
  build_sync_table();
  for (int c = 0; c < NUM_CHUNKS; c++) { s_chunks[c].lastHash = 0; s_chunks[c].lastSent = 0; }
  app_log("game: \"%s\" rando=%d door=%d fast=%d crc=%08x online=%d", title, s_isRando, s_isDoor, s_fast != 0, s_romCrc, online);
  s_running = emu_hooked();
  g_emuMainHook = on_main;
  if (online) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      Remote *r = &s_rem[i];
      if (!r->used) continue;
      memset(r->sentKeys, 0, sizeof(r->sentKeys));
      r->sentCount = 0;
      memset(r->sentPalValid, 0, sizeof(r->sentPalValid));
      send_hello(r->cid);
    }
  }
}

void game_stop(void) {
  s_running = false;
  s_online = false;
  g_emuMainHook = NULL;
  if (g_snes) g_snes->ppu->extraCount = 0;
}

// ------------------------------------------------------------------ drawing the other players
void game_pre_frame(void) {
  if (!g_snes) return;
  Ppu *ppu = g_snes->ppu;
  ppu->extraCount = 0;
  if (!s_running || !s_online || !L.inGame) return;
  R = emu_ram();
  // the camera the next frame will be drawn with
  int xo = (int16_t)(rd16(R + 0xE2) - rd16(R + 0x011A));
  int yo = (int16_t)(rd16(R + 0xE8) - rd16(R + 0x011C));
  int n = 0;
  for (int p = 0; p < MAX_PLAYERS; p++) {
    Remote *r = &s_rem[p];
    if (!rem_playing(r) || !can_see(r->location)) continue;
    if (app_ms() - r->tState > 1500) continue;
    // palettes, with the tunic recolored to the player's color
    for (int pal = 0; pal < 8; pal++) {
      if (!r->palHave[pal]) continue;
      memcpy(s_extraPal[p][pal], r->pal[pal], 32);
    }
    if (g_cfg.tintTunic && r->palHave[7]) {
      uint32_t c = g_playerColors[r->color % NUM_PLAYER_COLORS];
      int cr = (c >> 19) & 31, cg = (c >> 11) & 31, cb = (c >> 3) & 31;
      uint16_t light = cr | (cg << 5) | (cb << 10);
      uint16_t dark = (cr * 3 / 5) | ((cg * 3 / 5) << 5) | ((cb * 3 / 5) << 10);
      s_extraPal[p][7][10] = light; s_extraPal[p][7][12] = light;
      s_extraPal[p][7][9] = dark; s_extraPal[p][7][11] = dark;
    }
    for (int i = 0; i < r->nspr && n < PPU_MAX_EXTRA; i++) {
      const SprRec *s = &r->spr[i];
      int size = (s->flags & 1) ? 16 : 8;
      int sx = s->wx - xo, sy = s->wy - yo;
      if (sx <= -size || sx >= 256 || sy <= -size || sy >= 240) continue;
      int pal = s->flags >> 5;
      if (!r->palHave[pal]) continue;
      TileEnt *t = tile_find(s->key);
      if (!t) continue;
      memcpy(s_extraPx[n], t->px, size * size);
      PpuExtra *e = &ppu->extra[n];
      e->x = (int16_t)sx; e->y = (int16_t)sy;
      e->w = (uint8_t)size; e->h = (uint8_t)size;
      e->priority = (s->flags >> 3) & 3;
      e->hflip = (s->flags & 2) != 0;
      e->vflip = (s->flags & 4) != 0;
      e->px = s_extraPx[n];
      e->pal = s_extraPal[p][pal];
      n++;
    }
  }
  ppu->extraCount = n;
}

void game_post_frame(void) {
  if (s_joinState == JOIN_DOWNLOADING && s_joinFrom) {
    // nothing to do, the reliable channel delivers the patch in one piece
  }
}

void game_draw_overlay(void) {
  if (!s_running) return;
  uint64_t now = app_ms();
  if (s_online && L.inGame && g_cfg.showNames) {
    int xo = (int16_t)(rd16(R + 0xE2) - rd16(R + 0x011A));
    int yo = (int16_t)(rd16(R + 0xE8) - rd16(R + 0x011C));
    for (int p = 0; p < MAX_PLAYERS; p++) {
      Remote *r = &s_rem[p];
      if (!rem_playing(r) || !can_see(r->location) || now - r->tState > 1500) continue;
      if (r->flags & 16) continue;
      int sx = (r->x - xo + 8) * 2, sy = (r->y - yo - 9) * 2;
      if (sx < -40 || sx > CANVAS_W + 40 || sy < -20 || sy > CANVAS_H) continue;
      int w = ui_text_w(r->name, 1);
      ui_text(sx - w / 2, sy, r->name, g_playerColors[r->color % NUM_PLAYER_COLORS], 1);
    }
  }
  int y = CANVAS_H - 20;
  for (int i = 0; i < 6; i++) {
    Note *nt = &s_notes[i];
    if (!nt->text[0] || now - nt->t > 6000) continue;
    int w = ui_text_w(nt->text, 1) + 10;
    ui_blend(6, y - 3, w, 13, 0x000000, now - nt->t > 5500 ? 60 : 150);
    ui_text(11, y, nt->text, nt->color, 1);
    y -= 14;
  }
}

int game_players(PlayerView *out, int max) {
  int n = 0;
  if (max < 1) return 0;
  memset(&out[0], 0, sizeof(out[0]));
  out[0].cid = net_my_cid();
  snprintf(out[0].name, sizeof(out[0].name), "%s", g_cfg.name);
  out[0].color = g_cfg.color;
  out[0].inGame = s_running && L.inGame;
  out[0].location = L.location;
  if (out[0].inGame) { out[0].health = SR[0x36D]; out[0].maxHealth = SR[0x36C]; }
  out[0].direct = true; out[0].sameGame = true;
  n = 1;
  NetPeerInfo pi[MAX_PLAYERS];
  int np = net_peers(pi, MAX_PLAYERS);
  for (int i = 0; i < MAX_PLAYERS && n < max; i++) {
    Remote *r = &s_rem[i];
    if (!r->used || !r->hello) continue;
    PlayerView *v = &out[n++];
    memset(v, 0, sizeof(*v));
    v->cid = r->cid;
    snprintf(v->name, sizeof(v->name), "%s", r->name);
    v->color = r->color;
    v->inGame = rem_alive(r) && (r->flags & 1);
    v->location = r->location;
    v->health = r->health; v->maxHealth = r->maxHealth;
    v->sameGame = !s_running || r->romCrc == s_romCrc;
    for (int k = 0; k < np; k++) if (pi[k].cid == r->cid) { v->direct = pi[k].direct; v->rtt = pi[k].rtt; }
  }
  return n;
}
