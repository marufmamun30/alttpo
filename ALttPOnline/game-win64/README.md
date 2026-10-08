# A Link to the Past Online

Seamless online co-op for *The Legend of Zelda: A Link to the Past*, for up to
8 players, with a built-in randomizer. One program: start `ALttP Online.exe`.

You need your own ROM of **Zelda no Densetsu - Kamigami no Triforce (Japan,
v1.0)**, the version all randomizers are built on. Put it next to the program
or drag it onto the window. It never leaves your PC: players who join a
randomized game only receive the *differences* to that ROM.

## Playing online

* **HOST A ROOM**: pick a game (the original, or any seed in `seeds`), choose
  the room rules and open the room. You get a 5 letter **room code**.
* **JOIN A ROOM**: type the code. The seed is fetched from the room, saved to
  your `seeds` folder, and the game starts.
* Friends can join and leave at any time. The room stays open as long as one
  player is in it; the host leaving does not close it.
* Every player uses the game's own file menu and has their own save file. A new
  file catches up with the team's items and progress when it starts.

No port forwarding, no server to run, no IP addresses: see "How the connection
works" below.

### What is shared

| Shared with the team | Your own |
|---|---|
| All items, swords, shields, mail, gloves, bottles | Health and magic |
| Maps, compasses, big keys, small keys | Rupees |
| Heart containers and pieces of heart (maximum health) | Arrows |
| Pendants, crystals, bosses, story progress | Bombs you carry |
| Opened chests, opened doors, overworld changes | |
| The enemies around you, and what they drop | |

* **Bombs:** when someone collects bombs, every player's bomb count goes up by
  the same amount. Placing a bomb only uses up your own.
* **Small keys** are a team resource: a key found is a key for everyone, a key
  used is used for everyone (and the door it opened is open for everyone).
* **Players in the same area see and hear each other**, with their name and
  their tunic in their color (you see your own color on yourself too). Cut
  bushes, lifted pots and opened doors appear for everyone in the room.
* **Enemies are shared.** Players in the same room or area fight the same
  enemies, in the same places: an enemy one player beats is beaten for
  everyone, and what it drops goes to whoever picks it up first. An enemy goes
  after the player nearest to it. Bosses are the exception: every player
  fights their own copy.
* **Players can fight:** sword, spin attack, hammer, arrows, rods, boomerang
  and hookshot hit other players. Mail reduces sword damage. The host can turn
  this off in the room rules.

The host chooses the room rules (shared enemies, fighting, shared bombs, shared
keys, shared heart containers) when opening the room.

