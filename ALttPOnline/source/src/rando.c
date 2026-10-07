// Seed generation. The launcher exposes the options of the ALttP Door Randomizer
// (items, entrances, doors, enemies, ...) and runs its command line generator
// from the bundled "randomizer" folder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "app.h"

enum { TAB_ITEMS, TAB_WORLD, TAB_ENTRANCES, TAB_DUNGEONS, TAB_ENEMIES, TAB_GAME };
const char *g_randoTabs[] = {"ITEMS", "WORLD", "ENTRANCES", "DUNGEONS", "ENEMIES", "EXTRAS"};
int g_randoTabCount = 6;

#define C(key, label, tab, choices, names, def, help) {key, label, tab, RO_CHOICE, choices, names, def, 0, 0, 0, help}
#define B(key, label, tab, def, help) {key, label, tab, RO_BOOL, NULL, NULL, def, 0, 1, 1, help}
#define I(key, label, tab, def, mn, mx, st, help) {key, label, tab, RO_INT, NULL, NULL, def, mn, mx, st, help}

const RandoOpt g_randoOpts[] = {
  // ---------------- items
  C("logic", "Logic", TAB_ITEMS, "noglitches|minorglitches|owglitches|hybridglitches|nologic", "No Glitches|Minor Glitches|Overworld Glitches|Hybrid Major Glitches|No Logic", 0,
    "Which tricks the seed may expect from you. No Glitches is always beatable with normal play."),
  C("algorithm", "Item Placement", TAB_ITEMS, "balanced|vanilla_fill|major_only|dungeon_only|district", "Balanced|Vanilla Locations|Major Items in Major Spots|Dungeon Items Only|By District", 0,
    "How items are spread over the world. Vanilla Locations keeps every item where the original game has it."),
  C("swords", "Swords", TAB_ITEMS, "random|assured|swordless|vanilla", "Randomized|Assured (start with one)|Swordless|Vanilla Locations", 0,
    "Where the four swords are found."),
  C("progressive", "Progressive Items", TAB_ITEMS, "on|off|random", "On|Off|Random", 0,
    "Progressive swords, gloves, shields and mail always upgrade in order."),
  C("difficulty", "Item Pool", TAB_ITEMS, "normal|hard|expert", "Normal|Hard|Expert", 0,
    "Harder pools remove heart containers, shields, mail and other safety items."),
  C("item_functionality", "Item Functionality", TAB_ITEMS, "normal|hard|expert", "Normal|Hard|Expert", 0,
    "Harder settings weaken potions, the cape, byrna and other items."),
  C("accessibility", "Accessibility", TAB_ITEMS, "items|locations|none", "All Items Reachable|All Locations Reachable|Beatable Only", 0,
    "What the logic guarantees you can reach."),
  C("keyshuffle", "Small Keys", TAB_ITEMS, "none|wild|universal", "In Their Dungeon|Anywhere (Keysanity)|Universal Keys", 0,
    "Small keys can be shuffled into the whole world. They are shared by the whole team."),
  B("bigkeyshuffle", "Big Keys Anywhere", TAB_ITEMS, 0, "Big keys can be found outside of their dungeon."),
  B("mapshuffle", "Maps Anywhere", TAB_ITEMS, 0, "Dungeon maps can be found outside of their dungeon."),
  B("compassshuffle", "Compasses Anywhere", TAB_ITEMS, 0, "Compasses can be found outside of their dungeon."),
  C("restrict_boss_items", "Boss Items", TAB_ITEMS, "none|mapcompass|dungeon", "Anything|No Map or Compass|No Dungeon Items", 0,
    "Limits what a dungeon boss may drop."),
  B("shopsanity", "Shopsanity", TAB_ITEMS, 0, "Shop inventories are shuffled into the item pool."),
  C("dropshuffle", "Enemy Drops", TAB_ITEMS, "none|keys|underworld", "Vanilla|Key Drops Shuffled|All Underworld Drops", 0,
    "Keys (or everything) dropped by dungeon enemies join the item pool."),
  C("pottery", "Pots", TAB_ITEMS, "none|keys|cave|cavekeys|reduced|clustered|nonempty|dungeon|lottery", "Vanilla|Key Pots|Cave Pots|Cave + Key Pots|Reduced Dungeon Pots|Clustered Dungeon Pots|Non-Empty Pots|All Dungeon Pots|Every Pot (Lottery)", 0,
    "Which pots hold shuffled items."),
  B("colorizepots", "Mark Item Pots", TAB_ITEMS, 1, "Pots that hold a shuffled item get a different color."),
  B("shufflepots", "Shuffle Pot Contents", TAB_ITEMS, 0, "The ordinary contents of pots are shuffled between pots."),
  C("bow_mode", "Bow", TAB_ITEMS, "progressive|silvers|retro|retro_silvers", "Progressive|Silvers Separate|Rupee Bow|Rupee Bow + Silvers", 0,
    "Rupee Bow: arrows cost rupees instead of ammo."),
  C("flute_mode", "Flute", TAB_ITEMS, "normal|active", "Normal|Already Activated", 0,
    "Whether the flute still has to be played in Kakariko."),
  B("bombbag", "Bomb Bag Required", TAB_ITEMS, 0, "You can not carry bombs until a bomb upgrade has been found."),
  B("pseudoboots", "Pseudo Boots", TAB_ITEMS, 0, "Start with the ability to dash, but without the real boots."),
  C("take_any", "Take-Any Caves", TAB_ITEMS, "none|random|fixed", "None|Random|Fixed", 0,
    "Adds caves where you choose between a heart container and a potion."),
  C("beemizer", "Beemizer", TAB_ITEMS, "0|1|2|3|4", "Off|1|2|3|4", 0, "Replaces junk items with bees and bee traps."),
  B("hints", "Hint Tiles", TAB_ITEMS, 0, "Telepathy tiles give hints about item locations."),

  // ---------------- world
  C("mode", "World State", TAB_WORLD, "open|standard|inverted", "Open|Standard|Inverted", 0,
    "Open: start anywhere, no escape sequence. Standard: rescue Zelda first. Inverted: start in the Dark World."),
  C("goal", "Goal", TAB_WORLD, "ganon|pedestal|dungeons|triforcehunt|trinity|crystals|ganonhunt|completionist", "Defeat Ganon|Master Sword Pedestal|All Dungeons|Triforce Hunt|Trinity|Crystals Only|Ganon Hunt|Completionist", 0,
    "What the team has to do to finish the game."),
  C("crystals_gt", "Crystals for Ganon's Tower", TAB_WORLD, "7|6|5|4|3|2|1|0|random", NULL, 0, "Crystals needed to open Ganon's Tower."),
  C("crystals_ganon", "Crystals for Ganon", TAB_WORLD, "7|6|5|4|3|2|1|0|random", NULL, 0, "Crystals needed to hurt Ganon."),
  C("openpyramid", "Pyramid Hole", TAB_WORLD, "auto|yes|no", "Auto|Open|Closed", 0, "Whether the hole to Ganon is open from the start."),
  I("triforce_pool", "Triforce Pieces in Pool", TAB_WORLD, 0, 0, 100, 5, "For Triforce Hunt goals. 0 uses the default (30)."),
  I("triforce_goal", "Triforce Pieces Needed", TAB_WORLD, 0, 0, 100, 5, "For Triforce Hunt goals. 0 uses the default (20)."),
  C("timer", "Timer", TAB_WORLD, "none|display|timed|timed-ohko|ohko|timed-countdown", "None|Stopwatch|Timed|Timed One-Hit KO|One-Hit KO|Countdown", 0,
    "Adds a clock, or makes every hit fatal."),
  C("spoiler", "Spoiler Log", TAB_WORLD, "none|settings|semi|full", "None|Settings Only|Partial|Full", 3,
    "A text file next to the seed that lists where everything is."),

  // ---------------- entrances
  C("shuffle", "Entrance Shuffle", TAB_ENTRANCES, "vanilla|dungeonssimple|dungeonsfull|simple|restricted|full|lite|lean|swapped|crossed|insanity", "Vanilla|Dungeons (Simple)|Dungeons (Full)|Simple|Restricted|Full|Lite|Lean|Swapped|Crossed|Insanity", 0,
    "Shuffles where doors and caves lead. Crossed mixes Light and Dark World entrances; Insanity also decouples exits."),
  B("shuffleganon", "Include Ganon's Tower / Pyramid", TAB_ENTRANCES, 1, "The pyramid hole and Ganon's Tower take part in the shuffle."),
  B("shufflelinks", "Shuffle Link's House", TAB_ENTRANCES, 0, "Link's house can be anywhere."),
  B("shuffletavern", "Shuffle Tavern Back Door", TAB_ENTRANCES, 1, "The tavern's back entrance takes part in the shuffle."),
  C("skullwoods", "Skull Woods", TAB_ENTRANCES, "original|restricted|loose|followlinked", "Original|Restricted|Loose|Follow Linked", 0,
    "How the Skull Woods entrances and drops are shuffled."),
  C("linked_drops", "Linked Drops", TAB_ENTRANCES, "unset|linked|independent", "Default|Linked|Independent", 0,
    "Whether a drop hole and its matching cave entrance stay together."),
  C("overworld_map", "Overworld Map", TAB_ENTRANCES, "default|compass|map", "Default|Compass Shows Dungeons|Map Shows Dungeons", 0,
    "Lets a dungeon item reveal where the dungeon entrance is."),

  // ---------------- dungeons
  C("door_shuffle", "Door Shuffle", TAB_DUNGEONS, "vanilla|basic|partitioned|crossed", "Vanilla|Basic|Partitioned|Crossed", 0,
    "Shuffles the doors inside dungeons. Basic keeps each dungeon's rooms together; Crossed mixes rooms of all dungeons."),
  C("intensity", "Door Shuffle Intensity", TAB_DUNGEONS, "1|2|3|random", "1 - Doors and Spiral Stairs|2 - Plus Open Edges|3 - Plus Dungeon Lobbies|Random", 1,
    "How much of a dungeon's layout the door shuffle may change."),
  C("door_type_mode", "Door Types", TAB_DUNGEONS, "original|big|all|chaos", "Original|Big Key Doors|All Door Types|Chaos", 0,
    "Which doors may become key doors, bomb doors and so on."),
  C("trap_door_mode", "Trap Doors", TAB_DUNGEONS, "optional|vanilla|boss|oneway", "Removed Where Needed|Vanilla|Boss Rooms Only|One-Way Removed", 0,
    "How shutter doors that lock behind you are treated."),
  C("key_logic_algorithm", "Key Logic", TAB_DUNGEONS, "partial|strict|dangerous", "Partial Protection|Strict|Dangerous", 0,
    "How careful the logic is about wasting small keys. With shared keys, Strict is the safest for big teams."),
  B("decoupledoors", "Decouple Doors", TAB_DUNGEONS, 0, "Going back through a door may lead somewhere else."),
  B("door_self_loops", "Allow Self-Looping Doors", TAB_DUNGEONS, 0, "A door may lead back into the same room."),
  C("dungeon_counters", "Dungeon Chest Counters", TAB_DUNGEONS, "default|off|on|pickup", "Default|Off|On|On Compass Pickup", 0,
    "Shows how many items are left in the current dungeon."),
  C("mixed_travel", "Mixed Dungeon Travel", TAB_DUNGEONS, "prevent|allow|force", "Prevent|Allow|Force", 0,
    "For crossed doors: how travelling between mixed dungeons with the mirror is handled."),
  C("standardize_palettes", "Dungeon Palettes", TAB_DUNGEONS, "standardize|original", "Standardized|Original", 0,
    "For crossed doors: keep one palette per dungeon or the original palette per room."),
  B("experimental", "Experimental Features", TAB_DUNGEONS, 0, "Enables features the randomizer authors still mark as experimental."),

  // ---------------- enemies
  C("shufflebosses", "Boss Shuffle", TAB_ENEMIES, "none|simple|full|unique|random", "Off|Simple|Full|Unique|Random", 0,
    "Dungeon bosses are moved to other dungeons."),
  C("shuffleenemies", "Enemy Shuffle", TAB_ENEMIES, "none|shuffled", "Off|Shuffled", 0,
    "Enemies are replaced by other enemies all over the world."),
  C("enemy_health", "Enemy Health", TAB_ENEMIES, "default|easy|normal|hard|expert", "Default|Easy|Normal|Hard|Expert", 0,
    "Randomizes enemy health within a range."),
  C("enemy_damage", "Enemy Damage", TAB_ENEMIES, "default|shuffled|random", "Default|Shuffled|Random", 0,
    "Changes how hard enemies hit."),
  C("any_enemy_logic", "Enemy Drop Logic", TAB_ENEMIES, "allow_all|allow_drops|none", "Allow All|Allow Drops|None", 0,
    "Whether the logic may expect you to get keys from shuffled enemies."),

  // ---------------- extras
  B("quickswap", "Quick Swap (L/R)", TAB_GAME, 1, "Switch items with L and R without opening the menu."),
  C("fastmenu", "Menu Speed", TAB_GAME, "normal|instant|double|triple|quadruple|half", "Normal|Instant|Double|Triple|Quadruple|Half", 0, "How fast the item menu opens."),
  C("heartbeep", "Low Health Beep", TAB_GAME, "normal|double|half|quarter|off", "Normal|Double|Half|Quarter|Off", 0, "How often the low health warning beeps."),
  C("heartcolor", "Heart Color", TAB_GAME, "red|blue|green|yellow|random", "Red|Blue|Green|Yellow|Random", 0, "Color of the hearts in the HUD."),
  C("ow_palettes", "Overworld Colors", TAB_GAME, "default|random|blackout", "Default|Random|Blackout", 0, "Recolors the overworld."),
  C("uw_palettes", "Dungeon Colors", TAB_GAME, "default|random|blackout", "Default|Random|Blackout", 0, "Recolors dungeons and caves."),
  B("reduce_flashing", "Reduce Flashing", TAB_GAME, 1, "Tones down full screen flashes."),
  B("shuffle_sfx", "Shuffle Sound Effects", TAB_GAME, 0, "Sound effects are swapped around."),
  B("disablemusic", "Disable Music", TAB_GAME, 0, "Turns the background music off."),
  B("mirrorscroll", "Mirror Scroll", TAB_GAME, 0, "Start with a scroll that works like the mirror inside dungeons."),
  B("collection_rate", "Show Collection Rate", TAB_GAME, 0, "Shows the number of collected items in the HUD."),
};
int g_randoOptCount = sizeof(g_randoOpts) / sizeof(g_randoOpts[0]);
int g_randoVals[sizeof(g_randoOpts) / sizeof(g_randoOpts[0])];

