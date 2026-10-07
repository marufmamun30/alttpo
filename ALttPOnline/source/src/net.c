// Serverless rooms.
//
// Nobody has to host a server or forward a port:
//  * A room is a 5 letter code. From the code we derive a topic name and an
//    encryption key. Players meet on public MQTT brokers (outbound TCP only)
//    and exchange their addresses there, encrypted with the room key.
//  * Every player learns their public UDP address from public STUN servers and
//    the players punch UDP holes to each other (full mesh).
//  * Until a hole is open (or when the routers make it impossible) traffic
//    between the two players is relayed through the broker, so a room always
//    works; it is just slower on that link.
// On top of that there is an unreliable datagram channel and a reliable,
// ordered message channel (used for events and for sending the seed patch).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define sock_err() WSAGetLastError()
#define SOCK_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS || (e) == WSAEALREADY)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define sock_err() errno
#define SOCK_WOULDBLOCK(e) ((e) == EWOULDBLOCK || (e) == EAGAIN || (e) == EINPROGRESS)
#endif
#include <SDL.h>
#include "app.h"

// ------------------------------------------------------------------ crypto
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  size_t total = ((len + 9 + 63) / 64) * 64;
  uint8_t *m = calloc(1, total);
  memcpy(m, data, len);
  m[len] = 0x80;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
  for (size_t off = 0; off < total; off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)m[off + i * 4] << 24) | (m[off + i * 4 + 1] << 16) | (m[off + i * 4 + 2] << 8) | m[off + i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  free(m);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i];
  }
}

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
#define QR(a, b, c, d) a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12); a += b; d ^= a; d = rotl(d, 8); c += d; b ^= c; b = rotl(b, 7);

static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint8_t *data, int len) {
  uint32_t st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; i++) memcpy(&st[4 + i], key + i * 4, 4);
  st[12] = 0;
  for (int i = 0; i < 3; i++) memcpy(&st[13 + i], nonce + i * 4, 4);
  for (int off = 0; off < len; off += 64) {
    uint32_t x[16];
    memcpy(x, st, sizeof(x));
    for (int r = 0; r < 10; r++) {
      QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
      QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    uint8_t ks[64];
    for (int i = 0; i < 16; i++) { uint32_t v = x[i] + st[i]; memcpy(ks + i * 4, &v, 4); }
    for (int i = 0; i < 64 && off + i < len; i++) data[off + i] ^= ks[i];
    st[12]++;
  }
}

// ------------------------------------------------------------------ state
#define MAX_PEERS (MAX_PLAYERS - 1)
#define MAX_CAND 6
#define FRAG_SIZE 1100
#define RECV_WIN 512
#define NUM_BROKERS 2

typedef struct HostEnt {
  const char *host;
  int port;
  struct sockaddr_in addr;
  bool ok;
} HostEnt;

