"""
game_inspect.py - read-only snapshot of a running (or debugger-stopped) CoD2MP_s.exe: threads, registers, backtraces,
code at EIP, memory, with CoD2 / CoD2x symbols.

Works while another debugger (VS Code / gdb) holds the process - it only reads (no attach, no suspend). A running game
gives a moving snapshot (the threads are not stopped).

Usage (from the repo root, 64-bit Python on Windows):
    python tools/game_inspect.py                  the game of this repo (bin/windows/CoD2MP_s.exe)
    python tools/game_inspect.py --pid 1234       a given process
    python tools/game_inspect.py --all-threads    full analysis of every thread (default: main + threads not waiting)
    python tools/game_inspect.py --mem 0x5dd6c8:64 --mem eax   hex dump of memory (address or register of the main thread)
    python tools/game_inspect.py --out crash.txt  also write the output into a file

Symbols:
    CoD2MP_s.exe          src/mss32/symbols_cod2mp_s.h (Binary Ninja names)
    gfx_d3d_mp_x86_s.dll  src/mss32/symbols_gfx_d3d_mp_x86_s.h
    mss32.dll             nm + addr2line of the loaded DLL (debug build: function, file:line)
    other modules         module+offset
"""

import argparse
import bisect
import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MINGW_BIN = os.path.join(REPO, "tools", "mingw", "bin")
GAME_EXE = os.path.join(REPO, "bin", "windows", "CoD2MP_s.exe")
CONSOLE_LOG = os.path.join(REPO, "bin", "windows", "main", "console_mp.log")
SYSTEM_DIRS = [os.environ.get("SystemRoot", r"C:\Windows"), os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "Microsoft")]

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
for f in ("OpenProcess", "OpenThread", "CreateToolhelp32Snapshot"):
    getattr(k32, f).restype = wt.HANDLE

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040
TH32CS_SNAPTHREAD = 0x4
TH32CS_SNAPPROCESS = 0x2
MEM_COMMIT = 0x1000
PAGE_NAMES = {0x01: "NOACCESS", 0x02: "R", 0x04: "RW", 0x08: "WC", 0x10: "X", 0x20: "RX", 0x40: "RWX", 0x80: "WCX"}
STATE_NAMES = {0x1000: "COMMIT", 0x2000: "RESERVE", 0x10000: "FREE"}

out_lines = []


def p(s=""):
    print(s)
    out_lines.append(s)


# ----------------------------------------------------------------------------------------------------------------------
# Process
# ----------------------------------------------------------------------------------------------------------------------

class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD)]


def process_path(pid):
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(1024)
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
    k32.CloseHandle(h)
    return buf.value if ok else None


def find_game_processes():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    found = []
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == "cod2mp_s.exe":
            found.append((pe.th32ProcessID, process_path(pe.th32ProcessID)))
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return found