int rando_choice_count(int opt) {
  const RandoOpt *o = &g_randoOpts[opt];
  if (o->type != RO_CHOICE) return 0;
  int n = 1;
  for (const char *p = o->choices; *p; p++) if (*p == '|') n++;
  return n;
}

static const char *nth(const char *list, int idx, char *buf, int size) {
  const char *p = list;
  for (int i = 0; i < idx && p; i++) { p = strchr(p, '|'); if (p) p++; }
  buf[0] = 0;
  if (!p) return buf;
  int n = 0;
  while (p[n] && p[n] != '|' && n < size - 1) { buf[n] = p[n]; n++; }
  buf[n] = 0;
  return buf;
}

const char *rando_value_name(int opt, int value, char *buf, int size) {
  const RandoOpt *o = &g_randoOpts[opt];
  if (o->type == RO_BOOL) { snprintf(buf, size, "%s", value ? "On" : "Off"); return buf; }
  if (o->type == RO_INT) { if (value == 0 && o->min == 0) snprintf(buf, size, "Default"); else snprintf(buf, size, "%d", value); return buf; }
  return nth(o->choiceNames ? o->choiceNames : o->choices, value, buf, size);
}

static int opt_index(const char *key) {
  for (int i = 0; i < g_randoOptCount; i++) if (!strcmp(g_randoOpts[i].key, key)) return i;
  return -1;
}