In **OPTIONS** you can turn off the other players' sound effects and the
colored tunics (then everybody, you included, wears the game's own colors).

## The randomizer

**RANDOMIZER** in the main menu creates new seeds. It runs the
[ALttP Door Randomizer](https://github.com/aerinon/ALttPDoorRandomizer)
generator that is bundled in the `randomizer` folder.

* **PRESETS**: Original Adventure (English), Item Randomizer, Keysanity,
  Entrance Randomizer, Crossed Entrances, Door Randomizer (Basic / Crossed),
  Doors + Entrances, Enemizer, Triforce Hunt, Total Chaos...
* **ITEMS**: logic, item placement, swords, item pool, keysanity (keys, big
  keys, maps, compasses), shopsanity, enemy drops, pots, bow and flute modes...
* **WORLD**: Open / Standard / Inverted, the goal, crystal requirements,
  Triforce hunt numbers, timers, spoiler log.
* **ENTRANCES**: entrance shuffle from Dungeons-only to Insanity, plus its
  sub-options.
* **DUNGEONS**: door shuffle (Basic, Partitioned, Crossed), intensity, door
  types, trap doors, key logic, chest counters.
* **ENEMIES**: boss shuffle, enemy shuffle, enemy health and damage.
* **EXTRAS**: quick swap, menu speed, heart color and beep, palettes, flashing.
* **CREATE**: name the seed, optionally give a seed number (the same number and
  settings always give the same seed) and create it.

Seeds are stored as `seeds/<name>.sfc` with a spoiler log
`seeds/<name>_spoiler.txt`. Very demanding combinations sometimes fail for a
particular seed number; the launcher then tries other numbers automatically.
Crossed door seeds can take a few minutes to create.

`original-english` is a seed made with the "Original Adventure (English)"
preset: the normal game with English text.

## Controls

| | Keyboard | Gamepad (Xbox layout) |
|---|---|---|
| Move | Arrows | D-pad / left stick |
| Sword (B) | Z | A |
| Action (A) | X | B |
| Item (Y) | A | X |
| Map (X) | S | Y |
| Start / Select | Enter / Right Shift | Start / Back |
| L / R | D / C | LB / RB |
| Menu, room code | Esc | LB + RB + Start, or Guide |
| Player list | Tab (hold) | |
| Fullscreen / screenshot | F11 or Alt+Enter / F12 | |

Everything can be rebound in **OPTIONS > CONTROLS**. Menus also work with the
mouse. A button you press to open, use or close the menu is not passed on to
the game: it counts again once you have let go of it.

## Saving

Use the game's own **Save and Quit**. Save files are per game:
`saves/<game name>.srm` (three file slots each, as in the original). A second
copy of the program running on the same PC uses `saves-player2`, and so on, so
you can try co-op on one PC without the copies overwriting each other.

## How the connection works

* The room code is turned into a meeting point on two public message brokers
  (`broker.emqx.io` and `broker.hivemq.com`, plain MQTT, outbound TCP port
  1883). Players use it only to find each other; what they exchange there is
  encrypted with a key derived from the room code.
* Each player asks a public STUN server for their public address and all
  players open direct UDP connections to each other ("hole punching"). The
  player list in the Esc menu shows the ping of every direct link.
* If two players' routers do not allow a direct link, their traffic is passed
  through the broker instead. The player list shows **relayed** for that
  player; it works, but that player moves less smoothly on your screen.

Nothing needs to be installed or opened on the router. If Windows Firewall
asks about the program the first time, allow it; direct links work best then.

## Troubleshooting

* **"Could not reach the matchmaking service"**: the PC is offline, or a
  firewall blocks outbound TCP port 1883.
* **"Room ... was not found"**: check the code; the room exists only while
  someone is in it.
* **"another version of the game"**: all players in a room need the same
  version of `ALttP Online.exe`. The version is shown at the bottom of the
  title screen. When you update, send the new exe to everyone you play with.
* **Another player is shown as "different game!"**: they loaded another seed.
  Everyone in a room must play the room's seed (joining through the code does
  this automatically).
* **A door someone else opened still looks closed**: walk out of the room and
  back in. Changes are shown live for players who were in the room when they
  happened.
* **Enemies behave strangely with other players around**: the host can turn
  "Shared Enemies" off in the room rules; every player then has their own
  enemies again.
* **Sound crackles / game is slow**: the emulation needs roughly one third of
  a CPU core. Close other heavy programs.
* `alttpo.log` next to the program records what happened in the last session
  (`alttpo-previous.log` the one before): which matchmaking servers answered,
  who was found, and whether a player is reached directly or through the relay.

## Credits

* The game logic knowledge (memory map, what can be merged safely, how to pick
  Link's sprites) comes from [alttpo](https://github.com/alttpo/alttpo) by
  James Dunne and contributors, the project that pioneered ALttP online.
* Emulation core: [LakeSnes](https://github.com/angelo-wf/LakeSnes) by
  angelo_wf (MIT license).
* Seed generation: [ALttP Door Randomizer](https://github.com/aerinon/ALttPDoorRandomizer)
  by Aerinon, based on the Entrance Randomizer by LLCoolDave, KevinCathcart,
  AmazingAmpharos and others (MIT license), run with a bundled copy of
  [Python](https://www.python.org).
* [SDL2](https://www.libsdl.org) (zlib license) and zlib.

The Legend of Zelda is a trademark of Nintendo. This is a fan project and
contains no game data; you must supply your own ROM.