static HostEnt s_brokers[NUM_BROKERS] = {{"broker.emqx.io", 1883}, {"broker.hivemq.com", 1883}};
static HostEnt s_stuns[2] = {{"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}};
static SDL_atomic_t s_resolveState; // 0 idle, 1 running, 2 done
static uint64_t s_resolveAt;

typedef struct Mqtt {
  SOCKET s;
  int st; // 0 off, 1 connecting, 2 waiting for CONNACK, 3 ready
  uint8_t *rx; int rxLen;
  uint8_t *tx; int txLen, txCap;
  uint64_t tStart, tLastSend, tLastRecv, tRetry;
} Mqtt;
#define MQTT_RX 262144

typedef struct Frag {
  uint32_t seq;
  uint16_t len;
  uint8_t flags;
  bool acked;
  uint64_t sentAt;
  int sends;
  uint8_t data[FRAG_SIZE];
} Frag;

typedef struct RFrag {
  bool have;
  uint8_t flags;
  uint16_t len;
  uint8_t *data;
} RFrag;

typedef struct Peer {
  bool used, connected, direct;
  uint32_t cid;
  uint32_t myEpoch, theirEpoch; // identify this incarnation of the link on both sides
  struct sockaddr_in cand[MAX_CAND];
  int nCand;
  struct sockaddr_in addr;
  uint64_t tFirst, tHeard, tUdp, tPunch, tPing, tRelayPing, tRelaySend;
  int relayBudget;
  int rtt;
  // reliable send
  Frag **sq; int sqLen, sqCap;
  uint32_t sendSeq;
  // reliable receive
  uint32_t recvNext;
  RFrag rwin[RECV_WIN];
  uint8_t *asmBuf; int asmLen, asmCap;
  bool ackDue;
} Peer;

static Peer s_peers[MAX_PEERS];
static Mqtt s_mq[NUM_BROKERS];
static SOCKET s_udp = INVALID_SOCKET;
static int s_udpPort;
static int s_state = NET_OFF;
static bool s_isHost;
static char s_code[8];
static char s_topic[24];
static uint8_t s_key[32];
static uint32_t s_roomTag;
static uint32_t s_cid;
static uint32_t s_sigSeq;
static uint64_t s_tStart, s_tAnnounce, s_tStun, s_tBrokerReady;
static int s_stunTries;
static struct sockaddr_in s_cands[MAX_CAND];
static int s_nCands, s_nLocalCands;
static bool s_havePublic;
static char s_status[96];
static NetRecvFn s_onRecv;
static NetPeerFn s_onPeer;
static uint64_t s_seen[512];
static int s_seenPos;
static int s_upBytes, s_downBytes, s_upBps, s_downBps;
static uint64_t s_tStats;
static bool s_wsa;
static bool s_noUdp; // test switch: force everything through the relay

enum { P_PING = 1, P_PONG, P_DATA, P_REL, P_ACK, P_BYE };
enum { S_ANNOUNCE = 1, S_LEAVE, S_RELAY };

static uint64_t now_ms(void) { return app_ms(); }

static void set_nonblock(SOCKET s) {
#ifdef _WIN32
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static uint32_t rnd32(void) {
  static uint64_t st;
  if (!st) st = ((uint64_t)time(NULL) << 20) ^ SDL_GetPerformanceCounter() ^ ((uint64_t)(uintptr_t)&st << 7);
  st ^= st << 13; st ^= st >> 7; st ^= st << 17;
  return (uint32_t)(st >> 16);
}

// ------------------------------------------------------------------ DNS (background thread)
static int resolve_thread(void *arg) {
  (void)arg;
  HostEnt *all[NUM_BROKERS + 2];
  int n = 0;
  for (int i = 0; i < NUM_BROKERS; i++) all[n++] = &s_brokers[i];
  for (int i = 0; i < 2; i++) all[n++] = &s_stuns[i];
  for (int i = 0; i < n; i++) {
    if (all[i]->ok) continue;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(all[i]->host, NULL, &hints, &res) == 0 && res) {
      struct sockaddr_in a = *(struct sockaddr_in *)res->ai_addr;
      a.sin_port = htons((uint16_t)all[i]->port);
      all[i]->addr = a;
      all[i]->ok = true;
    }
    if (res) freeaddrinfo(res);
  }
  SDL_AtomicSet(&s_resolveState, 2);
  return 0;
}

static void start_resolve(void) {
  if (SDL_AtomicGet(&s_resolveState) == 1) return;
  bool all = true;
  for (int i = 0; i < NUM_BROKERS; i++) all = all && s_brokers[i].ok;
  for (int i = 0; i < 2; i++) all = all && s_stuns[i].ok;
  if (all) return;
  SDL_AtomicSet(&s_resolveState, 1);
  s_resolveAt = now_ms();
  SDL_Thread *t = SDL_CreateThread(resolve_thread, "resolve", NULL);
  if (t) SDL_DetachThread(t); else SDL_AtomicSet(&s_resolveState, 0);
}

void net_init(void) {
#ifdef _WIN32
  if (!s_wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); s_wsa = true; }
#endif
  for (int i = 0; i < NUM_BROKERS; i++) s_mq[i].s = INVALID_SOCKET;
  const char *b = getenv("ALTTPO_BROKER"); // "host:port", for tests with a private broker
  if (b) {
    static char host[128];
    snprintf(host, sizeof(host), "%s", b);
    char *c = strchr(host, ':');
    int port = 1883;
    if (c) { *c = 0; port = atoi(c + 1); }
    s_brokers[0].host = host; s_brokers[0].port = port; s_brokers[0].ok = false;
    s_brokers[1].host = host; s_brokers[1].port = port; s_brokers[1].ok = false;
  }
  s_cid = rnd32() | 1;
  s_noUdp = getenv("ALTTPO_NO_UDP") != NULL;
  start_resolve();
}

void net_set_callbacks(NetRecvFn recv, NetPeerFn peer) { s_onRecv = recv; s_onPeer = peer; }
int net_state(void) { return s_state; }
const char *net_status_text(void) { return s_status; }
const char *net_room_code(void) { return s_code; }
uint32_t net_my_cid(void) { return s_cid; }
void net_stats(int *up, int *down) { if (up) *up = s_upBps; if (down) *down = s_downBps; }

// ------------------------------------------------------------------ MQTT
static void mq_close(Mqtt *m) {
  if (m->s != INVALID_SOCKET) closesocket(m->s);
  m->s = INVALID_SOCKET;
  m->st = 0;
  m->rxLen = m->txLen = 0;
}

static void mq_queue(Mqtt *m, const uint8_t *d, int n) {
  if (m->txLen + n > m->txCap) {
    m->txCap = (m->txLen + n) * 2 + 4096;
    m->tx = realloc(m->tx, m->txCap);
  }
  memcpy(m->tx + m->txLen, d, n);
  m->txLen += n;
}

static int put_varlen(uint8_t *p, int v) {
  int n = 0;
  do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; p[n++] = b; } while (v);
  return n;
}

static void mq_packet(Mqtt *m, uint8_t hdr, const uint8_t *a, int an, const uint8_t *b, int bn) {
  uint8_t h[6];
  h[0] = hdr;
  int n = 1 + put_varlen(h + 1, an + bn);
  mq_queue(m, h, n);
  if (an) mq_queue(m, a, an);
  if (bn) mq_queue(m, b, bn);
}

