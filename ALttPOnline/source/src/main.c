// A Link to the Past Online - program entry, window, input, audio and timing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif
#include <SDL.h>
#include "app.h"
#include "emu.h"

Config g_cfg;
char g_baseDir[512];
static char s_dataDir[512];
static int s_instance = 1; // 2..8 for extra copies running on the same PC

const uint32_t g_playerColors[MAX_PLAYERS + 4] = {
  0x48C848, 0x4890F8, 0xF05050, 0xF8D038, 0xC070F0, 0xF89038, 0x40D8D0, 0xF880B8, 0xF8F8F8, 0x909090, 0x90C020, 0xA06830,
};
const char *g_playerColorNames[MAX_PLAYERS + 4] = {
  "Green", "Blue", "Red", "Yellow", "Purple", "Orange", "Teal", "Pink", "White", "Gray", "Lime", "Brown",
};

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static SDL_AudioDeviceID s_audio;
static SDL_GameController *s_pad;
static bool s_quit;
static FILE *s_logFile;
static uint32_t s_frameNo;
static int s_capKey = -1, s_capBtn = -1;
static bool s_capturing;
static uint64_t s_padRepeatAt;
static int s_padHeld;
static uint16_t s_padBlock; // buttons held while a menu was open: ignored by the game until released

// ------------------------------------------------------------------ helpers
uint64_t app_ms(void) {
  return SDL_GetPerformanceCounter() * 1000 / SDL_GetPerformanceFrequency();
}

void app_log(const char *fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (s_logFile) { fprintf(s_logFile, "[%8u] %s\n", (unsigned)s_frameNo, buf); fflush(s_logFile); }
}

void app_path(char *out, int size, const char *rel) {
  bool user = !strncmp(rel, "saves", 5) || !strcmp(rel, "alttpo.ini") || !strcmp(rel, "alttpo.log") || !strncmp(rel, "screenshots", 11);
  if (s_instance > 1 && !strncmp(rel, "saves", 5)) snprintf(out, size, "%ssaves-player%d%s", s_dataDir, s_instance, rel + 5);
  else if (s_instance > 1 && !strcmp(rel, "alttpo.log")) snprintf(out, size, "%salttpo-player%d.log", s_dataDir, s_instance);
  else snprintf(out, size, "%s%s", user ? s_dataDir : g_baseDir, rel);
}

void app_quit(void) { s_quit = true; }

static void make_dir(const char *p) {
#ifdef _WIN32
  _mkdir(p);
#else
  mkdir(p, 0755);
#endif
}

// ------------------------------------------------------------------ config
void cfg_reset_controls(void) {
  static const int keys[12] = {
    SDL_SCANCODE_Z, SDL_SCANCODE_A, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_X, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_C,
  };
  static const int pad[12] = {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
  };
  memcpy(g_cfg.keys, keys, sizeof(keys));
  memcpy(g_cfg.pad, pad, sizeof(pad));
}

