// The launcher and the in-game menu, drawn in the style of the game's own menus.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <SDL.h>
#include "app.h"
#include "emu.h"

enum {
  SC_NONE, SC_NOROM, SC_MAIN, SC_ONLINE, SC_GAMESEL, SC_HOSTOPTS, SC_JOIN, SC_CONNECT,
  SC_RANDO, SC_RANDO_RUN, SC_OPTIONS, SC_CONTROLS, SC_PAUSE, SC_MESSAGE, SC_HELP,
};

static int s_screen = SC_MAIN;
static int s_sel[16];
static int s_scroll[16];
static bool s_inGame, s_gameOnline;
static char s_gameName[64];
static char s_message[200];
static int s_messageBack = SC_MAIN;
static uint64_t s_bannerUntil;
static int s_purpose; // 0 solo, 1 host

// text editing
static struct { bool active; char *buf; int max; int mode; char backup[64]; } s_edit;

// seeds
typedef struct SeedEnt { char name[48]; time_t mtime; } SeedEnt;
static SeedEnt s_seeds[128];
static int s_seedCount;

// randomizer screen
static int s_randoTab; // 0 presets, 1..N option tabs, N+1 generate
static char s_seedName[24] = "";
static char s_seedNumber[12] = "";
static int s_randoResult;
static int s_randoTries;
static bool s_randoAutoNumber;
static char s_code[8] = "";
static int s_bindWait = -1;
static int s_genSel = 2;
static int s_tabSel[8], s_tabScroll[8];

typedef struct Item { const char *label; const char *value; bool disabled; const char *help; uint32_t valueColor; } Item;
enum { EV_NONE, EV_OK, EV_LEFT, EV_RIGHT };

// ------------------------------------------------------------------ widgets
static void edit_begin(char *buf, int max, int mode) {
  s_edit.active = true; s_edit.buf = buf; s_edit.max = max; s_edit.mode = mode;
  snprintf(s_edit.backup, sizeof(s_edit.backup), "%s", buf);
  SDL_StartTextInput();
}

static bool edit_char_ok(char c) {
  if (s_edit.mode == 1) return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  if (s_edit.mode == 2) return c >= '0' && c <= '9';
  if (s_edit.mode == 3) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  return c >= 32 && c < 127;
}

// returns 1 when committed, -1 when cancelled
static int edit_update(void) {
  if (!s_edit.active) return 0;
  int len = (int)strlen(s_edit.buf);
  for (const char *p = g_uiText; *p; p++) {
    char c = *p;
    if (s_edit.mode == 1 && c >= 'a' && c <= 'z') c -= 32;
    if (!edit_char_ok(c) || len >= s_edit.max) continue;
    s_edit.buf[len++] = c; s_edit.buf[len] = 0;
  }
  if ((g_uiPressed & UI_DEL) && len > 0) s_edit.buf[--len] = 0;
  // gamepad: up/down change the last character, right adds one, left removes one
  static const char *kCycle = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const char *cyc = s_edit.mode == 2 ? "0123456789" : kCycle;
  int cl = (int)strlen(cyc);
  if (g_uiPressed & (UI_UP | UI_DOWN)) {
    if (len == 0) { s_edit.buf[len++] = cyc[0]; s_edit.buf[len] = 0; }
    else {
      char c = s_edit.buf[len - 1];
      if (c >= 'a' && c <= 'z') c -= 32;
      const char *at = strchr(cyc, c);
      int i = at ? (int)(at - cyc) : 0;
      i = (i + ((g_uiPressed & UI_UP) ? 1 : cl - 1)) % cl;
      s_edit.buf[len - 1] = cyc[i];
    }
  }
  if ((g_uiPressed & UI_RIGHT) && len < s_edit.max) { s_edit.buf[len++] = cyc[0]; s_edit.buf[len] = 0; }
  if ((g_uiPressed & UI_LEFT) && len > 0) s_edit.buf[--len] = 0;
  if (g_uiPressed & UI_OK) { s_edit.active = false; SDL_StopTextInput(); g_uiPressed = 0; return 1; }
  if (g_uiPressed & UI_BACK) { strcpy(s_edit.buf, s_edit.backup); s_edit.active = false; SDL_StopTextInput(); g_uiPressed = 0; return -1; }
  g_uiPressed = 0;
  return 0;
}

static const char *edit_show(const char *buf, char *tmp, int size) {
  if (s_edit.active && s_edit.buf == buf) snprintf(tmp, size, "%s%s", buf, (g_uiTick / 20) & 1 ? "_" : " ");
  else snprintf(tmp, size, "%s", buf[0] ? buf : "-");
  return tmp;
}

// A vertical list. Returns the event for *sel.
static int menu(const Item *items, int n, int *sel, int *scroll, int x, int y, int w, int rows, int scale) {
  int rowH = scale == 2 ? 22 : 13;
  int ev = EV_NONE;
  if (n <= 0) return ev;
  if (*sel >= n) *sel = n - 1;
  if (*sel < 0) *sel = 0;
  if (!s_edit.active) {
    int dir = 0;
    if (g_uiPressed & UI_UP) dir = -1;
    if (g_uiPressed & UI_DOWN) dir = 1;
    if (dir) {
      for (int k = 0; k < n; k++) {
        *sel = (*sel + dir + n) % n;
        if (!items[*sel].disabled) break;
      }
    }
    if (items[*sel].disabled) for (int k = 0; k < n; k++) if (!items[k].disabled) { *sel = k; break; }
  }
  int sc = scroll ? *scroll : 0;
  if (*sel < sc) sc = *sel;
  if (*sel >= sc + rows) sc = *sel - rows + 1;
  if (sc > n - rows) sc = n - rows;
  if (sc < 0) sc = 0;
  if (scroll) *scroll = sc;
  for (int r = 0; r < rows && sc + r < n; r++) {
    int i = sc + r;
    int ry = y + r * rowH;
    bool hot = (i == *sel);
    if (!s_edit.active && !items[i].disabled && g_mouseX >= x && g_mouseX < x + w && g_mouseY >= ry - 2 && g_mouseY < ry + rowH - 2) {
      if (g_mouseMoved) { *sel = i; hot = true; }
      if (g_mouseClick && i == *sel) ev = EV_OK;
    }
    if (hot) {
      ui_blend(x + 22, ry - 3, w - 22, rowH - 1, 0x3050C0, 110);
      ui_cursor(x - 4, ry - 6 + (scale == 1 ? -2 : 0));
    }
    uint32_t col = items[i].disabled ? COL_DARK : (hot ? COL_WHITE : 0xD0D8E8);
    ui_text(x + 28, ry, items[i].label, col, scale);
    if (items[i].value) {
      uint32_t vc = items[i].valueColor ? items[i].valueColor : (hot ? COL_GOLD : 0xC8B060);
      if (items[i].disabled) vc = COL_DARK;
      ui_text_right(x + w - 8, ry, items[i].value, vc, scale);
    }
  }
  if (sc > 0) ui_text(x + w - 4, y - 2, "^", COL_GOLD, 1);
  if (sc + rows < n) ui_text(x + w - 4, y + rows * rowH - 12, "v", COL_GOLD, 1);
  if (!s_edit.active && !items[*sel].disabled) {
    if (g_uiPressed & UI_OK) ev = EV_OK;
    else if (g_uiPressed & UI_LEFT) ev = EV_LEFT;
    else if (g_uiPressed & UI_RIGHT) ev = EV_RIGHT;
  }
  return ev;
}