static void set_choice(const char *key, const char *value) {
  int i = opt_index(key);
  if (i < 0) return;
  const RandoOpt *o = &g_randoOpts[i];
  if (o->type != RO_CHOICE) { g_randoVals[i] = atoi(value); return; }
  int n = rando_choice_count(i);
  char buf[64];
  for (int k = 0; k < n; k++) if (!strcmp(nth(o->choices, k, buf, sizeof(buf)), value)) { g_randoVals[i] = k; return; }
}

void rando_defaults(void) {
  for (int i = 0; i < g_randoOptCount; i++) g_randoVals[i] = g_randoOpts[i].def;
}

typedef struct Preset { const char *name, *desc, *settings; } Preset;
static const Preset kPresets[] = {
  {"Original Adventure (English)", "The original game, every item where it always was, with English text. A good first co-op run.",
   "mode=standard algorithm=vanilla_fill swords=vanilla goal=ganon"},
  {"Item Randomizer", "Classic randomizer: open world, all items shuffled, dungeons and entrances untouched.",
   "mode=open"},
  {"Standard Item Randomizer", "Rescue Zelda first, then find the shuffled items.",
   "mode=standard swords=random"},
  {"Keysanity", "Maps, compasses and all keys can be anywhere in the world.",
   "mode=open keyshuffle=wild bigkeyshuffle=1 mapshuffle=1 compassshuffle=1"},
  {"Entrance Randomizer", "Items and every cave, house and dungeon entrance are shuffled.",
   "mode=open shuffle=full"},
  {"Crossed Entrances", "Entrances are shuffled across both worlds.",
   "mode=open shuffle=crossed"},
  {"Door Randomizer (Basic)", "Items shuffled and the doors inside each dungeon are rearranged.",
   "mode=open door_shuffle=basic intensity=2 dungeon_counters=on"},
  {"Door Randomizer (Crossed)", "Dungeon rooms are mixed between all dungeons.",
   "mode=open door_shuffle=crossed intensity=3 dungeon_counters=on"},
  {"Doors + Entrances", "Entrances and dungeon doors are both shuffled. Bring a map and some friends.",
   "mode=open shuffle=crossed door_shuffle=crossed intensity=3 dungeon_counters=on keyshuffle=wild"},
  {"Enemizer", "Items plus shuffled enemies and bosses.",
   "mode=open shufflebosses=full shuffleenemies=shuffled enemy_health=default"},
  {"Triforce Hunt", "Collect Triforce pieces as a team instead of fighting Ganon.",
   "mode=open goal=triforcehunt triforce_pool=30 triforce_goal=20"},
  {"Total Chaos", "Everything at once: crossed doors, insanity entrances, keysanity, pots, enemies and bosses.",
   "mode=open shuffle=insanity door_shuffle=crossed intensity=3 keyshuffle=wild bigkeyshuffle=1 mapshuffle=1 compassshuffle=1 shufflebosses=full shuffleenemies=shuffled pottery=lottery dropshuffle=underworld shopsanity=1 dungeon_counters=on"},
};
int g_randoPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
const char *rando_preset_name(int i) { return kPresets[i].name; }
const char *rando_preset_desc(int i) { return kPresets[i].desc; }