void cfg_load(void) {
  memset(&g_cfg, 0, sizeof(g_cfg));
  const char *user = getenv("USERNAME");
  if (!user || !*user) user = getenv("USER");
  snprintf(g_cfg.name, sizeof(g_cfg.name), "%.12s", user && *user ? user : "Link");
  for (char *c = g_cfg.name; *c; c++) if (*c < 32 || *c > 126) *c = '_';
  g_cfg.volume = 7;
  g_cfg.scale = 3;
  g_cfg.showNames = true;
  g_cfg.tintTunic = true;
  g_cfg.playerSounds = true;
  cfg_reset_controls();
  char path[600];
  app_path(path, sizeof(path), "alttpo.ini");
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *k = line, *v = eq + 1;
    v[strcspn(v, "\r\n")] = 0;
    if (!strcmp(k, "name") && *v) snprintf(g_cfg.name, sizeof(g_cfg.name), "%.12s", v);
    else if (!strcmp(k, "color")) g_cfg.color = atoi(v) % NUM_PLAYER_COLORS;
    else if (!strcmp(k, "volume")) g_cfg.volume = atoi(v);
    else if (!strcmp(k, "scale")) g_cfg.scale = atoi(v);
    else if (!strcmp(k, "fullscreen")) g_cfg.fullscreen = atoi(v) != 0;
    else if (!strcmp(k, "smooth")) g_cfg.smooth = atoi(v) != 0;
    else if (!strcmp(k, "show_names")) g_cfg.showNames = atoi(v) != 0;
    else if (!strcmp(k, "tint_tunic")) g_cfg.tintTunic = atoi(v) != 0;
    else if (!strcmp(k, "player_sounds")) g_cfg.playerSounds = atoi(v) != 0;
    else if (!strcmp(k, "last_room")) snprintf(g_cfg.lastRoom, sizeof(g_cfg.lastRoom), "%.5s", v);
    else if (!strcmp(k, "last_game")) snprintf(g_cfg.lastGame, sizeof(g_cfg.lastGame), "%.60s", v);
    else if (!strncmp(k, "key", 3) && atoi(k + 3) < 12) g_cfg.keys[atoi(k + 3)] = atoi(v);
    else if (!strncmp(k, "pad", 3) && atoi(k + 3) < 12) g_cfg.pad[atoi(k + 3)] = atoi(v);
    else if (!strcmp(k, "pvp")) g_room.pvp = atoi(v) != 0;
    else if (!strcmp(k, "share_bombs")) g_room.shareBombs = atoi(v) != 0;
    else if (!strcmp(k, "share_keys")) g_room.shareKeys = atoi(v) != 0;
    else if (!strcmp(k, "share_hearts")) g_room.shareHearts = atoi(v) != 0;
    else if (!strcmp(k, "share_enemies")) g_room.syncEnemies = atoi(v) != 0;
  }
  fclose(f);
  if (g_cfg.volume < 0 || g_cfg.volume > 10) g_cfg.volume = 7;
  if (g_cfg.scale < 1 || g_cfg.scale > 6) g_cfg.scale = 3;
  if (g_cfg.color < 0) g_cfg.color = 0;
}

void cfg_save(void) {
  if (getenv("ALTTPO_NO_SAVE_CFG") || s_instance > 1) return;
  char path[600];
  app_path(path, sizeof(path), "alttpo.ini");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "name=%s\ncolor=%d\nvolume=%d\nscale=%d\nfullscreen=%d\nsmooth=%d\nshow_names=%d\ntint_tunic=%d\nplayer_sounds=%d\nlast_room=%s\nlast_game=%s\n",
          g_cfg.name, g_cfg.color, g_cfg.volume, g_cfg.scale, g_cfg.fullscreen, g_cfg.smooth, g_cfg.showNames, g_cfg.tintTunic, g_cfg.playerSounds, g_cfg.lastRoom, g_cfg.lastGame);
  fprintf(f, "pvp=%d\nshare_bombs=%d\nshare_keys=%d\nshare_hearts=%d\nshare_enemies=%d\n", g_room.pvp, g_room.shareBombs, g_room.shareKeys, g_room.shareHearts, g_room.syncEnemies);
  for (int i = 0; i < 12; i++) fprintf(f, "key%d=%d\n", i, g_cfg.keys[i]);
  for (int i = 0; i < 12; i++) fprintf(f, "pad%d=%d\n", i, g_cfg.pad[i]);
  fclose(f);
}

