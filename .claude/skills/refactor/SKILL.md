---
name: refactor
description: Refactor pass over the changed code - code review for bugs, simplification, cleanup, and splitting / moving code into properly separated modules that follow the CoD2x file layout and style. Run as /refactor (uncommitted changes), /refactor <path...>, or /refactor <commit or range>.
disable-model-invocation: true
argument-hint: "[path... | commit | range]  (default: uncommitted changes)"
---

# /refactor - review, simplify and modularize the changed code

Arguments: `$ARGUMENTS`

Rewrite the changed code into its proper final form: find bugs, simplify, clean up, and move code
into well separated modules in the repo's style. **Behavior stays the same** unless you find a bug
(bug fixes are listed separately in the report).

---

## 1. Scope

| Arguments | Scope |
| --- | --- |
| empty | Uncommitted changes: `git status --porcelain`, `git diff HEAD`, plus untracked files. If there are none, use the last commit (`git show HEAD`) and say so. |
| paths | Those files / folders. |
| commit or range (`HEAD~3..`, `abc123`) | Files changed in it (`git diff --stat <range>`). |

Refactor the **changed code and what it directly touches** (its header, callers, wiring in
`hook.cpp`, the `cod2_*.h` declarations it uses). Do not sweep unrelated parts of the repo - if you
see something worth doing elsewhere, list it under "Not done" in the report.

Never edit: `src/other/*` (reference dumps), generated `symbols_*.h`, `src/shared/version.h`,
`src/mss32/version.rc`, `src/shared/mongoose/*`, `tools/mingw`, `tools/openssl_mingw`.

## 2. Read

Read every file in scope **fully**, plus its `.h`, the callers of anything you plan to move
(`Grep`), and the `_patch` / `_init` / `_frame` wiring in `src/mss32/hook.cpp` and
`src/linux/hook.cpp`. Understand the code before touching it.

## 3. Review - four passes

1. **Correctness** - real bugs: buffer sizes / missing NUL, `snprintf` truncation, threads touching
   game state off the main thread, missing `#if COD2X_WIN32` on Windows-only code in `src/shared`,
   wrong `ADDR(win, linux)` pairs, client behavior that differs on old servers without a `g_cod2x`
   gate (see the `cod2-hook-function` skill), leaks of handles / allocations, init-order
   dependencies.
2. **Simplification** - dead code, leftover debug prints, duplicated blocks -> one helper, deep
   nesting -> early returns, needless flags / globals, over-generic code with one caller, comments
   that repeat the code or describe the history of the change.
3. **Reuse** - something the repo already has: `src/shared/common.*`, `shared.*`, `dvar.*`,
   `files.*`, `json.h`, `http_client.h`, `logger.*`, `cod2_*.h` engine declarations,
   `thread_setName` / `thread_isNotMain`, `SHOW_ERROR*`. Prefer the existing helper over a new one.
4. **Modularization** - the layout rules below. Does each file have one responsibility? Is code in
   the right folder? Are engine declarations in the right `cod2_*.h`?

## 4. Project layout and style (follow it exactly)

### Folders
| Folder | Built into | Contains |
| --- | --- | --- |
| `src/mss32/` | Windows client `mss32.dll` only | Client features: HUD, renderer, window, input, console, updater, crash handling. Uses `#include "shared.h"` (the mss32 one, pulls `windows.h`). |
| `src/shared/` | **both** Windows `mss32.dll` and Linux `libCoD2x.so` | Server / game logic, GSC, match, iwd, http. Platform splits via `#if COD2X_WIN32` / `#if COD2X_LINUX` and `ADDR(win, linux)` addresses. |
| `src/linux/` | Linux server only | Linux hook, exception, updater. |
| `src/shared/cod2_*.h` | both | Engine structs, enums, globals, dvars and engine function pointers (`cod2_client.h`, `cod2_player.h`, `cod2_dvars.h`, `cod2_menu.h`, `cod2_script.h`, ...). Engine declarations live **here**, never in a feature module. |

Code used by only the client belongs in `src/mss32`, even if it was written in `src/shared`.
Code that both builds need belongs in `src/shared`, not duplicated.

### Module = one feature = `name.cpp` + `name.h`
- File names lower snake case: `freeze.cpp`, `debug_pipe.cpp`, `gsc_player.cpp`.
- Public functions and globals use the module prefix: `freeze_frame()`, `freeze_lastHeartbeat`,
  `gsc_player_getip()`. Engine names (`CL_`, `R_`, `Con_`, `SV_`) only for real engine functions
  and their reimplementations.
