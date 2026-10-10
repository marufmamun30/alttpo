// Shared enemies.
//
// Every player still runs the whole game, enemies included. For each enemy that
// several players have in front of them, exactly one player is its "authority":
// that game moves the enemy, lets it think and decides what happens to it. The
// other games overwrite their copy with what the authority sends.
//
//  * Who is the authority?  The player who hits, stuns or lifts an enemy takes it
//    over on the spot, so a hit is always decided in the attacker's own game,
//    without delay. Otherwise it drifts to the player the enemy is closest to
//    (that is the player it chases), and away from players who do not have it on
//    screen, because the game freezes sprites that are off screen. Claims are
//    numbered: the highest number wins and the lower player id breaks a tie, so
//    everybody comes to the same answer.
//  * Identity.  The game numbers the sprites it loads (dungeons: the slot the
//    room's sprite list puts them in, overworld: the map cell they start in).
//    That number is the same in every player's game and is what travels.
//  * Death.  A copy never dies by itself: it waits in the last frame of its death
//    animation for the authority's verdict, which arrives as an entry in the
//    "dead list" every player keeps for the place they are in and repeats to the
//    others (so a player who walks in later learns it too). An entry says one of:
//    the enemy is gone; it is gone and left a pickup (the pickup is copied once,
//    afterwards every game looks after it itself); the pickup has been collected
//    (it disappears for everybody). Enemies that carry a key or a shuffled item
//    die in every game on their own, so the game's (or the randomizer's) own code
//    creates the item; only the pickup is shared.
//    The list belongs to one place and is dropped the moment the game names another
//    one, which is before the game runs there: sprite numbers start at 0 in every
//    room, and the packets sent while still walking in already carry the new name.
//  * Not shared: bosses, characters, the room's own traps, and projectiles (every
//    game lets the shared enemy fire its own).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "app.h"
#include "enemy.h"

#define NF 56                 // bytes of state per sprite
#define NONE 0xFFFF
#define PH_DROP 0x8000        // id flag: the enemy has turned into what it dropped
#define DEAD_DROP 1           // dead list: the enemy is gone, what it left is still there
#define DEAD_GONE 2           //   it is gone and so is anything it left
#define DEAD_TAKEN 3          //   a player collected what it left
#define RF_PAUSED 1           // record flags: off the authority's screen (frozen there)
#define RF_LOCAL 2            //   every game creates its drop itself
#define RF_PRIZE 4            //   a plain pickup, can be copied
#define PF_RUNNING 1          // packet flags: the sender's game is moving its sprites
#define PF_OWN_LIST 2         //   its dead list was collected in the place the packet names
#define MAX_DEAD 96
#define PREC_MAX 24

enum {
  F_XL, F_XH, F_YL, F_YH, F_Z, F_XS, F_YS, F_ZS, F_XV, F_YV, F_ZV, F_STATE, F_TYPE, F_HEALTH,
  F_AI, F_A, F_B, F_C, F_GFX, F_D, F_T0, F_T1, F_T2, F_T3, F_T4,
  F_SUBTYPE, F_FLAGS2, F_FLAGS3, F_WALL, F_SUBTYPE2, F_E, F_RECOIL, F_HEADDIR, F_ANIM, F_G, F_HIT,
  F_FLOOR, F_YREC, F_XREC, F_OAMF, F_FLAGS4,
  F_STUN, F_FLAGS, F_PRIO, F_IGNORE, F_UNK2, F_FLAGS5, F_DEFL, F_DIE, F_BUMP, F_DMG,
  F_I, F_UNK3, F_UNK4, F_UNK5, F_UNK1,
};

// where the game keeps those bytes: 16 entries each, one per sprite slot
static const uint32_t kField[NF] = {
  0x0D10, 0x0D30, 0x0D00, 0x0D20, 0x0F70, 0x0D70, 0x0D60, 0x0F90, 0x0D50, 0x0D40, 0x0F80, 0x0DD0, 0x0E20, 0x0E50,
  0x0D80, 0x0D90, 0x0DA0, 0x0DB0, 0x0DC0, 0x0DE0, 0x0DF0, 0x0E00, 0x0E10, 0x0EE0, 0x0F10,
  0x0E30, 0x0E40, 0x0E60, 0x0E70, 0x0E80, 0x0E90, 0x0EA0, 0x0EB0, 0x0EC0, 0x0ED0, 0x0EF0,
  0x0F20, 0x0F30, 0x0F40, 0x0F50, 0x0F60,
  0x0B58, 0x0B6B, 0x0B89, 0x0BA0, 0x0BB0, 0x0BE0, 0x0CAA, 0x0CBA, 0x0CD2, 0x0CE2,
  0x1F9C2, 0x1FA1C, 0x1FA2C, 0x1FA3C, 0x1FA4C,
};