static void footer(const char *text) {
  ui_blend(0, CANVAS_H - 18, CANVAS_W, 18, 0x000000, 170);
  ui_text_center(CANVAS_W / 2, CANVAS_H - 13, text, COL_GRAY, 1);
}

static void help_box(const char *text) {
  if (!text || !*text) return;
  ui_frame(24, CANVAS_H - 74, CANVAS_W - 48, 52);
  ui_text_wrap(36, CANVAS_H - 64, CANVAS_W - 72, text, 0xD8E0F0, 1);
}

static void show_message(const char *msg, int back) {
  snprintf(s_message, sizeof(s_message), "%s", msg);
  s_messageBack = back;
  s_screen = SC_MESSAGE;
}

// ------------------------------------------------------------------ game sessions
static void attract_start(void) {
  if (s_inGame || emu_loaded() || !g_baseRom) return;
  if (getenv("ALTTPO_NO_ATTRACT")) return;
  emu_load(g_baseRom, g_baseRomLen, NULL, false);
}

static bool start_game(const uint8_t *rom, int len, const char *name, bool online) {
  char rel[120], srm[600], dir[600];
  app_path(dir, sizeof(dir), "saves");
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0755);
#endif
  snprintf(rel, sizeof(rel), "saves/%s.srm", name);
  app_path(srm, sizeof(srm), rel);
  game_stop();
  if (!emu_load(rom, len, srm, online)) return false;
  snprintf(s_gameName, sizeof(s_gameName), "%s", name);
  snprintf(g_cfg.lastGame, sizeof(g_cfg.lastGame), "%s", name);
  game_set_hosted(rom, len, name);
  game_start(online);
  s_inGame = true;
  s_gameOnline = online;
  s_screen = SC_NONE;
  return true;
}

static void leave_game(void) {
  net_leave();
  game_stop();
  emu_close();
  s_inGame = false;
  s_gameOnline = false;
  cfg_save();
  s_screen = SC_MAIN;
  attract_start();
}

static bool load_game_rom(const char *name, uint8_t **rom, int *len) {
  if (!strcmp(name, "original")) {
    if (!g_baseRom) return false;
    *rom = malloc(g_baseRomLen);
    memcpy(*rom, g_baseRom, g_baseRomLen);
    *len = g_baseRomLen;
    return true;
  }
  char rel[120], path[600];
  snprintf(rel, sizeof(rel), "seeds/%s.sfc", name);
  app_path(path, sizeof(path), rel);
  return file_read(path, rom, len);
}

static bool begin_solo(const char *name) {
  uint8_t *rom; int len;
  if (!load_game_rom(name, &rom, &len)) return false;
  bool ok = start_game(rom, len, name, false);
  free(rom);
  return ok;
}

static bool begin_host(const char *name) {
  uint8_t *rom; int len;
  if (!load_game_rom(name, &rom, &len)) return false;
  bool ok = start_game(rom, len, name, true);
  free(rom);
  if (!ok) return false;
  char code[8];
  net_host(code);
  snprintf(g_cfg.lastRoom, sizeof(g_cfg.lastRoom), "%s", code);
  s_bannerUntil = app_ms() + 15000;
  cfg_save();
  return true;
}

static void begin_join(const char *code) {
  game_stop();
  net_join(code);
  game_join_begin();
  snprintf(g_cfg.lastRoom, sizeof(g_cfg.lastRoom), "%s", code);
  s_screen = SC_CONNECT;
}

static int seed_cmp(const void *a, const void *b) {
  const SeedEnt *x = a, *y = b;
  return (y->mtime > x->mtime) - (y->mtime < x->mtime);
}

static void scan_seeds(void) {
  s_seedCount = 0;
  char dir[600];
  app_path(dir, sizeof(dir), "seeds/");
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && s_seedCount < 128) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot || strcasecmp(dot, ".sfc") != 0) continue;
    int l = (int)(dot - e->d_name);
    if (l <= 0 || l >= 48) continue;
    SeedEnt *s = &s_seeds[s_seedCount++];
    memcpy(s->name, e->d_name, l); s->name[l] = 0;
    char p[900];
    struct stat st;
    snprintf(p, sizeof(p), "%s%s", dir, e->d_name);
    s->mtime = stat(p, &st) == 0 ? st.st_mtime : 0;
  }
  closedir(d);
  qsort(s_seeds, s_seedCount, sizeof(SeedEnt), seed_cmp);
}

// ------------------------------------------------------------------ screens
static void draw_backdrop(int dim) {
  if (emu_loaded()) {
    ui_blit2x(g_emuFrame, 256, 224, dim);
  } else {
    for (int y = 0; y < CANVAS_H; y++) {
      int b = 24 + y * 40 / CANVAS_H;
      ui_fill(0, y, CANVAS_W, 1, (uint32_t)((b / 4) << 16 | (b / 3) << 8 | b));
    }
  }
}

static void draw_title(int y) {
  ui_triforce(CANVAS_W / 2, y + 26, 56);
  ui_text_center(CANVAS_W / 2 + 3, y + 62, "A LINK TO THE PAST", COL_WHITE, 3);
  ui_text_plain(CANVAS_W / 2 + 2 - ui_text_w("A LINK TO THE PAST", 3) / 2, y + 62, "A LINK TO THE PAST", COL_WHITE, 3);
  int w = ui_text_w("ONLINE", 3);
  ui_fill(CANVAS_W / 2 - w / 2 - 60, y + 100, 50, 2, COL_GOLD_D);
  ui_fill(CANVAS_W / 2 + w / 2 + 10, y + 100, 50, 2, COL_GOLD_D);
  ui_text_center(CANVAS_W / 2 + 3, y + 90, "ONLINE", COL_GOLD, 3);
  ui_text_plain(CANVAS_W / 2 + 2 - w / 2, y + 90, "ONLINE", COL_GOLD, 3);
}

