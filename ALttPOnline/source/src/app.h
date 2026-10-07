// A Link to the Past Online - shared declarations
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_VERSION "1.0"
#define PROTO_VERSION 1
#define MAX_PLAYERS 8

// ---------------------------------------------------------------- canvas / ui
#define CANVAS_W 512
#define CANVAS_H 448
extern uint32_t g_canvas[CANVAS_W * CANVAS_H];

#define COL_WHITE   0xF8F8F8
#define COL_GOLD    0xF8D038
#define COL_GOLD_D  0xB07818
#define COL_GRAY    0x98A0B0
#define COL_DARK    0x586070
#define COL_GREEN   0x58E058
#define COL_RED     0xF04848
#define COL_BLUE    0x68A8F8
#define COL_PANEL   0x0C1438

void ui_fill(int x, int y, int w, int h, uint32_t c);
void ui_blend(int x, int y, int w, int h, uint32_t c, int alpha);
void ui_frame(int x, int y, int w, int h);
void ui_frame_titled(int x, int y, int w, int h, const char *title);
int  ui_text(int x, int y, const char *s, uint32_t c, int scale);
int  ui_text_plain(int x, int y, const char *s, uint32_t c, int scale);
int  ui_text_w(const char *s, int scale);
void ui_text_center(int cx, int y, const char *s, uint32_t c, int scale);
void ui_text_right(int rx, int y, const char *s, uint32_t c, int scale);
int  ui_text_wrap(int x, int y, int w, const char *s, uint32_t c, int scale);
void ui_triforce(int cx, int cy, int size);
void ui_cursor(int x, int y);
void ui_heart(int x, int y, uint32_t c);
void ui_blit2x(const uint32_t *src, int sw, int sh, int dim);
extern uint32_t g_uiTick;

// input actions for menus
enum {
  UI_UP = 1, UI_DOWN = 2, UI_LEFT = 4, UI_RIGHT = 8, UI_OK = 16, UI_BACK = 32,
  UI_TAB_L = 64, UI_TAB_R = 128, UI_MENU = 256, UI_ALT = 512, UI_DEL = 1024,
};
extern int g_uiPressed;        // actions pressed this frame (with key repeat)
extern char g_uiText[32];      // characters typed this frame
extern int g_mouseX, g_mouseY; // in canvas coordinates
extern bool g_mouseClick, g_mouseMoved;

// ---------------------------------------------------------------- config
typedef struct Config {
  char name[13];
  int color;
  int volume;        // 0..10
  int scale;         // window scale 1..6 (x256)
  bool fullscreen;
  bool smooth;
  bool showNames;
  bool tintTunic;
  int keys[12];      // SDL scancodes: B Y Select Start Up Down Left Right A X L R
  int pad[12];       // SDL controller buttons
  char lastRoom[8];
  char lastGame[64];
} Config;
extern Config g_cfg;
extern char g_baseDir[512]; // ends with a slash
void cfg_load(void);
void cfg_save(void);
void cfg_reset_controls(void);
void app_path(char *out, int size, const char *rel);
void app_log(const char *fmt, ...);
uint64_t app_ms(void);
void app_quit(void);
void app_apply_video(void);
bool app_capture_binding(int *scancode, int *button); // true once the user pressed something
extern const uint32_t g_playerColors[MAX_PLAYERS + 4];
extern const char *g_playerColorNames[MAX_PLAYERS + 4];
#define NUM_PLAYER_COLORS 12

// ---------------------------------------------------------------- emulator
typedef struct Snes Snes;
extern Snes *g_snes;
extern uint32_t g_emuFrame[256 * 224];
extern uint8_t *g_baseRom;       // the user's 1 MB Japanese v1.0 ROM (NULL when missing)
extern int g_baseRomLen;
bool emu_find_base_rom(void);
bool emu_load(const uint8_t *rom, int len, const char *srmPath, bool online);
void emu_close(void);
bool emu_loaded(void);
void emu_frame(uint16_t pad);
void emu_audio(int16_t *out, int frames);
void emu_save_sram(bool force);
uint32_t emu_rom_crc(void);
uint8_t emu_rom8(uint32_t snesAddr);
void emu_rom_poke(uint32_t snesAddr, uint8_t v);
uint8_t *emu_ram(void);
uint8_t *emu_sram(int *size);
bool emu_screenshot(const char *path);

// ---------------------------------------------------------------- net
enum { NET_OFF, NET_CONNECTING, NET_SEARCHING, NET_ONLINE, NET_FAILED };

