#ifndef PNG_H
#define PNG_H
#include <stdint.h>
#include <stdbool.h>

// pixels are 0x00RRGGBB, pitch given in pixels
bool png_write(const char *path, const uint32_t *pixels, int w, int h, int pitchPixels);

#endif
