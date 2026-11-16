---
name: cod2-game-control
description: Drive a debug CoD2MP_s.exe that Claude started, through its control pipe with tools/game_pipe.py - run console commands at once and get their output back, read the client state, dvars and the last console lines, wait for a state or a log line, and play the client - aim at players' body parts, look, move, fire, press keys, type into chat / console / menu fields and click menu items. Use for automated in-game tests instead of command-line `+wait` chains, keystrokes or window focus. Never for a game Claude did not start.
---

# Controlling a debug game through its pipe

Debug builds of CoD2x (`#if DEBUG`) listen on the named pipe `\\.\pipe\cod2x-<pid>` (`src/mss32/debug_pipe.cpp`). A test
script sends one request line, the game runs it on the main thread at the start of the next frame and replies. The
pipe is polled without waiting, so the game never blocks on the script. Release builds have no pipe and no test commands.

Playing the client is done with normal console commands of `src/mss32/debug_input.cpp` (debug builds, client only):
the pipe just runs them like `connect`. They go through the engine's own input paths - button commands (`+attack`),
queued key / character events (`Sys_QueEvent`, like Windows input), the menu cursor and the view angles - so nothing
needs the window focus.

Why not `+wait N` chains: zPAM sets `g_competitive 1` and CoD2x then blocks `wait` once connected; a pending `wait`
also holds back commands that menus queue themselves (zPAM's moddownload answer then misses its 3 s check), and frame
counting is fragile. A pipe command runs when the test needs it and returns its result.

## When to use it
- Automated tests on a game **Claude started** (client, listen or Windows dedicated server from `bin/windows`).
- Never another game: not one the user started, not one of another worktree or session. The pid in the pipe name makes
  sure a request reaches exactly the game you started - always pass the pid you got from `Start-Process -PassThru`.

## Starting a game and finding the pipe
```powershell
$sp = "<scratchpad>"                                   # back up the profile config first, restore it after the run
Copy-Item bin\windows\main\players\default\config_mp.cfg "$sp\config_mp.cfg.bak"
$c = Start-Process -FilePath (Resolve-Path bin\windows\CoD2MP_s.exe) -ArgumentList "+set r_fullscreen 0" `
     -WorkingDirectory (Resolve-Path bin\windows) -PassThru
python tools/game_pipe.py --pid $c.Id --connect-timeout 60 !ping          # waits until the game is up -> "pong"
```
The game prints `Control pipe: \\.\pipe\cod2x-<pid>` at startup (in `!log`). Running games:
`Get-Process CoD2MP_s | Select Id, StartTime, Path` - only use the ones you started.

Players to shoot at: the test mod `test_killfeed` (`tools/test/select_test_mod.ps1 -Mod test_killfeed`, then start with
`+set fs_game mods/test +exec test.cfg +set scr_test_auto 0 +set scr_test_bots 2`) is a listen server with bots,
`g_debugDamage 1` (every hit is logged as `PlayerDamage: ... hitLoc:head`) and cheats (`setviewpos`, `give ammo`).
`scr_test_lineup 1` puts the bots in front of you again.

## Pipe requests
| Request | Reply |
| --- | --- |
| `<console command>` | runs at once with `Cmd_ExecuteString` (does not wait behind a `wait` in the command buffer); everything printed while it runs |
| `!ping` | `pong` |
| `!state` | `clientState=8 clientStateName=ACTIVE cl_ingame=1 server=127.0.0.1:28975 mapname= g_cod2x=7 g_competitive=1 demo_isPlaying=0 loc_language=0` |
| `!dvar <name>` | `name=`, `value=`, `type=`, `flags=0x.... NAMES`, `latched=`, `default=` lines; error if the dvar does not exist |
| `!log <n>` | last n of ~500 console lines, color codes removed; includes lines written only to console_mp.log (`      dvar set ...`) |

Wire format: one UTF-8 line per request; reply = output lines + terminator line `\x1e` `ok` or `\x1e` `error <message>`.
The test commands below make the reply `error` (tool exit code 1) when they fail; engine commands only print errors.

## Test commands (debug_input.cpp)
| Command | What it does |
| --- | --- |
| `players` | id, team, health, distance, alive / dead and name of every player (`(you)` = this client) |
| `bones <id\|name>` | body parts that can be aimed at and their bones / positions |
| `aim <id\|name> [part\|bone] [ms\|hold]` | turns the view to the body part (default `head`); with ms / `hold` follows it each frame (`aim off`). Prints the angles and the **line of fire**: `clear`, `blocked by player <id> <name>` or `blocked by the world` |
| `look <pitch> <yaw>` / `turn <pitch> <yaw>` | absolute world view angles (pitch < 0 = up) / relative turn (yaw > 0 = left) |
| `press <+button> [ms=500]` | holds a button: `+attack`, `+forward`, `+back`, `+moveleft`, `+moveright`, `+gostand` (jump), `+movedown`, `+prone`, `+melee`, `+reload`, `+frag`, `+smoke`, `+activate`, `+leanleft`, `+leanright`, `+holdbreath`, ... |
| `move <forward\|back\|left\|right\|jump\|crouch\|prone> [ms=500]`, `fire [ms=50]` | short forms of `press` |
| `key <keyname> [ms=0]` | presses a key like the keyboard / mouse (runs binds, works in menus, console, chat): `ENTER`, `ESCAPE`, `TAB`, `SPACE`, `BACKSPACE`, `MOUSE1`, `UPARROW`, `F1`, `a`, ... (the names of `bind`) |
| `type <text>` | types the text as characters into the field that has the input (chat, console, menu edit field) |
| `menus [all]` | open menus (top first) and their shown items: cursor position of the center (640x480), item name, text, dvar |
| `click <item>` / `click <x> <y>` `[keyname=MOUSE1]` | moves the menu cursor to the item (name or text, exact first, then a part of the text) or x y, and clicks |
| `release` | releases every held button / key and stops aiming |

Body parts: `head` (5 units over `j_head`, the joint at the bottom of the head - a shot at the joint itself hits the
neck), `neck`, `chest`, `stomach`, `pelvis`, `shoulder_l/r`, `hand_l/r`, `knee_l/r`, `foot_l/r`, or any bone name.

Also useful engine commands: `chatmodepublic` / `chatmodeteam` (open the chat field), `toggleads` / `leaveads` (aim down
the sights), `viewpos` (own position and yaw), `setviewpos x y z yaw` (cheats), `give ammo` (cheats), `cmdlist <filter>`.

## Examples
```powershell
# Shoot a bot in the head - aim down the sights, hip fire of rifles has a large spread; check the server's damage log
python tools/game_pipe.py --pid $C "aim bot0 head" toggleads; Start-Sleep 1
python tools/game_pipe.py --pid $C "aim bot0 head" fire; Start-Sleep 0.3; python tools/game_pipe.py --pid $C leaveads
python tools/game_pipe.py --pid $C --wait-log "PlayerDamage: .* -> #\d+ bot0 .*hitLoc:head" --timeout 5
# Strafe while following the target, then stop
python tools/game_pipe.py --pid $C "aim bot1 chest hold" "move right 1500"; ...; python tools/game_pipe.py --pid $C "aim off"
# Chat
python tools/game_pipe.py --pid $C chatmodepublic; python tools/game_pipe.py --pid $C "type hello" "key ENTER"
# Menu
python tools/game_pipe.py --pid $C "key ESCAPE"; python tools/game_pipe.py --pid $C menus "click MENU_BACKTOGAME"
# Raw buttons (requests may start with '-')
python tools/game_pipe.py --pid $C +forward; Start-Sleep 0.3; python tools/game_pipe.py --pid $C -forward
```
Keys, characters and clicks are queued and handled at the start of the next frame - send the next step as a separate
call (or after a short sleep) when it depends on the result (`chatmodepublic`, then `type`; `key ESCAPE`, then `click`).

### Client on a local zPAM server
```powershell
python tools/game_pipe.py --pid $c.Id "connect 127.0.0.1:28975"
python tools/game_pipe.py --pid $c.Id --wait-state clientState=active --timeout 90
python tools/game_pipe.py --pid $c.Id --wait-log "dvar set server16" --timeout 30    # needs developer 1 on the client
python tools/game_pipe.py --pid $c.Id "openscriptmenu serverinfo_dm close"
python tools/game_pipe.py --pid $c.Id "openscriptmenu team_britishgerman spectator"
python tools/game_pipe.py --pid $s.Id --wait-log "menuresponse\(team_britishgerman, spectator\)" --timeout 10
```
Test server (zPAM): its own `fs_basepath` / `fs_homepath` whose `main` holds only the original `iw_*.iwd` (hardlinks
are fine), one zPAM iwd, `zpam_maps_v7.iwd`, `players/active.txt` + a profile folder, and `_callback_tests.gsc` /
`_callback_match.gsc` copied from `bin/windows/main`; `+set dedicated 1 +set developer_script 1 +set logfile 2
+set sv_wwwDownload 1 +set sv_wwwBaseURL ...` on a free UDP port (`Get-NetUDPEndpoint`; other sessions use 28970).
Start it from its own exe folder (hardlinks of `bin/windows` + its own `mss32.dll` copy) if the client may hot reload
- two games hot reloading from one folder race for `mss32_hotreload_N.dll` and one stops in an error box.

## tools/game_pipe.py options
- `--connect-timeout` (default 5 s) retries opening the pipe (game starting, hot reload, pipe busy - it has one instance).
- `--reply-timeout` (default 30 s) per request; a game that hangs or crashed into the crash dialog gives exit 1, never a
  hung script. A long `map` load needs more (`--reply-timeout 60`).
- `--wait-state KEY=VALUE` (repeatable, case-insensitive; `clientState=active` matches the name, `=8` the number),
  `--wait-log REGEX`, `--timeout`, `--interval`.
- Exit codes chain in PowerShell: `python tools/game_pipe.py ...; if ($LASTEXITCODE) { ... }` - 0 ok, 1 error / timeout,
  **2 the game crashed** (or its process is gone).
- The wait loops release the pipe between polls, so another script can send requests while one waits.

## Crashes
A crash never makes a test wait for its timeout:
- The crash handler (debug builds) sends the pipe client, before the crash dialog, the output of the running command and
  `error crash: ACCESS_VIOLATION at 0x7A5E7EFE (mss32.dll+0x7EFE): Write to address 0x00000000 ..., report <file>`.
  A connected client that is not waiting gets it as the next reply.
- Later requests: the tool sees the crash dialog of the pid ("Call of Duty 2 - Application Crash") and names the newest
  `CoD2MP_s.exe.crash.*.txt`; a process that is gone (killed, crash without dialog) is reported too. All exit with 2.
- `debug_crash [ms]` (debug builds) crashes the game on purpose - at once, or ms later in a frame (outside any request).
- After a crash: read the report, inspect with `cod2-game-inspect` if needed, then stop the process (the dialog waits).

### Listen server with zPAM / test scripts
`+set g_gametype strat +devmap mp_carentan_fix` works on a listen server (the map command loads the listen server
iwds first, `iwd_beforeMapCommand`), and map changes while connected keep loose scripts like
`main/_callback_tests.gsc` (`SV_SpawnServer_FS_Restart`, iwd.cpp). The
listen server loads from `main` only the original, CoD2x and newest zPAM iwds - put test scripts as loose files in
`main` (or in an `fs_game` folder), a test iwd in `main` is ignored. `+set fs_homepath <scratchpad>\home` keeps the
config and logs out of `bin/windows`.

## Notes
- `line of fire: clear` is a good sign, not a guarantee - the client trace uses player bounding boxes and the server may
  still hit thin geometry first (with `sv_wallbang 1` the hit then has `iDFlags:96` and little damage).
- A command that ends in `Com_Error` (ERR_DROP) should reply with the output so far and `error command aborted by
  Com_Error` (by design - not exercised in a test yet). `quit` gets no reply (exit code 2, the process is gone).
- Commands that only queue (`Cbuf_AddText`) run on a later frame - read their output with `!log` / `--wait-log`.
- Moving the real mouse over a focused windowed game moves the menu cursor too (window.cpp) - keep the game unfocused.

## Rules
- Never focus the game window, send it keystrokes or mouse input - everything goes through the pipe.
- Back up `bin/windows/main/players/default/config_mp.cfg` before a run and restore it after (the game rewrites it).
- Stop only processes you started (`Stop-Process -Id <your pid>`); leave every other game alone.
- Hot reload (`reload` or a new `mss32.dll`) closes the pipe and opens it again with the same name - the tool reconnects.
  Known engine issue (also without the pipe): after a hot reload the next cgame load (reconnect, map change) crashes in
  `UI_LoadMenus` (0x0054718A) - restart the game instead of reloading when the test reconnects.
- A frozen or crashed game: inspect it with the `cod2-game-inspect` skill before stopping it.