static void screen_norom(void) {
  draw_backdrop(0);
  draw_title(24);
  ui_frame(40, 190, CANVAS_W - 80, 190);
  ui_text_center(CANVAS_W / 2, 206, "ROM NEEDED", COL_GOLD, 2);
  ui_text_wrap(60, 236, CANVAS_W - 120,
    "This game runs on your own copy of the Super Famicom game\n"
    "\"Zelda no Densetsu - Kamigami no Triforce\" (Japan, v1.0).\n"
    "It is the version every randomizer is built on.\n\n"
    "Drag the ROM file (.sfc) onto this window, or place it in the same folder as this program and start it again.",
    COL_WHITE, 1);
  footer("ESC  Quit");
  if (g_uiPressed & UI_BACK) app_quit();
  if (g_baseRom) { s_screen = SC_MAIN; attract_start(); }
}

static void screen_main(void) {
  draw_backdrop(205);
  draw_title(30);
  Item it[5] = {
    {"PLAY ONLINE", NULL, false, "Create a room or join another player with a room code.", 0},
    {"SINGLE PLAYER", NULL, false, "Play the original game or a randomized seed on your own.", 0},
    {"RANDOMIZER", NULL, false, "Create a new seed: shuffle items, entrances, dungeon doors, enemies and more.", 0},
    {"OPTIONS", NULL, false, "Your name and color, controls, video and sound.", 0},
    {"QUIT", NULL, false, "", 0},
  };
  ui_frame(128, 182, 256, 136);
  int ev = menu(it, 5, &s_sel[SC_MAIN], NULL, 140, 198, 232, 5, 2);
  help_box(it[s_sel[SC_MAIN]].help);
  footer("ARROWS  Move     ENTER  Select     F11  Fullscreen          v" APP_VERSION);
  if (ev == EV_OK) {
    switch (s_sel[SC_MAIN]) {
      case 0: s_screen = SC_ONLINE; break;
      case 1: s_purpose = 0; scan_seeds(); s_sel[SC_GAMESEL] = 0; s_screen = SC_GAMESEL; break;
      case 2: s_screen = SC_RANDO; break;
      case 3: s_screen = SC_OPTIONS; break;
      case 4: app_quit(); break;
    }
  }
}

static void screen_online(void) {
  draw_backdrop(170);
  ui_frame_titled(72, 60, 368, 170, "PLAY ONLINE");
  char who[64];
  snprintf(who, sizeof(who), "Playing as %s", g_cfg.name);
  ui_text_center(CANVAS_W / 2, 84, who, g_playerColors[g_cfg.color % NUM_PLAYER_COLORS], 1);
  char last[48] = "";
  if (g_cfg.lastRoom[0]) snprintf(last, sizeof(last), "%s", g_cfg.lastRoom);
  Item it[3] = {
    {"HOST A ROOM", NULL, false, "Pick a game and open a room. You get a 5 letter room code to share.", 0},
    {"JOIN A ROOM", NULL, false, "Enter the room code the host gave you. The game they are playing is fetched automatically.", 0},
    {"REJOIN LAST ROOM", last, g_cfg.lastRoom[0] == 0, "Go back to the last room you were in, if it is still open.", 0},
  };
  int ev = menu(it, 3, &s_sel[SC_ONLINE], NULL, 96, 116, 320, 3, 2);
  help_box(it[s_sel[SC_ONLINE]].help);
  footer("ENTER  Select     ESC  Back");
  if (ev == EV_OK) {
    if (s_sel[SC_ONLINE] == 0) { s_purpose = 1; scan_seeds(); s_sel[SC_GAMESEL] = 0; s_screen = SC_GAMESEL; }
    else if (s_sel[SC_ONLINE] == 1) { s_code[0] = 0; s_screen = SC_JOIN; edit_begin(s_code, 5, 1); }
    else begin_join(g_cfg.lastRoom);
  }
  if (g_uiPressed & UI_BACK) s_screen = SC_MAIN;
}

static void screen_gamesel(void) {
  draw_backdrop(190);
  ui_frame_titled(40, 36, CANVAS_W - 80, 330, s_purpose ? "HOST: CHOOSE A GAME" : "CHOOSE A GAME");
  static Item it[132];
  static char dates[132][20];
  int n = 0;
  it[n++] = (Item){"Original Game", "Japanese text", false, "Vanilla 1.0 JP ROM. Use the randomizer preset \"Original Adventure (English)\" for Machine Translated ROM.", 0};
  for (int i = 0; i < s_seedCount; i++) {
    struct tm *tm = localtime(&s_seeds[i].mtime);
    if (tm) strftime(dates[i], sizeof(dates[i]), "%b %d %H:%M", tm); else dates[i][0] = 0;
    it[n++] = (Item){s_seeds[i].name, dates[i], false, "A randomized seed from your seeds folder. Progress is saved per seed.", 0};
  }
  it[n++] = (Item){"Create a new seed...", NULL, false, "Open the randomizer.", COL_GREEN};
  int ev = menu(it, n, &s_sel[SC_GAMESEL], &s_scroll[SC_GAMESEL], 56, 60, CANVAS_W - 112, 13, 2);
  help_box(it[s_sel[SC_GAMESEL]].help);
  footer("ENTER  Select     ESC  Back");
  if (ev == EV_OK) {
    int s = s_sel[SC_GAMESEL];
    if (s == n - 1) { s_screen = SC_RANDO; return; }
    snprintf(s_gameName, sizeof(s_gameName), "%s", s == 0 ? "original" : s_seeds[s - 1].name);
    if (s_purpose == 1) { s_sel[SC_HOSTOPTS] = 4; s_screen = SC_HOSTOPTS; }
    else if (!begin_solo(s_gameName)) show_message("That game could not be loaded.", SC_GAMESEL);
  }
  if (g_uiPressed & UI_BACK) s_screen = s_purpose ? SC_ONLINE : SC_MAIN;
}