// ordinary enemies; everything else keeps running separately in every game
static const uint8_t kSyncTypes[] = {
  0x00, 0x01, 0x02, 0x08, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x13, 0x15, 0x17, 0x18, 0x19,
  0x20, 0x22, 0x23, 0x24, 0x26, 0x27, 0x3E, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
  0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x51, 0x55, 0x56, 0x58, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60,
  0x61, 0x63, 0x64, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0x71, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x83,
  0x84, 0x85, 0x86, 0x8A, 0x8B, 0x8E, 0x8F, 0x91, 0x94, 0x99, 0x9A, 0x9B, 0xA1, 0xA5, 0xA6, 0xA7,
  0xA8, 0xA9, 0xAA, 0xC3, 0xC4, 0xC7, 0xC9, 0xCA, 0xCF, 0xD0, 0xD1, 0xD3,
};
static uint8_t s_syncType[256];

typedef struct Ent {
  uint16_t id;          // NONE = slot not tracked
  uint8_t phase;        // 0 enemy, 1 what it left behind
  bool local;           // its drop is made by each game (key, shuffled item)
  bool dropAuth;        // it turned into a drop while we were its authority
  bool announce;        // ... and the others have not been told yet
  bool forced;          // its death was already started here
  uint64_t doomed;      // reported dead with a pickup we have not been shown yet (since when)
  uint32_t auth;        // the player whose game runs it (0 = not known yet)
  uint16_t seq;         // number of the claim we follow
  uint64_t heard;       // when another player last claimed it
  uint64_t changed;     // when its authority last changed
  uint32_t applied;     // stamp and sender of the last record written into the slot
  uint32_t appliedFrom;
  uint32_t recFrame;    // our frame at that moment
  int32_t recX, recY;   // its position (1/256 pixels) and observed speed per frame
  int32_t velX, velY;
  uint8_t rec[NF];
  uint8_t pre[NF];      // the slot as we left it last frame
} Ent;

typedef struct PRec { uint16_t id; uint16_t seq; uint8_t flags; uint32_t stamp; uint64_t rx; uint8_t f[NF]; } PRec;

typedef struct PeerEn {
  bool used, running;
  bool ranHere;         // it has told us that it runs in the place it names
  uint32_t cid, loc, stamp, sentFrame;
  uint64_t rx;
  PRec rec[PREC_MAX];
  uint16_t dead[64];
  int ndead;
} PeerEn;

static Ent s_ent[16];
static uint16_t s_dead[MAX_DEAD];
static int s_ndead, s_deadRot;
static PeerEn s_peer[MAX_PLAYERS];
static const EnemyCtx *C;
static uint8_t *R;
static uint64_t s_now;
static uint32_t s_frame, s_me, s_loc, s_lastRun;
static bool s_indoors, s_locValid;
static int s_logBudget = 200;
static uint64_t s_logRefill;

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void elog(const char *fmt, ...) {
  if (s_now - s_logRefill > 1000) { s_logRefill = s_now; if (s_logBudget < 40) s_logBudget = 40; }
  if (s_logBudget <= 0) return;
  s_logBudget--;
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  app_log("enemy: %s", buf);
}

static bool loc_eq(uint32_t a, uint32_t b) {
  if (a == b) return true;
  return (a & 0x010000) && (b & 0x010000) && (a & 0xFFFF) == (b & 0xFFFF);
}

// ------------------------------------------------------------------ the game's sprite tables
static bool is_drop(uint8_t type) { return type >= 0xD8 && type <= 0xE6; }
static bool is_prize(uint8_t type) { return (type >= 0xD8 && type <= 0xE3) || type == 0xE6; }
static bool is_key(uint8_t type) { return type == 0xE4 || type == 0xE5; }

static uint16_t slot_id(int k) {
  if (s_indoors) { uint8_t n = R[0x0BC0 + k]; return n < 0x10 ? n : NONE; }
  uint16_t w = rd16(R + 0x0BC0 + k * 2);
  return w < 0x1000 ? w : NONE;
}

static void set_slot_id(int k, uint16_t id) {
  if (s_indoors) R[0x0BC0 + k] = (uint8_t)id;
  else wr16(R + 0x0BC0 + k * 2, id);
}

static uint8_t *loaded_byte(uint16_t id) { return R + 0x1EF80 + (id >> 3); }
static uint8_t loaded_mask(uint16_t id) { return 0x80 >> (id & 7); }

static void read_fields(int k, uint8_t *f) {
  for (int i = 0; i < NF; i++) f[i] = R[kField[i] + k];
}