- Lifecycle functions, wired in `src/mss32/hook.cpp` and/or `src/linux/hook.cpp` (order matters,
  keep the comments there):
  - `<module>_patch()` - memory patches before the entry point runs
  - `<module>_init()` - once after common init: dvars, commands
  - `<module>_frame()` - every frame
  - A new module must be added there; a removed one must be removed there.
- New files start from `src/_template.h.in` / `src/_template.cpp.in` (same as `make template`).
  The build globs the folders, so no CMake edit is needed.

### Header (`name.h`)
```cpp
#ifndef NAME_H
#define NAME_H

#include "../shared/cod2_script.h"   // only what the declarations need

void name_doThing(int clientNum);
void name_frame();
void name_init();
void name_patch();

#endif
```
Only the public API. No `static` functions, no file-local state, no `windows.h` unless a declared
type needs it.

### Source (`name.cpp`)
```cpp
#include "name.h"              // own header first

#include <windows.h>           // system headers
#include <stdio.h>

#include "shared.h"            // project: shared.h first, then others
#include "exception.h"
#include "../shared/cod2_dvars.h"

dvar_t* name_enabled = NULL;   // module state, prefixed; file-local helpers and state are static

/** One-line doc comment above each function. */
void name_frame() {
```
- `extern` declarations of another module's globals inside a `.cpp` are a smell - declare them in
  that module's header and include it.
- Engine function reimplementations keep the Binary Ninja header line
  (`// 004bb730    char const* stPbCl_LoadClientDll(void* pbcl @ eax)`) and mark changes with
  `// CoD2x: ...` / `// CoD2x: End`. Do not rename or "clean up" these - they mirror the database.
  Hook mechanics (`_Win32` wrappers, `ASM(...)`, `patch_call`) follow the `cod2-hook-function`
  skill; never introduce naked asm.
- Plain C-style C++ (char buffers, `snprintf`, structs); `std::string` / `std::vector` are fine
  where the file already uses them. Keep the file's existing indentation (most files 4 spaces,
  `gsc_*` files tabs).

### When to split a file
Split by **responsibility**, not by line count (a 1500-line file with one job is fine). Split when
a file mixes clearly separate jobs, e.g. data collection + UI + network sending.

- One feature in several files: `<feature>_<part>.cpp` with a shared `<feature>_internal.h`, which
  starts with a comment listing which file owns what. Model: `src/mss32/exception.cpp`,
  `exception_stack.cpp`, `exception_report.cpp`, `exception_dialog.cpp` + `exception_internal.h`.
  The public `<feature>.h` stays small; only `<feature>*.cpp` include `_internal.h`.
- A family of similar parts: shared prefix, e.g. `gsc_player.cpp`, `gsc_match.cpp`, `gsc_http.cpp`.
- A generic helper used by several modules goes to `src/shared/common.*` / `shared.*` (or its own
  small module), not copied.

## 5. Apply

1. Before editing, print a short plan: findings grouped as **Bugs**, **Simplify**, **Move / split**,
   one line each with `file:line`. Then do it - do not wait for confirmation unless a change would
   alter gameplay / network behavior or delete a feature.
2. Edit only with the Edit / Write tools (never sed / python / shell redirection). Moving code =
   Write the new file, then Edit the old one; keep comments with the code they describe.
3. Update all callers, includes, and the `hook.cpp` wiring after every move.
4. Do not commit.

## 6. Verify

- Windows (always): `tools/mingw/bin/mingw32-make build_win` with `tools/mingw/bin` on `PATH`
  (the VS Code "Build Win" task). Fix all new errors **and warnings**.
- Linux (when `src/shared` or `src/linux` changed):
  `wsl -d Ubuntu-18.04 --cd /mnt/d/CoD2x make build_linux` - name the distro explicitly, the
  default WSL distro may be another Ubuntu without cmake / 32-bit libraries. If it hangs or fails
  to start, report that Linux was not built instead of retrying.
- Re-read `git diff` once at the end: no behavior change hidden in a "cleanup", no lost comments,
  no stray debug code.

## 7. Report

Short, in this order:
- **Bugs fixed** - what was wrong, the scenario, `file:line`.
- **Moved / split** - old place -> new place, new files.
- **Simplified** - one line each.
- **Not done** - worth doing but out of scope or behavior-changing; ask about these.
- **Build** - Windows / Linux result.
