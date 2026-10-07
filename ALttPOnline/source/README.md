# A Link to the Past Online - source

The player-facing description is in `../game-win64/README.md`. This file is for
people who want to build or change the program.

## Building

**Windows (MSYS2, MinGW64)** with `mingw-w64-x86_64-gcc`, `make`,
`mingw-w64-x86_64-SDL2` and `mingw-w64-x86_64-zlib` installed:

```sh
C:\msys64\usr\bin\bash.exe -l /full/path/to/source/build.sh
```

`build.sh` sets up the MinGW64 environment, runs `make` and copies the result
to `../game-win64/ALttP Online.exe`. SDL2 is linked statically, the exe needs
no DLLs. Pass the full path of the script: a login shell starts in the home
folder.

**Linux / macOS**: `make` (needs SDL2 and zlib development packages). The game
and the netcode are portable; starting the seed generator (`rando.c`) is only
implemented for Windows.

The icon is created by `tools/gen_icon.py`, the room name table `src/names.h`
by `tools/gen_names.py`.

## Layout

| File | What it does |
|---|---|
| `src/main.c` | Window, input, audio, timing, config file, test hooks |
| `src/ui.c` | The 512x448 canvas, the pixel font and the menu widgets |
| `src/launcher.c` | All menu screens, starting / joining / leaving games |
| `src/emu.c` | Wrapper around the SNES core: ROM, save RAM, main-loop hook |
| `src/snes/` | LakeSnes, with three additions marked `ALTTPO` (see below) |
| `src/net.c` | Rooms without servers: MQTT signalling, STUN, UDP hole punching, relay fallback, reliable channel |
| `src/game.c` | Everything about the game: reading its memory, merging save data, keys, bombs, PvP, sprites of other players, tilemap changes, sending the seed to joiners |
| `src/rando.c` | Randomizer options, presets, running the generator |

### Additions to the SNES core

* `cpu.c`: a program counter hook. The game's main loop is at `00:8053`
  (`JSR ClearOamBuffer`, then `JSL Module_MainRouting`); `game.c:on_main` runs
  there once per game frame.
* `snes.c`: 256 bytes at `FF:7F00` are mapped to a patch buffer. `emu.c`
  redirects the `JSL Module_MainRouting` to it, so the game can be made to call
  its own routines (reload the sword graphics after a sword arrives, open the
  pyramid, ...) before the frame's normal work.
* `ppu.c`: "extra" sprites are composited into the OBJ layer line buffer with a
  priority and their own palette. Other players are real sprites to the PPU:
  they go behind walls and take part in color math (dark rooms, transparency).

### How the co-op works

Every player runs their own game. Once per game frame (`on_main`):

1. **Read**: module, location, coordinates, Link's OAM entries and their tile
   graphics, palettes, hitboxes.
2. **Send** (`send_state`, 60 Hz to players in the same area, slower to others
   and over the relay): position and the sprite list. Sprites refer to tile
   graphics by a hash of their pixels; tiles and palettes are sent once per
   player over the reliable channel and cached.
3. **Save data** (`send_sram` / `merge_*`): each player broadcasts the relevant
   ranges of the save data mirror at `$7EF000` whenever they change (and every
   few seconds). Receivers merge with per-field rules taken from alttpo:
   maximum, bitwise OR, or a custom rule (bottles, flute, progress flags...).
   Because the state is merged rather than replayed, lost packets and late
   joiners need no special handling.
4. **Events** (reliable): things that are counts instead of states.
   Small keys are sent as `+1 / -1` per dungeon; a player entering the game asks
   another player for the current key counts. Bomb pickups are sent as `+N` and
   added to the game's own refill counter. PvP hits are decided by the
   attacker and applied by the victim with the game's own damage and recoil.
5. **Tilemap changes**: the two tilemap layers in WRAM are compared with the
   previous frame while the game is in a settled state; changes are sent as
   runs and applied to WRAM, the collision attributes and VRAM by players in
   the same room (a Lamport stamp decides who is newer).

Joining: the first `HELLO` from a player who is in a game tells the joiner the
ROM's CRC and the room rules. If the joiner does not have that seed, it asks
for it and receives `zlib(seed XOR original ROM)`.

### Networking

See the comment at the top of `net.c`. The room code is hashed (SHA-256) into
an MQTT topic and a ChaCha20 key. Signalling messages (`ANNOUNCE`, `LEAVE`,
`RELAY`) are published encrypted on both brokers and de-duplicated. Peers ping
all announced addresses until a pong arrives over UDP; until then (or if that
never happens) packets travel as `RELAY` messages, rate limited. Each link has
an epoch on both sides so a one-sided timeout is noticed and both ends restart
their reliable stream.

## Testing without a screen

Set `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` and drive the program
with environment variables (`tools/t.sh`, `tools/t2.sh` and `tools/t4.sh` are
ready-made one, two and four player runs; `tools/sheet.py` makes a contact
sheet of the screenshots):

| Variable | Meaning |
|---|---|
| `ALTTPO_AUTO` | `solo`, `host`, `join:CODE`, `rando:<preset>`, `screen:<n>` |
| `ALTTPO_GAME`, `ALTTPO_ROOM`, `ALTTPO_SEEDNAME` | seed to load, fixed room code, name for the generated seed |
| `ALTTPO_NAME`, `ALTTPO_COLOR`, `ALTTPO_DATA` | player name / color, folder for saves, log and config |
| `ALTTPO_INPUTS` | `frame:hexbits,...` controller state (bits: B Y Select Start Up Down Left Right A X L R) |
| `ALTTPO_UI` | `frame:action,...` menu actions (`up down left right ok back menu tabl tabr text=...`) |
| `ALTTPO_POKE`, `ALTTPO_PEEK` | `frame:addr=value,...` WRAM writes; addresses logged once per second |
| `ALTTPO_SHOT_DIR`, `ALTTPO_SHOT_EVERY`, `ALTTPO_EXIT_AFTER`, `ALTTPO_FAST` | screenshots, exit frame, run unthrottled |
| `ALTTPO_NO_UDP`, `ALTTPO_NO_SEED_LOOKUP`, `ALTTPO_BROKER` | force the relay path, force a seed download, use a private MQTT broker |

## Licenses

The new code is provided as is, for personal use with your own copy of the
game. `src/snes/` is LakeSnes (MIT, see `src/snes/LICENSE.txt`). The bundled
randomizer is MIT licensed (`randomizer/app/LICENSE`). Game knowledge was
learned from the alttpo project.
