#ifndef EMU_H
#define EMU_H
#include <stdint.h>
#include <stdbool.h>

extern void (*g_emuMainHook)(void);   // runs once per game frame, right before the game's main routine
extern uint32_t g_emuMainRouting;     // address of Module_MainRouting (0 when the ROM is not hooked)
void emu_set_patch(const uint8_t *code, int len);
bool emu_hooked(void);
bool emu_try_rom_file(const char *path);

#endif