class Process:
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            sys.exit("OpenProcess(%d) failed: error %d" % (pid, ctypes.get_last_error()))
        self.modules = self._modules()

    def read(self, addr, size):
        if addr <= 0 or size <= 0:
            return None
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)) and n.value:
            return buf.raw[:n.value]
        # partial read: page by page
        data = b""
        a = addr
        while len(data) < size:
            chunk = min(0x1000 - (a & 0xfff), size - len(data))
            b = ctypes.create_string_buffer(chunk)
            if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(a), b, chunk, ctypes.byref(n)) or not n.value:
                break
            data += b.raw[:n.value]
            a += n.value
        return data or None

    def u32(self, addr):
        d = self.read(addr, 4)
        return struct.unpack("<I", d)[0] if d and len(d) == 4 else None

    def query(self, addr):
        m = MBI()
        if not k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)):
            return None
        return m

    def _modules(self):
        arr = (ctypes.c_void_p * 2048)()
        needed = wt.DWORD()
        psapi.EnumProcessModulesEx(self.h, arr, ctypes.sizeof(arr), ctypes.byref(needed), 1)  # LIST_MODULES_32BIT
        mods = []
        for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
            if not arr[i]:
                continue
            mi = MODULEINFO()
            psapi.GetModuleInformation(self.h, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
            path = ctypes.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW(self.h, ctypes.c_void_p(arr[i]), path, 1024)
            mods.append(Module(mi.base or 0, mi.size, path.value))
        mods.sort(key=lambda m: m.base)
        return mods

    def module_at(self, addr):
        for m in self.modules:
            if m.base <= addr < m.base + m.size:
                return m
        return None

    def threads(self):
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
        te = THREADENTRY32()
        te.dwSize = ctypes.sizeof(te)
        tids = []
        ok = k32.Thread32First(snap, ctypes.byref(te))
        while ok:
            if te.th32OwnerProcessID == self.pid:
                tids.append(te.th32ThreadID)
            ok = k32.Thread32Next(snap, ctypes.byref(te))
        k32.CloseHandle(snap)
        return tids

    def debugger_present(self):
        b = wt.BOOL()
        k32.CheckRemoteDebuggerPresent(self.h, ctypes.byref(b))
        return bool(b.value)


# ----------------------------------------------------------------------------------------------------------------------
# Modules and symbols
# ----------------------------------------------------------------------------------------------------------------------

def load_symbol_header(path):
    """[(start, len, name)] sorted, from a symbols_*.h of the repo"""
    syms = []
    if os.path.exists(path):
        rx = re.compile(r'\{\s*0x([0-9a-fA-F]+)u?,\s*0x([0-9a-fA-F]+)u?,\s*"([^"]*)"')
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = rx.search(line)
                if m:
                    syms.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    syms.sort()
    return syms


def pe_image_base(path):
    try:
        with open(path, "rb") as f:
            head = f.read(0x400)
        off = struct.unpack_from("<I", head, 0x3c)[0]
        return struct.unpack_from("<I", head, off + 0x18 + 0x1c)[0]  # OptionalHeader.ImageBase (PE32)
    except Exception:
        return None


class Module:
    def __init__(self, base, size, path):
        self.base = base
        self.size = size
        self.path = path
        self.name = os.path.basename(path)
        self.lname = self.name.lower()
        self._syms = None   # [(va in the file's image base, len or None, name)]
        self.file_base = None

    def is_system(self):
        p = os.path.normcase(self.path)
        return any(p.startswith(os.path.normcase(d) + os.sep) for d in SYSTEM_DIRS)

    def symbols(self):
        if self._syms is not None:
            return self._syms
        self._syms = []
        if self.lname == "cod2mp_s.exe":
            self.file_base = 0x400000
            self._syms = load_symbol_header(os.path.join(REPO, "src", "mss32", "symbols_cod2mp_s.h"))
        elif self.lname == "gfx_d3d_mp_x86_s.dll":
            self.file_base = 0x10000000
            self._syms = load_symbol_header(os.path.join(REPO, "src", "mss32", "symbols_gfx_d3d_mp_x86_s.h"))
        elif self.lname == "mss32.dll" and not self.is_system():
            self.file_base = pe_image_base(self.path)
            nm = os.path.join(MINGW_BIN, "nm.exe")
            if self.file_base and os.path.exists(nm):
                try:
                    res = subprocess.run([nm, "--defined-only", "-C", self.path], capture_output=True, text=True,
                                         errors="replace", timeout=120)
                    for line in res.stdout.splitlines():
                        parts = line.split(" ", 2)
                        if len(parts) == 3 and parts[1] in "tT":
                            self._syms.append((int(parts[0], 16), None, parts[2]))
                    self._syms.sort()
                except Exception:
                    pass
        self._starts = [s[0] for s in self._syms]
        return self._syms

    def symbolize(self, addr):
        """'module!symbol+0xoff' or 'module+0xoff'"""
        syms = self.symbols()
        rva = addr - self.base
        if syms and self.file_base is not None:
            va = self.file_base + rva
            i = bisect.bisect_right(self._starts, va) - 1
            if i >= 0:
                start, length, name = syms[i]
                if length is None or va < start + length:
                    return "%s!%s+0x%x" % (self.name, name, va - start)
        return "%s+0x%x" % (self.name, rva)

    def file_va(self, addr):
        if self.file_base is None:
            self.symbols()
        return None if self.file_base is None else self.file_base + addr - self.base


def describe(proc, addr):
    m = proc.module_at(addr)
    return m.symbolize(addr) if m else None


def source_lines(proc, addrs):
    """file:line of mss32.dll addresses (debug build), one addr2line call"""
    a2l = os.path.join(MINGW_BIN, "addr2line.exe")
    by_mod = {}
    for a in addrs:
        m = proc.module_at(a)
        if m and m.lname == "mss32.dll" and not m.is_system():
            by_mod.setdefault(m, []).append(a)
    result = {}
    if not os.path.exists(a2l):
        return result
    for m, lst in by_mod.items():
        vas = [m.file_va(a) for a in lst]
        if None in vas:
            continue
        try:
            res = subprocess.run([a2l, "-e", m.path, "-f", "-C"] + ["0x%x" % (v - 1) for v in vas],
                                 capture_output=True, text=True, errors="replace", timeout=120)
            lines = res.stdout.splitlines()
            for i, a in enumerate(lst):
                if 2 * i + 1 < len(lines):
                    loc = lines[2 * i + 1].strip()
                    if not loc.startswith("??"):
                        loc = loc.split(" (")[0].replace("\\", "/")  # without " (discriminator N)"
                        root = REPO.replace("\\", "/") + "/"
                        result[a] = loc[len(root):] if loc.lower().startswith(root.lower()) else loc
        except Exception:
            pass
    return result


# ----------------------------------------------------------------------------------------------------------------------
# Threads
# ----------------------------------------------------------------------------------------------------------------------

REGS = ["edi", "esi", "ebx", "edx", "ecx", "eax", "ebp", "eip"]


def thread_context(tid):
    h = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, tid)
    if not h:
        return None, None
    ctx = ctypes.create_string_buffer(716)  # WOW64_CONTEXT / x86 CONTEXT
    struct.pack_into("<I", ctx, 0, 0x10007)  # CONTEXT_i386 | CONTROL | INTEGER | SEGMENTS
    get = k32.Wow64GetThreadContext if ctypes.sizeof(ctypes.c_void_p) == 8 else k32.GetThreadContext
    ok = get(h, ctx)
    start = ctypes.c_void_p()
    ntdll.NtQueryInformationThread(h, 9, ctypes.byref(start), ctypes.sizeof(start), None)  # Win32StartAddress
    k32.CloseHandle(h)
    if not ok:
        return None, start.value
    r = dict(zip(REGS, struct.unpack_from("<8I", ctx, 0x9C)))
    r["cs"], r["eflags"], r["esp"], r["ss"] = struct.unpack_from("<4I", ctx, 0xBC)
    r["fs"] = struct.unpack_from("<I", ctx, 0x90)[0]
    return r, start.value


