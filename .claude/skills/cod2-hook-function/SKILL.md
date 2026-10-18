---
name: cod2-hook-function
description: How CoD2x hooks or reimplements a function of CoD2MP_s.exe, the renderer DLL or the Linux server binary - first mapping the function in the Binary Ninja databases (Windows exe, gfx DLL, Linux server, Mac reference build - real name, register params, types), then writing a plain C/C++ hook in the repo's style, gated by g_cod2x when it changes game logic shared with the client. Use whenever adding or changing a hook, replacing/reimplementing an engine function, calling an engine function from our code, using the Binary Ninja MCP tools, or naming things in a Binary Ninja database.
---

# Hooking a CoD2 engine function

CoD2x code reads like normal C/C++: no naked functions, no top-level asm, no jumps into the middle
of engine functions. Engine code is reached only through `patch_call` / `patch_jump` and the
`ASM(...)` / `ASM_CALL(...)` helpers from `src/shared/assembly.h`.

Every task has two phases:
1. **Map the function in Binary Ninja** - real name, clean prototype, named callees and globals.
2. **Write the code** - with exactly the same names and prototypes as the database.

---

## Phase 1 - Map the function in Binary Ninja

### Databases
All four are 32-bit x86, CoD2 1.3, under `D:/CoD2x-Decompilation/bin/`:

| File | Binary | Role |
| --- | --- | --- |
| `windows/CoD2MP_s.exe.bndb` | Windows client + listen server, base `0x400000` | **Maintained.** Target of `src/mss32` and the Windows half of `src/shared`. No symbols - every name is ours. Register arguments (`@ eax`), many inlined helpers. |
| `windows/gfx_d3d_mp_x86_s.dll.bndb` | Windows renderer DLL, base `0x10000000` | **Maintained** for what we touch. Reached only through the exe's `gfx_funcs` table (`0x0068a1e8`). |
| `linux/cod2_lnxded.bndb` | Linux dedicated server, base `0x8048000` | **Maintained.** Target of the Linux half of `src/shared`. Original C symbols for most functions, all arguments on the stack - the easiest place to read a shared function and its argument order. |
| `mac/Call of Duty 2 Multiplayer 1.3.bndb` | Mac client, base `0x1000` | **Reference only, never patched.** Real C++ symbols with argument types - the best source of names, types and struct names. Static functions have no symbol; the IDA dump `src/other/Call_of_Duty_2_Multiplayer_MAC_1.3.c` (search `Name/*DEFINITION*/`) names some of them. Addresses, registers and SSE code are Mac specific. |

### Before you start: are the right databases open?
Call `bn_open_item_list` before the first Binary Ninja step and check the task's needs:

| Task | Windows exe | Linux server | gfx DLL | Mac |
| --- | --- | --- | --- | --- |
| Client-only code (`src/mss32`: HUD, cgame, console, UI) | required | - | if it touches rendering | recommended |
| Renderer (`gfx_funcs` replacement, text / drawing) | required | - | required | recommended |
| Shared code (`src/shared`: server, game, bg_* / animations) | required | required | - | recommended |
| Linux-only server code (`src/linux`) | - | required | - | recommended |
| Naming / looking up one function | that binary | - | - | - |

- **Required database missing** - stop. Do not continue with guesses, pseudo-C dumps or other
  databases. Tell the user the full path to open and wait for confirmation.
- **Recommended (Mac) missing** - ask the user to open it and say what is lost (real names and
  argument types). Continue only if they decline.

The MCP server works on one **active view**. A database opened in the Binary Ninja UI is not active
automatically: `bn_binary_view_list`, then `bn_binary_view_set_active` with the PE / ELF / Mach-O
view (not Raw / Mapped / Universal). Switch back to the Windows exe when done.

### Goal
Not a fully reversed game - **every function and global our code touches is mapped** in the
Windows and Linux databases (the two we ship): real name, correct prototype, meaningful argument
and variable names. Leave unrelated code alone.

