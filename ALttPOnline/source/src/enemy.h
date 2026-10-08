// Shared enemies (see enemy.c)
#ifndef ENEMY_H
#define ENEMY_H
#include "app.h"

#define M_ENEMY 3   // unreliable message, same id space as the messages in game.c

typedef struct EnemyPeer { uint32_t cid; int16_t x, y; bool direct; } EnemyPeer;

typedef struct EnemyCtx {
  uint8_t *ram;        // WRAM
  uint32_t frame;      // game frame counter
  uint64_t now;        // ms
  uint32_t me;         // our player id
  bool running;        // the game moves its sprites this frame (no menu, text or transition)
  bool rando;          // door randomizer ROM: enemies can carry shuffled items
  uint32_t location;
  int16_t x, y;        // Link
  int npeers;          // players standing in the same place
  EnemyPeer peers[MAX_PLAYERS];
} EnemyCtx;

void enemy_reset(void);
void enemy_frame(const EnemyCtx *c);
void enemy_recv(uint32_t cid, const uint8_t *d, int len);
void enemy_peer_left(uint32_t cid);

#endif