static void mq_start(Mqtt *m, HostEnt *h) {
  mq_close(m);
  if (!h->ok) return;
  if (!m->rx) m->rx = malloc(MQTT_RX);
  m->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (m->s == INVALID_SOCKET) return;
  set_nonblock(m->s);
  int one = 1;
  setsockopt(m->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
  connect(m->s, (struct sockaddr *)&h->addr, sizeof(h->addr));
  m->st = 1;
  m->tStart = m->tLastRecv = m->tLastSend = now_ms();
  // CONNECT is queued right away, it is flushed once the socket is writable
  uint8_t vh[64];
  int n = 0;
  static const uint8_t proto[] = {0, 4, 'M', 'Q', 'T', 'T', 4, 2, 0, 60};
  memcpy(vh, proto, 10); n = 10;
  char cidstr[32];
  snprintf(cidstr, sizeof(cidstr), "zo%08x%04x", s_cid, rnd32() & 0xffff);
  int cl = (int)strlen(cidstr);
  vh[n++] = 0; vh[n++] = (uint8_t)cl;
  memcpy(vh + n, cidstr, cl); n += cl;
  mq_packet(m, 0x10, vh, n, NULL, 0);
}

static void mq_subscribe(Mqtt *m) {
  uint8_t b[160];
  int n = 0;
  b[n++] = 0; b[n++] = 1; // packet id
  char t[2][64];
  snprintf(t[0], 64, "alttpo%d/%s/all", PROTO_VERSION, s_topic);
  snprintf(t[1], 64, "alttpo%d/%s/%08x", PROTO_VERSION, s_topic, s_cid);
  for (int i = 0; i < 2; i++) {
    int l = (int)strlen(t[i]);
    b[n++] = 0; b[n++] = (uint8_t)l;
    memcpy(b + n, t[i], l); n += l;
    b[n++] = 0; // QoS 0
  }
  mq_packet(m, 0x82, b, n, NULL, 0);
}

static void sig_handle(const uint8_t *payload, int len);

static void mq_poll(Mqtt *m, HostEnt *h) {
  uint64_t t = now_ms();
  if (m->st == 0) {
    if (s_state != NET_OFF && h->ok && t - m->tRetry > 4000) { m->tRetry = t; mq_start(m, h); }
    return;
  }
  // flush
  while (m->txLen > 0) {
    int n = send(m->s, (const char *)m->tx, m->txLen, 0);
    if (n > 0) {
      memmove(m->tx, m->tx + n, m->txLen - n);
      m->txLen -= n;
      m->tLastSend = t;
      if (m->st == 1) m->st = 2;
    } else {
      int e = sock_err();
      if (n < 0 && (SOCK_WOULDBLOCK(e)
#ifdef _WIN32
          || e == WSAENOTCONN
#endif
          )) break;
      mq_close(m);
      return;
    }
  }
  if (m->st <= 2 && t - m->tStart > 10000) { mq_close(m); return; }
  // receive
  for (;;) {
    int space = MQTT_RX - m->rxLen;
    if (space <= 0) { mq_close(m); return; }
    int n = recv(m->s, (char *)m->rx + m->rxLen, space, 0);
    if (n > 0) { m->rxLen += n; m->tLastRecv = t; continue; }
    if (n == 0) { mq_close(m); return; }
    int e = sock_err();
    if (SOCK_WOULDBLOCK(e)
#ifdef _WIN32
        || e == WSAENOTCONN
#endif
        ) break;
    mq_close(m);
    return;
  }
  // parse
  int off = 0;
  while (m->rxLen - off >= 2) {
    uint8_t *p = m->rx + off;
    int rem = 0, mult = 1, i = 1;
    bool complete = false;
    while (i < m->rxLen - off && i <= 4) {
      rem += (p[i] & 0x7f) * mult;
      mult *= 128;
      if (!(p[i++] & 0x80)) { complete = true; break; }
    }
    if (!complete) break;
    if (m->rxLen - off < i + rem) break;
    uint8_t type = p[0] >> 4;
    uint8_t *body = p + i;
    if (type == 2) { // CONNACK
      if (rem >= 2 && body[1] == 0) {
        m->st = 3;
        mq_subscribe(m);
        s_tAnnounce = 0;
      } else {
        mq_close(m);
        return;
      }
    } else if (type == 3) { // PUBLISH
      if (rem >= 2) {
        int tl = (body[0] << 8) | body[1];
        if (2 + tl <= rem) sig_handle(body + 2 + tl, rem - 2 - tl);
      }
    }
    off += i + rem;
  }
  if (off) { memmove(m->rx, m->rx + off, m->rxLen - off); m->rxLen -= off; }
  if (m->st == 3) {
    if (t - m->tLastSend > 25000) { uint8_t ping[2] = {0xc0, 0}; mq_queue(m, ping, 2); m->tLastSend = t; }
    if (t - m->tLastRecv > 90000) mq_close(m);
  }
}

static bool mq_any_ready(void) {
  for (int i = 0; i < NUM_BROKERS; i++) if (s_mq[i].st == 3) return true;
  return false;
}

// publishes an encrypted signalling message; dst 0 = the whole room
static void sig_publish(uint32_t dst, uint8_t type, const uint8_t *body, int len) {
  if (!mq_any_ready()) return;
  uint8_t *buf = malloc(len + 64);
  for (int i = 0; i < 3; i++) { uint32_t r = rnd32(); memcpy(buf + i * 4, &r, 4); }
  uint8_t *p = buf + 12;
  p[0] = 'Z'; p[1] = 'O'; p[2] = PROTO_VERSION; p[3] = type;
  memcpy(p + 4, &s_cid, 4);
  uint32_t seq = ++s_sigSeq;
  memcpy(p + 8, &seq, 4);
  if (len) memcpy(p + 12, body, len);
  chacha20_xor(s_key, buf, p, 12 + len);
  char topic[64];
  if (dst) snprintf(topic, sizeof(topic), "alttpo%d/%s/%08x", PROTO_VERSION, s_topic, dst);
  else snprintf(topic, sizeof(topic), "alttpo%d/%s/all", PROTO_VERSION, s_topic);
  int tl = (int)strlen(topic);
  uint8_t th[66];
  th[0] = 0; th[1] = (uint8_t)tl;
  memcpy(th + 2, topic, tl);
  for (int i = 0; i < NUM_BROKERS; i++)
    if (s_mq[i].st == 3) mq_packet(&s_mq[i], 0x30, th, tl + 2, buf, 24 + len);
  s_upBytes += 24 + len + tl + 4;
  free(buf);
}

// ------------------------------------------------------------------ peers
static Peer *peer_find(uint32_t cid) {
  for (int i = 0; i < MAX_PEERS; i++) if (s_peers[i].used && s_peers[i].cid == cid) return &s_peers[i];
  return NULL;
}

static void peer_free(Peer *p) {
  for (int i = 0; i < p->sqLen; i++) free(p->sq[i]);
  free(p->sq);
  for (int i = 0; i < RECV_WIN; i++) free(p->rwin[i].data);
  free(p->asmBuf);
  memset(p, 0, sizeof(*p));
}

static void peer_drop(Peer *p, const char *why) {
  uint32_t cid = p->cid;
  bool was = p->connected;
  app_log("net: peer %08x dropped (%s)", cid, why);
  peer_free(p);
  if (was && s_onPeer) s_onPeer(cid, false);
}

static Peer *peer_get(uint32_t cid) {
  Peer *p = peer_find(cid);
  if (p) return p;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!s_peers[i].used) {
      p = &s_peers[i];
      memset(p, 0, sizeof(*p));
      p->used = true;
      p->cid = cid;
      p->tFirst = p->tHeard = now_ms();
      p->rtt = 100;
      p->myEpoch = rnd32() | 1;
      app_log("net: new peer %08x", cid);
      return p;
    }
  }
  return NULL;
}