### Steps
1. **Find the function** - from a call site, a string (`bn_string_list`), a dvar name or the xrefs
   of a named function. Read **both** `bn_function_decompile` and `bn_function_disassembly`: the
   decompiler hides register arguments, x87 compares and signedness (`movzx`/`jbe` vs `movsx`/`jle`).

2. **Compare the platforms** - find the same function in Linux (same strings, same callees) and in
   Mac. Linux gives the argument order and often the name, Mac the name and the types. Differences
   usually mean an inlined helper or a compiler quirk, not different logic.

3. **Find the real name** (best source first):
   - Mac symbols (database or IDA dump), then Linux symbols.
   - Quake 3 / Enemy Territory (id-Software/Enemy-Territory, ioq3) and CoD4 - many functions are
     near copies (`Conbuf_AppendText`, `Cmd_TokenizeStringInternal`).
   - `src/other/CoD2MP_s.c` (old IDA decompile) and `src/other/*.bndb_pseudo_c.txt` - snapshots,
     the live database wins.

4. **Set the prototype** (`bn_function_prototype_set`) in Windows **and** Linux, so the decompile
   reads like source:
   - Drop `__convention("regparm")` / `__fastcall` / `__stdcall` guesses - keep default `cdecl` and
     mark register arguments with `@ reg`.
   - Real argument names and types (`struct lerpFrame*`), in source order, stack arguments in push
     order.
   - Registers come from the disassembly: what callers load right before the `call`, and which of
     those the function reads before writing.
   ```
   00468b10    uint32_t Conbuf_AppendText(char* msg @ ecx)
   00421d30    int32_t FS_BuildOSPath(char* ospath @ eax, char* qpath @ ecx, void* game @ edx, char* base)
   004210d0    int32_t Cmd_TokenizeStringInternal(char* text_in @ ecx, int32_t max_tokens, char** argv, char* text_out @ eax)
   004f9850    void BG_SetNewAnimation(struct lerpFrame* lf @ ebx, struct clientInfo* ci, void* newAnimationNumber, int32_t isComplete)
   ```

5. **Name what the code will use** - callees (`bn_symbol_define` / `bn_symbol_rename`,
   `FunctionSymbol`), globals (`DataSymbol`), locals (`bn_variable_rename`, `bn_variable_set_type`).
   Use the names the C code uses. Do not overwrite a user's name unless the evidence is better;
   when a name turns out wrong, rename it in the databases and the code together.

6. **Structures** - create one (`bn_type_struct_create` / `bn_type_parse`) when the original source
   clearly had it (`entityState_s`, `clientInfo_t`, `dvar_t`) and it helps the decompile. Unused
   fields can stay `field_XX` / padding.

7. **Comments** (`bn_comment_set`) for non-obvious facts: an inlined helper, a vanilla bug, a CoD2x
   patch at that address.

8. **Save** (`bn_open_item_save`) after a batch of changes; run `bn_analysis_update_and_wait` after
   mutations made with analysis updates skipped.

---

## Phase 2 - Write the code

### Names say what is original
An engine name in our code is a promise: original name, original logic.

| Kind | Name | Body |
| --- | --- | --- |
| **Reimplemented engine function** | Real name (Mac first, then Quake / ET / CoD4) - `CL_AddDeathMessageText`, not a made-up `CL_AddConsoleText` | Original logic 1:1 - same checks, same order, same loops. Inlined engine helpers become small functions with their real names (`ColorIndex`, `SEH_ReadCharFromString`) and a comment that they are inlined in the Windows build. |
| **Hook that wraps the original** (call-site `patch_call`, gfx slot) | The engine name of the **hooked function** - `CL_GetServerIPAddress` hooked in `CG_DrawScoreboard`, `I_CleanStr` hooked in `G_Say` | CoD2x block first, then the call of the original. Never glue caller and callee into one name (`CG_DrawScoreboard_CL_GetServerIPAddress`). |
| **CoD2x helper** (new logic) | Module prefix - `Text_`, `TextStyle_`, `Demo_`, ... | Anything. Never an engine-looking prefix (`Con_`, `CL_`, `CG_`, `R_`, `SV_`, `G_`). A hook that does more than wrap the original puts that logic into a helper. |