// ------------------------------------------------------------------ video
void app_apply_video(void) {
  if (!s_window) return;
  SDL_SetWindowFullscreen(s_window, g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (!g_cfg.fullscreen) {
    SDL_SetWindowSize(s_window, 256 * g_cfg.scale, 224 * g_cfg.scale);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
  SDL_ShowCursor(g_cfg.fullscreen ? SDL_DISABLE : SDL_ENABLE);
  if (s_texture) SDL_SetTextureScaleMode(s_texture, g_cfg.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

static SDL_Rect dest_rect(void) {
  int ww, wh;
  SDL_GetRendererOutputSize(s_renderer, &ww, &wh);
  SDL_Rect r;
  // keep 8:7 pixels; use whole multiples when the window is big enough and the picture is not smoothed
  double sx = (double)ww / 256, sy = (double)wh / 224;
  double s = sx < sy ? sx : sy;
  if (!g_cfg.smooth && s >= 1.0) s = (int)s;
  if (s <= 0) s = 1;
  r.w = (int)(256 * s); r.h = (int)(224 * s);
  r.x = (ww - r.w) / 2; r.y = (wh - r.h) / 2;
  return r;
}

static void present(void) {
  SDL_UpdateTexture(s_texture, NULL, g_canvas, CANVAS_W * 4);
  SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
  SDL_RenderClear(s_renderer);
  SDL_Rect r = dest_rect();
  SDL_RenderCopy(s_renderer, s_texture, NULL, &r);
  SDL_RenderPresent(s_renderer);
}

bool app_capture_binding(int *scancode, int *button) {
  if (!scancode) { s_capturing = true; s_capKey = s_capBtn = -1; return false; }
  if (s_capKey < 0 && s_capBtn < 0) return false;
  *scancode = s_capKey; *button = s_capBtn;
  s_capturing = false;
  s_capKey = s_capBtn = -1;
  return true;
}

// ------------------------------------------------------------------ input
static void open_pad(void) {
  if (s_pad) return;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) { s_pad = SDL_GameControllerOpen(i); if (s_pad) return; }
  }
}

static uint16_t read_game_pad(void) {
  uint16_t bits = 0;
  const Uint8 *ks = SDL_GetKeyboardState(NULL);
  for (int i = 0; i < 12; i++) {
    if (g_cfg.keys[i] > 0 && g_cfg.keys[i] < SDL_NUM_SCANCODES && ks[g_cfg.keys[i]]) bits |= 1 << i;
    if (s_pad && g_cfg.pad[i] >= 0 && SDL_GameControllerGetButton(s_pad, (SDL_GameControllerButton)g_cfg.pad[i])) bits |= 1 << i;
  }
  if (s_pad) {
    int ax = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (ax < -14000) bits |= 1 << 6;
    if (ax > 14000) bits |= 1 << 7;
    if (ay < -14000) bits |= 1 << 4;
    if (ay > 14000) bits |= 1 << 5;
  }
  // never left+right or up+down together
  if ((bits & 0x30) == 0x30) bits &= ~0x30;
  if ((bits & 0xC0) == 0xC0) bits &= ~0xC0;
  return bits;
}

static int pad_ui_state(void) {
  if (!s_pad) return 0;
  int st = 0;
  int ax = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
  if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) st |= UI_UP;
  if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) st |= UI_DOWN;
  if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) st |= UI_LEFT;
  if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) st |= UI_RIGHT;
  return st;
}

static void handle_event(const SDL_Event *e) {
  switch (e->type) {
    case SDL_QUIT: s_quit = true; break;
    case SDL_KEYDOWN: {
      SDL_Scancode sc = e->key.keysym.scancode;
      if (s_capturing && !e->key.repeat) { s_capKey = sc; return; }
      if ((sc == SDL_SCANCODE_RETURN && (e->key.keysym.mod & KMOD_ALT)) || sc == SDL_SCANCODE_F11) {
        if (!e->key.repeat) { g_cfg.fullscreen = !g_cfg.fullscreen; app_apply_video(); }
        return;
      }
      if (sc == SDL_SCANCODE_F12 && !e->key.repeat) {
        char dir[600], p[700];
        app_path(dir, sizeof(dir), "screenshots");
        make_dir(dir);
        snprintf(p, sizeof(p), "%s/shot-%u.png", dir, (unsigned)time(NULL));
        emu_screenshot(p);
        game_notify("Screenshot saved");
        return;
      }
      switch (sc) {
        case SDL_SCANCODE_UP: g_uiPressed |= UI_UP; break;
        case SDL_SCANCODE_DOWN: g_uiPressed |= UI_DOWN; break;
        case SDL_SCANCODE_LEFT: g_uiPressed |= UI_LEFT; break;
        case SDL_SCANCODE_RIGHT: g_uiPressed |= UI_RIGHT; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: if (!e->key.repeat) g_uiPressed |= UI_OK; break;
        case SDL_SCANCODE_ESCAPE: if (!e->key.repeat) g_uiPressed |= UI_BACK | UI_MENU; break;
        case SDL_SCANCODE_BACKSPACE: g_uiPressed |= UI_DEL; break;
        case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_Q: g_uiPressed |= UI_TAB_L; break;
        case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_E: g_uiPressed |= UI_TAB_R; break;
        default: break;
      }
      break;
    }
    case SDL_TEXTINPUT: {
      size_t l = strlen(g_uiText);
      for (const char *p = e->text.text; *p && l < sizeof(g_uiText) - 1; p++)
        if ((unsigned char)*p >= 32 && (unsigned char)*p < 127) g_uiText[l++] = *p;
      g_uiText[l] = 0;
      break;
    }
    case SDL_CONTROLLERDEVICEADDED: open_pad(); break;
    case SDL_CONTROLLERDEVICEREMOVED:
      if (s_pad && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) { SDL_GameControllerClose(s_pad); s_pad = NULL; open_pad(); }
      break;
    case SDL_CONTROLLERBUTTONDOWN:
      if (s_capturing) { s_capBtn = e->cbutton.button; return; }
      switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: g_uiPressed |= UI_OK; break;
        case SDL_CONTROLLER_BUTTON_B: g_uiPressed |= UI_BACK; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: g_uiPressed |= UI_TAB_L; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: g_uiPressed |= UI_TAB_R; break;
        case SDL_CONTROLLER_BUTTON_GUIDE: g_uiPressed |= UI_MENU; break;
        case SDL_CONTROLLER_BUTTON_START:
          // L + R + Start opens the menu while playing
          if (s_pad && SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) && SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) g_uiPressed |= UI_MENU;
          else if (launcher_active()) g_uiPressed |= UI_OK;
          break;
        default: break;
      }
      break;
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: {
      SDL_Rect r = dest_rect();
      int mx = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x;
      int my = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
      if (r.w > 0 && r.h > 0) {
        g_mouseX = (mx - r.x) * CANVAS_W / r.w;
        g_mouseY = (my - r.y) * CANVAS_H / r.h;
      }
      if (e->type == SDL_MOUSEMOTION) g_mouseMoved = true;
      else if (e->button.button == SDL_BUTTON_LEFT) { g_mouseClick = true; g_mouseMoved = true; }
      else if (e->button.button == SDL_BUTTON_RIGHT) g_uiPressed |= UI_BACK;
      break;
    }
    case SDL_DROPFILE:
      if (e->drop.file) {
        if (!g_baseRom && emu_try_rom_file(e->drop.file)) {
          // remember it by keeping a copy next to the program
          char p[600];
          snprintf(p, sizeof(p), "%salttp-jp.sfc", g_baseDir);
          file_write(p, g_baseRom, g_baseRomLen);
        }
        SDL_free(e->drop.file);
      }
      break;
  }
}