static void peer_connected(Peer *p) {
  if (p->connected) return;
  p->connected = true;
  if (s_state == NET_SEARCHING) { s_state = NET_ONLINE; snprintf(s_status, sizeof(s_status), "Connected"); }
  app_log("net: peer %08x connected", p->cid);
  if (s_onPeer) s_onPeer(p->cid, true);
}

int net_peers(NetPeerInfo *out, int max) {
  int n = 0;
  for (int i = 0; i < MAX_PEERS && n < max; i++) {
    Peer *p = &s_peers[i];
    if (!p->used || !p->connected) continue;
    out[n].cid = p->cid; out[n].direct = p->direct; out[n].rtt = p->rtt; out[n].connected = true;
    n++;
  }
  return n;
}

bool net_peer_direct(uint32_t cid) {
  Peer *p = peer_find(cid);
  return p && p->direct;
}

int net_rel_backlog(uint32_t cid) {
  Peer *p = peer_find(cid);
  return p ? p->sqLen : 0;
}

// ------------------------------------------------------------------ packets
#define PKT_HEAD 23
static int pkt_head(uint8_t *b, uint8_t type, const Peer *p) {
  b[0] = 'Z'; b[1] = 'O'; b[2] = type;
  memcpy(b + 3, &s_roomTag, 4);
  memcpy(b + 7, &s_cid, 4);
  memcpy(b + 11, &p->cid, 4);
  memcpy(b + 15, &p->myEpoch, 4);
  memcpy(b + 19, &p->theirEpoch, 4);
  return PKT_HEAD;
}

static void udp_send(const struct sockaddr_in *a, const uint8_t *b, int n) {
  if (s_udp == INVALID_SOCKET) return;
  sendto(s_udp, (const char *)b, n, 0, (const struct sockaddr *)a, sizeof(*a));
  s_upBytes += n + 28;
}

// sends a packet to a peer on the best path; "important" packets ignore the relay budget
static void pkt_send(Peer *p, const uint8_t *b, int n, bool important) {
  if (p->direct) { udp_send(&p->addr, b, n); return; }
  uint64_t t = now_ms();
  if (t - p->tRelaySend >= 1000) { p->tRelaySend = t; p->relayBudget = 24; }
  if (!important) {
    if (p->relayBudget <= 8) return;
  }
  if (p->relayBudget <= 0) return;
  p->relayBudget--;
  sig_publish(p->cid, S_RELAY, b, n);
}

static void rel_deliver(Peer *p);