void rando_apply_preset(int idx) {
  rando_defaults();
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", kPresets[idx].settings);
  for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = 0;
    set_choice(tok, eq + 1);
  }
}

// ------------------------------------------------------------------ running the generator
#define LOG_LINES 64
static char s_log[LOG_LINES][120];
static int s_logCount;
static char s_partial[240];
static bool s_runningGen;
static int s_result;
static char s_outPath[600];
static char s_seedName[48];
static char s_workDir[600];
#ifdef _WIN32
static HANDLE s_proc, s_pipe;
#endif

static void log_line(const char *s) {
  if (!*s) return;
  snprintf(s_log[s_logCount % LOG_LINES], sizeof(s_log[0]), "%s", s);
  s_logCount++;
  app_log("rando: %s", s);
}

const char *rando_log_line(int fromEnd) {
  if (fromEnd >= s_logCount || fromEnd >= LOG_LINES) return "";
  return s_log[(s_logCount - 1 - fromEnd) % LOG_LINES];
}

const char *rando_output_path(void) { return s_outPath; }
bool rando_running(void) { return s_runningGen; }

bool rando_available(void) {
  char p[600];
  struct stat st;
  app_path(p, sizeof(p), "randomizer/python/python.exe");
  if (stat(p, &st) != 0) return false;
  app_path(p, sizeof(p), "randomizer/app/DungeonRandomizer.py");
  return stat(p, &st) == 0;
}