// ------------------------------------------------------------------ test script hooks
typedef struct Script { const char *p; int frame; } Script;
static Script s_scInputs, s_scUi, s_scPoke;
static uint16_t s_scriptPad;
static bool s_scriptPadOn;

static void script_init(Script *s, const char *env) {
  s->p = getenv(env);
  s->frame = (s->p && *s->p) ? atoi(s->p) : -1;
}

static const char *script_take(Script *s, char *val, int size) {
  // "frame:value,frame:value"
  const char *c = strchr(s->p, ':');
  if (!c) { s->frame = -1; return NULL; }
  c++;
  int n = 0;
  while (c[n] && c[n] != ',' && n < size - 1) { val[n] = c[n]; n++; }
  val[n] = 0;
  s->p = c[n] == ',' ? c + n + 1 : c + n;
  s->frame = *s->p ? atoi(s->p) : -1;
  return val;
}

static void run_scripts(void) {
  char v[128];
  while (s_scInputs.frame >= 0 && (int)s_frameNo >= s_scInputs.frame) {
    if (script_take(&s_scInputs, v, sizeof(v))) { s_scriptPad = (uint16_t)strtol(v, NULL, 16); s_scriptPadOn = true; }
  }
  while (s_scUi.frame >= 0 && (int)s_frameNo >= s_scUi.frame) {
    if (!script_take(&s_scUi, v, sizeof(v))) break;
    if (!strcmp(v, "up")) g_uiPressed |= UI_UP;
    else if (!strcmp(v, "down")) g_uiPressed |= UI_DOWN;
    else if (!strcmp(v, "left")) g_uiPressed |= UI_LEFT;
    else if (!strcmp(v, "right")) g_uiPressed |= UI_RIGHT;
    else if (!strcmp(v, "ok")) g_uiPressed |= UI_OK;
    else if (!strcmp(v, "back")) g_uiPressed |= UI_BACK;
    else if (!strcmp(v, "menu")) g_uiPressed |= UI_MENU;
    else if (!strcmp(v, "tabl")) g_uiPressed |= UI_TAB_L;
    else if (!strcmp(v, "tabr")) g_uiPressed |= UI_TAB_R;
    else if (!strncmp(v, "text=", 5)) snprintf(g_uiText, sizeof(g_uiText), "%s", v + 5);
  }
  while (s_scPoke.frame >= 0 && (int)s_frameNo >= s_scPoke.frame) {
    if (!script_take(&s_scPoke, v, sizeof(v))) break;
    char *eq = strchr(v, '=');
    if (eq && emu_loaded()) {
      *eq = 0;
      long addr = strtol(v, NULL, 16);
      long val = strtol(eq + 1, NULL, 16);
      if (addr >= 0 && addr < 0x20000) { emu_ram()[addr] = (uint8_t)val; app_log("poke %05lx=%02lx", addr, val); }
    }
  }
}