static void pkt_handle(const uint8_t *b, int n, const struct sockaddr_in *from) {
  if (n < PKT_HEAD || b[0] != 'Z' || b[1] != 'O') return;
  uint32_t tag, src, dst, srcEpoch, dstEpoch;
  memcpy(&tag, b + 3, 4); memcpy(&src, b + 7, 4); memcpy(&dst, b + 11, 4);
  memcpy(&srcEpoch, b + 15, 4); memcpy(&dstEpoch, b + 19, 4);
  if (tag != s_roomTag || src == s_cid || dst != s_cid) return;
  uint8_t type = b[2];
  if (s_noUdp && from) return;
  Peer *p = peer_get(src);
  if (!p) return;
  if (p->theirEpoch && srcEpoch != p->theirEpoch) {
    // the other side forgot about us and started over: do the same
    struct sockaddr_in cand[MAX_CAND];
    int nc = p->nCand;
    memcpy(cand, p->cand, sizeof(cand));
    peer_drop(p, "restarted");
    p = peer_get(src);
    if (!p) return;
    memcpy(p->cand, cand, sizeof(cand));
    p->nCand = nc;
  }
  p->theirEpoch = srcEpoch;
  if ((type == P_DATA || type == P_REL || type == P_ACK) && dstEpoch != p->myEpoch) return; // for an older incarnation
  uint64_t t = now_ms();
  p->tHeard = t;
  if (from) p->tUdp = t;
  const uint8_t *d = b + PKT_HEAD;
  int dl = n - PKT_HEAD;
  switch (type) {
    case P_PING: {
      uint8_t r[32];
      int rn = pkt_head(r, P_PONG, p);
      if (dl >= 4) { memcpy(r + rn, d, 4); rn += 4; }
      if (from) {
        if (!p->direct) p->addr = *from;
        udp_send(from, r, rn);
      } else {
        sig_publish(src, S_RELAY, r, rn);
      }
      peer_connected(p);
      break;
    }
    case P_PONG:
      if (from) {
        if (!p->direct) app_log("net: direct link to %08x", src);
        p->direct = true;
        p->addr = *from;
        if (dl >= 4) {
          uint32_t ts; memcpy(&ts, d, 4);
          int rtt = (int)((uint32_t)t - ts);
          if (rtt >= 0 && rtt < 5000) p->rtt = (p->rtt * 3 + rtt) / 4;
        }
      } else if (dl >= 4 && !p->direct) {
        uint32_t ts; memcpy(&ts, d, 4);
        int rtt = (int)((uint32_t)t - ts);
        if (rtt >= 0 && rtt < 10000) p->rtt = (p->rtt + rtt) / 2;
      }
      peer_connected(p);
      break;
    case P_DATA:
      if (!p->connected) break;
      if (s_onRecv && dl > 0) s_onRecv(src, d, dl, false);
      break;
    case P_REL: {
      if (dl < 5) break;
      peer_connected(p);
      uint32_t seq; memcpy(&seq, d, 4);
      uint8_t flags = d[4];
      int pl = dl - 5;
      p->ackDue = true;
      uint32_t idx = seq - p->recvNext;
      if (idx >= RECV_WIN) break; // old duplicate or too far ahead
      RFrag *f = &p->rwin[idx];
      if (f->have) break;
      f->have = true; f->flags = flags; f->len = (uint16_t)pl;
      f->data = malloc(pl ? pl : 1);
      memcpy(f->data, d + 5, pl);
      rel_deliver(p);
      break;
    }
    case P_ACK: {
      if (dl < 8) break;
      uint32_t cum, sack; memcpy(&cum, d, 4); memcpy(&sack, d + 4, 4);
      for (int i = 0; i < p->sqLen; i++) {
        Frag *f = p->sq[i];
        uint32_t rel = f->seq - cum;
        if ((int32_t)(f->seq - cum) < 0) f->acked = true;
        else if (rel >= 1 && rel <= 32 && (sack & (1u << (rel - 1)))) f->acked = true;
      }
      int k = 0;
      while (k < p->sqLen && p->sq[k]->acked) { free(p->sq[k]); k++; }
      if (k) { memmove(p->sq, p->sq + k, (p->sqLen - k) * sizeof(Frag *)); p->sqLen -= k; }
      break;
    }
    case P_BYE:
      peer_drop(p, "left");
      break;
  }
}

static void rel_deliver(Peer *p) {
  uint32_t cid = p->cid;
  while (p->rwin[0].have) {
    RFrag f = p->rwin[0];
    memmove(&p->rwin[0], &p->rwin[1], sizeof(RFrag) * (RECV_WIN - 1));
    memset(&p->rwin[RECV_WIN - 1], 0, sizeof(RFrag));
    p->recvNext++;
    if (f.flags & 1) p->asmLen = 0;
    if (p->asmLen + f.len > p->asmCap) {
      p->asmCap = (p->asmLen + f.len) * 2 + 2048;
      p->asmBuf = realloc(p->asmBuf, p->asmCap);
    }
    memcpy(p->asmBuf + p->asmLen, f.data, f.len);
    p->asmLen += f.len;
    free(f.data);
    if (f.flags & 2) {
      int len = p->asmLen;
      p->asmLen = 0;
      if (s_onRecv && len > 0) {
        uint8_t *copy = malloc(len);
        memcpy(copy, p->asmBuf, len);
        s_onRecv(cid, copy, len, true);
        free(copy);
        if (peer_find(cid) != p) return; // the callback removed the peer
      }
    }
  }
}

static void rel_pump(Peer *p, uint64_t t) {
  int win = p->direct ? 48 : 6;
  int rto = p->direct ? p->rtt * 2 + 60 : 1500;
  if (rto < 80) rto = 80;
  for (int i = 0; i < p->sqLen && i < win; i++) {
    Frag *f = p->sq[i];
    if (f->acked) continue;
    if (f->sends && t - f->sentAt < (uint64_t)(rto * (f->sends > 4 ? 2 : 1))) continue;
    uint8_t b[FRAG_SIZE + 32];
    int n = pkt_head(b, P_REL, p);
    memcpy(b + n, &f->seq, 4); n += 4;
    b[n++] = f->flags;
    memcpy(b + n, f->data, f->len); n += f->len;
    if (!p->direct && p->relayBudget <= 0 && t - p->tRelaySend < 1000) break;
    pkt_send(p, b, n, true);
    f->sentAt = t;
    f->sends++;
  }
  if (p->ackDue) {
    p->ackDue = false;
    uint8_t b[32];
    int n = pkt_head(b, P_ACK, p);
    uint32_t sack = 0;
    for (int i = 1; i <= 32; i++) if (p->rwin[i].have) sack |= 1u << (i - 1);
    memcpy(b + n, &p->recvNext, 4); n += 4;
    memcpy(b + n, &sack, 4); n += 4;
    pkt_send(p, b, n, true);
  }
}