def is_waiting(proc, eip):
    m = proc.module_at(eip)
    return m is not None and m.lname in ("ntdll.dll", "win32u.dll", "kernelbase.dll")


def ff2_length(modrm, sib):
    """Length of 'call r/m32' (FF /2) from its ModRM (and SIB)"""
    mod, rm = modrm >> 6, modrm & 7
    if mod == 3:
        return 2
    n = 2
    if rm == 4:
        n += 1
        if mod == 0 and sib is not None and (sib & 7) == 5:
            n += 4
    if mod == 0 and rm == 5:
        n += 4
    elif mod == 1:
        n += 1
    elif mod == 2:
        n += 4
    return n


def follows_call(proc, ret):
    """True if the instruction before ret is a call (the address is a return address)"""
    b = proc.read(ret - 7, 7)
    if not b or len(b) < 7:
        return False
    if b[2] == 0xE8:  # call rel32
        return True
    for k in range(2, 8):
        i = 7 - k
        if b[i] == 0xFF and ((b[i + 1] >> 3) & 7) == 2:
            sib = b[i + 2] if i + 2 < 7 else None
            if ff2_length(b[i + 1], sib) == k:
                return True
    return False


def stack_region(proc, esp):
    m = proc.query(esp)
    if not m or m.State != MEM_COMMIT:
        return esp, esp
    return esp, (m.BaseAddress or 0) + m.RegionSize


def ebp_chain(proc, ebp, esp, top, limit=64):
    frames = []
    while esp <= ebp < top and len(frames) < limit:
        d = proc.read(ebp, 8)
        if not d or len(d) < 8:
            break
        nxt, ret = struct.unpack("<II", d)
        frames.append((ebp, ret))
        if nxt <= ebp:
            break
        ebp = nxt
    return frames


def stack_scan(proc, esp, top, max_bytes, game_only):
    data = proc.read(esp, min(top - esp, max_bytes)) or b""
    hits = []
    for i in range(0, len(data) - 3, 4):
        v = struct.unpack_from("<I", data, i)[0]
        m = proc.module_at(v)
        if not m or (game_only and m.is_system()):
            continue
        if follows_call(proc, v):
            hits.append((i, v))
    return hits