static void screen_hostopts(void) {
  draw_backdrop(190);
  ui_frame_titled(56, 60, CANVAS_W - 112, 220, "ROOM RULES");
  char g[80];
  snprintf(g, sizeof(g), "Game: %s", strcmp(s_gameName, "original") ? s_gameName : "Original Game");
  ui_text_center(CANVAS_W / 2, 84, g, COL_GRAY, 1);
  Item it[5] = {
    {"Players Can Fight", g_room.pvp ? "On" : "Off", false, "Swords, arrows, rods and the hammer hit other players. Everyone still works toward the same goal.", 0},
    {"Shared Bomb Pickups", g_room.shareBombs ? "On" : "Off", false, "When someone collects bombs, everybody's bomb count goes up. Using bombs only costs your own.", 0},
    {"Shared Small Keys", g_room.shareKeys ? "On" : "Off", false, "Small keys belong to the team: finding one gives it to everyone, using one takes it from everyone.", 0},
    {"Shared Heart Containers", g_room.shareHearts ? "On" : "Off", false, "Heart containers and pieces of heart raise everyone's maximum health. Current health is always your own.", 0},
    {"OPEN THE ROOM", NULL, false, "Items, dungeon items, progress and opened chests are always shared. Rupees, arrows, magic and health are your own.", COL_GREEN},
  };
  int ev = menu(it, 5, &s_sel[SC_HOSTOPTS], NULL, 72, 110, CANVAS_W - 144, 5, 2);
  help_box(it[s_sel[SC_HOSTOPTS]].help);
  footer("ENTER / LEFT / RIGHT  Change     ESC  Back");
  if (ev != EV_NONE) {
    switch (s_sel[SC_HOSTOPTS]) {
      case 0: g_room.pvp = !g_room.pvp; break;
      case 1: g_room.shareBombs = !g_room.shareBombs; break;
      case 2: g_room.shareKeys = !g_room.shareKeys; break;
      case 3: g_room.shareHearts = !g_room.shareHearts; break;
      case 4:
        if (ev == EV_OK && !begin_host(s_gameName)) show_message("That game could not be loaded.", SC_GAMESEL);
        break;
    }
  }
  if (g_uiPressed & UI_BACK) s_screen = SC_GAMESEL;
}

static void screen_join(void) {
  draw_backdrop(190);
  ui_frame_titled(96, 110, 320, 150, "JOIN A ROOM");
  ui_text_center(CANVAS_W / 2, 138, "Enter the room code", COL_WHITE, 1);
  for (int i = 0; i < 5; i++) {
    int x = CANVAS_W / 2 - 5 * 22 + i * 44 + 2;
    ui_fill(x, 160, 36, 44, 0x000000);
    ui_fill(x + 2, 162, 32, 40, i == (int)strlen(s_code) ? 0x283888 : 0x182058);
    if (i < (int)strlen(s_code)) { char c[2] = {s_code[i], 0}; ui_text(x + 9, 172, c, COL_GOLD, 3); }
    else if (i == (int)strlen(s_code) && ((g_uiTick / 20) & 1)) ui_fill(x + 8, 194, 20, 3, COL_GOLD);
  }
  ui_text_center(CANVAS_W / 2, 224, "ENTER  Join      ESC  Back", COL_GRAY, 1);
  footer("Type the 5 letter code the host sees on their screen");
  int r = edit_update();
  if (r == 1) {
    if (strlen(s_code) == 5) begin_join(s_code);
    else edit_begin(s_code, 5, 1);
  } else if (r == -1) {
    s_screen = SC_ONLINE;
  } else if (!s_edit.active) {
    edit_begin(s_code, 5, 1);
  }
}

static void screen_connect(void) {
  draw_backdrop(190);
  ui_frame_titled(72, 120, 368, 140, "JOINING");
  char line[120];
  const char *status = net_status_text();
  int js = game_join_state();
  if (net_state() == NET_ONLINE) {
    if (js == JOIN_WAIT_INFO) status = "Connected. Asking what the room is playing...";
    else if (js == JOIN_DOWNLOADING) status = "Receiving the seed from the host...";
    else status = "Starting...";
  }
  snprintf(line, sizeof(line), "Room %s", net_room_code());
  ui_text_center(CANVAS_W / 2, 146, line, COL_GOLD, 2);
  ui_text_wrap(92, 180, 328, status, COL_WHITE, 1);
  int dots = (g_uiTick / 15) % 4;
  for (int i = 0; i < 3; i++) ui_triforce(CANVAS_W / 2 - 24 + i * 24, 226, i < dots ? 14 : 8);
  footer("ESC  Cancel");
  if (net_state() == NET_FAILED) { char m[120]; snprintf(m, sizeof(m), "%s", net_status_text()); net_leave(); show_message(m, SC_ONLINE); return; }
  if (js == JOIN_ERROR) { char m[120]; snprintf(m, sizeof(m), "%s", game_join_error()); net_leave(); show_message(m, SC_ONLINE); return; }
  if (js == JOIN_READY) {
    uint8_t *rom; int len; char name[64];
    if (game_join_take_rom(&rom, &len, name, sizeof(name))) {
      bool ok = start_game(rom, len, name, true);
      free(rom);
      if (!ok) { net_leave(); show_message("The game could not be started.", SC_ONLINE); return; }
      s_bannerUntil = app_ms() + 6000;
      cfg_save();
    }
    return;
  }
  if (g_uiPressed & UI_BACK) { net_leave(); s_screen = SC_ONLINE; attract_start(); }
}

static void change_opt(int i, int dir) {
  const RandoOpt *o = &g_randoOpts[i];
  if (o->type == RO_BOOL) g_randoVals[i] = !g_randoVals[i];
  else if (o->type == RO_INT) {
    int v = g_randoVals[i] + dir * o->step;
    if (v > o->max) v = o->min;
    if (v < o->min) v = o->max;
    g_randoVals[i] = v;
  } else {
    int n = rando_choice_count(i);
    g_randoVals[i] = (g_randoVals[i] + dir + n) % n;
  }
}

static uint32_t random_seed_number(void) {
  return (uint32_t)((SDL_GetPerformanceCounter() ^ (uint64_t)time(NULL) * 7919) % 999999999u) + 1;
}

static void start_generate(void) {
  s_randoTries = 1;
  s_randoAutoNumber = !s_seedNumber[0];
  if (!s_seedName[0]) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(s_seedName, sizeof(s_seedName), "seed-%m%d-%H%M", tm);
  }
  uint32_t num = s_seedNumber[0] ? (uint32_t)strtoul(s_seedNumber, NULL, 10) : random_seed_number();
  if (!s_seedNumber[0]) snprintf(s_seedNumber, sizeof(s_seedNumber), "%u", num);
  if (!rando_start(s_seedName, num)) { show_message("The randomizer could not be started. Is the \"randomizer\" folder next to the program?", SC_RANDO); return; }
  s_randoResult = 0;
  s_sel[SC_RANDO_RUN] = 0;
  s_screen = SC_RANDO_RUN;
}