void net_send(uint32_t cid, const void *data, int len) {
  if (s_state != NET_ONLINE || len > 1300) return;
  uint8_t b[1400];
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer *p = &s_peers[i];
    if (!p->used || !p->connected) continue;
    if (cid && p->cid != cid) continue;
    int n = pkt_head(b, P_DATA, p);
    memcpy(b + n, data, len);
    pkt_send(p, b, n + len, false);
  }
}

void net_send_rel(uint32_t cid, const void *data, int len) {
  if (s_state != NET_ONLINE || len <= 0) return;
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer *p = &s_peers[i];
    if (!p->used || !p->connected) continue;
    if (cid && p->cid != cid) continue;
    const uint8_t *d = data;
    int left = len;
    bool first = true;
    while (left > 0) {
      int take = left > FRAG_SIZE ? FRAG_SIZE : left;
      Frag *f = calloc(1, sizeof(Frag));
      f->seq = p->sendSeq++;
      f->len = (uint16_t)take;
      f->flags = (first ? 1 : 0) | (left == take ? 2 : 0);
      memcpy(f->data, d, take);
      if (p->sqLen == p->sqCap) { p->sqCap = p->sqCap * 2 + 16; p->sq = realloc(p->sq, p->sqCap * sizeof(Frag *)); }
      p->sq[p->sqLen++] = f;
      d += take; left -= take; first = false;
    }
  }
}

// ------------------------------------------------------------------ signalling
static void send_announce(void) {
  uint8_t b[8 + MAX_CAND * 6];
  int n = 0;
  b[n++] = s_isHost ? 1 : 0;
  b[n++] = (uint8_t)s_nCands;
  for (int i = 0; i < s_nCands; i++) {
    memcpy(b + n, &s_cands[i].sin_addr, 4); n += 4;
    memcpy(b + n, &s_cands[i].sin_port, 2); n += 2;
  }
  sig_publish(0, S_ANNOUNCE, b, n);
  s_tAnnounce = now_ms();
}

static void sig_handle(const uint8_t *payload, int len) {
  if (len < 24 || len > 4096) return;
  s_downBytes += len;
  uint8_t buf[4096];
  int pl = len - 12;
  memcpy(buf, payload + 12, pl);
  chacha20_xor(s_key, payload, buf, pl);
  if (buf[0] != 'Z' || buf[1] != 'O' || buf[2] != PROTO_VERSION) return;
  uint8_t type = buf[3];
  uint32_t src, seq;
  memcpy(&src, buf + 4, 4); memcpy(&seq, buf + 8, 4);
  if (src == s_cid) return;
  uint64_t id = ((uint64_t)src << 32) | seq;
  for (int i = 0; i < 512; i++) if (s_seen[i] == id) return; // same message from the other broker
  s_seen[s_seenPos] = id; s_seenPos = (s_seenPos + 1) & 511;
  const uint8_t *b = buf + 12;
  int n = pl - 12;
  switch (type) {
    case S_ANNOUNCE: {
      if (n < 2) return;
      bool known = peer_find(src) != NULL;
      Peer *p = peer_get(src);
      if (!p) return;
      p->tHeard = now_ms();
      int nc = b[1];
      if (nc > MAX_CAND) nc = MAX_CAND;
      if (n < 2 + nc * 6) return;
      p->nCand = nc;
      for (int i = 0; i < nc; i++) {
        memset(&p->cand[i], 0, sizeof(p->cand[i]));
        p->cand[i].sin_family = AF_INET;
        memcpy(&p->cand[i].sin_addr, b + 2 + i * 6, 4);
        memcpy(&p->cand[i].sin_port, b + 6 + i * 6, 2);
      }
      if (!known) send_announce(); // let the newcomer know about us right away
      break;
    }
    case S_LEAVE: {
      Peer *p = peer_find(src);
      if (p) peer_drop(p, "left");
      break;
    }
    case S_RELAY:
      pkt_handle(b, n, NULL);
      break;
  }
}

// ------------------------------------------------------------------ UDP / STUN
static void add_cand(uint32_t ipNet, uint16_t portNet) {
  for (int i = 0; i < s_nCands; i++)
    if (s_cands[i].sin_addr.s_addr == ipNet && s_cands[i].sin_port == portNet) return;
  if (s_nCands >= MAX_CAND) return;
  memset(&s_cands[s_nCands], 0, sizeof(s_cands[0]));
  s_cands[s_nCands].sin_family = AF_INET;
  s_cands[s_nCands].sin_addr.s_addr = ipNet;
  s_cands[s_nCands].sin_port = portNet;
  s_nCands++;
}

