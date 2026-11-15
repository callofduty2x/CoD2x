"""
Talk to a running debug CoD2x game through its control pipe (\\\\.\\pipe\\cod2x-<pid>, src/mss32/debug_pipe.cpp).
Playing the client (aim, move, fire, keys, typing, menu clicks) is done with console commands of src/mss32/debug_input.cpp.

Usage (64-bit Python, standard library only):
  python tools/game_pipe.py --pid 1234 "openscriptmenu serverinfo_dm close"   # console command, prints its output
  python tools/game_pipe.py --pid 1234 !ping
  python tools/game_pipe.py --pid 1234 !state
  python tools/game_pipe.py --pid 1234 !dvar g_competitive
  python tools/game_pipe.py --pid 1234 !log 50
  python tools/game_pipe.py --pid 1234 --wait-state clientState=active --timeout 60   # polls !state
  python tools/game_pipe.py --pid 1234 --wait-state clientState=8 --wait-state cl_ingame=1
  python tools/game_pipe.py --pid 1234 --wait-log "dvar set server16" --timeout 30  # polls !log
  python tools/game_pipe.py --pid 1234 players "aim bot0 head" fire                 # play the client
  python tools/game_pipe.py --pid 1234 chatmodepublic "type hello" "key ENTER"
  python tools/game_pipe.py --pid 1234 "key ESCAPE" menus "click MENU_BACKTOGAME"
  python tools/game_pipe.py --pid 1234 +forward -forward     # requests may start with '-'

Several requests can be given; they are sent in order and the tool stops at the first error.
Exit code: 0 = every request replied "ok" / the wait condition was met, 1 = "error", timeout or no pipe,
2 = the game crashed (its crash handler replied "error crash: <fault>, report <file>", or it shows the crash dialog)
or the process is gone.
"""

import argparse
import ctypes
import glob
import msvcrt
import os
import re
import sys
import time
from ctypes import wintypes

sys.dont_write_bytecode = True  # no tools/__pycache__ from the import below
from game_inspect import process_path

TERMINATOR = b"\x1e"
CRASH_DIALOG_CLASS = "CoD2x_CrashDialog"    # CRASH_DLG_CLASS in src/mss32/exception_dialog.cpp

SYNCHRONIZE = 0x00100000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
ERROR_PIPE_BUSY = 231

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                                    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
_kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_user32 = ctypes.WinDLL("user32", use_last_error=True)
_user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
_user32.FindWindowExW.restype = wintypes.HWND
_user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]


class GameCrashed(Exception):
    """The game crashed: its crash handler replied "error crash: <fault>, report <file>" (output = what the command printed),
    it shows the crash dialog, or the process is gone."""
    def __init__(self, message, output=()):
        super().__init__(message)
        self.output = list(output)


def crash_dialog_shown(pid):
    """True if the process shows the CoD2x crash dialog (the game crashed, the main thread waits in the dialog)."""
    hwnd = None
    while True:
        hwnd = _user32.FindWindowExW(None, hwnd, CRASH_DIALOG_CLASS, None)
        if not hwnd:
            return False
        owner = wintypes.DWORD(0)
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            return True


def newest_crash_report(pid):
    """Path of the newest CoD2MP_s.exe.crash.*.txt in the folder of the game, or "?" """
    exe = process_path(pid)
    reports = glob.glob(os.path.join(os.path.dirname(exe), "CoD2MP_s.exe.crash.*.txt")) if exe else []
    return max(reports, key=os.path.getmtime) if reports else "?"