static void screen_rando(void) {
  draw_backdrop(205);
  int ntabs = g_randoTabCount + 2;
  // tab bar
  ui_frame(8, 8, CANVAS_W - 16, 30);
  static const char *kFirst = "PRESETS", *kLast = "CREATE";
  int tx = 18;
  for (int t = 0; t < ntabs; t++) {
    const char *nm = t == 0 ? kFirst : (t == ntabs - 1 ? kLast : g_randoTabs[t - 1]);
    int w = ui_text_w(nm, 1) + 10;
    bool cur = t == s_randoTab;
    if (cur) { ui_fill(tx - 2, 14, w, 18, COL_GOLD_D); ui_fill(tx - 1, 15, w - 2, 16, 0x283888); }
    if (g_mouseClick && g_mouseX >= tx - 2 && g_mouseX < tx + w && g_mouseY >= 12 && g_mouseY < 34) { s_randoTab = t; g_mouseClick = false; }
    ui_text(tx + 3, 19, nm, cur ? COL_GOLD : (t == ntabs - 1 ? COL_GREEN : COL_GRAY), 1);
    tx += w + 6;
  }
  if (!s_edit.active) {
    if (g_uiPressed & UI_TAB_L) s_randoTab = (s_randoTab + ntabs - 1) % ntabs;
    if (g_uiPressed & UI_TAB_R) s_randoTab = (s_randoTab + 1) % ntabs;
  }
  ui_frame(8, 44, CANVAS_W - 16, 322);
  const char *help = "";
  static Item it[64];
  static char vals[64][48];
  if (s_randoTab == 0) {
    int n = g_randoPresetCount;
    for (int i = 0; i < n; i++) it[i] = (Item){rando_preset_name(i), NULL, false, rando_preset_desc(i), 0};
    ui_text(24, 56, "Pick a starting point, then fine tune it on the other pages.", COL_GRAY, 1);
    int ev = menu(it, n, &s_sel[SC_RANDO], &s_scroll[SC_RANDO], 20, 76, CANVAS_W - 40, 13, 2);
    help = it[s_sel[SC_RANDO]].help;
    if (ev == EV_OK) { rando_apply_preset(s_sel[SC_RANDO]); s_randoTab = ntabs - 1; s_genSel = 2; }
  } else if (s_randoTab == ntabs - 1) {
    char t1[40], t2[40];
    edit_show(s_seedName, t1, sizeof(t1));
    edit_show(s_seedNumber, t2, sizeof(t2));
    if (!s_seedName[0] && !(s_edit.active && s_edit.buf == s_seedName)) snprintf(t1, sizeof(t1), "(automatic)");
    if (!s_seedNumber[0] && !(s_edit.active && s_edit.buf == s_seedNumber)) snprintf(t2, sizeof(t2), "(random)");
    Item gi[4] = {
      {"Seed Name", t1, false, "The name of the seed in your game list. Letters, digits, - and _.", 0},
      {"Seed Number", t2, false, "The same number with the same settings always creates the same seed. Leave empty for a random one.", 0},
      {"CREATE SEED", NULL, !rando_available() || !g_baseRom, "Runs the randomizer. Simple seeds take a few seconds, crossed door seeds can take a few minutes.", COL_GREEN},
      {"Reset All Settings", NULL, false, "Put every option back to its default.", 0},
    };
    // summary of what differs from the defaults
    int y = 176, shown = 0;
    ui_text(24, 160, "Changed from default:", COL_GRAY, 1);
    int total = 0;
    for (int i = 0; i < g_randoOptCount; i++) if (g_randoVals[i] != g_randoOpts[i].def) total++;
    for (int i = 0; i < g_randoOptCount && shown < 13; i++) {
      if (g_randoVals[i] == g_randoOpts[i].def) continue;
      char b[48], l[96];
      if (shown == 12 && total > 13) snprintf(l, sizeof(l), "... and %d more", total - 12);
      else snprintf(l, sizeof(l), "%s: %s", g_randoOpts[i].label, rando_value_name(i, g_randoVals[i], b, sizeof(b)));
      ui_text(24, y + shown * 12, l, 0xC8D0E0, 1);
      shown++;
    }
    if (!shown) ui_text(24, y, "(nothing - a plain open item randomizer)", 0xC8D0E0, 1);
    if (!rando_available()) ui_text(24, 316, "The randomizer folder is missing.", COL_RED, 1);
    int *sel = &s_genSel;
    int ev = menu(gi, 4, sel, NULL, 20, 56, CANVAS_W - 40, 4, 2);
    help = gi[*sel].help;
    int r = edit_update();
    (void)r;
    if (ev == EV_OK) {
      if (*sel == 0) edit_begin(s_seedName, 20, 3);
      else if (*sel == 1) edit_begin(s_seedNumber, 9, 2);
      else if (*sel == 2) { start_generate(); return; }
      else { rando_defaults(); }
    }
  } else {
    int tab = s_randoTab - 1;
    int map[64], n = 0;
    for (int i = 0; i < g_randoOptCount && n < 64; i++) {
      if (g_randoOpts[i].tab != tab) continue;
      map[n] = i;
      rando_value_name(i, g_randoVals[i], vals[n], sizeof(vals[n]));
      it[n] = (Item){g_randoOpts[i].label, vals[n], false, g_randoOpts[i].help, g_randoVals[i] != g_randoOpts[i].def ? COL_GREEN : 0};
      n++;
    }
    int *sel = &s_tabSel[tab];
    int ev = menu(it, n, sel, &s_tabScroll[tab], 20, 58, CANVAS_W - 40, 23, 1);
    help = it[*sel].help;
    if (ev == EV_OK || ev == EV_RIGHT) change_opt(map[*sel], 1);
    else if (ev == EV_LEFT) change_opt(map[*sel], -1);
  }
  help_box(help);
  footer("Q / E  Page     LEFT / RIGHT  Change     ESC  Back");
  if (!s_edit.active && (g_uiPressed & UI_BACK)) s_screen = SC_MAIN;
}