typedef struct NetPeerInfo {
  uint32_t cid;
  bool direct;       // UDP path established (otherwise relayed)
  int rtt;           // ms
  bool connected;
} NetPeerInfo;

typedef void (*NetRecvFn)(uint32_t cid, const uint8_t *data, int len, bool reliable);
typedef void (*NetPeerFn)(uint32_t cid, bool joined);

void net_init(void);
void net_shutdown(void);
void net_set_callbacks(NetRecvFn recv, NetPeerFn peer);
void net_host(char *codeOut);              // creates a room, writes a 5 letter code
void net_join(const char *code);
void net_leave(void);
void net_poll(void);
int  net_state(void);
const char *net_status_text(void);
const char *net_room_code(void);
uint32_t net_my_cid(void);
int  net_peers(NetPeerInfo *out, int max);
bool net_peer_direct(uint32_t cid);
void net_send(uint32_t cid, const void *data, int len);       // unreliable, cid 0 = everyone
void net_send_rel(uint32_t cid, const void *data, int len);   // reliable + ordered, cid 0 = everyone
int  net_rel_backlog(uint32_t cid);
void net_stats(int *upBps, int *downBps);

// ---------------------------------------------------------------- game sync
typedef struct RoomSettings {
  uint8_t pvp;           // players can hurt each other
  uint8_t shareBombs;    // bomb pickups are given to everyone
  uint8_t shareKeys;     // small keys are shared
  uint8_t shareHearts;   // heart containers / pieces are shared
  uint8_t reserved[4];
} RoomSettings;
extern RoomSettings g_room;

typedef struct PlayerView {
  uint32_t cid;
  char name[13];
  int color;
  bool inGame;
  uint32_t location;
  int health, maxHealth;  // in 1/8 hearts
  bool direct;
  int rtt;
  bool sameGame;
} PlayerView;

void game_init(void);
void game_start(bool online);     // call after emu_load
void game_stop(void);
void game_pre_frame(void);        // builds extra sprites for the frame about to run
void game_post_frame(void);
void game_draw_overlay(void);     // labels + notifications, drawn on the canvas
void game_notify(const char *fmt, ...);
int  game_players(PlayerView *out, int max);
const char *game_location_name(uint32_t location);
void game_net_recv(uint32_t cid, const uint8_t *data, int len, bool reliable);
void game_net_peer(uint32_t cid, bool joined);
// room / rom negotiation for joiners
enum { JOIN_IDLE, JOIN_WAIT_INFO, JOIN_DOWNLOADING, JOIN_READY, JOIN_ERROR };
int  game_join_state(void);
int  game_join_progress(void);    // 0..100
const char *game_join_error(void);
void game_join_begin(void);
bool game_join_take_rom(uint8_t **rom, int *len, char *nameOut, int nameSize);
void game_set_hosted(const uint8_t *rom, int len, const char *name); // what we serve to joiners

// ---------------------------------------------------------------- randomizer
enum { RO_CHOICE, RO_BOOL, RO_INT };
typedef struct RandoOpt {
  const char *key;
  const char *label;
  int tab;
  int type;
  const char *choices;      // '|' separated values for RO_CHOICE
  const char *choiceNames;  // '|' separated display names (NULL = same as values)
  int def;
  int min, max, step;       // RO_INT
  const char *help;
} RandoOpt;
extern const RandoOpt g_randoOpts[];
extern int g_randoOptCount;
extern int g_randoVals[];
extern const char *g_randoTabs[];
extern int g_randoTabCount;
extern int g_randoPresetCount;
const char *rando_preset_name(int i);
const char *rando_preset_desc(int i);
void rando_apply_preset(int i);
void rando_defaults(void);
const char *rando_value_name(int opt, int value, char *buf, int size);
int  rando_choice_count(int opt);
bool rando_available(void);
bool rando_start(const char *seedName, uint32_t seedNumber); // spawns the generator
bool rando_running(void);
int  rando_poll(void);            // 0 running, 1 success, -1 failed
const char *rando_log_line(int fromEnd);
const char *rando_output_path(void);
void rando_cancel(void);

// ---------------------------------------------------------------- launcher
void launcher_init(void);
void launcher_frame(void);        // handles input + draws on the canvas
bool launcher_active(void);       // true while a menu is covering the game
bool launcher_wants_attract(void);
void launcher_open_pause(void);
void launcher_auto(const char *mode); // test hook
bool launcher_game_runs(void);
bool launcher_in_game(void);

// helpers
uint32_t crc32_buf(const uint8_t *data, int len);
bool file_read(const char *path, uint8_t **data, int *len);
bool file_write(const char *path, const uint8_t *data, int len);
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