static void peek_log(void) {
  static const char *env; static bool init;
  if (!init) { env = getenv("ALTTPO_PEEK"); init = true; }
  if (!env || !emu_loaded() || (s_frameNo % 60) != 0) return;
  char line[512]; int n = 0;
  const char *p = env;
  while (*p && n < 480) {
    long a = strtol(p, (char **)&p, 16);
    if (a >= 0 && a < 0x20000) n += snprintf(line + n, sizeof(line) - n, "%05lx=%02x ", a, emu_ram()[a]);
    if (*p == ',') p++; else break;
  }
  app_log("peek %s", line);
}

// ------------------------------------------------------------------ main
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "A Link to the Past Online", SDL_GetError(), NULL);
    return 1;
  }
  char *base = SDL_GetBasePath();
  snprintf(g_baseDir, sizeof(g_baseDir), "%s", base ? base : "./");
  SDL_free(base);
  for (char *c = g_baseDir; *c; c++) if (*c == '\\') *c = '/';
  const char *data = getenv("ALTTPO_DATA");
  if (data && *data) {
    snprintf(s_dataDir, sizeof(s_dataDir), "%s", data);
    for (char *c = s_dataDir; *c; c++) if (*c == '\\') *c = '/';
    size_t l = strlen(s_dataDir);
    if (l && s_dataDir[l - 1] != '/') strcat(s_dataDir, "/");
    make_dir(s_dataDir);
  } else {
    snprintf(s_dataDir, sizeof(s_dataDir), "%s", g_baseDir);
  }
  char p[600];
#ifdef _WIN32
  // a lock file per running copy; the handle is closed by the system when we exit
  for (int i = 1; i <= MAX_PLAYERS; i++) {
    snprintf(p, sizeof(p), "%salttpo-%d.lock", s_dataDir, i);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) { s_instance = i; break; }
  }
