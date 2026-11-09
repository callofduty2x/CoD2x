# Test mods

Small mods for testing CoD2x features in the game. Each folder is one mod. A mod is a zPAM gametype: it runs on top
of the zPAM and map pack IWDs in `main` and adds only its own gametype script.

| Mod | Tests |
|---|---|
| `test_killfeed` | the killfeed: extended GSC `obituary` with icons, `cg_gameMessage*` cvars |
| `test_language` | translations per player (`.str` with all languages), GSC `getLanguage()`, font sets `cl_fontSet` |

## Run

In VS Code: launch configuration `Win tests`. It builds `mss32.dll`, then asks in the terminal which test mod to run:

```
Test mods in tests/mods:
  *  1)  test_killfeed            Killfeed: obituary icons, cg_gameMessage* cvars - bots, auto-run cases

Number or name (Enter = test_killfeed):
```

and starts the game with the debugger: `CoD2MP_s.exe +set fs_game mods/test +exec test.cfg`.

Without VS Code (no debugger):

```
powershell tools/test/select_test_mod.ps1 -Run                     ask, then start the game
powershell tools/test/select_test_mod.ps1 -Mod test_killfeed -Run  no question
powershell tools/test/select_test_mod.ps1 -Mod test_killfeed -Run -Dedicated -GameArgs "+set scr_test_interval 1"
```

`select_test_mod.ps1` zips only the selected mod, each time it runs, into `bin/windows/mods/test/<name>.iwd`
(other IWDs there are deleted) and writes `bin/windows/mods/test/test.cfg` with `exec <name>.cfg`. Only one test mod
is loaded at a time, so test mods can replace the same files without affecting each other. The logs are in
`bin/windows/mods/test/`: `console_mp.log`, and `games_mp.log` with the test output.

Why `fs_game` and not a `zzz_` IWD in `main`: a listen server (a game client running `devmap`) loads from `main`
only the original IWDs and the latest zPAM and map pack (see `iwd.cpp`). Other IWDs in `main` are ignored.

## Adding a test mod

```
tests/mods/<name>/<name>.cfg                        first line: // description shown in the selection
tests/mods/<name>/maps/mp/gametypes/<name>.gsc      the gametype
tests/mods/<name>/maps/mp/gametypes/<name>.txt      "Gametype name"
```

The cfg sets up the server and starts the map (`set g_gametype <name>`, `devmap mp_toujane`). Use the strict
settings of `test_killfeed.cfg`: `developer 2`, `developer_script 1` (asserts, `/# #/` blocks, debug prints - a
script error stops the server with its call stack), `logfile 2`.

The gametype uses zPAM's systems - copy `initTestSystems()` and the `main()` of `test_killfeed.gsc`:

- `initTestSystems()` is zPAM's `InitSystems()` without `rules::Init()` (it knows only zPAM's gametypes and asserts
  for others) and without the installation check (it fails for a `main` folder with old zPAM IWDs and spawns
  everyone outside the map)
- `addEventListener(...)` for zPAM events, `level.onAfterConnected`, `level.onAfterPlayerDamaged`,
  `level.onAfterPlayerKilled`, `level.spawnSpectator` for the code callbacks
- only the zPAM modules the mod needs, e.g. `_bots` (adding bots), `_weapons` and `_damagefeedback` (used by zPAM's
  damage callback) - not `InitModules()` (readyup, menus, ...)

`println` prints only with `developer_script 1`; `logPrint` writes to `games_mp.log` always.

## test_killfeed

Gametype `test_killfeed`. You spawn right away with a scoped kar98k, M1 Garand and grenades. Six bots are added
and lined up in front of you. Every 3 seconds the next test case runs: a bot kills another bot (or itself, or falls)
and the case calls `obituary` with its own arguments. The case and the expected killfeed line are printed in the
center of the screen, to the console and to `games_mp.log`.

```
e4 4 icons
[springfield] [wallbang] [noscope] [headshot] [extra]
```

The game has no wallbang / no-scope icons, stock materials stand in for them (a real mod has its own):
`wallbang` = `hint_mantle`, `noscope` = `stance_stand`, `extra` = `objpoint_star`, `extra2` = `headicon_dead`.

Cases (`scr_test_list 1` prints them):

- `v1` - `v8` vanilla: no icons, the killfeed must look like with an old mod, also `MOD_HEAD_SHOT` shows only the
  headshot icon
- `e1` - `e18` extended: icons with the weapon, 4 icons, more than 4, undefined / empty icons, the weapon icon passed
  again, the same icon twice, melee, square icons, weapon without a kill icon, suicide, falling, you as the attacker,
  between normal game messages, all bots at once
- manual only (`scr_test_case <id>`): `m1` a bot kills you, `x1` / `x2` script errors (icon not precached, number
  instead of a shader name) - with `developer_script 1` the error stops the server, as it should

Your own kills use the real kill, as a mod would do it: wallbang (`sv_wallbang 1` in the cfg, flag 64 in `iDFlags`),
no-scope (sniper rifle with `playerADS() < 1`), headshot. `scr_test_real_icons 0` calls `obituary` without icons, as
an old mod.

Server cvars:

```
scr_test_auto 0              stop the cases (1 runs them)
scr_test_interval 3          seconds between the cases
scr_test_case e4             run one case now
scr_test_list 1              print the cases to the console
scr_test_bots 6              number of bots
scr_test_lineup 1            line up the bots in front of you again (after you moved, e.g. behind a wall)
scr_test_lineup_distance 256 distance of the bots
scr_test_real_icons 1        icons for your own kills
scr_test_onehit 1            your hits kill with one shot
scr_test_hint 0              no case text in the center of the screen
```

Client things to try while the cases run:

```
cg_gameMessageScale 1.5
cg_gameMessageOffsetX 200
cg_gameMessageOffsetY -100
cg_gameMessageWidth 20                          the victim goes to the next line
name "^1Very^2Long^3Player^4Name^5With^6Colors"  long names with "e16" (scr_test_case e16)
```

## test_language

Hud elements and messages with localized strings from `localizedstrings/test_language.str` (english, french, german,
czech, polish, russian in one file), plain strings chosen on the server with `getLanguage()`, font samples.

- The `.str` file is UTF-8. CoD2x converts it to the code page of the fonts of the player when it is loaded (cp1252
  western, cp1250 czech / polish, cp1251 russian, ...), the game itself has no Unicode.
- Other languages than the ones of the CoD2x language IWD need their localized IWDs in `main`
  (`localized_<language>_iw*.iwd`), the game does not allow a language without them (`loc_language` falls back to the language of the install).
- The languages and the CoD4 fonts are in the CoD2x language IWD (`main/localized_CoD2x_01.iwd`). Run it with
  `+set loc_language 14` (czech), `1` (french), `6` (russian), ... In the game `F5` = `cl_fontSet cod4`, `F6` = fonts of the game.
