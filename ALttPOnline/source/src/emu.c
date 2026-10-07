// Wrapper around the SNES core: ROM loading, save RAM, the main-loop hook and
// the patch buffer used to make the game run extra routines.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include "app.h"
#include "emu.h"
#include "png.h"
#include "snes/snes.h"

#define JP10_CRC 0x3322EFFCu

Snes *g_snes;
uint32_t g_emuFrame[256 * 224];
uint8_t *g_baseRom;
int g_baseRomLen;
void (*g_emuMainHook)(void);
uint32_t g_emuMainRouting;

static bool s_loaded;
static bool s_online;
static uint32_t s_romCrc;
static char s_srmPath[600];
static uint8_t *s_srmSaved;
static int s_srmSize;
static uint64_t s_lastSrmCheck;

uint32_t crc32_buf(const uint8_t *data, int len) {
  return (uint32_t)crc32(0, data, len);
}

bool file_read(const char *path, uint8_t **data, int *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0 || n > 64 * 1024 * 1024) { fclose(f); return false; }
  uint8_t *d = malloc(n + 1);
  if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return false; }
  fclose(f);
  d[n] = 0;
  *data = d;
  *len = (int)n;
  return true;
}

bool file_write(const char *path, const uint8_t *data, int len) {
  char tmp[640];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == (size_t)len;
  fclose(f);
  if (!ok) { remove(tmp); return false; }
  remove(path);
  return rename(tmp, path) == 0;
}

static bool try_base_rom(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
  if (st.st_size != 0x100000 && st.st_size != 0x100200) return false;
  uint8_t *d; int n;
  if (!file_read(path, &d, &n)) return false;
  uint8_t *rom = d;
  if (n == 0x100200) { rom += 0x200; n -= 0x200; }
  if (crc32_buf(rom, n) != JP10_CRC) { free(d); return false; }
  g_baseRom = malloc(n);
  memcpy(g_baseRom, rom, n);
  g_baseRomLen = n;
  free(d);
  app_log("base ROM: %s", path);
  return true;
}

static bool scan_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return false;
  struct dirent *e;
  bool found = false;
  while (!found && (e = readdir(d)) != NULL) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot) continue;
    if (strcasecmp(dot, ".sfc") != 0 && strcasecmp(dot, ".smc") != 0) continue;
    char p[700];
    snprintf(p, sizeof(p), "%s%s", dir, e->d_name);
    found = try_base_rom(p);
  }
  closedir(d);
  return found;
}

// Looks for the Japanese v1.0 ROM next to the program (and one and two folders up).
bool emu_find_base_rom(void) {
  if (g_baseRom) return true;
  const char *env = getenv("ALTTPO_ROM");
  if (env && try_base_rom(env)) return true;
  char p[600];
  snprintf(p, sizeof(p), "%s", g_baseDir);
  if (scan_dir(p)) return true;
  snprintf(p, sizeof(p), "%s../", g_baseDir);
  if (scan_dir(p)) return true;
  snprintf(p, sizeof(p), "%s../../", g_baseDir);
  return scan_dir(p);
}

bool emu_try_rom_file(const char *path) {
  if (g_baseRom) return true;
  return try_base_rom(path);
}

static void on_pc_hook(void *mem) {
  (void)mem;
  if (g_emuMainHook) g_emuMainHook();
}

uint8_t emu_rom8(uint32_t a) {
  Cart *c = g_snes->cart;
  return c->rom[(((a & 0x7f0000) >> 1) | (a & 0x7fff)) & (c->romSize - 1)];
}

void emu_rom_poke(uint32_t a, uint8_t v) {
  Cart *c = g_snes->cart;
  c->rom[(((a & 0x7f0000) >> 1) | (a & 0x7fff)) & (c->romSize - 1)] = v;
}

// Sets the code that runs right before the game's main routine this frame.
// "code" must leave the CPU flags/registers as the game expects (JSLs only).
void emu_set_patch(const uint8_t *code, int len) {
  uint8_t *p = g_snes->patchCode;
  int n = 0;
  if (len > 240) len = 0;
  if (len) { memcpy(p, code, len); n = len; }
  p[n++] = 0x22; // JSL Module_MainRouting
  p[n++] = g_emuMainRouting & 0xff;
  p[n++] = (g_emuMainRouting >> 8) & 0xff;
  p[n++] = (g_emuMainRouting >> 16) & 0xff;
  p[n++] = 0x6b; // RTL
}