static int fx(const uint8_t *f) { return (int16_t)(f[F_XL] | (f[F_XH] << 8)); }
static int fy(const uint8_t *f) { return (int16_t)(f[F_YL] | (f[F_YH] << 8)); }
static int spr_x(int k) { return (int16_t)(R[0x0D10 + k] | (R[0x0D30 + k] << 8)); }
static int spr_y(int k) { return (int16_t)(R[0x0D00 + k] | (R[0x0D20 + k] << 8)); }

// the part of the world the game keeps sprites alive (and running) in
static bool in_window(int x, int y, int margin) {
  int dx = (int16_t)(x - rd16(R + 0xE2)), dy = (int16_t)(y - rd16(R + 0xE8));
  return dx >= -0x40 + margin && dx < 0x130 - margin && dy >= -0x40 + margin && dy < 0x130 - margin;
}

// does this enemy leave something behind that every game has to create itself?
static bool local_death(int k) {
  if (R[0x0CBA + k] != 0) return true;                // key or big key
  return s_indoors && C->rando && R[0x0730 + k] != 0; // the randomizer's item drops
}

static int find_slot(uint16_t id) {
  for (int k = 0; k < 16; k++) if (s_ent[k].id == id) return k;
  return -1;
}

// dead list entries: the id in the low 12 bits, what became of it in the top two
static int dead_kind(uint16_t v) { return (v & 0x8000) ? DEAD_TAKEN : (v & 0x4000) ? DEAD_DROP : DEAD_GONE; }
static uint16_t dead_entry(uint16_t id, int kind) { return id | (kind == DEAD_TAKEN ? 0x8000 : kind == DEAD_DROP ? 0x4000 : 0); }

// 0 when the id is not in the list
static int in_dead(uint16_t id) {
  for (int i = 0; i < s_ndead; i++) if ((s_dead[i] & 0x0FFF) == id) return dead_kind(s_dead[i]);
  return 0;
}

// the kinds only ever move forward: left a pickup -> gone -> taken
static void add_dead(uint16_t id, int kind) {
  for (int i = 0; i < s_ndead; i++) {
    if ((s_dead[i] & 0x0FFF) != id) continue;
    if (kind > dead_kind(s_dead[i])) s_dead[i] = dead_entry(id, kind);
    return;
  }
  if (s_ndead == MAX_DEAD) { memmove(s_dead, s_dead + 1, sizeof(uint16_t) * (MAX_DEAD - 1)); s_ndead--; }
  s_dead[s_ndead++] = dead_entry(id, kind);
}

// ------------------------------------------------------------------ the other players
static PeerEn *peer_get(uint32_t cid, bool create) {
  for (int i = 0; i < MAX_PLAYERS; i++) if (s_peer[i].used && s_peer[i].cid == cid) return &s_peer[i];
  if (!create) return NULL;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PeerEn *p = &s_peer[i];
    if (p->used) continue;
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->cid = cid;
    for (int j = 0; j < PREC_MAX; j++) p->rec[j].id = NONE;
    return p;
  }
  return NULL;
}

static const EnemyPeer *ctx_peer(uint32_t cid) {
  for (int i = 0; i < C->npeers; i++) if (C->peers[i].cid == cid) return &C->peers[i];
  return NULL;
}

static uint64_t peer_ttl(const PeerEn *p) {
  const EnemyPeer *cp = ctx_peer(p->cid);
  return cp && cp->direct ? 250 : 1300;
}

static bool peer_valid(const PeerEn *p) {
  return p->used && p->running && loc_eq(p->loc, s_loc) && ctx_peer(p->cid) && s_now - p->rx < peer_ttl(p);
}

static bool better(uint16_t seqA, uint32_t cidA, uint16_t seqB, uint32_t cidB) {
  return seqA > seqB || (seqA == seqB && cidA < cidB);
}

// the strongest claim another player makes for an enemy
static PRec *best_claim(uint16_t id, PeerEn **who) {
  PRec *best = NULL;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PeerEn *p = &s_peer[i];
    if (!peer_valid(p)) continue;
    uint64_t ttl = peer_ttl(p);
    for (int j = 0; j < PREC_MAX; j++) {
      PRec *r = &p->rec[j];
      if (r->id != id || s_now - r->rx >= ttl) continue;
      if (!best || better(r->seq, p->cid, best->seq, (*who)->cid)) { best = r; *who = p; }
    }
  }
  return best;
}

// ------------------------------------------------------------------ changing the local game
static void write_record(int k, const uint8_t *f) {
  for (int i = 0; i < NF; i++) {
    uint8_t v = f[i];
    if (i == F_DMG) v = 0;                 // damage is worked out by the authority alone
    if (i == F_STATE && v == 10) v = 11;   // carried by the other player: just sits where they hold it
    R[kField[i] + k] = v;
  }
}

static void drop_carried(int k) {
  if (R[0x0DD0 + k] == 10) { R[0x0308] = 0; R[0x0309] = 0; } // Link's hands are empty again
}