static void open_udp(void) {
  if (s_udp != INVALID_SOCKET) closesocket(s_udp);
  s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  bind(s_udp, (struct sockaddr *)&a, sizeof(a));
  socklen_t al = sizeof(a);
  getsockname(s_udp, (struct sockaddr *)&a, &al);
  s_udpPort = ntohs(a.sin_port);
  set_nonblock(s_udp);
  int buf = 1 << 20;
  setsockopt(s_udp, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof(buf));
#ifdef _WIN32
  // do not let ICMP "port unreachable" replies break the socket while punching
  DWORD off = 0, ret = 0;
  WSAIoctl(s_udp, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
#endif
  s_nCands = 0;
  // primary LAN address: the one the OS would use to reach the internet
  SOCKET t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in g;
  memset(&g, 0, sizeof(g));
  g.sin_family = AF_INET; g.sin_port = htons(53); g.sin_addr.s_addr = htonl(0x08080808);
  if (connect(t, (struct sockaddr *)&g, sizeof(g)) == 0) {
    al = sizeof(a);
    if (getsockname(t, (struct sockaddr *)&a, &al) == 0 && a.sin_addr.s_addr) add_cand(a.sin_addr.s_addr, htons((uint16_t)s_udpPort));
  }
  closesocket(t);
  char host[256];
  if (gethostname(host, sizeof(host)) == 0) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) == 0) {
      for (struct addrinfo *r = res; r && s_nCands < 3; r = r->ai_next) {
        uint32_t ip = ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr;
        if ((ntohl(ip) >> 16) == 0xA9FE || (ntohl(ip) >> 24) == 127) continue;
        add_cand(ip, htons((uint16_t)s_udpPort));
      }
      freeaddrinfo(res);
    }
  }
  if (s_nCands == 0) add_cand(htonl(0x7f000001), htons((uint16_t)s_udpPort));
  s_nLocalCands = s_nCands;
  s_havePublic = false;
  s_stunTries = 0;
  s_tStun = 0;
}