def describe_value(proc, v):
    """What a register / stack value points to"""
    d = describe(proc, v)
    if d:
        return d
    m = proc.query(v) if v >= 0x10000 else None
    if not m or m.State != MEM_COMMIT:
        if m:
            return "(%s, not readable)" % STATE_NAMES.get(m.State, hex(m.State))
        return ""
    data = proc.read(v, 32)
    if not data:
        return ""
    s = re.match(rb"[\x20-\x7e\t\r\n]{4,}", data)
    if s:
        return '-> "%s"' % s.group(0).decode("ascii", "replace").replace("\n", "\\n")
    words = " ".join("%08x" % w for w in struct.unpack_from("<4I", data.ljust(16, b"\0")))
    return "-> %s" % words


def disassemble(proc, addr, before=24, after=48):
    objdump = os.path.join(MINGW_BIN, "objdump.exe")
    data = proc.read(addr - before, before + after)
    if not data:
        return ["    (code not readable)"]
    if not os.path.exists(objdump):
        return ["    " + data.hex()]
    lines = []
    # Disassemble from a few starting points before addr, keep the one that hits addr exactly
    best = None
    for start in range(0, before):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
            f.write(data[start:])
            tmp = f.name
        try:
            res = subprocess.run([objdump, "-D", "-b", "binary", "-m", "i386", "-M", "intel",
                                  "--adjust-vma=0x%x" % (addr - before + start), tmp],
                                 capture_output=True, text=True, errors="replace", timeout=30)
        finally:
            os.unlink(tmp)
        ins = []
        for line in res.stdout.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)", line)
            if m:
                ins.append((int(m.group(1), 16), m.group(2).strip(), m.group(3).strip()))
        if any(a == addr for a, _, _ in ins):
            best = ins
            break
    if best is None:
        return ["    (no instruction boundary found at %08x) %s" % (addr, data[before:before + 16].hex())]
    for a, raw, text in best:
        if a < addr - before + 0 or a > addr + 32:
            continue
        if text.startswith("(bad)"):
            break
        mark = "=>" if a == addr else "  "
        sym = describe(proc, a) if a == addr else ""
        lines.append("  %s %08x  %-22s %s%s" % (mark, a, raw, text, ("    ; " + sym) if sym else ""))
    return lines[-14:] if len(lines) > 14 else lines


