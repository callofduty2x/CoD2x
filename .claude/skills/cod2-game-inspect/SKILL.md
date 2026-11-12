---
name: cod2-game-inspect
description: Read-only snapshot of a running, hung or debugger-stopped CoD2MP_s.exe with tools/game_inspect.py - threads, registers, backtraces with CoD2 / CoD2x symbols (mss32.dll with file:line), code at EIP, memory dumps, console_mp.log tail. Use whenever the user says the game crashed, froze, hangs or is stopped in the VS Code debugger, asks to "inspect the game", "look at the game memory / threads / stack", or wants to know where the game is right now. Works while VS Code's gdb holds the process (Claude can not connect to that debugger).
---

# Inspecting a running game

Claude can not attach to the VS Code debug session: OpenDebugAD7 owns gdb's MI pipes and Windows allows one debugger
per process. `tools/game_inspect.py` reads the process instead - `ReadProcessMemory`, `VirtualQueryEx`,
`Wow64GetThreadContext` - without attaching or suspending it, so it is safe while the game is stopped in the debugger
or still running, and it never changes the game.

## When to run it
- The user reports a crash, freeze or hang, or the game is stopped in the VS Code debugger ("game crashed",
  "it's frozen", "debugger stopped somewhere").
- The user asks where the game is, what a thread is doing, or for a register / memory value.
- A game Claude started does not respond (before stopping it, see what it is doing).

Do not ask first - it is read-only. Only inspect a game process the user points to or the one of this worktree
(`bin/windows/CoD2MP_s.exe`); never stop or kill a game Claude did not start.

## Usage
From the repo root (64-bit Python, PowerShell):
```
python tools/game_inspect.py                       # the game of this repo (fails with a list if 0 or several run)
python tools/game_inspect.py --pid 1234            # another game (other worktree: its own bin\windows)
python tools/game_inspect.py --all-threads         # full analysis of every thread
python tools/game_inspect.py --mem eax:64 --mem 0x5dd6c8:32   # hex dumps - register of the main thread or address
python tools/game_inspect.py --stack 0x10000       # scan more stack (default 0x4000 bytes)
python tools/game_inspect.py --log 50              # more of console_mp.log (0 = none)
python tools/game_inspect.py --out <scratchpad>\inspect.txt    # also save the output
```
Find the pid: `Get-Process CoD2MP_s | Select Id, StartTime, Path`. A run takes a few seconds (nm / addr2line of
the debug mss32.dll).

## Output
- Process: path, "debugger attached: yes/no", non-system modules with address ranges.
- Every thread: EIP location and start function (`hotreload_watch_dll_thread`, `freeze_watchdogThreadProc`, the
  render thread in d3d9 ...). Full analysis for the main thread (first one) and every thread not waiting in
  ntdll / kernelbase / win32u:
  - registers, each with what it points to (symbol, string, memory words, "not readable");
  - memory state at EIP; `!! eip is outside of any module` = call / jump through a bad pointer, the caller is the
    return address at `[esp]`;
  - disassembly around EIP (objdump);
  - backtrace from the EBP chain, then a stack scan of return addresses (each checked to follow a `call`), which
    also finds frames without EBP - the scan may contain stale return addresses of older calls, trust the chain first.
- `--mem` hex dumps, tail of `bin/windows/main/console_mp.log`.

Symbols: `mss32.dll` (debug build) - nm + addr2line of the loaded DLL, `function+off  src/...:line`;
`CoD2MP_s.exe` - `src/mss32/symbols_cod2mp_s.h`; `gfx_d3d_mp_x86_s.dll` - `src/mss32/symbols_gfx_d3d_mp_x86_s.h`
(only named functions); other modules module+offset. Unknown exe / gfx addresses: look them up in Binary Ninja
(`bn_function_list` with `start`/`end` around the address finds the containing function).

## Reading the result
- Start with the main thread backtrace, then the non-waiting threads. Name the deepest CoD2x frame (`src/...:line`)
  and read that code.
- Not available without being the debugger: the exception code and faulting address. If needed, the user can type
  `-exec info signals` / look at the stop reason in VS Code, or `-exec bt`, `-exec info registers` in the Debug
  Console.
- A running (not stopped) game gives a moving snapshot - registers and stack of a thread can disagree.
- Seen once (2026-11-12): the main thread in PunkBuster (`pbcl.dll`, called from `WinMain+0x292` every frame, also
  with `cl_punkbuster 0`), jumped into reserved memory (`0x00200212`) through a register. Probably a deliberate
  exception PunkBuster handles itself (anti-debug) that gdb stops on as first-chance - not confirmed; check whether
  Continue in VS Code goes on, and update this note. No CoD2x frame in the chain = not our code.
- Report plainly: where each relevant thread is, what is ours and what is not, what can not be known from a snapshot.
