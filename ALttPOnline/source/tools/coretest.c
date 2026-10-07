// Headless core check: boots a ROM, feeds scripted input, writes PNG screenshots.
// usage: coretest rom.sfc outprefix frames shotEvery "frame:buttons,frame:buttons"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/snes/snes.h"
#include "../src/png.h"

static uint8_t pix[512 * 480 * 4];
static uint32_t out[256 * 240];

int main(int argc, char **argv) {
  if (argc < 5) return 1;
  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("no rom\n"); return 1; }
  fseek(f, 0, SEEK_END); int len = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *rom = malloc(len); fread(rom, 1, len, f); fclose(f);
  Snes *snes = snes_init();
  if (!snes_loadRom(snes, rom, len)) return 1;
  snes_setPixelFormat(snes, pixelFormatXRGB);
  int frames = atoi(argv[3]), every = atoi(argv[4]);
  const char *script = argc > 5 ? argv[5] : "";
  int nextFrame = -1, nextBtn = 0; const char *sp = script;
  if (*sp) { nextFrame = atoi(sp); sp = strchr(sp, ':') + 1; nextBtn = strtol(sp, NULL, 16); }
  clock_t t0 = clock();
  for (int i = 0; i < frames; i++) {
    if (i == nextFrame) {
      for (int b = 0; b < 12; b++) snes_setButtonState(snes, 1, b, (nextBtn >> b) & 1);
      sp = strchr(sp, ',');
      if (sp) { sp++; nextFrame = atoi(sp); sp = strchr(sp, ':') + 1; nextBtn = strtol(sp, NULL, 16); }
      else nextFrame = -1;
    }
    snes_runFrame(snes);
    if (every && (i % every) == every - 1) {
      snes_setPixels(snes, pix);
      for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++)
          out[y * 256 + x] = *(uint32_t *)&pix[(y * 2 * 512 + x * 2) * 4] & 0xffffff;
      char name[512];
      snprintf(name, sizeof(name), "%s_%05d.png", argv[2], i + 1);
      png_write(name, out, 256, 240, 256);
    }
  }
  double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
  printf("%d frames in %.2fs = %.0f fps; module=%02x sub=%02x\n", frames, s, frames / s, snes->ram[0x10], snes->ram[0x11]);
  return 0;
}