bool emu_load(const uint8_t *rom, int len, const char *srmPath, bool online) {
  emu_close();
  g_snes = snes_init();
  if (!snes_loadRom(g_snes, rom, len)) {
    snes_free(g_snes);
    g_snes = NULL;
    return false;
  }
  snes_setPixelFormat(g_snes, pixelFormatXRGB);
  s_romCrc = crc32_buf(rom, len);
  s_online = online;
  s_loaded = true;
  memset(g_emuFrame, 0, sizeof(g_emuFrame));
  g_snes->ppu->extraCount = 0;

  // battery RAM
  s_srmPath[0] = 0;
  free(s_srmSaved);
  s_srmSaved = NULL;
  s_srmSize = g_snes->cart->ramSize;
  if (srmPath && s_srmSize > 0) {
    snprintf(s_srmPath, sizeof(s_srmPath), "%s", srmPath);
    s_srmSaved = calloc(1, s_srmSize);
    uint8_t *d; int n;
    if (file_read(srmPath, &d, &n)) {
      if (n >= s_srmSize) {
        memcpy(g_snes->cart->ram, d, s_srmSize);
        memcpy(s_srmSaved, d, s_srmSize);
      }
      free(d);
    }
  }

  // main loop hook: 00:8053 JSR ClearOamBuffer / 00:8056 JSL Module_MainRouting
  g_emuMainRouting = 0;
  if (emu_rom8(0x008053) == 0x20 && emu_rom8(0x008056) == 0x22) {
    g_emuMainRouting = emu_rom8(0x008057) | (emu_rom8(0x008058) << 8) | (emu_rom8(0x008059) << 16);
    emu_rom_poke(0x008057, 0x00);
    emu_rom_poke(0x008058, 0x7f);
    emu_rom_poke(0x008059, 0xff);
    g_snes->patchMapped = true;
    memset(g_snes->patchCode, 0xea, sizeof(g_snes->patchCode));
    emu_set_patch(NULL, 0);
    g_snes->cpu->hookPc = 0x8053;
    g_snes->cpu->pcHook = on_pc_hook;
  } else {
    app_log("warning: main loop signature not found, online sync disabled for this ROM");
  }
  return true;
}

bool emu_hooked(void) { return s_loaded && g_emuMainRouting != 0; }

void emu_save_sram(bool force) {
  if (!s_loaded || !s_srmPath[0] || !s_srmSaved) return;
  uint64_t now = app_ms();
  if (!force && now - s_lastSrmCheck < 3000) return;
  s_lastSrmCheck = now;
  if (memcmp(s_srmSaved, g_snes->cart->ram, s_srmSize) == 0) return;
  memcpy(s_srmSaved, g_snes->cart->ram, s_srmSize);
  if (!file_write(s_srmPath, s_srmSaved, s_srmSize)) app_log("could not write %s", s_srmPath);
}

void emu_close(void) {
  if (!s_loaded) return;
  emu_save_sram(true);
  snes_free(g_snes);
  g_snes = NULL;
  s_loaded = false;
  g_emuMainRouting = 0;
}

bool emu_loaded(void) { return s_loaded; }
uint32_t emu_rom_crc(void) { return s_romCrc; }
uint8_t *emu_ram(void) { return g_snes->ram; }
uint8_t *emu_sram(int *size) { if (size) *size = g_snes->cart->ramSize; return g_snes->cart->ram; }

void emu_frame(uint16_t pad) {
  if (!s_loaded) return;
  g_snes->input1->currentState = pad;
  snes_runFrame(g_snes);
  Ppu *ppu = g_snes->ppu;
  int base = ppu->evenFrame ? 0 : 239;
  for (int y = 0; y < 224; y++) {
    const uint8_t *row = &ppu->pixelBuffer[(y + base) * 2048 + 4];
    uint32_t *dst = g_emuFrame + y * 256;
    for (int x = 0; x < 256; x++) {
      uint32_t c;
      memcpy(&c, row + x * 8, 4);
      dst[x] = c & 0xffffff;
    }
  }
  emu_save_sram(false);
}

void emu_audio(int16_t *out, int frames) {
  if (!s_loaded) { memset(out, 0, frames * 4); return; }
  dsp_getSamples(g_snes->apu->dsp, out, frames);
}

bool emu_screenshot(const char *path) {
  return png_write(path, g_canvas, CANVAS_W, CANVAS_H, CANVAS_W);
}