static void stun_send(void) {
  for (int i = 0; i < 2; i++) {
    if (!s_stuns[i].ok) continue;
    uint8_t b[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    for (int k = 0; k < 3; k++) { uint32_t r = rnd32(); memcpy(b + 8 + k * 4, &r, 4); }
    udp_send(&s_stuns[i].addr, b, 20);
  }
}

static bool stun_handle(const uint8_t *b, int n) {
  if (n < 20 || b[0] > 1 || b[4] != 0x21 || b[5] != 0x12 || b[6] != 0xA4 || b[7] != 0x42) return false;
  if (b[0] != 0x01 || b[1] != 0x01) return true;
  int i = 20;
  while (i + 4 <= n) {
    int t = (b[i] << 8) | b[i + 1], l = (b[i + 2] << 8) | b[i + 3];
    if (i + 4 + l > n) break;
    if ((t == 0x0020 || t == 0x0001) && l >= 8 && b[i + 5] == 1) {
      uint8_t ip[4]; uint8_t port[2];
      memcpy(port, b + i + 6, 2); memcpy(ip, b + i + 8, 4);
      if (t == 0x0020) {
        port[0] ^= 0x21; port[1] ^= 0x12;
        ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42;
      }
      uint32_t ipn; uint16_t pn;
      memcpy(&ipn, ip, 4); memcpy(&pn, port, 2);
      int before = s_nCands;
      add_cand(ipn, pn);
      if (s_nCands != before) { s_havePublic = true; s_tAnnounce = 0; }
      else s_havePublic = true;
      return true;
    }
    i += 4 + ((l + 3) & ~3);
  }
  return true;
}

static void udp_poll(void) {
  if (s_udp == INVALID_SOCKET) return;
  uint8_t b[2048];
  for (int k = 0; k < 512; k++) {
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(s_udp, (char *)b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) {
#ifdef _WIN32
      if (n < 0 && sock_err() == WSAECONNRESET) continue;
#endif
      break;
    }
    s_downBytes += n + 28;
    if (stun_handle(b, n)) continue;
    pkt_handle(b, n, &from);
  }
}

// ------------------------------------------------------------------ room control
static void room_begin(const char *code, bool host) {
  net_leave();
  snprintf(s_code, sizeof(s_code), "%s", code);
  for (char *c = s_code; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
  char tmp[64];
  uint8_t h[32];
  snprintf(tmp, sizeof(tmp), "alttpo-topic-v%d:%s", PROTO_VERSION, s_code);
  sha256((const uint8_t *)tmp, strlen(tmp), h);
  for (int i = 0; i < 8; i++) snprintf(s_topic + i * 2, 3, "%02x", h[i]);
  memcpy(&s_roomTag, h + 8, 4);
  snprintf(tmp, sizeof(tmp), "alttpo-key-v%d:%s", PROTO_VERSION, s_code);
  sha256((const uint8_t *)tmp, strlen(tmp), s_key);
  s_isHost = host;
  s_state = NET_CONNECTING;
  s_tStart = now_ms();
  s_tBrokerReady = 0;
  s_tAnnounce = 0;
  snprintf(s_status, sizeof(s_status), "Contacting matchmaking service...");
  memset(s_seen, 0, sizeof(s_seen));
  open_udp();
  start_resolve();
  for (int i = 0; i < NUM_BROKERS; i++) { s_mq[i].tRetry = 0; if (s_brokers[i].ok) { mq_start(&s_mq[i], &s_brokers[i]); s_mq[i].tRetry = now_ms(); } }
  app_log("net: %s room %s (cid %08x, udp port %d)", host ? "hosting" : "joining", s_code, s_cid, s_udpPort);
}

void net_host(char *codeOut) {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  char code[8];
  const char *force = getenv("ALTTPO_ROOM");
  if (force && strlen(force) == 5) snprintf(code, sizeof(code), "%s", force);
  else {
    for (int i = 0; i < 5; i++) code[i] = alphabet[rnd32() % 32];
    code[5] = 0;
  }
  room_begin(code, true);
  if (codeOut) strcpy(codeOut, s_code);
}

void net_join(const char *code) { room_begin(code, false); }

void net_leave(void) {
  if (s_state == NET_OFF) return;
  if (mq_any_ready()) {
    sig_publish(0, S_LEAVE, NULL, 0);
  }
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer *p = &s_peers[i];
    if (!p->used) continue;
    if (p->direct) { uint8_t b[32]; int n = pkt_head(b, P_BYE, p); udp_send(&p->addr, b, n); }
    bool was = p->connected;
    uint32_t cid = p->cid;
    peer_free(p);
    if (was && s_onPeer) s_onPeer(cid, false);
  }
  // give the LEAVE message a moment to go out
  for (int k = 0; k < 10; k++) {
    bool pending = false;
    for (int i = 0; i < NUM_BROKERS; i++) {
      Mqtt *m = &s_mq[i];
      if (m->st == 3 && m->txLen > 0) {
        int n = send(m->s, (const char *)m->tx, m->txLen, 0);
        if (n > 0) { memmove(m->tx, m->tx + n, m->txLen - n); m->txLen -= n; }
        if (m->txLen > 0) pending = true;
      }
    }
    if (!pending) break;
    SDL_Delay(10);
  }
  for (int i = 0; i < NUM_BROKERS; i++) mq_close(&s_mq[i]);
  if (s_udp != INVALID_SOCKET) closesocket(s_udp);
  s_udp = INVALID_SOCKET;
  s_state = NET_OFF;
  s_code[0] = 0;
  s_status[0] = 0;
}

void net_shutdown(void) { net_leave(); }

void net_poll(void) {
  uint64_t t = now_ms();
  if (t - s_tStats >= 1000) {
    s_upBps = s_upBytes; s_downBps = s_downBytes;
    s_upBytes = s_downBytes = 0;
    s_tStats = t;
  }
  if (SDL_AtomicGet(&s_resolveState) == 2) SDL_AtomicSet(&s_resolveState, 0);
  if (s_state == NET_OFF || s_state == NET_FAILED) return;
  if (SDL_AtomicGet(&s_resolveState) == 0 && t - s_resolveAt > 5000) start_resolve();

  for (int i = 0; i < NUM_BROKERS; i++) mq_poll(&s_mq[i], &s_brokers[i]);
  udp_poll();

  t = now_ms(); // the polls above stamp peers with the current time
  bool ready = mq_any_ready();
  if (ready && !s_tBrokerReady) s_tBrokerReady = t;

  if (s_state == NET_CONNECTING) {
    if (ready) {
      if (s_isHost) { s_state = NET_ONLINE; snprintf(s_status, sizeof(s_status), "Room open"); }
      else { s_state = NET_SEARCHING; snprintf(s_status, sizeof(s_status), "Looking for room %s...", s_code); }
    } else if (t - s_tStart > 15000) {
      s_state = NET_FAILED;
      snprintf(s_status, sizeof(s_status), "Could not reach the matchmaking service. Check your internet connection.");
      return;
    }
  } else if (s_state == NET_SEARCHING) {
    if (t - s_tBrokerReady > 14000) {
      s_state = NET_FAILED;
      snprintf(s_status, sizeof(s_status), "Room %s was not found.", s_code);
      return;
    }
  }

  // public address discovery
  if (!s_havePublic && s_stunTries < 8 && t - s_tStun > 400) {
    bool any = s_stuns[0].ok || s_stuns[1].ok;
    if (any) { stun_send(); s_stunTries++; s_tStun = t; }
  }

  // announce ourselves: often at first, then slowly
  if (ready) {
    uint64_t age = t - s_tBrokerReady;
    uint64_t every = age < 12000 ? 1500 : 6000;
    if (s_state == NET_SEARCHING) every = 1200;
    if (t - s_tAnnounce > every) send_announce();
  }

  for (int i = 0; i < MAX_PEERS; i++) {
    Peer *p = &s_peers[i];
    if (!p->used) continue;
    if ((int64_t)(t - p->tHeard) > 20000) { peer_drop(p, "timeout"); continue; }
    if (p->direct && (int64_t)(t - p->tUdp) > 5000) {
      p->direct = false;
      app_log("net: direct link to %08x lost, relaying", p->cid);
    }
    uint32_t ts = (uint32_t)t;
    uint8_t b[32];
    int n = pkt_head(b, P_PING, p);
    memcpy(b + n, &ts, 4); n += 4;
    if (p->direct) {
      if (t - p->tPing > 500) { p->tPing = t; udp_send(&p->addr, b, n); }
    } else {
      // punch: ping every candidate address (and the address they last reached us from)
      uint64_t every = (t - p->tFirst < 15000) ? 150 : 1000;
      if (t - p->tPunch > every) {
        p->tPunch = t;
        for (int c = 0; c < p->nCand; c++) udp_send(&p->cand[c], b, n);
        if (p->addr.sin_port) udp_send(&p->addr, b, n);
      }
      if (t - p->tRelayPing > (p->connected ? 2500 : 700)) {
        p->tRelayPing = t;
        sig_publish(p->cid, S_RELAY, b, n);
      }
    }
    if (p->connected) rel_pump(p, t);
  }
}