If the real name is unknown, use the Binary Ninja name and say so in the header comment; rename in
both places once the real name is found.

### Header comment and CoD2x blocks
Above every hook or reimplementation, the Binary Ninja declaration line (8-digit lowercase address,
4 spaces, cleaned prototype). Renderer functions add `(gfx)`:
```cpp
// 00468b10    uint32_t Conbuf_AppendText(char* msg @ ecx)
// 1000c030 (gfx)    char* R_DrawText(char* text, int32_t maxChars, void* font, ...)
```
Write the body as normal C with a Quake-style signature. If the Quake / ET source matches, start
from it (keep its comments and formatting); otherwise port the cleaned decompile - named variables,
loops, no `goto` unless needed.

Every non-original line is inside a block - no end-of-line `// CoD2x` tags, no unmarked small changes.
Removed original code stays inside the block, commented out:
```cpp
// CoD2x: short reason
...changed code...
// CoD2x: End
```

### Register arguments: `_Win32` / `_Linux` entry points
Windows functions take some arguments in registers. A thin `_Win32` wrapper reads them first with
`ASM( movr, var, "reg" )` and calls the clean function; stack arguments stay normal parameters:
```cpp
// 004210d0    int32_t Cmd_TokenizeStringInternal(char* text_in @ ecx, int32_t max_tokens, char** argv, char* text_out @ eax)
int Cmd_TokenizeStringInternal_Win32(int32_t max_tokens, char** argv) {
    char* text_in;
    char* text_out;
    ASM( movr, text_in, "ecx" );
    ASM( movr, text_out, "eax" );

    return Cmd_TokenizeStringInternal(text_in, max_tokens, argv, text_out);
}
```
Shared code (`src/shared`) adds `Name_Linux(...)` with all arguments on the stack when the clean
function's signature does not fit, and picks per platform with `ADDR(win, linux)` / `WL(win, linux)`.

### Calling engine functions: `inline` wrappers in `cod2_*.h`
Every engine function our code calls is wrapped **once**, in the header of its subsystem - never in
a module `.cpp`:

| Header | Functions |
| --- | --- |
| `src/shared/cod2_file.h` | `FS_*` |
| `src/shared/cod2_server.h`, `cod2_script.h` | `SV_*`, `Scr_*` |
| `src/shared/cod2_dvars.h`, `cod2_cmd.h`, `cod2_net.h`, `cod2_player.h` | dvars, commands, network, player |
| `src/shared/cod2_shared.h` | `Info_*`, `Q_*` and other shared utilities |
| `src/shared/cod2_client.h` | `CL_*`, `CG_*`, `Con_*` of the Windows client |
| `src/mss32/drawing.h` | `UI_*` / drawing (needs `fontHandle_t`, `materialHandle_t`) |

Rules for a wrapper:
- **Exact engine name** and the clean argument list - calling it reads like calling the original.
- Shared functions use `ADDR(win, linux)`. When the conventions differ, `WL(...)` picks the stack
  count and register / `PUSH` per argument, or the whole `ASM_CALL`.
- Above it a **description of what the function does**, read from the decompile - not guessed from
  the name: what it returns, what the arguments mean (enum values, units), side effects. Under it the
  Binary Ninja declaration line (address + registers). A bare declaration line is not enough.
  Use the header's comment style (`//` in `cod2_*.h`, `/** ... */` in `drawing.h`).