static void remove_sprite(int k, Ent *e, bool markDead) {
  drop_carried(k);
  if (markDead && s_indoors) {
    // what the game does when it kills a sprite: it stays away while the room is remembered
    uint8_t n = R[0x0BC0 + k];
    uint16_t room = rd16(R + 0x048E);
    if (!(R[0x0CAA + k] & 1) && n < 0x10 && room < 0x800) {
      uint8_t *w = R + 0x1DF80 + room * 2;
      wr16(w, rd16(w) | (1 << n));
    }
  }
  R[0x0DD0 + k] = 0;
  set_slot_id(k, s_indoors ? 0xFF : 0xFFFF);
  e->id = NONE;
}

static void force_death(int k, Ent *e) {
  uint8_t st = R[0x0DD0 + k];
  if (e->forced || (st != 9 && st != 11)) return;
  e->forced = true;
  R[0x0DD0 + k] = 6;
  R[0x0DF0 + k] = 15;
  R[0x0E40 + k] += 4;
  R[0x0EF0 + k] = 0;
  R[0x0CE2 + k] = 0;
}

static void track(int k, uint16_t id, int phase) {
  Ent *e = &s_ent[k];
  memset(e, 0, sizeof(*e));
  e->id = id;
  e->phase = (uint8_t)phase;
  e->heard = s_now;
  if (C->npeers == 0) e->auth = s_me;
  read_fields(k, e->pre);
}

static void claim(int k, Ent *e, const PRec *b, const char *why) {
  uint16_t s = e->seq;
  if (b && b->seq > s) s = b->seq;
  // damage the previous authority had registered but not yet applied comes along
  if (e->auth != s_me && e->applied && s_frame - e->recFrame < 30 && e->rec[F_DMG] > R[0x0CE2 + k]) R[0x0CE2 + k] = e->rec[F_DMG];
  if (e->auth != s_me || b) elog("slot %d id %03x type %02x: taking over (%s), claim %d", k, e->id, R[0x0E20 + k], why, s + 1);
  e->seq = (uint16_t)(s + 1);
  e->auth = s_me;
  e->changed = s_now;
}

// ------------------------------------------------------------------ per frame
static void vanished(int k, Ent *e, bool quiet) {
  if (!quiet && R[0x0DD0 + k] == 0) {
    int x = fx(e->pre), y = fy(e->pre);
    if (e->phase == 0) {
      if (e->auth == s_me) {
        // killed, or merely put away by the game because it left the screen?
        bool killed = s_indoors ? (!(e->pre[F_DEFL] & 0x40) || in_window(x, y, 0))
                                : (*loaded_byte(e->id) & loaded_mask(e->id)) != 0;
        if (killed) { add_dead(e->id, DEAD_GONE); elog("slot %d id %03x type %02x: gone", k, e->id, e->pre[F_TYPE]); }
      }
    } else {
      bool timedOut = e->pre[F_STUN] != 0 && e->pre[F_STUN] <= 2;
      if (!timedOut && in_window(x, y, 0)) { add_dead(e->id, DEAD_TAKEN); elog("slot %d id %03x: pickup %02x collected", k, e->id, e->pre[F_TYPE]); }
      else if (e->dropAuth) add_dead(e->id, DEAD_GONE);
    }
  }
  e->id = NONE;
}

static void scan_slots(bool quiet) {
  for (int k = 0; k < 16; k++) {
    Ent *e = &s_ent[k];
    uint8_t st = R[0x0DD0 + k], type = R[0x0E20 + k];
    uint16_t nid = st ? slot_id(k) : NONE;
    if (e->id != NONE) {
      // a dropped key hands its number over to another byte, the slot still tells us who it is
      bool same = st != 0 && st != 8 && (nid == e->id || (nid == NONE && is_drop(type)));
      if (!same) vanished(k, e, quiet);
      else if (e->phase == 0) {
        if (is_drop(type)) {
          e->phase = 1;
          e->dropAuth = e->announce = e->auth == s_me;
          if (!is_prize(type)) e->local = true;
          if (e->dropAuth) { add_dead(e->id, DEAD_DROP); elog("slot %d id %03x: left a %02x behind", k, e->id, type); }
        } else {
          e->local = local_death(k);
        }
      }
    }
    if (e->id == NONE && st >= 9 && nid != NONE) {
      if (s_syncType[type]) track(k, nid, 0);
      else if (is_key(type)) { track(k, nid, 1); s_ent[k].local = true; }
    }
  }
}

static void merge_peer_dead(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PeerEn *p = &s_peer[i];
    if (!p->used || !loc_eq(p->loc, s_loc) || !ctx_peer(p->cid) || s_now - p->rx > 3000) continue;
    for (int j = 0; j < p->ndead; j++) add_dead(p->dead[j] & 0x0FFF, dead_kind(p->dead[j]));
  }
}