class GamePipe:
    def __init__(self, pid, connect_timeout=5.0, reply_timeout=30.0):
        self.pid = pid
        self.path = r"\\.\pipe\cod2x-%d" % pid
        self.connect_timeout = connect_timeout
        self.reply_timeout = reply_timeout
        self.f = None
        # Kept open: tells if the process ended, and the pid can not be reused meanwhile
        self.process = _kernel32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)

    def _available(self):
        """Bytes waiting in the pipe, without blocking. Raises ConnectionError if the game closed it."""
        avail = wintypes.DWORD(0)
        handle = msvcrt.get_osfhandle(self.f.fileno())
        if not _kernel32.PeekNamedPipe(handle, None, 0, None, ctypes.byref(avail), None):
            raise ConnectionError("the game closed the pipe (error %d)" % ctypes.get_last_error())
        return avail.value

    def check_alive(self):
        """Raises GameCrashed if the game process is gone or shows the crash dialog."""
        if not self.process:
            raise GameCrashed("there is no game process %d" % self.pid)
        if _kernel32.WaitForSingleObject(self.process, 0) == 0:  # signaled: the process ended
            code = wintypes.DWORD(0)
            _kernel32.GetExitCodeProcess(self.process, ctypes.byref(code))
            raise GameCrashed("the game process %d is gone (exit code %d)" % (self.pid, code.value))
        if crash_dialog_shown(self.pid):
            raise GameCrashed("crash: the game shows the crash dialog, report %s" % newest_crash_report(self.pid))

    def open(self):
        # The game may be starting, reloading (hot reload) or busy with another client: retry for a while
        deadline = time.monotonic() + self.connect_timeout
        while True:
            try:
                self.f = open(self.path, "r+b", buffering=0)
                return
            except OSError as e:
                self.check_alive()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ConnectionError("can not open %s: %s" % (self.path, e))
                if getattr(e, "winerror", None) == ERROR_PIPE_BUSY:
                    _kernel32.WaitNamedPipeW(self.path, int(min(remaining, 1.0) * 1000))  # wakes when the other client leaves
                else:
                    time.sleep(0.2)

    def close(self):
        if self.f:
            try:
                self.f.close()
            except OSError:
                pass
            self.f = None

    def request(self, line):
        """Sends one request, returns (ok, output_lines, error_message). Raises GameCrashed when the game crashed."""
        if "\n" in line:
            raise ValueError("a request is one line")
        if not self.f:
            self.open()
        try:
            self.f.write(line.encode("utf-8") + b"\n")
            buffer = b""
            deadline = time.monotonic() + self.reply_timeout
            delay = 0.001  # the game replies within a frame or two - poll fast first, then back off
            while True:
                # The reply ends with the terminator line "\x1e" "ok" / "\x1e" "error <message>" (console lines never
                # contain 0x1e, the game removes it)
                start = buffer.rfind(b"\n", 0, len(buffer) - 1) + 1  # start of the last line (0 if only one)
                if buffer.endswith(b"\n") and buffer[start:start + 1] == TERMINATOR:
                    break
                # Never block: the game may hang, crash into a dialog or sit in a long load
                n = self._available()
                if n:
                    buffer += self.f.read(n)
                    continue
                if time.monotonic() >= deadline:
                    raise TimeoutError("no reply to %r within %gs (game hung or busy?)" % (line, self.reply_timeout))
                time.sleep(delay)
                delay = min(delay * 2, 0.02)
        except OSError:
            self.close()        # a late reply would be read as the reply of the next request
            self.check_alive()  # a broken pipe or no reply from a crashed game is a crash
            raise

        status = buffer[start + 1:].decode("utf-8", "replace").strip()
        output = buffer[:start].decode("utf-8", "replace").splitlines()
        if status == "ok":
            return True, output, ""
        error = status[len("error"):].strip() if status.startswith("error") else status
        if error.startswith("crash:"):
            self.close()
            raise GameCrashed(error, output)
        return False, output, error


def parse_state(line):
    state = {}
    for pair in line.split():
        key, _, value = pair.partition("=")
        state[key] = value
    return state


def state_matches(state, key, value):
    """Case-insensitive; clientState=active compares the name (clientStateName), clientState=8 the number."""
    if key == "clientState" and not value.isdigit():
        key = "clientStateName"
    return state.get(key, "").lower() == value.lower()


def poll(pipe, request, find, what, timeout, interval):
    """
    Sends the request until find(output_lines) returns a line to print, or the timeout is over. A broken or busy pipe
    (game starting, hot reload) is reopened by the next request; a crash ends the poll with GameCrashed. Returns True if found.
    """
    reply_timeout = pipe.reply_timeout
    deadline = time.monotonic() + timeout
    last = ""
    try:
        while True:
            try:
                pipe.reply_timeout = max(1.0, min(reply_timeout, deadline - time.monotonic()))
                ok, output, _ = pipe.request(request)
                if ok:
                    found = find(output)
                    if found is not None:
                        print(found)
                        return True
                    last = output[-1] if output else ""
            except OSError as e:  # ConnectionError and TimeoutError are OSErrors
                last = "(pipe: %s)" % e
            if time.monotonic() >= deadline:
                print("timeout after %gs waiting for %s, last: %s" % (timeout, what, last), file=sys.stderr)
                return False
            pipe.close()  # the pipe has one instance: let other scripts in between the polls
            time.sleep(interval)
    finally:
        pipe.reply_timeout = reply_timeout