static void screen_rando_run(void) {
  draw_backdrop(205);
  ui_frame_titled(24, 40, CANVAS_W - 48, 250, "CREATING SEED");
  if (rando_running()) s_randoResult = rando_poll();
  if (s_randoResult < 0 && s_randoAutoNumber && s_randoTries < 6) {
    // some seed numbers do not work out with demanding settings: roll another one
    uint32_t num = random_seed_number();
    snprintf(s_seedNumber, sizeof(s_seedNumber), "%u", num);
    s_randoTries++;
    if (rando_start(s_seedName, num)) s_randoResult = 0;
  }
  for (int i = 0; i < 12; i++) {
    const char *l = rando_log_line(11 - i);
    char cut[80];
    snprintf(cut, sizeof(cut), "%.74s", l);
    ui_text(40, 62 + i * 12, cut, i == 11 ? COL_WHITE : 0xA8B0C8, 1);
  }
  if (s_randoResult == 0) {
    int dots = (g_uiTick / 15) % 4;
    for (int i = 0; i < 3; i++) ui_triforce(CANVAS_W / 2 - 24 + i * 24, 250, i < dots ? 16 : 9);
    if (s_randoTries > 1) {
      char t[64];
      snprintf(t, sizeof(t), "That seed number did not work out. Attempt %d of 6...", s_randoTries);
      ui_text_center(CANVAS_W / 2, 214, t, COL_GOLD, 1);
    }
    footer("Please wait...     ESC  Cancel");
    if (g_uiPressed & UI_BACK) { rando_cancel(); s_screen = SC_RANDO; }
    return;
  }
  if (s_randoResult > 0) {
    char m[96];
    snprintf(m, sizeof(m), "Seed \"%s\" is ready!", s_seedName);
    ui_text_center(CANVAS_W / 2, 222, m, COL_GREEN, 2);
    Item it[3] = {
      {"HOST ONLINE WITH THIS SEED", NULL, false, "", 0},
      {"PLAY IT ALONE", NULL, false, "", 0},
      {"BACK TO THE RANDOMIZER", NULL, false, "", 0},
    };
    ui_frame(96, 300, 320, 92);
    int ev = menu(it, 3, &s_sel[SC_RANDO_RUN], NULL, 104, 316, 304, 3, 2);
    footer("The seed is saved in the \"seeds\" folder, with its spoiler log");
    if (ev == EV_OK) {
      char name[48];
      snprintf(name, sizeof(name), "%s", s_seedName);
      int s = s_sel[SC_RANDO_RUN];
      s_seedName[0] = 0; s_seedNumber[0] = 0;
      if (s == 0) { snprintf(s_gameName, sizeof(s_gameName), "%s", name); s_purpose = 1; scan_seeds(); s_sel[SC_HOSTOPTS] = 4; s_screen = SC_HOSTOPTS; }
      else if (s == 1) { if (!begin_solo(name)) show_message("The seed could not be loaded.", SC_RANDO); }
      else s_screen = SC_RANDO;
    }
    if (g_uiPressed & UI_BACK) { s_seedName[0] = 0; s_seedNumber[0] = 0; s_screen = SC_RANDO; }
  } else {
    ui_text_center(CANVAS_W / 2, 240, "The seed could not be created.", COL_RED, 2);
    ui_text_center(CANVAS_W / 2, 264, "Some combinations of settings do not work together. Try again or change them.", COL_GRAY, 1);
    footer("ENTER / ESC  Back");
    if (g_uiPressed & (UI_BACK | UI_OK)) { s_seedNumber[0] = 0; s_screen = SC_RANDO; }
  }
}

static void screen_options(void) {
  draw_backdrop(s_inGame ? 150 : 190);
  ui_frame_titled(72, 40, 368, 300, "OPTIONS");
  char name[24], vol[8], scale[8];
  edit_show(g_cfg.name, name, sizeof(name));
  snprintf(vol, sizeof(vol), "%d", g_cfg.volume);
  snprintf(scale, sizeof(scale), "%dx", g_cfg.scale);
  Item it[10] = {
    {"Name", name, false, "The name other players see above your head.", 0},
    {"Tunic Color", g_playerColorNames[g_cfg.color % NUM_PLAYER_COLORS], false, "The color of your name, and of your tunic on other players' screens.", g_playerColors[g_cfg.color % NUM_PLAYER_COLORS]},
    {"Show Names", g_cfg.showNames ? "On" : "Off", false, "Draw player names above the other players.", 0},
    {"Color Other Players", g_cfg.tintTunic ? "On" : "Off", false, "Recolor the other players' tunics with their chosen color so you can tell them apart.", 0},
    {"Volume", vol, false, "", 0},
    {"Window Size", scale, false, "", 0},
    {"Fullscreen", g_cfg.fullscreen ? "On" : "Off", false, "F11 or Alt+Enter also switches.", 0},
    {"Smooth Picture", g_cfg.smooth ? "On" : "Off", false, "Blend pixels when the picture is scaled. Off gives sharp pixels.", 0},
    {"CONTROLS...", NULL, false, "Change keyboard and gamepad buttons.", 0},
    {"BACK", NULL, false, "", 0},
  };
  int *sel = &s_sel[SC_OPTIONS];
  int ev = menu(it, 10, sel, NULL, 88, 62, 336, 10, 2);
  help_box(it[*sel].help);
  footer("LEFT / RIGHT  Change     ESC  Back");
  int r = edit_update();
  if (r == 1 && !g_cfg.name[0]) strcpy(g_cfg.name, "Link");
  int dir = ev == EV_LEFT ? -1 : 1;
  if (ev != EV_NONE) {
    switch (*sel) {
      case 0: if (ev == EV_OK) edit_begin(g_cfg.name, 12, 0); break;
      case 1: g_cfg.color = (g_cfg.color + dir + NUM_PLAYER_COLORS) % NUM_PLAYER_COLORS; break;
      case 2: g_cfg.showNames = !g_cfg.showNames; break;
      case 3: g_cfg.tintTunic = !g_cfg.tintTunic; break;
      case 4: g_cfg.volume += dir; if (g_cfg.volume > 10) g_cfg.volume = ev == EV_OK ? 0 : 10; if (g_cfg.volume < 0) g_cfg.volume = 0; break;
      case 5: g_cfg.scale += dir; if (g_cfg.scale > 6) g_cfg.scale = ev == EV_OK ? 1 : 6; if (g_cfg.scale < 1) g_cfg.scale = 1; app_apply_video(); break;
      case 6: g_cfg.fullscreen = !g_cfg.fullscreen; app_apply_video(); break;
      case 7: g_cfg.smooth = !g_cfg.smooth; app_apply_video(); break;
      case 8: if (ev == EV_OK) { s_bindWait = -1; s_screen = SC_CONTROLS; } break;
      case 9: if (ev == EV_OK) { cfg_save(); s_screen = s_inGame ? SC_PAUSE : SC_MAIN; } break;
    }
  }
  if (!s_edit.active && (g_uiPressed & UI_BACK)) { cfg_save(); s_screen = s_inGame ? SC_PAUSE : SC_MAIN; }
}