```cpp
// Read file in path
inline int FS_ReadFile(const char* path, void** buffer) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x00423240, 0x080a0a9c), 2, PUSH(path), PUSH(buffer));
    return ret;
}

// Delete file in location fs_homepath + fs_game + filename (Windows: filename @ eax)
inline bool FS_Delete(const char *filename) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x00422e00, 0x080a03dc), WL(0, 1), WL(EAX, PUSH)(filename));
    return ret;
}

// Puts the current console line into the on-screen message window of the channel:
// 0 = mini console, 1 = game messages, 2 = bold game messages, 3 = subtitles (other channels are ignored).
// lineFeed is 1 when the line is finished, duration is how long the line stays on screen.
// 004042c0    void Con_UpdateNotifyLine(int32_t lineFeed @ eax, int32_t channel @ ecx, int32_t duration @ edx)
inline void Con_UpdateNotifyLine(int32_t lineFeed, int32_t channel, int32_t duration) {
    ASM_CALL(RETURN_VOID, 0x004042c0, 0, EAX(lineFeed), ECX(channel), EDX(duration));
}
```
`ASM_CALL(ret, address, stackArgCount, args...)`:
- `ret` - `RETURN(var)` or `RETURN_VOID`; `ASM_CALL_RETURN(type, ...)` for one-line wrappers.
- `args` - `EAX(x)`, `ECX(x)`, `EDX(x)`, `EBX(x)`, `ESI(x)`, `EDI(x)`, `PUSH(x)` in source order
  (the macro pushes in reverse).

### Calling the original from a hook
`patch_call` replaces only the `call` instructions - the original function stays intact at its
address. A hook therefore calls it **directly by address**, under `// Call the original function`.
No saved function pointer, and no `cod2_*.h` wrapper of the same name (the hook owns the name):
```cpp
// 0044a8d0    void I_CleanStr(char* string @ eax)
void I_CleanStr(char* name)
{
    // CoD2x: ...
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, ADDR(0x0044a8d0, 0x080b7e54), WL(0, 1), WL(EAX, PUSH)(name));
}
```
Same pattern: `SV_Shutdown`, `Sys_ListFiles`, `CL_ServerInfoPacket`, `CL_InitDownloads`,
`CL_GetServerIPAddress`, `SEH_LocalizeTextMessage`.

Exceptions:
- `patch_jump` overwrites the start of the function - the original can not be called, so it must be
  reimplemented.
- Renderer slots have no fixed address (the DLL is loaded at runtime) - they call the saved slot
  pointer, see below.

### Installing the hook
In the module's `*_patch()` (runs before the game starts):
- **Prefer `patch_call`** on every call site (`bn_function_callers` / `bn_function_xrefs_to` list
  them). Verify each site is an `E8` call. Comment the caller next to it.
- **`patch_jump`** on the function start only to replace the whole function for all callers.
- The target `patch_call` returns is kept only to **revert** a runtime patch
  (`animation_originalPointer`, see version gating) - not to call the original.

### Renderer functions (`gfx_funcs`)
The renderer DLL is reached only through `gfx_funcs`. The engine copies the table after the renderer
loads (game start, `vid_restart`), so the replacement goes into the slot from the module's `_frame()`
when the slot is not ours yet; the original pointer is saved (`text_gfx`) and `_unload()` puts it back
(hot reload). See `src/mss32/text.cpp`:
```cpp
#define gfx_funcs_R_DrawText (*((R_DrawText_t*)0x0068a31c))

// 1000c030 (gfx)    char* R_DrawText(char* text, int32_t maxChars, void* font, ...)
static void R_DrawText(const char* text, int maxChars, fontHandle_t* font, ...)
{
    // CoD2x: text with CoD2x codes is drawn by CoD2x
    if (Text_HasCoD2xCodes(text)) {
        Text_Draw(text, maxChars, font, ...);
        return;
    }
    // CoD2x: End

    text_gfx.R_DrawText(text, maxChars, font, ...);
}
```
Check the real signature in the gfx database first - neighbouring slots can differ
(`R_AddCmdDrawTextInSpace` takes vectors, not `x, y`).