def wait_state(pipe, conditions, timeout, interval):
    wanted = []
    for c in conditions:
        key, sep, value = c.partition("=")
        if not sep:
            raise SystemExit("--wait-state needs key=value, got %r" % c)
        wanted.append((key, value))

    def find(output):
        if output and all(state_matches(parse_state(output[0]), k, v) for k, v in wanted):
            return output[0]
        return None

    return poll(pipe, "!state", find, " ".join(conditions), timeout, interval)


def wait_log(pipe, pattern, timeout, interval, lines):
    regex = re.compile(pattern)
    return poll(pipe, "!log %d" % lines, lambda output: next((line for line in output if regex.search(line)), None),
                "log line /%s/" % pattern, timeout, interval)


def main():
    parser = argparse.ArgumentParser(description="Send requests to a debug CoD2x game through its control pipe.")
    parser.add_argument("--pid", type=int, required=True, help="process id of CoD2MP_s.exe (the pipe is cod2x-<pid>)")
    parser.add_argument("--wait-state", action="append", default=[], metavar="KEY=VALUE",
                        help="poll !state until KEY has VALUE (repeatable, all must match)")
    parser.add_argument("--wait-log", metavar="REGEX", help="poll !log until a line matches REGEX")
    parser.add_argument("--log-lines", type=int, default=500, help="lines searched by --wait-log (default 500)")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds for --wait-state / --wait-log (default 30)")
    parser.add_argument("--interval", type=float, default=0.25, help="poll interval in seconds (default 0.25)")
    parser.add_argument("--connect-timeout", type=float, default=5.0, help="seconds to retry opening the pipe (default 5)")
    parser.add_argument("--reply-timeout", type=float, default=30.0,
                        help="seconds to wait for the reply of one request (default 30; a map load can take a while)")
    parser.add_argument("requests", nargs="*", help='requests: a console command, !ping, !state, !dvar <name>, !log <n>')
    # Requests may start with '-' ("-forward" releases a button): everything from the first request on is a request, not an option
    argv = sys.argv[1:]
    options_with_value = {s for a in parser._actions if a.option_strings and a.nargs != 0 for s in a.option_strings}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--":
            break
        if a.split("=", 1)[0] in options_with_value:
            i += 1 if "=" in a else 2
            continue
        if a in ("-h", "--help"):
            i += 1
            continue
        argv.insert(i, "--")
        break
    args = parser.parse_args(argv)

    # "!log 50" / "!dvar name" may come as two arguments - join the number / name to its request
    requests = []
    for r in args.requests:
        if requests and ((requests[-1] == "!log" and r.isdigit()) or (requests[-1] == "!dvar" and not r.startswith("!"))):
            requests[-1] += " " + r
        else:
            requests.append(r)

    if not requests and not args.wait_state and not args.wait_log:
        parser.error("nothing to do: give a request, --wait-state or --wait-log")

    sys.stdout.reconfigure(line_buffering=True)  # output shows up at once when piped / redirected

    pipe = GamePipe(args.pid, args.connect_timeout, args.reply_timeout)
    try:
        if args.wait_state and not wait_state(pipe, args.wait_state, args.timeout, args.interval):
            return 1
        if args.wait_log and not wait_log(pipe, args.wait_log, args.timeout, args.interval, args.log_lines):
            return 1
        for r in requests:
            ok, output, error = pipe.request(r)
            for line in output:
                print(line)
            if not ok:
                print("error: %s" % error, file=sys.stderr)
                return 1
        return 0
    except GameCrashed as e:
        for line in e.output:
            print(line)
        print("error: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    finally:
        pipe.close()


if __name__ == "__main__":
    sys.exit(main())