static void apply_dead(void) {
  for (int k = 0; k < 16; k++) {
    Ent *e = &s_ent[k];
    int kind = e->id == NONE ? 0 : in_dead(e->id);
    if (!kind) continue;
    if (kind == DEAD_TAKEN) { elog("slot %d id %03x: taken by another player", k, e->id); remove_sprite(k, e, e->phase == 0); }
    else if (e->phase == 0) {
      if (e->local) force_death(k, e);
      else if (kind == DEAD_DROP && (!e->doomed || s_now - e->doomed < 1500)) {
        if (!e->doomed) e->doomed = s_now; // what it left behind is shown to us in a moment (apply_drops)
      } else { elog("slot %d id %03x: killed by another player", k, e->id); remove_sprite(k, e, true); }
    }
  }
}

static void predict(int k, Ent *e) {
  // no news this frame: carry on the way it was moving
  uint32_t dt = s_frame - e->recFrame;
  if (dt == 0 || dt > 10 || e->rec[F_STATE] != 9 || (e->velX == 0 && e->velY == 0)) return;
  int32_t x = e->recX + e->velX * (int32_t)dt, y = e->recY + e->velY * (int32_t)dt;
  R[0x0D70 + k] = x & 0xff; R[0x0D10 + k] = (x >> 8) & 0xff; R[0x0D30 + k] = (x >> 16) & 0xff;
  R[0x0D60 + k] = y & 0xff; R[0x0D00 + k] = (y >> 8) & 0xff; R[0x0D20 + k] = (y >> 16) & 0xff;
}

static void apply_claim(int k, Ent *e, const PRec *b, const PeerEn *bp) {
  if (b->stamp == e->applied && bp->cid == e->appliedFrom) { predict(k, e); return; }
  int32_t x = ((b->f[F_XH] << 8 | b->f[F_XL]) << 8) | b->f[F_XS];
  int32_t y = ((b->f[F_YH] << 8 | b->f[F_YL]) << 8) | b->f[F_YS];
  int32_t ds = (int32_t)(b->stamp - e->applied);
  e->velX = e->velY = 0;
  if (bp->cid == e->appliedFrom && ds > 0 && ds <= 6) {
    int32_t vx = (x - e->recX) / ds, vy = (y - e->recY) / ds;
    if (vx > -0x400 && vx < 0x400 && vy > -0x400 && vy < 0x400) { e->velX = vx; e->velY = vy; }
  }
  e->recX = x; e->recY = y;
  e->applied = b->stamp; e->appliedFrom = bp->cid; e->recFrame = s_frame;
  memcpy(e->rec, b->f, NF);
  write_record(k, b->f);
}

static int dist(int x, int y, int lx, int ly) {
  int dx = abs(x - lx), dy = abs(y - ly);
  return dx > dy ? dx : dy;
}

// One enemy: who runs it, and if that is not us, make our copy follow. Returns true
// when the others should hear about a change right away.
static bool update_enemy(int k) {
  Ent *e = &s_ent[k];
  PeerEn *bp = NULL;
  PRec *b = best_claim(e->id, &bp);
  bool mine = e->auth == s_me, urgent = false;
  uint8_t st = R[0x0DD0 + k];

  if (e->doomed) {
    // it is dead, we are only waiting to be shown what it left
    if (st >= 1 && st <= 7 && R[0x0DF0 + k] < 3) R[0x0DF0 + k] = 3;
    return false;
  }
  if (!mine) {
    // did our own player just do something to it? (copies never carry damage of their own)
    bool touched = R[0x0CE2 + k] != 0
                || (st != e->pre[F_STATE] && (st == 10 || st == 2))
                || R[0x0E20 + k] != e->pre[F_TYPE]
                || (R[0x0EA0 + k] != 0 && e->pre[F_RECOIL] == 0);
    if (touched) { claim(k, e, b, "hit"); mine = urgent = true; }
  }

  if (mine) {
    if (b && better(b->seq, bp->cid, e->seq, s_me)) {
      elog("slot %d id %03x: handed to %08x, claim %d", k, e->id, bp->cid, b->seq);
      drop_carried(k);
      e->auth = bp->cid; e->seq = b->seq; e->changed = e->heard = s_now;
      mine = false;
    }
  } else if (b) {
    if (e->auth != bp->cid) { e->auth = bp->cid; e->changed = s_now; }
    e->seq = b->seq;
    e->heard = s_now;
  } else {
    // nobody speaks for it (any more)
    bool relayed = false;
    for (int i = 0; i < C->npeers; i++) if (!C->peers[i].direct) relayed = true;
    if (C->npeers == 0 || s_now - e->heard > (relayed ? 1500u : 400u)) { claim(k, e, NULL, "free"); mine = urgent = true; }
  }

  if (!mine && b && st >= 9 && s_now - e->changed > 700) {
    // the enemy should belong to a player who has it on screen, and to the nearest one
    int x = spr_x(k), y = spr_y(k);
    if (in_window(x, y, 0) && !R[0x0F00 + k]) {
      const char *why = NULL;
      if (b->flags & RF_PAUSED) why = "off their screen";
      else if (((s_frame + k) & 7) == 0) {
        const EnemyPeer *pp = ctx_peer(bp->cid);
        if (pp && dist(x, y, C->x, C->y) + 40 < dist(x, y, pp->x, pp->y)) why = "nearer";
      }
      if (why) { claim(k, e, b, why); mine = urgent = true; }
    }
  }

  if (!mine) {
    if (b) apply_claim(k, e, b, bp);
    // a copy does not finish dying on its own
    st = R[0x0DD0 + k];
    if (st >= 1 && st <= 7 && !local_death(k) && R[0x0DF0 + k] < 3) R[0x0DF0 + k] = 3;
  }
  return urgent;
}

