# A Link to the Past Online - source

The player-facing description is in `../game-win64/README.md`. This file is for
people who want to build or change the program.

## Building

**Windows (MSYS2, MinGW64)** with `mingw-w64-x86_64-gcc`, `make`,
`mingw-w64-x86_64-SDL2` and `mingw-w64-x86_64-zlib` installed:

```sh
C:\msys64\usr\bin\bash.exe -l /full/path/to/source/build.sh [install]
```

`build.sh` sets up the MinGW64 environment and runs `make`; the result is
`alttpo.exe` in this folder. With `install` it is also copied to
`../game-win64/ALttP Online.exe`. That is a separate step on purpose: players
in one room need the same version, so an unfinished build must not end up in
the folder somebody is playing from. SDL2 is linked statically, the exe needs
no DLLs. Pass the full path of the script: a login shell starts in the home
folder.

Two version numbers matter. `PROTO_VERSION` (`app.h`) is the game protocol:
raise it when players with the old and the new build must not play together.
`NET_VERSION` (`net.c`) covers the matchmaking topics, the room key and the
packet layout and should stay as it is unless those change: it is what lets
two different builds find each other and be told that they differ, instead of
each seeing an empty room. `tools/tver.sh` runs an older exe against the
current one to check that.

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
| `src/game.c` | Everything about the game: reading its memory, merging save data, keys, bombs, PvP, sprites and sounds of other players, tunic colors, tilemap changes, sending the seed to joiners |
| `src/enemy.c` | Shared enemies: who runs which enemy, mirroring it into the other games, deaths and drops |
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
* `snes.c`: a hook on the four bytes the game sends to the sound CPU. The game
  hands over its two sound effect bytes once per frame; `game.c` notes them for
  the other players and, on frames where the game has nothing to play, puts a
  sound of another player there instead.

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

6. **Enemies** (`enemy.c`, 60 Hz on a direct link): for every enemy that
   several players have loaded, one player is its authority and sends its
   complete sprite state (56 bytes, zeros left out); the others overwrite their
   copy with it and keep the game from changing the outcome (copies carry no
   damage and do not finish dying). The authority is whoever last hit the
   enemy, otherwise the nearest player who has it on screen; claims are
   numbered so that everybody agrees. Sprites are identified by the number the
   game gives them when it loads a room or area, which is the same in every
   game. Deaths travel as a list every player keeps for the place they are in
   and repeats: "gone", "gone and left a pickup" or "pickup collected". The
   list is dropped the moment the game names another place, before the game
   runs there, because the numbers start again at 0 in every room. Enemies
   that carry a key or a randomizer item die in every game on their own, so the
   game's own code creates the item. The comment at the top of `enemy.c` has
   the details.
7. **Sounds**: the last few sound effects ride on the state packets with a
   running number, the receiver plays the ones it has not played yet (unless
   its own game just played the same sound, which is what a shared enemy makes
   happen).
8. **Warp to Player** (since 1.3): the player who is asked answers, once Link
   stands still, with where they are in the terms the game has for its doors.
   Outdoors that is the block the game notes down when Link walks into a house
   (`$7EC140`, 0x32 bytes: area, scroll, coordinates, camera), which it uses
   to put him back at the same spot when he comes out. Indoors it is one entry
   of the game's table of entrances (room, scroll, coordinates, camera, room
   edges, tile set, floor, dungeon, layer, quadrants), filled in with their
   live values, plus that block. The player who warps starts the game's
   "walk through a door" (module 0F, then 08 or 06). Just before the game
   loads, the block is written to WRAM with a room number that makes the game
   take the "coming out of a house" path, or the entry is written over the one
   of the same entrance in the ROM image for that single frame. Everything
   else (graphics, palettes, sprites, music, keys) is the game's own loading.
   Afterwards Link is on the entrance the other player came through, so a
   defeat or the mirror takes him where it would take them.
9. **Players on the world map**: every player sends where the map should show
   them twice a second: Link's coordinates outdoors, otherwise the last place
   he was outdoors (for a file that starts indoors, the exit of that room from
   the game's exit table). The markers are drawn on our own canvas with the
   arithmetic and the table the game uses for Link's marker
   (`map_project`), so they follow both zoom levels and the scrolling.
10. **Saving on leaving**: before an online game is closed, `game_save_before_exit`
   lets the game run its own Save and Quit (module 17, the routine every
   randomizer hooks) and runs the emulation until the file is written.

All of that leans on where things are in the Japanese v1.0 ROM (the addresses
are together near the top of that part of `game.c`). `check_rom` looks for the
code and tables when a game starts and turns the three features off for a ROM
that is laid out differently.

The player's own tunic color is not painted over the picture: the four mail
palettes in the loaded ROM image are changed (colors 9 to 12), so the game
itself loads, fades and flashes the new colors.

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
with environment variables. `tools/t.sh`, `tools/t2.sh` and `tools/t4.sh` are
ready-made one, two and four player runs, `tools/t4e.sh` is four players
fighting the same guards, `tools/troom.sh` is two players walking from one
castle room into the next a few frames apart (the guards of the second room
must still be there; it can also run an older build as one of the players),
`tools/twarp.sh` is one player warping to the other in ten situations
(overworld, houses, a dungeon room, both worlds, as a bunny), `tools/tsave.sh`
a player who leaves a room without saving and comes back, `tools/tmap.sh`
puts a player on the other one's world map (checked by eye on the
screenshots), `tools/nav.sh` helps to find a walking route and
`tools/sheet.py` makes a contact sheet of the screenshots. The scripts run
`alttpo.exe` from a folder of its own, so the installed game is left alone.
Each script says at its top what it needs and what it prints.

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
| `ALTTPO_ENEMY_LOG`, `ALTTPO_SFX_LOG` | log every tracked sprite each N frames and every change of place (with a clock shared by all copies on the PC); log shared sound effects |

A sprite can be put into a running game with `ALTTPO_POKE`: write its position,
type and room number (`0bc0+slot`), then state 8 (`0dd0+slot`), and the game
sets it up on the next frame. That is how the indoor enemy tests work without
walking to a dungeon.

A player can be sent through any door of the game the same way, which is how
`troom.sh` gets into Hyrule Castle: write the entrance number to `010e`, `06`
to `010c`, `00` to `11` and `b0` and `0f` to `10`. That is what the game does
itself when Link walks into a door (entrance 03, 04 and 05 are the castle's
west, front and east door). The file needs to be past the intro. Writing `01`
to `037b` every few frames keeps sprites from hurting Link, so a scripted walk
ends in the same place every time. All writes of one `ALTTPO_POKE` list have to
be in the order of their frames: an entry is not looked at before the ones in
front of it are done.

Two more of the game's own moves that the tests use: outdoors, `7b`=`00`,
`2db`=`01`, `b0`=`00`, `5d`=`14` and `11`=`23` is what stepping on a warp tile
does (Link goes to the other world, as a bunny without the moon pearl), and
`10c`=`09`, `11`=`0a`, `200`=`00`, `10`=`0e` is the flute's bird bringing up its
map. Select, Down, A is the game's Save and Quit.

## Licenses

The new code is provided as is, for personal use with your own copy of the
game. `src/snes/` is LakeSnes (MIT, see `src/snes/LICENSE.txt`). The bundled
randomizer is MIT licensed (`randomizer/app/LICENSE`). Game knowledge was
learned from the alttpo project.