static void clean_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char p[900];
    snprintf(p, sizeof(p), "%s%s", dir, e->d_name);
    remove(p);
  }
  closedir(d);
}

bool rando_start(const char *seedName, uint32_t seedNumber) {
#ifdef _WIN32
  if (s_runningGen || !g_baseRom) return false;
  s_partial[0] = 0;
  s_result = 0;
  s_outPath[0] = 0;
  snprintf(s_seedName, sizeof(s_seedName), "%s", seedName);
  app_path(s_workDir, sizeof(s_workDir), "randomizer/work/");
  CreateDirectoryA(s_workDir, NULL);
  clean_dir(s_workDir);

  // the generator reads the original ROM from a file
  char romPath[700], jsonPath[700];
  snprintf(romPath, sizeof(romPath), "%sbase.sfc", s_workDir);
  if (!file_write(romPath, g_baseRom, g_baseRomLen)) { log_line("Could not write to the randomizer folder."); return false; }

  // switches go through a settings file, choices on the command line
  snprintf(jsonPath, sizeof(jsonPath), "%ssettings.json", s_workDir);
  FILE *f = fopen(jsonPath, "w");
  if (!f) return false;
  fprintf(f, "{\n");
  for (int i = 0; i < g_randoOptCount; i++) {
    const RandoOpt *o = &g_randoOpts[i];
    if (o->type == RO_BOOL) fprintf(f, "  \"%s\": %s,\n", o->key, g_randoVals[i] ? "true" : "false");
    else if (o->type == RO_INT) fprintf(f, "  \"%s\": %d,\n", o->key, g_randoVals[i]);
  }
  fprintf(f, "  \"calc_playthrough\": %s,\n", "true");
  fprintf(f, "  \"saveonexit\": \"never\",\n  \"suppress_meta\": true\n}\n");
  fclose(f);

  char cmd[4096], exe[700], cwd[700];
  app_path(exe, sizeof(exe), "randomizer/python/python.exe");
  app_path(cwd, sizeof(cwd), "randomizer/app");
  for (char *c = exe; *c; c++) if (*c == '/') *c = '\\';
  for (char *c = cwd; *c; c++) if (*c == '/') *c = '\\';
  int n = snprintf(cmd, sizeof(cmd), "\"%s\" -u DungeonRandomizer.py --settingsfile \"%s\" --rom \"%s\" --outputpath \"%s.\" --outputname seed --seed %u",
                   exe, jsonPath, romPath, s_workDir, seedNumber);
  char buf[64];
  for (int i = 0; i < g_randoOptCount; i++) {
    const RandoOpt *o = &g_randoOpts[i];
    if (o->type != RO_CHOICE) continue;
    n += snprintf(cmd + n, sizeof(cmd) - n, " --%s %s", o->key, nth(o->choices, g_randoVals[i], buf, sizeof(buf)));
  }

  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE rd, wr;
  if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nul;
  SetEnvironmentVariableA("PYTHONIOENCODING", "utf-8");
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
  CloseHandle(wr);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) { CloseHandle(rd); log_line("Could not start the randomizer."); return false; }
  CloseHandle(pi.hThread);
  s_proc = pi.hProcess;
  s_pipe = rd;
  s_runningGen = true;
  app_log("rando: %s", cmd);
  log_line("Starting the randomizer...");
  return true;