// "it left this behind": turn our copy of the enemy into the same pickup
static void apply_drops(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PeerEn *p = &s_peer[i];
    if (!peer_valid(p)) continue;
    for (int j = 0; j < PREC_MAX; j++) {
      PRec *r = &p->rec[j];
      if (r->id == NONE || !(r->id & PH_DROP) || s_now - r->rx > 2500) continue;
      uint16_t id = r->id & 0x7FFF;
      int k = find_slot(id);
      if (k < 0 || in_dead(id) == DEAD_TAKEN) continue;
      Ent *e = &s_ent[k];
      if (e->phase != 0 || (e->auth == s_me && !e->doomed)) continue;
      if ((r->flags & RF_LOCAL) || e->local || local_death(k)) force_death(k, e);
      else if (r->flags & RF_PRIZE) {
        elog("slot %d id %03x: left a %02x behind (says %08x)", k, id, r->f[F_TYPE], p->cid);
        write_record(k, r->f);
        e->phase = 1;
        e->dropAuth = false;
        e->auth = 0;
        e->doomed = 0;
      }
    }
  }
}

// enemies another player runs that our game does not have (any more): bring them in
static void materialize(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    PeerEn *p = &s_peer[i];
    if (!peer_valid(p)) continue;
    for (int j = 0; j < PREC_MAX; j++) {
      PRec *r = &p->rec[j];
      if (r->id == NONE || (r->id & PH_DROP) || s_now - r->rx >= peer_ttl(p)) continue;
      uint16_t id = r->id;
      if (find_slot(id) >= 0 || in_dead(id)) continue;
      if (!s_syncType[r->f[F_TYPE]] || r->f[F_STATE] < 9) continue;
      PeerEn *bp = NULL;
      if (best_claim(id, &bp) != r) continue;
      int x = fx(r->f), y = fy(r->f), k = -1;
      if (s_indoors) {
        if (id > 15 || R[0x0DD0 + id] != 0) continue;
        if ((r->f[F_DEFL] & 0x40) && !in_window(x, y, 16)) continue;
        k = id;
      } else {
        if (!in_window(x, y, 16)) continue;
        bool present = false;
        for (int s = 0; s < 16; s++) if (R[0x0DD0 + s] != 0 && rd16(R + 0x0BC0 + s * 2) == id) present = true;
        if (present) continue;
        for (int s = 13; s >= 0; s--) if (R[0x0DD0 + s] == 0) { k = s; break; }
        if (k < 0) continue;
        *loaded_byte(id) |= loaded_mask(id);
      }
      write_record(k, r->f);
      set_slot_id(k, id);
      R[0x0C9A + k] = s_indoors ? R[0x048E] : R[0x040A];
      R[0x0F00 + k] = 0;
      track(k, id, 0);
      Ent *e = &s_ent[k];
      e->auth = p->cid; e->seq = r->seq; e->changed = s_now;
      e->applied = r->stamp; e->appliedFrom = p->cid; e->recFrame = s_frame;
      e->recX = ((r->f[F_XH] << 8 | r->f[F_XL]) << 8) | r->f[F_XS];
      e->recY = ((r->f[F_YH] << 8 | r->f[F_YL]) << 8) | r->f[F_YS];
      memcpy(e->rec, r->f, NF);
      elog("slot %d id %03x type %02x: brought in from %08x", k, id, r->f[F_TYPE], p->cid);
    }
  }
}