static const char *kButtonNames[12] = {"B  (Sword)", "Y  (Item)", "Select", "Start", "Up", "Down", "Left", "Right", "A  (Action)", "X  (Map)", "L", "R"};
static const int kButtonOrder[12] = {4, 5, 6, 7, 8, 0, 9, 1, 10, 11, 3, 2};

static void screen_controls(void) {
  draw_backdrop(s_inGame ? 150 : 190);
  ui_frame_titled(56, 30, 400, 350, "CONTROLS");
  ui_text(232, 50, "KEYBOARD", COL_GRAY, 1);
  ui_text(352, 50, "GAMEPAD", COL_GRAY, 1);
  static Item it[14];
  static char vals[14][48];
  for (int i = 0; i < 12; i++) {
    int b = kButtonOrder[i];
    const char *k = SDL_GetScancodeName((SDL_Scancode)g_cfg.keys[b]);
    const char *p = g_cfg.pad[b] >= 0 ? SDL_GameControllerGetStringForButton((SDL_GameControllerButton)g_cfg.pad[b]) : "-";
    if (s_bindWait == i) snprintf(vals[i], sizeof(vals[i]), "press a key or button...");
    else snprintf(vals[i], sizeof(vals[i]), "%-12.12s  %-10.10s", k && *k ? k : "-", p ? p : "-");
    it[i] = (Item){kButtonNames[b], vals[i], false, "", s_bindWait == i ? COL_GREEN : 0};
  }
  it[12] = (Item){"Reset to Defaults", NULL, false, "", 0};
  it[13] = (Item){"BACK", NULL, false, "", 0};
  int *sel = &s_sel[SC_CONTROLS];
  if (s_bindWait >= 0) {
    // waiting for the new binding: the list does not react
    int saved = g_uiPressed; g_uiPressed = 0;
    bool mc = g_mouseClick; g_mouseClick = false;
    menu(it, 14, sel, NULL, 64, 66, 384, 14, 2);
    g_uiPressed = saved; g_mouseClick = mc;
    int sc = -1, bt = -1;
    if (app_capture_binding(&sc, &bt)) {
      int b = kButtonOrder[s_bindWait];
      if (sc == SDL_SCANCODE_ESCAPE) { /* cancelled */ }
      else if (sc >= 0) g_cfg.keys[b] = sc;
      else if (bt >= 0) g_cfg.pad[b] = bt;
      s_bindWait = -1;
      g_uiPressed = 0;
    }
    footer("Press the new key or gamepad button     ESC  Cancel");
    return;
  }
  int ev = menu(it, 14, sel, NULL, 64, 66, 384, 14, 2);
  footer("ENTER  Rebind     ESC  Back        In game: ESC opens the menu, TAB shows the players");
  if (ev == EV_OK) {
    if (*sel < 12) { s_bindWait = *sel; app_capture_binding(NULL, NULL); }
    else if (*sel == 12) cfg_reset_controls();
    else { cfg_save(); s_screen = SC_OPTIONS; }
  }
  if (g_uiPressed & UI_BACK) { cfg_save(); s_screen = SC_OPTIONS; }
}

static void draw_players(int x, int y, int w) {
  PlayerView pv[MAX_PLAYERS];
  int n = game_players(pv, MAX_PLAYERS);
  int h = 34 + n * 30;
  ui_frame(x, y, w, h);
  char hdr[48];
  if (net_state() != NET_OFF) snprintf(hdr, sizeof(hdr), "ROOM %s", net_room_code());
  else snprintf(hdr, sizeof(hdr), "PLAYERS");
  ui_text(x + 12, y + 10, hdr, COL_GOLD, 2);
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d/%d", n, MAX_PLAYERS);
  ui_text_right(x + w - 12, y + 14, cnt, COL_GRAY, 1);
  for (int i = 0; i < n; i++) {
    int ry = y + 32 + i * 30;
    uint32_t col = g_playerColors[pv[i].color % NUM_PLAYER_COLORS];
    ui_fill(x + 12, ry + 2, 6, 14, col);
    ui_text(x + 24, ry, pv[i].name, COL_WHITE, 2);
    char info[64];
    if (!pv[i].sameGame) snprintf(info, sizeof(info), "different game!");
    else if (!pv[i].inGame) snprintf(info, sizeof(info), "in the menus");
    else snprintf(info, sizeof(info), "%.34s", game_location_name(pv[i].location));
    ui_text(x + 24, ry + 17, info, pv[i].sameGame ? COL_GRAY : COL_RED, 1);
    if (pv[i].inGame && pv[i].maxHealth) {
      int hearts = pv[i].maxHealth / 8, full = pv[i].health / 8;
      int hx = x + w - 12 - (hearts > 10 ? 10 : hearts) * 8;
      for (int k = 0; k < hearts && k < 20; k++)
        ui_heart(hx + (k % 10) * 8, ry + (k / 10) * 8, k < full ? COL_RED : 0x503038);
    }
    if (i > 0) {
      char net[32];
      if (pv[i].direct) snprintf(net, sizeof(net), "%d ms", pv[i].rtt);
      else snprintf(net, sizeof(net), "relayed");
      ui_text_right(x + w - 12, ry + 18, net, pv[i].direct ? COL_GREEN : COL_GOLD, 1);
    }
  }
}

static void screen_pause(void) {
  draw_backdrop(140);
  ui_frame_titled(24, 60, 216, 170, "MENU");
  Item it[4] = {
    {"RESUME", NULL, false, "", 0},
    {"OPTIONS", NULL, false, "", 0},
    {"HOW TO PLAY", NULL, false, "", 0},
    {s_gameOnline ? "LEAVE ROOM" : "QUIT TO TITLE", NULL, false, "", 0},
  };
  int ev = menu(it, 4, &s_sel[SC_PAUSE], NULL, 32, 88, 200, 4, 2);
  draw_players(250, 60, 240);
  if (s_gameOnline) {
    ui_frame(24, 244, 216, 86);
    ui_text(38, 256, "Players join with the code", COL_GRAY, 1);
    ui_text(38, 272, net_room_code(), COL_GOLD, 3);
    int up, down;
    net_stats(&up, &down);
    char st[64];
    snprintf(st, sizeof(st), "up %d.%d  down %d.%d KB/s", up / 1024, (up % 1024) * 10 / 1024, down / 1024, (down % 1024) * 10 / 1024);
    ui_text(38, 310, st, COL_DARK, 1);
    footer("The game keeps running while this menu is open");
  } else {
    footer("Use Save and Quit in the game to keep your progress");
  }
  if (ev == EV_OK) {
    switch (s_sel[SC_PAUSE]) {
      case 0: s_screen = SC_NONE; break;
      case 1: s_screen = SC_OPTIONS; break;
      case 2: s_screen = SC_HELP; break;
      case 3: leave_game(); break;
    }
  }
  if (g_uiPressed & (UI_BACK | UI_MENU)) s_screen = SC_NONE;
}