#else
  (void)seedName; (void)seedNumber;
  return false;
#endif
}

#ifdef _WIN32
static void pump_output(void) {
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(s_pipe, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
    char buf[512];
    DWORD got = 0;
    if (!ReadFile(s_pipe, buf, sizeof(buf) - 1, &got, NULL) || got == 0) break;
    for (DWORD i = 0; i < got; i++) {
      char c = buf[i];
      size_t l = strlen(s_partial);
      if (c == '\n' || c == '\r') { log_line(s_partial); s_partial[0] = 0; }
      else if (l < sizeof(s_partial) - 1 && (unsigned char)c >= 32 && (unsigned char)c < 127) { s_partial[l] = c; s_partial[l + 1] = 0; }
    }
  }
}
#endif

// 0 = still running, 1 = seed ready (see rando_output_path), -1 = failed
int rando_poll(void) {
#ifdef _WIN32
  if (!s_runningGen) return s_result;
  pump_output();
  DWORD code = STILL_ACTIVE;
  if (WaitForSingleObject(s_proc, 0) != WAIT_OBJECT_0) return 0;
  GetExitCodeProcess(s_proc, &code);
  pump_output();
  if (s_partial[0]) { log_line(s_partial); s_partial[0] = 0; }
  CloseHandle(s_proc); CloseHandle(s_pipe);
  s_runningGen = false;
  s_result = -1;
  // collect the results
  char seedsDir[600];
  app_path(seedsDir, sizeof(seedsDir), "seeds/");
  CreateDirectoryA(seedsDir, NULL);
  DIR *d = opendir(s_workDir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot || !strcmp(e->d_name, "base.sfc")) continue;
      char src[900], dst[900];
      snprintf(src, sizeof(src), "%s%s", s_workDir, e->d_name);
      if (!strcasecmp(dot, ".sfc")) {
        snprintf(dst, sizeof(dst), "%s%s.sfc", seedsDir, s_seedName);
        uint8_t *data; int len;
        if (file_read(src, &data, &len)) {
          if (file_write(dst, data, len)) { snprintf(s_outPath, sizeof(s_outPath), "%s", dst); s_result = 1; }
          free(data);
        }
      }
    }
    closedir(d);
  }
  if (s_result == 1 && (d = opendir(s_workDir)) != NULL) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      const char *dot = strrchr(e->d_name, '.');
      if (!dot || strcasecmp(dot, ".txt") != 0 || !strstr(e->d_name, "poiler")) continue;
      char src[900], dst[900];
      snprintf(src, sizeof(src), "%s%s", s_workDir, e->d_name);
      snprintf(dst, sizeof(dst), "%s%s_spoiler.txt", seedsDir, s_seedName);
      uint8_t *data; int len;
      if (file_read(src, &data, &len)) { file_write(dst, data, len); free(data); }
    }
    closedir(d);
  }
  clean_dir(s_workDir);
  if (s_result != 1) {
    char m[64];
    snprintf(m, sizeof(m), "The randomizer stopped with an error (code %lu).", (unsigned long)code);
    log_line(m);
  }
  return s_result;
#else
  return -1;
#endif
}

void rando_cancel(void) {
#ifdef _WIN32
  if (!s_runningGen) return;
  TerminateProcess(s_proc, 1);
  WaitForSingleObject(s_proc, 2000);
  CloseHandle(s_proc); CloseHandle(s_pipe);
  s_runningGen = false;
  s_result = -1;
  clean_dir(s_workDir);
#endif
}