static int put_record(uint8_t *b, const Ent *e, int k) {
  uint8_t f[NF];
  read_fields(k, f);
  uint8_t flags = 0;
  if (e->phase == 0) {
    if (R[0x0F00 + k] || !in_window(fx(f), fy(f), 0)) flags |= RF_PAUSED;
    if (e->local) flags |= RF_LOCAL;
  } else {
    flags |= e->local ? RF_LOCAL : RF_PRIZE;
  }
  wr16(b, e->id | (e->phase ? PH_DROP : 0));
  wr16(b + 2, e->seq);
  b[4] = flags;
  uint8_t *mask = b + 5;
  memset(mask, 0, 7);
  int n = 12;
  for (int i = 0; i < NF; i++) if (f[i]) { mask[i >> 3] |= 1 << (i & 7); b[n++] = f[i]; }
  return n;
}

static void send_packet(bool running, bool urgent) {
  if (C->npeers == 0) return;
  uint8_t b[1280];
  int n = 0;
  b[n++] = M_ENEMY;
  b[n++] = (running ? PF_RUNNING : 0) | PF_OWN_LIST;
  wr32(b + n, C->location); n += 4;
  wr32(b + n, s_frame); n += 4;
  int nd = s_ndead > 48 ? 48 : s_ndead;
  b[n++] = (uint8_t)nd;
  for (int i = 0; i < nd; i++) { wr16(b + n, s_dead[(s_deadRot + i) % s_ndead]); n += 2; }
  if (s_ndead > 48) s_deadRot = (s_deadRot + 7) % s_ndead;
  int cntPos = n++, cnt = 0;
  if (running) {
    for (int k = 0; k < 16; k++) {
      Ent *e = &s_ent[k];
      if (e->id == NONE) continue;
      if (e->phase == 0) {
        if (e->auth != s_me) continue;
      } else {
        // what an enemy left behind is only mentioned now and then
        if (!e->dropAuth) continue;
        if (e->announce) { e->announce = false; urgent = true; }
        else if ((s_frame & 3) != 0) continue;
      }
      n += put_record(b + n, e, k);
      cnt++;
    }
  }
  b[cntPos] = (uint8_t)cnt;
  for (int i = 0; i < C->npeers; i++) {
    PeerEn *p = peer_get(C->peers[i].cid, true);
    if (!p) continue;
    // every frame on a direct link (every other one in a crowd), a few times a second through the
    // relay; on frames that are multiples of four, which are the ones that carry the pickups
    uint32_t since = s_frame - p->sentFrame;
    bool due;
    if (!running) due = since >= 10;
    else if (C->peers[i].direct) due = C->npeers <= 2 || (s_frame & 1) == 0;
    else due = (s_frame % 12) == 0;
    if (!due && !(urgent && since >= 4)) continue;
    p->sentFrame = s_frame;
    net_send(p->cid, b, n);
  }
}

// test hook: ALTTPO_ENEMY_LOG=<frames> writes every tracked sprite to the log at that interval,
// with a clock that is the same for all copies running on one PC
static int debug_every(void) {
  static int every = -1;
  if (every < 0) { const char *e = getenv("ALTTPO_ENEMY_LOG"); every = e ? atoi(e) : 0; }
  return every;
}

static void debug_dump(void) {
  int every = debug_every();
  if (!every || (s_frame % every) != 0) return;
  for (int k = 0; k < 16; k++) {
    const Ent *e = &s_ent[k];
    if (e->id == NONE) continue;
    app_log("enemy: t=%07u slot %2d id %03x%s type %02x st %2d hp %3d at %5d,%5d who %08x claim %d%s", (unsigned)(s_now % 10000000), k, e->id,
            e->phase ? "+" : " ", R[0x0E20 + k], R[0x0DD0 + k], R[0x0E50 + k], spr_x(k), spr_y(k), e->auth, e->seq, e->auth == s_me ? " (me)" : "");
  }
}

// ... and with the same hook, every time we get to another place or the game stops or starts moving
static void debug_place(bool running) {
  static uint32_t loc = ~0u;
  static int was = -1;
  if ((s_loc == loc && running == was) || !debug_every()) return;
  loc = s_loc; was = running;
  app_log("enemy: t=%07u place %05x %s", (unsigned)(s_now % 10000000), s_loc, running ? "running" : "waiting");
}

