// Minimal PNG writer (RGB, 8 bit) on top of zlib. Used for screenshots.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <zlib.h>
#include "png.h"

static void put32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
  uint8_t hdr[8];
  put32(hdr, len);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (len) fwrite(data, 1, len, f);
  uint32_t crc = crc32(0, hdr + 4, 4);
  if (len) crc = crc32(crc, data, len);
  uint8_t c[4];
  put32(c, crc);
  fwrite(c, 1, 4, f);
}

// pixels: 0x00RRGGBB per pixel
bool png_write(const char *path, const uint32_t *pixels, int w, int h, int pitchPixels) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  put32(ihdr, w); put32(ihdr + 4, h);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  chunk(f, "IHDR", ihdr, 13);
  size_t rawLen = (size_t)h * (w * 3 + 1);
  uint8_t *raw = malloc(rawLen);
  uint8_t *q = raw;
  for (int y = 0; y < h; y++) {
    *q++ = 0;
    const uint32_t *row = pixels + (size_t)y * pitchPixels;
    for (int x = 0; x < w; x++) {
      uint32_t c = row[x];
      *q++ = c >> 16; *q++ = c >> 8; *q++ = c;
    }
  }
  uLongf zl = compressBound(rawLen);
  uint8_t *z = malloc(zl);
  compress2(z, &zl, raw, rawLen, 6);
  chunk(f, "IDAT", z, (uint32_t)zl);
  chunk(f, "IEND", NULL, 0);
  free(raw); free(z);
  fclose(f);
  return true;
}