### Engine globals
```cpp
#define cl_consoleBufferPos   (*((uint32_t*)0x00601788))
#define sv_cheats             (*((dvar_t**)0x00c5c5cc))
```

---

## Version gating with `g_cod2x` (shared game logic)

A CoD2x client must play correctly on vanilla 1.3 servers and on older CoD2x servers. Code that runs
on **both** sides - `bg_*` movement and animations (`src/shared/animation.cpp`), pmove, anything the
client predicts - must behave like the connected server, or prediction and the player model desync.

**How `g_cod2x` works**
- An int dvar `0 .. APP_VERSION_PROTOCOL` (`src/shared/game.cpp`), `DVAR_SYSTEMINFO`, read-only in
  release builds. Value = **protocol of the server**: `0` = vanilla 1.3 (or CoD2x older than 1.4.5.1),
  `N` = CoD2x 1.4.N.x.
- Server: a dedicated / Linux server forces it to `APP_VERSION_PROTOCOL` (`SV_SpawnServer`) and sends it
  in `SV_ClientBegin` (`SV_SetClientCvar`). As systeminfo it reaches the client early while connecting
  (`iwd.cpp` uses it to pick CoD2x IWDs).
- Client: `cgame_frame` (`src/mss32/cgame.cpp`) resets it to `0` on disconnect and, on change, prints
  the mode and switches runtime patches (`animation_changeFix(g_cod2x >= 3)`).

**Rules**
- Gate every behavior change of shared logic with the protocol that introduced it; keep the vanilla
  code in the other branch. New changes use the current `APP_VERSION_PROTOCOL` (makefile
  `VERSION_PROTOCOL`); a change removed later gets a range (`>= 3 && < 4`, prone legs in
  `BG_PlayerAngles`).
  ```cpp
  // CoD2x: Set speed to 1 to perfectly match player angles with the character
  if (g_cod2x->value.integer >= 3) {
      swingSpeed = 1.0;
  }
  // CoD2x: End
  ```
- Byte patches of shared code that follow the server are applied **and reverted** at runtime (the
  client changes servers without restarting): save with `patch_nop(addr, n, saved)` / `patch_call`,
  restore with `patch_copy` / `patch_call` - see `animation_changeFix`.
- Shared hooks need `_Win32` + `_Linux` entry points and `ADDR(win, linux)` call sites
  (`BG_PlayerAngles_Win32` reads `ci` from `edi`, `BG_PlayerAngles_Linux` gets it on the stack). Code
  running on both sides of a listen server keeps per-side state (`animation_isServer()`,
  `animation_getIndexForPlayerData`).
- Not needed for pure client features (HUD, text, console) or server-only code (commands, GSC, network).

---

## Checklist
- [ ] Required Binary Ninja databases open (stopped and asked if not)
- [ ] Windows + Linux databases (compared with Mac): real name, clean prototype (`@ reg`, types, argument
      names), callees / globals / used variables named, database saved
- [ ] Names: engine name only on original logic or a wrapping hook (named after the hooked function);
      new logic in module-prefixed helpers
- [ ] Binary Ninja declaration line above every hook; all changes inside `// CoD2x:` ... `// CoD2x: End`
- [ ] Clean C function + `_Win32` (and `_Linux` if shared) entry point reading registers first
- [ ] Engine calls through `inline` wrappers in `cod2_*.h` / `drawing.h` (never in a `.cpp`), each with a
      description from the decompile above the declaration line
- [ ] Hook calls its original by address (`// Call the original function` + `ASM_CALL`)
- [ ] `patch_call` on every call site (or `patch_jump` with a reason)
- [ ] Shared game logic gated with `g_cod2x`, runtime patches revertible
- [ ] Builds without warnings; behavior checked in game