static void screen_help(void) {
  draw_backdrop(190);
  ui_frame_titled(24, 30, CANVAS_W - 48, 370, "HOW TO PLAY");
  ui_text_wrap(40, 54, CANVAS_W - 80,
    "SHARED WITH THE TEAM\n"
    "  Every item, sword, shield, mail and bottle. Maps, compasses, big keys and small keys. "
    "Heart containers, pendants, crystals, opened chests, beaten bosses and story progress.\n\n"
    "YOUR OWN\n"
    "  Health, magic, rupees, arrows and bombs. When anyone picks up bombs everybody gets them, "
    "but placing a bomb only uses yours.\n\n"
    "PLAYING TOGETHER\n"
    "  Everyone picks a save slot in the game's own file menu; a new file catches up with the team as soon as "
    "it starts. Players in the same area see each other. Cut bushes, opened doors and lifted pots "
    "show up for everyone in the room. If the room allows it, your sword, arrows, rods and hammer hurt other players.\n\n"
    "SAVING\n"
    "  Use the game's Save and Quit (open the item menu, then press Select... or just die). Each player keeps a save "
    "file per seed on their own PC, and anyone can host the next session.\n\n"
    "KEYS\n"
    "  ESC menu    TAB player list    F11 fullscreen    F12 screenshot",
    0xD8E0F0, 1);
  footer("ENTER / ESC  Back");
  if (g_uiPressed & (UI_BACK | UI_OK)) s_screen = s_inGame ? SC_PAUSE : SC_MAIN;
}

static void screen_message(void) {
  draw_backdrop(190);
  ui_frame(72, 140, 368, 130);
  ui_text_wrap(92, 164, 328, s_message, COL_WHITE, 1);
  ui_text_center(CANVAS_W / 2, 240, "OK", COL_GOLD, 2);
  ui_cursor(CANVAS_W / 2 - 44, 232);
  if (g_uiPressed & (UI_OK | UI_BACK) || g_mouseClick) { s_screen = s_messageBack; attract_start(); }
}

static void hud_in_game(void) {
  uint64_t now = app_ms();
  if (s_gameOnline && now < s_bannerUntil && net_room_code()[0]) {
    char b[96];
    if (net_state() == NET_ONLINE) snprintf(b, sizeof(b), "ROOM CODE  %s", net_room_code());
    else if (net_state() == NET_FAILED) snprintf(b, sizeof(b), "OFFLINE: could not reach the matchmaking service");
    else snprintf(b, sizeof(b), "Opening room...");
    int w = ui_text_w(b, 2) + 24;
    ui_frame(CANVAS_W / 2 - w / 2, 6, w, 30);
    ui_text_center(CANVAS_W / 2, 14, b, COL_GOLD, 2);
    ui_text_center(CANVAS_W / 2, 40, "Share the code with other players.  ESC opens the menu.", COL_WHITE, 1);
  }
  const Uint8 *ks = SDL_GetKeyboardState(NULL);
  if (ks[SDL_SCANCODE_TAB]) draw_players(CANVAS_W - 250, 10, 240);
}

// ------------------------------------------------------------------ entry points
void launcher_init(void) {
  rando_defaults();
  if (!g_baseRom) s_screen = SC_NOROM;
  else { s_screen = SC_MAIN; attract_start(); }
}

bool launcher_active(void) { return s_screen != SC_NONE; }
bool launcher_wants_attract(void) { return !s_inGame; }

void launcher_open_pause(void) {
  if (s_screen == SC_NONE && s_inGame) { s_sel[SC_PAUSE] = 0; s_screen = SC_PAUSE; }
}

// Whether the emulated game should advance this frame.
bool launcher_game_runs(void) {
  if (!s_inGame) return true;              // title screen backdrop
  if (s_screen == SC_NONE) return true;
  return s_gameOnline;                     // online games can not pause
}

bool launcher_in_game(void) { return s_inGame; }

void launcher_frame(void) {
  switch (s_screen) {
    case SC_NONE:
      ui_blit2x(g_emuFrame, 256, 224, 0);
      game_draw_overlay();
      hud_in_game();
      if (g_uiPressed & UI_MENU) launcher_open_pause();
      // losing the room while playing
      if (s_gameOnline && net_state() == NET_FAILED && s_bannerUntil < app_ms()) s_bannerUntil = app_ms() + 8000;
      break;
    case SC_NOROM: screen_norom(); break;
    case SC_MAIN: screen_main(); break;
    case SC_ONLINE: screen_online(); break;
    case SC_GAMESEL: screen_gamesel(); break;
    case SC_HOSTOPTS: screen_hostopts(); break;
    case SC_JOIN: screen_join(); break;
    case SC_CONNECT: screen_connect(); break;
    case SC_RANDO: screen_rando(); break;
    case SC_RANDO_RUN: screen_rando_run(); break;
    case SC_OPTIONS: screen_options(); break;
    case SC_CONTROLS: screen_controls(); break;
    case SC_PAUSE: screen_pause(); game_draw_overlay(); break;
    case SC_HELP: screen_help(); break;
    case SC_MESSAGE: screen_message(); break;
  }
}

// test hook: ALTTPO_AUTO=solo | host | join:CODE | rando:<preset index> ; ALTTPO_GAME picks the seed
void launcher_auto(const char *mode) {
  const char *game = getenv("ALTTPO_GAME");
  if (!game) game = "original";
  if (!strcmp(mode, "solo")) { if (!begin_solo(game)) app_log("auto: could not load %s", game); }
  else if (!strcmp(mode, "host")) { if (!begin_host(game)) app_log("auto: could not load %s", game); }
  else if (!strncmp(mode, "join:", 5)) begin_join(mode + 5);
  else if (!strncmp(mode, "rando:", 6)) {
    rando_apply_preset(atoi(mode + 6));
    snprintf(s_seedName, sizeof(s_seedName), "%s", getenv("ALTTPO_SEEDNAME") ? getenv("ALTTPO_SEEDNAME") : "autoseed");
    s_randoTab = g_randoTabCount + 1;
    start_generate();
  } else if (!strncmp(mode, "screen:", 7)) {
    s_screen = atoi(mode + 7);
    if (s_screen == SC_GAMESEL) scan_seeds();
    const char *tab = getenv("ALTTPO_TAB");
    if (tab) s_randoTab = atoi(tab);
  }
}