void enemy_frame(const EnemyCtx *c) {
  C = c; R = c->ram; s_now = c->now; s_frame = c->frame; s_me = c->me;
  if (!s_syncType[kSyncTypes[0]]) for (int i = 0; i < (int)sizeof(kSyncTypes); i++) s_syncType[kSyncTypes[i]] = 1;
  if (!s_locValid || c->location != s_loc) {
    // Another place. What we know about the old one ends here and not when the game runs again:
    // on the way in (stairs, a door that shuts behind Link) we already send packets with the new name.
    for (int k = 0; k < 16; k++) s_ent[k].id = NONE;
    s_ndead = s_deadRot = 0;
    s_loc = c->location;
    s_locValid = true;
    s_lastRun = 0;
  }
  s_indoors = (s_loc & 0x010000) != 0;
  debug_place(c->running);
  if (!c->running) {
    // menu, text, transition: our sprites stand still, the others take over what we were running
    send_packet(false, false);
    return;
  }
  bool resumed = c->frame - s_lastRun > 1;
  s_lastRun = c->frame;
  if (resumed) {
    // the game may have loaded its sprites again while we were not looking
    // (a sprite that is just being set up although we have it down as dead: our list is history)
    for (int k = 0; k < 16; k++) {
      uint16_t id = R[0x0DD0 + k] == 8 ? slot_id(k) : NONE;
      if (id != NONE && in_dead(id)) { s_ndead = 0; break; }
    }
    for (int k = 0; k < 16; k++) s_ent[k].heard = s_now;
  }
  merge_peer_dead();
  scan_slots(resumed);
  apply_drops();
  apply_dead();
  bool urgent = false;
  for (int k = 0; k < 16; k++)
    if (s_ent[k].id != NONE && s_ent[k].phase == 0 && update_enemy(k)) urgent = true;
  materialize();
  for (int k = 0; k < 16; k++) if (s_ent[k].id != NONE) read_fields(k, s_ent[k].pre);
  send_packet(true, urgent);
  debug_dump();
}

void enemy_recv(uint32_t cid, const uint8_t *d, int len) {
  if (len < 12) return;
  PeerEn *p = peer_get(cid, true);
  if (!p) return;
  bool running = (d[1] & PF_RUNNING) != 0;
  uint32_t loc = rd32(d + 2), stamp = rd32(d + 6);
  uint64_t now = app_ms();
  if (stamp <= p->stamp && now - p->rx < 500) return; // late or doubled packet
  if (loc != p->loc || !running) for (int j = 0; j < PREC_MAX; j++) p->rec[j].id = NONE;
  // Whose place is the dead list from? Versions up to 1.1 do not say, and while they walk into a room
  // they still send the list of the room they left under the new room's name. Their list is only
  // believed once they run there, and for as long as we keep hearing from them without a break.
  if (loc != p->loc || now - p->rx > 1000) p->ranHere = false;
  if (running) p->ranHere = true;
  bool ownList = (d[1] & PF_OWN_LIST) || p->ranHere;
  if (!ownList && d[10] && debug_every()) app_log("enemy: t=%07u dead list of %08x (%d) not believed, it does not run in %05x yet", (unsigned)(now % 10000000), cid, d[10], loc);
  p->loc = loc; p->stamp = stamp; p->running = running;
  p->rx = now;
  p->ndead = 0;
  int n = 10;
  int nd = d[n++];
  if (len < n + nd * 2 + 1) return;
  if (ownList) p->ndead = nd > 64 ? 64 : nd;
  for (int i = 0; i < p->ndead; i++) p->dead[i] = rd16(d + n + i * 2);
  n += nd * 2;
  int nr = d[n++];
  for (int i = 0; i < nr; i++) {
    if (len < n + 12) return;
    uint16_t id = rd16(d + n), seq = rd16(d + n + 2);
    uint8_t flags = d[n + 4];
    const uint8_t *mask = d + n + 5;
    n += 12;
    PRec *r = NULL, *unused = NULL, *oldest = &p->rec[0];
    for (int j = 0; j < PREC_MAX; j++) {
      PRec *q = &p->rec[j];
      if (q->id == id) { r = q; break; }
      if (q->id == NONE) { if (!unused) unused = q; }
      else if (q->rx < oldest->rx) oldest = q;
    }
    if (!r) r = unused ? unused : oldest;
    r->id = id; r->seq = seq; r->flags = flags; r->stamp = stamp; r->rx = p->rx;
    for (int f = 0; f < NF; f++) {
      if (mask[f >> 3] & (1 << (f & 7))) { if (n >= len) { r->id = NONE; return; } r->f[f] = d[n++]; }
      else r->f[f] = 0;
    }
  }
  // every packet lists all the enemies its sender runs: the rest it has let go of
  for (int j = 0; j < PREC_MAX; j++) {
    PRec *q = &p->rec[j];
    if (q->id != NONE && !(q->id & PH_DROP) && q->stamp != stamp) q->id = NONE;
  }
}

void enemy_peer_left(uint32_t cid) {
  PeerEn *p = peer_get(cid, false);
  if (p) p->used = false;
}

void enemy_reset(void) {
  for (int k = 0; k < 16; k++) s_ent[k].id = NONE;
  for (int i = 0; i < MAX_PLAYERS; i++) for (int j = 0; j < PREC_MAX; j++) s_peer[i].rec[j].id = NONE;
  s_ndead = 0;
  s_locValid = false;
  s_lastRun = 0;
}