def analyze_thread(proc, tid, r, start, full, scan_bytes):
    eip = r["eip"]
    loc = describe(proc, eip) or "(no module)"
    start_s = describe(proc, start) if start else None
    p("=== thread %d  eip=%08x  %s%s" % (tid, eip, loc, ("   [start %s]" % start_s) if start_s else ""))
    if not full:
        return
    p("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x" % (r["eax"], r["ebx"], r["ecx"], r["edx"], r["esi"], r["edi"]))
    p("  ebp=%08x esp=%08x eflags=%08x" % (r["ebp"], r["esp"], r["eflags"]))
    for reg in ("eax", "ebx", "ecx", "edx", "esi", "edi"):
        d = describe_value(proc, r[reg])
        if d:
            p("    %s %s" % (reg, d))

    m = proc.query(eip)
    if m:
        p("  eip memory: %s %s (region %08x size %x)" % (STATE_NAMES.get(m.State, hex(m.State)),
                                                        PAGE_NAMES.get(m.Protect & 0xff, hex(m.Protect)),
                                                        m.BaseAddress or 0, m.RegionSize))
    if not proc.module_at(eip):
        p("  !! eip is outside of any module - a call/jump through a bad pointer, the caller is the return address at [esp]")
    p("  code:")
    for line in disassemble(proc, eip):
        p(line)

    esp = r["esp"]
    _, top = stack_region(proc, esp)
    p("  stack %08x..%08x" % (esp, top))
    top_words = proc.read(esp, 32)
    if top_words:
        p("  [esp] " + " ".join("%08x" % w for w in struct.unpack_from("<%dI" % (len(top_words) // 4), top_words)))
    ret0 = proc.u32(esp)
    if ret0 is not None and proc.module_at(ret0) and follows_call(proc, ret0):
        p("  [esp] is a return address: %08x %s" % (ret0, describe(proc, ret0)))

    frames = ebp_chain(proc, r["ebp"], esp, top)
    scan = stack_scan(proc, esp, top, scan_bytes, game_only=False)
    lines = source_lines(proc, [ret for _, ret in frames] + [v for _, v in scan] + [eip])

    p("  backtrace (ebp chain):")
    p("    #0 %08x %s%s" % (eip, loc, ("  " + lines[eip]) if eip in lines else ""))
    for i, (ebp, ret) in enumerate(frames, 1):
        p("    #%d %08x %s%s   (ebp=%08x)" % (i, ret, describe(proc, ret) or "?", ("  " + lines[ret]) if ret in lines else "", ebp))
    p("  return addresses on the stack (scan, also frames without ebp):")
    for off, v in scan[:80]:
        p("    [esp+%04x] %08x %s%s" % (off, v, describe(proc, v), ("  " + lines[v]) if v in lines else ""))
    if len(scan) > 80:
        p("    ... %d more" % (len(scan) - 80))


# ----------------------------------------------------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------------------------------------------------

def parse_mem(spec, regs):
    addr_s, _, len_s = spec.partition(":")
    addr = regs[addr_s] if regs and addr_s in regs else int(addr_s, 0)
    return addr, int(len_s, 0) if len_s else 64


def hexdump(proc, addr, length):
    data = proc.read(addr, length)
    if not data:
        p("  %08x: (not readable)" % addr)
        return
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        p("  %08x: %-48s %s" % (addr + i, " ".join("%02x" % c for c in chunk),
                                 "".join(chr(c) if 0x20 <= c < 0x7f else "." for c in chunk)))


def main():
    ap = argparse.ArgumentParser(description="Read-only snapshot of a running CoD2MP_s.exe (threads, registers, stacks)")
    ap.add_argument("--pid", type=int, help="process id (default: the CoD2MP_s.exe of this repo)")
    ap.add_argument("--all-threads", action="store_true", help="full analysis of every thread")
    ap.add_argument("--stack", type=lambda s: int(s, 0), default=0x4000, help="bytes of stack to scan (default 0x4000)")
    ap.add_argument("--mem", action="append", default=[], help="ADDR[:LEN] or REG[:LEN] of the main thread, hex dump")
    ap.add_argument("--log", type=int, default=25, help="lines of console_mp.log to show (0 = none)")
    ap.add_argument("--out", help="also write the output into this file")
    args = ap.parse_args()

    pid = args.pid
    if pid is None:
        games = find_game_processes()
        mine = [(i, path) for i, path in games if path and os.path.normcase(path) == os.path.normcase(GAME_EXE)]
        if len(mine) != 1:
            print("Game processes:" if games else "No CoD2MP_s.exe running")
            for i, path in games:
                print("  %6d %s" % (i, path))
            sys.exit("Use --pid (%d of this repo: %s)" % (len(mine), GAME_EXE))
        pid = mine[0][0]

    proc = Process(pid)
    p("Process %d  %s  debugger attached: %s" % (pid, process_path(pid), "yes" if proc.debugger_present() else "no"))
    p("Modules (not system):")
    for m in proc.modules:
        if not m.is_system():
            p("  %08x-%08x %s" % (m.base, m.base + m.size, m.path))
    p()

    tids = proc.threads()
    main_regs = None
    for n, tid in enumerate(tids):
        r, start = thread_context(tid)
        if r is None:
            p("=== thread %d: context not readable (error %d)" % (tid, ctypes.get_last_error()))
            continue
        main = n == 0  # the first thread of the snapshot is the main thread
        if main:
            main_regs = r
        full = args.all_threads or main or not is_waiting(proc, r["eip"])
        analyze_thread(proc, tid, r, start, full, args.stack)
        if full:
            p()

    for spec in args.mem:
        addr, length = parse_mem(spec, main_regs)
        p("Memory %08x (%d bytes)%s:" % (addr, length, ("  " + describe(proc, addr)) if describe(proc, addr) else ""))
        hexdump(proc, addr, length)
        p()

    if args.log and os.path.exists(CONSOLE_LOG) and os.path.normcase(process_path(pid) or "") == os.path.normcase(GAME_EXE):
        with open(CONSOLE_LOG, encoding="utf-8", errors="replace") as f:
            tail = f.read().splitlines()[-args.log:]
        p("console_mp.log (last %d lines):" % len(tail))
        for line in tail:
            p("  " + line)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write("\n".join(out_lines) + "\n")


if __name__ == "__main__":
    main()