#endif
  app_path(p, sizeof(p), "alttpo.log");
  {
    // keep the log of the session before this one: it is what gets asked for when something went wrong
    char prev[640];
    snprintf(prev, sizeof(prev), "%.*s-previous.log", (int)strlen(p) - 4, p);
    remove(prev);
    rename(p, prev);
  }
  s_logFile = fopen(p, "w");
  app_log("A Link to the Past Online %s", APP_VERSION);
  app_path(p, sizeof(p), "seeds");
  make_dir(p);

  cfg_load();
  if (s_instance > 1) {
    // a second copy on the same PC plays as another person
    size_t l = strlen(g_cfg.name);
    if (l > 11) l = 11;
    snprintf(g_cfg.name + l, sizeof(g_cfg.name) - l, "%d", s_instance);
    g_cfg.color = (g_cfg.color + s_instance - 1) % NUM_PLAYER_COLORS;
  }
  if (getenv("ALTTPO_NAME")) snprintf(g_cfg.name, sizeof(g_cfg.name), "%.12s", getenv("ALTTPO_NAME"));
  if (getenv("ALTTPO_COLOR")) g_cfg.color = atoi(getenv("ALTTPO_COLOR")) % NUM_PLAYER_COLORS;

  bool headless = getenv("SDL_VIDEODRIVER") && !strcmp(getenv("SDL_VIDEODRIVER"), "dummy");
  s_window = SDL_CreateWindow("A Link to the Past Online", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              256 * g_cfg.scale, 224 * g_cfg.scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!s_window) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "A Link to the Past Online", SDL_GetError(), NULL); return 1; }
  SDL_SetWindowMinimumSize(s_window, 256, 224);
  s_renderer = SDL_CreateRenderer(s_window, -1, headless ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED);
  if (!s_renderer) s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
  s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CANVAS_W, CANVAS_H);
  app_apply_video();

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
  s_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (s_audio) SDL_PauseAudioDevice(s_audio, 0);
  open_pad();
  SDL_StopTextInput();
  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

  net_init();
  game_init();
  emu_find_base_rom();
  launcher_init();

  script_init(&s_scInputs, "ALTTPO_INPUTS");
  script_init(&s_scUi, "ALTTPO_UI");
  script_init(&s_scPoke, "ALTTPO_POKE");
  int shotEvery = getenv("ALTTPO_SHOT_EVERY") ? atoi(getenv("ALTTPO_SHOT_EVERY")) : 0;
  const char *shotDir = getenv("ALTTPO_SHOT_DIR");
  int exitAfter = getenv("ALTTPO_EXIT_AFTER") ? atoi(getenv("ALTTPO_EXIT_AFTER")) : 0;
  bool fast = getenv("ALTTPO_FAST") != NULL;
  if (getenv("ALTTPO_AUTO")) launcher_auto(getenv("ALTTPO_AUTO"));

  const double frameTime = 1.0 / 60.0988;
  uint64_t freq = SDL_GetPerformanceFrequency();
  double next = (double)SDL_GetPerformanceCounter() / freq;
  static int16_t audioBuf[820 * 2];

  while (!s_quit) {
    s_frameNo++;
    g_uiTick++;
    g_uiPressed = 0;
    g_uiText[0] = 0;
    g_mouseClick = false;
    g_mouseMoved = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(&e);
    // gamepad direction repeat for menus
    int ps = pad_ui_state();
    uint64_t now = app_ms();
    if (ps != s_padHeld) { s_padHeld = ps; g_uiPressed |= ps; s_padRepeatAt = now + 350; }
    else if (ps && now >= s_padRepeatAt) { g_uiPressed |= ps; s_padRepeatAt = now + 110; }
    run_scripts();

    net_poll();

    bool menuOpen = launcher_active();
    // A button that opened, used or closed a menu must not reach the game as well (Start would
    // open the game's own menu): whatever is held while a menu is up stays ignored until released.
    uint16_t held = s_scriptPadOn ? s_scriptPad : read_game_pad();
    if (menuOpen || (g_uiPressed & UI_MENU)) s_padBlock = held;
    else s_padBlock &= held;
    if (emu_loaded() && launcher_game_runs()) {
      uint16_t pad = 0;
      if (launcher_in_game() && !menuOpen) pad = held & ~s_padBlock;
      game_pre_frame();
      emu_frame(pad);
      game_post_frame();
      // the console runs a little faster than 60 Hz: stretch or squeeze each frame's
      // audio slightly so the queue stays around 50 ms instead of drifting
      int queued = s_audio ? (int)(SDL_GetQueuedAudioSize(s_audio) / 4) : 0;
      int want = 800;
      if (queued > 3400) want = 794;
      else if (queued < 1600) want = 806;
      emu_audio(audioBuf, want);
      if (s_audio) {
        int vol = g_cfg.volume;
        if (!launcher_in_game()) vol = vol * 6 / 10;       // the title music stays in the background
        if (vol != 10) for (int i = 0; i < want * 2; i++) audioBuf[i] = (int16_t)(audioBuf[i] * vol / 10);
        if (queued < 12000) SDL_QueueAudio(s_audio, audioBuf, want * 4);
      }
      peek_log();
    }
    launcher_frame();
    present();

    if (shotDir && ((shotEvery && (s_frameNo % shotEvery) == 0))) {
      char sp[700];
      snprintf(sp, sizeof(sp), "%s/f%06u.png", shotDir, (unsigned)s_frameNo);
      emu_screenshot(sp);
    }
    if (exitAfter && (int)s_frameNo >= exitAfter) s_quit = true;

    if (!fast) {
      next += frameTime;
      double t = (double)SDL_GetPerformanceCounter() / freq;
      if (t > next + 0.1) next = t; // fell behind, do not try to catch up
      while (t < next) {
        double left = next - t;
        if (left > 0.002) SDL_Delay((Uint32)((left - 0.001) * 1000));
        t = (double)SDL_GetPerformanceCounter() / freq;
      }
    }
  }

  rando_cancel();
  net_shutdown();
  game_stop();
  emu_close();
  cfg_save();
  if (s_audio) SDL_CloseAudioDevice(s_audio);
  SDL_DestroyTexture(s_texture);
  SDL_DestroyRenderer(s_renderer);
  SDL_DestroyWindow(s_window);
  SDL_Quit();
  if (s_logFile) fclose(s_logFile);
  return 0;
}
