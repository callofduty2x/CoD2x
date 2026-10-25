// http_client.h pulls in mongoose; include it first (matches the other telemetry/updater files).
#include "../shared/http_client.h"
#include "../shared/json.h"

#include "exception.h"

#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <string>
#include <thread>
#ifndef __USE_GNU
    #define __USE_GNU
#endif
#include <dlfcn.h>
#include <sched.h>              // sched_getaffinity, CPU_COUNT
#include <sys/resource.h>       // getrlimit
#include <gnu/libc-version.h>   // gnu_get_libc_version

#include "shared.h"
#include "../shared/logger.h"
#include "../shared/cod2_dvars.h"
#include "symbols_libcod2x.h"   // resolve libCoD2x.so's own (static) functions, like symbols_mss32.h on Windows

// Telemetry endpoint that receives crash reports as JSON (same as the Windows client).
#define TELEMETRY_ENDPOINT_URL "https://master.cod2x.me/api/telemetry/cod2x-exceptions"
//#define TELEMETRY_ENDPOINT_URL "http://localhost:8080/api/telemetry/cod2x-exceptions"

// Crash handler of the Linux dedicated server.
// On SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT it writes "cod2x_crash.<time>.<pid>.log" into the working directory
// (and the same text to stderr, so it is in the docker / screen log): signal, fault address, registers, the stack
// (glibc backtrace and the EBP chain, which is reliable for cod2_lnxded) and the memory map.
// Then the default action runs, so a core dump is still created when enabled.

static char exception_altStack[64 * 1024];
static volatile sig_atomic_t exception_inHandler = 0;
static int exception_fd = -1;
static int exception_testMode = 0;  // >0 in a self-test run; tags the report so the backend acks without storing

static void exception_write(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    if (exception_fd >= 0 && write(exception_fd, buf, n) < 0) { /* nothing to do */ }
    if (write(STDERR_FILENO, buf, n) < 0) { /* nothing to do */ }
}

// Read memory of this process without faulting (fails with EFAULT on unmapped memory).
static bool exception_safeRead(unsigned int addr, void* out, size_t len) {
    struct iovec local = { out, len };
    struct iovec remote = { (void*)(uintptr_t)addr, len };
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)len;
}

// "module+0xOFFSET (symbol+0xDISP)"
static void exception_describeAddress(unsigned int addr, char* out, size_t outSize) {
    Dl_info info;
    if (dladdr((void*)(uintptr_t)addr, &info) == 0 || !info.dli_fname) {
        snprintf(out, outSize, "<unknown>");
        return;
    }
    const char* name = strrchr(info.dli_fname, '/');
    name = name ? name + 1 : info.dli_fname;
    unsigned int base = (unsigned int)(uintptr_t)info.dli_fbase;
    // The executable is not position independent, its offsets are the absolute addresses of the Binary Ninja database
    unsigned int offset = (base == 0x08048000) ? addr : addr - base;
    if (info.dli_sname)
        snprintf(out, outSize, "%s+0x%x (%s+0x%x)", name, offset, info.dli_sname, addr - (unsigned int)(uintptr_t)info.dli_saddr);
    else
        snprintf(out, outSize, "%s+0x%x", name, offset);
}

static const char* exception_signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGFPE:  return "SIGFPE (arithmetic exception)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGABRT: return "SIGABRT (abort)";
        default:      return "unknown signal";
    }
}

// ---------------------------------------------------------------------------
// Rich JSON report (same shape the Windows client sends; fed to the telemetry backend)
//
// Built in the signal handler into a fixed static buffer (no heap) and written to a
// "<name>.pending.json" file. Delivery happens later from exception_resendPending() on the next
// server start - the crashing process never runs OpenSSL/HTTP, which keeps the handler safe.
// ---------------------------------------------------------------------------

static char exception_json[512 * 1024];
static size_t exception_jsonPos = 0;

static void jreset() { exception_jsonPos = 0; exception_json[0] = '\0'; }

static void jraw(const char* fmt, ...) {
    if (exception_jsonPos + 1 >= sizeof(exception_json)) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(exception_json + exception_jsonPos, sizeof(exception_json) - exception_jsonPos, fmt, args);
    va_end(args);
    if (n < 0) return;
    exception_jsonPos += (size_t)n;
    if (exception_jsonPos >= sizeof(exception_json)) exception_jsonPos = sizeof(exception_json) - 1;
}

// Append one raw char.
static void jputc(char c) {
    if (exception_jsonPos + 1 < sizeof(exception_json)) {
        exception_json[exception_jsonPos++] = c;
        exception_json[exception_jsonPos] = '\0';
    }
}

// Append a JSON string value (quoted + escaped).
static void jstr(const char* s) {
    jputc('"');
    for (const char* p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  jraw("\\\""); break;
            case '\\': jraw("\\\\"); break;
            case '\n': jraw("\\n"); break;
            case '\r': jraw("\\r"); break;
            case '\t': jraw("\\t"); break;
            default:
                if (c < 0x20) jraw("\\u%04x", c);
                else jputc((char)c);
        }
    }
    jputc('"');
}

// "key":"value",
static void jkv(const char* key, const char* value) {
    jraw("\"%s\":", key);
    jstr(value);
    jputc(',');
}

// Append fieldName:[ "line", ... ] from text split on newlines (empty lines skipped). No trailing comma.
static void jlinesArray(const char* fieldName, const char* text) {
    jraw("\"%s\":[", fieldName);
    bool first = true;
    const char* p = text ? text : "";
    while (*p) {
        const char* start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (p > start) {
            if (!first) jputc(',');
            first = false;
            char line[1024];
            size_t len = (size_t)(p - start);
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, start, len);
            line[len] = '\0';
            jstr(line);
        }
        while (*p == '\n' || *p == '\r') p++;
    }
    jputc(']');
}

// Resolve an address to module name, offset within the module, and "symbol+0xDISP".
static void exception_resolveFrame(unsigned int addr, char* mod, size_t modSz, unsigned int* offset, char* sym, size_t symSz) {
    mod[0] = '\0'; sym[0] = '\0'; *offset = addr;
    Dl_info info;
    if (dladdr((void*)(uintptr_t)addr, &info) == 0 || !info.dli_fname) {
        snprintf(mod, modSz, "<unknown>");
        return;
    }
    const char* name = strrchr(info.dli_fname, '/');
    name = name ? name + 1 : info.dli_fname;
    snprintf(mod, modSz, "%s", name);
    unsigned int base = (unsigned int)(uintptr_t)info.dli_fbase;
    // The executable is not position independent; its offsets are the absolute Binary Ninja addresses.
    *offset = (base == 0x08048000) ? addr : addr - base;

    // Prefer our generated table for libCoD2x.so: dladdr only sees exported (.dynsym) symbols, so our
    // own static functions would otherwise be <no symbol>. The table (from .symtab) resolves them.
    if (strcmp(name, "libCoD2x.so") == 0) {
        const LibCod2xSymbol* s = symbols_libcod2x_find(addr - base);
        if (s) {
            unsigned int disp = (addr - base) - s->rva;
            if (disp == 0) snprintf(sym, symSz, "%s", s->name);
            else           snprintf(sym, symSz, "%s+0x%x", s->name, disp);
            return;
        }
    }

    if (info.dli_sname)
        snprintf(sym, symSz, "%s+0x%x", info.dli_sname, addr - (unsigned int)(uintptr_t)info.dli_saddr);
}

// Append one "stacks" entry: a frame list produced by walking from 'eip' using the given method.
static void exception_jsonStackByEbp(unsigned int eip, unsigned int ebp, unsigned long tid) {
    jraw("{");
    jkv("threadRole", "crashing thread");
    jkv("threadName", "");
    jraw("\"threadId\":\"%lu\",", tid);
    jkv("method", "EBP chain");
    jraw("\"frames\":[");
    char mod[128], sym[256];
    unsigned int off;
    int n = 0;
    exception_resolveFrame(eip, mod, sizeof(mod), &off, sym, sizeof(sym));
    jraw("{\"frameNumber\":%d,", n++);
    jraw("\"instructionAddress\":\"0x%08x\",", eip);
    jraw("\"moduleOffset\":\"0x%08x\",", off);
    jkv("moduleName", mod); jkv("sectionName", ""); jraw("\"resolvedSymbol\":"); jstr(sym[0] ? sym : "<no symbol>"); jraw(",\"instruction\":\"\"}");
    unsigned int frame = ebp;
    for (int i = 1; i < 64 && frame != 0 && (frame & 3) == 0; i++) {
        unsigned int words[2];
        if (!exception_safeRead(frame, words, sizeof(words)) || words[1] == 0) break;
        exception_resolveFrame(words[1], mod, sizeof(mod), &off, sym, sizeof(sym));
        jraw(",{\"frameNumber\":%d,", n++);
        jraw("\"instructionAddress\":\"0x%08x\",", words[1]);
        jraw("\"moduleOffset\":\"0x%08x\",", off);
        jkv("moduleName", mod); jkv("sectionName", ""); jraw("\"resolvedSymbol\":"); jstr(sym[0] ? sym : "<no symbol>"); jraw(",\"instruction\":\"\"}");
        if (words[0] <= frame) break;
        frame = words[0];
    }
    jraw("],\"diagnostics\":{\"method\":\"EBP chain (frame pointer unwind)\"}}");
}

static void exception_jsonStackByBacktrace(unsigned long tid) {
    jraw("{");
    jkv("threadRole", "crashing thread");
    jkv("threadName", "");
    jraw("\"threadId\":\"%lu\",", tid);
    jkv("method", "backtrace");
    jraw("\"frames\":[");
    void* frames[64];
    int count = backtrace(frames, 64);
    char mod[128], sym[256];
    unsigned int off;
    for (int i = 0; i < count; i++) {
        unsigned int a = (unsigned int)(uintptr_t)frames[i];
        exception_resolveFrame(a, mod, sizeof(mod), &off, sym, sizeof(sym));
        jraw("%s{\"frameNumber\":%d,", i ? "," : "", i);
        jraw("\"instructionAddress\":\"0x%08x\",", a);
        jraw("\"moduleOffset\":\"0x%08x\",", off);
        jkv("moduleName", mod); jkv("sectionName", ""); jraw("\"resolvedSymbol\":"); jstr(sym[0] ? sym : "<no symbol>"); jraw(",\"instruction\":\"\"}");
    }
    jraw("],\"diagnostics\":{\"method\":\"glibc backtrace\"}}");
}

// Append a memory block {name,address,hex,note}.
static void exception_jsonMemory(const char* name, unsigned int addr, size_t len) {
    jraw("{"); jkv("name", name); jraw("\"address\":\"0x%08x\",", addr);
    unsigned char buf[512];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (addr && exception_safeRead(addr, buf, len)) {
        jraw("\"hex\":\"");
        for (size_t i = 0; i < len; i++) jraw("%02x", buf[i]);
        jraw("\"");
    } else {
        jraw("\"hex\":\"\",\"note\":\"unreadable\"");
    }
    jraw("}");
}

// Loaded modules from /proc/self/maps: one entry per distinct file with an executable mapping.
static void exception_jsonModules() {
    jraw("\"modules\":[");
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) { jraw("]"); return; }
    char buf[8192];
    ssize_t total = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (total <= 0) { jraw("]"); return; }
    buf[total] = '\0';

    char seen[64][256];
    int seenCount = 0;
    bool first = true;
    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        // "start-end perms offset dev inode   path"
        unsigned int start = 0; char perms[8] = {0}; char path[256] = {0};
        if (sscanf(line, "%x-%*x %7s %*x %*x:%*x %*u %255[^\n]", &start, perms, path) >= 2 &&
            path[0] == '/' && perms[2] == 'x') {
            const char* name = strrchr(path, '/'); name = name ? name + 1 : path;
            bool dup = false;
            for (int i = 0; i < seenCount; i++) if (strcmp(seen[i], name) == 0) { dup = true; break; }
            if (!dup && seenCount < 64) {
                snprintf(seen[seenCount++], 256, "%s", name);
                jraw("%s{", first ? "" : ","); first = false;
                jraw("\"baseAddress\":\"0x%08x\",", start);
                jkv("moduleName", name);
                jraw("\"path\":"); jstr(path);
                jraw("}");
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    jraw("]");
}

// A few server dvars, for context (empty if the dvar system is not ready).
static void exception_jsonCvars() {
    static const char* names[] = { "sv_hostname", "mapname", "g_gametype", "fs_game", "dedicated", "net_port", "sv_maxclients", "protocol", "sv_punkbuster" };
    jraw("\"cvars\":{");
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        dvar_t* dv = Dvar_GetDvarByName(names[i]);
        char val[256];
        if (!dv) continue;
        switch (dv->type) {
            case DVAR_TYPE_BOOL:  snprintf(val, sizeof(val), "%s", dv->value.boolean ? "true" : "false"); break;
            case DVAR_TYPE_INT:   snprintf(val, sizeof(val), "%d", dv->value.integer); break;
            case DVAR_TYPE_FLOAT: snprintf(val, sizeof(val), "%.4f", dv->value.decimal); break;
            case DVAR_TYPE_STRING: snprintf(val, sizeof(val), "%s", dv->value.string ? dv->value.string : ""); break;
            default: snprintf(val, sizeof(val), "%d", dv->value.integer); break;
        }
        if (!first) jputc(',');
        first = false;
        jraw("\"%s\":", names[i]); jstr(val);
    }
    jraw("}");
}

// ---------------------------------------------------------------------------
// Static system environment, gathered once at startup (exception_patch), cached here.
// These are cheap /proc + libc reads that never change, so the signal handler only has to read the
// dynamic state (free memory, load, thread count). Mirrors how the Windows client caches SYS_*.
// ---------------------------------------------------------------------------
static char   sys_distro[160]   = "";  // /etc/os-release PRETTY_NAME, e.g. "Ubuntu 22.04.3 LTS"
static char   sys_kernel[96]    = "";  // uname release
static char   sys_arch[65]      = "";  // uname machine (x86_64 / i686); utsname fields are 65 bytes
static char   sys_libc[32]      = "";  // glibc version
static char   sys_cpuModel[128] = "";  // /proc/cpuinfo "model name"
static int    sys_cpuPhysical   = 0;   // distinct physical cores (best effort)
static char   sys_virt[64]      = "";  // container / hypervisor, or "none"
static char   sys_affinity[64]  = "";  // "N of M" from sched_getaffinity
static int    sys_isRoot        = 0;   // geteuid() == 0
static long   sys_ulimitStackKB = -1;  // -1 = unlimited / unknown
static long   sys_ulimitCoreKB  = -1;
static long   sys_ulimitNoFile  = -1;
static time_t sys_procStart     = 0;   // for process uptime

// Read KEY="value" from /etc/os-release (strips the quotes). Returns false if not present.
static bool exception_readOsRelease(const char* key, char* out, size_t outSz) {
    out[0] = '\0';
    int fd = open("/etc/os-release", O_RDONLY);
    if (fd < 0) return false;
    char buf[2048]; ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    size_t klen = strlen(key);
    for (char* line = buf; line && *line; ) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            const char* v = line + klen + 1;
            if (*v == '"') { v++; char* q = strchr((char*)v, '"'); if (q) *q = '\0'; }
            snprintf(out, outSz, "%s", v);
            return out[0] != '\0';
        }
        line = nl ? nl + 1 : NULL;
    }
    return false;
}

// Best-effort container / hypervisor detection using only file reads (no fork, unlike systemd-detect-virt).
static void exception_detectVirt() {
    if (access("/.dockerenv", F_OK) == 0) { snprintf(sys_virt, sizeof(sys_virt), "docker"); return; }
    int fd = open("/proc/1/cgroup", O_RDONLY);
    if (fd >= 0) {
        char buf[1024]; ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
        if (n > 0) { buf[n] = '\0';
            if (strstr(buf, "kubepods"))  { snprintf(sys_virt, sizeof(sys_virt), "kubernetes"); return; }
            if (strstr(buf, "docker"))    { snprintf(sys_virt, sizeof(sys_virt), "docker"); return; }
            if (strstr(buf, "lxc"))       { snprintf(sys_virt, sizeof(sys_virt), "lxc"); return; }
        }
    }
    char prod[128] = "";
    fd = open("/sys/class/dmi/id/product_name", O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, prod, sizeof(prod) - 1); close(fd);
        if (n > 0) { prod[n] = '\0'; char* nl = strchr(prod, '\n'); if (nl) *nl = '\0';
            if (strstr(prod, "VMware") || strstr(prod, "VirtualBox") || strstr(prod, "KVM") ||
                strstr(prod, "QEMU") || strstr(prod, "Virtual Machine") || strstr(prod, "Xen")) {
                snprintf(sys_virt, sizeof(sys_virt), "%s", prod); return; } } }
    if (strstr(sys_kernel, "microsoft") || strstr(sys_kernel, "WSL")) { snprintf(sys_virt, sizeof(sys_virt), "wsl"); return; }
    snprintf(sys_virt, sizeof(sys_virt), "none");
}

static void exception_gatherStaticSystemInfo() {
    sys_procStart = time(NULL);

    struct utsname un; memset(&un, 0, sizeof(un));
    if (uname(&un) == 0) {
        snprintf(sys_kernel, sizeof(sys_kernel), "%s", un.release);
        snprintf(sys_arch,   sizeof(sys_arch),   "%s", un.machine);
    }

    if (!exception_readOsRelease("PRETTY_NAME", sys_distro, sizeof(sys_distro)))
        exception_readOsRelease("NAME", sys_distro, sizeof(sys_distro));

    snprintf(sys_libc, sizeof(sys_libc), "%s", gnu_get_libc_version());

    // CPU model name + (approximate) physical core count from /proc/cpuinfo.
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd >= 0) {
        static char cbuf[131072]; ssize_t n = read(fd, cbuf, sizeof(cbuf) - 1); close(fd);
        if (n > 0) { cbuf[n] = '\0';
            char* mm = strstr(cbuf, "model name");
            if (mm) { char* c = strchr(mm, ':'); char* e = strchr(mm, '\n');
                if (c && e && e > c + 2) { size_t len = (size_t)(e - (c + 2)); if (len >= sizeof(sys_cpuModel)) len = sizeof(sys_cpuModel) - 1;
                    memcpy(sys_cpuModel, c + 2, len); sys_cpuModel[len] = '\0'; } }
            int maxCoreId = -1; char* p = cbuf;
            while ((p = strstr(p, "core id")) != NULL) { char* c = strchr(p, ':'); if (c) { int id = atoi(c + 1); if (id > maxCoreId) maxCoreId = id; } p += 7; }
            sys_cpuPhysical = (maxCoreId >= 0) ? maxCoreId + 1 : 0;
        }
    }

    cpu_set_t set; CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        snprintf(sys_affinity, sizeof(sys_affinity), "%d of %ld", CPU_COUNT(&set), online > 0 ? online : 0L);
    }

    sys_isRoot = (geteuid() == 0) ? 1 : 0;

    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK,  &rl) == 0) sys_ulimitStackKB = (rl.rlim_cur == RLIM_INFINITY) ? -1 : (long)(rl.rlim_cur / 1024);
    if (getrlimit(RLIMIT_CORE,   &rl) == 0) sys_ulimitCoreKB  = (rl.rlim_cur == RLIM_INFINITY) ? -1 : (long)(rl.rlim_cur / 1024);
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) sys_ulimitNoFile  = (rl.rlim_cur == RLIM_INFINITY) ? -1 : (long)rl.rlim_cur;

    exception_detectVirt();
}

// Parse a "Key:\t<N> kB" line from a /proc/self/status buffer. Returns -1 if not found.
static long exception_readProcStatusKB(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    return atol(p);
}

// Builds the full crash JSON into exception_json[].
static void exception_buildJson(int sig, siginfo_t* info, ucontext_t* uc, const char* reportPath) {
    unsigned int eip = 0, esp = 0, ebp = 0;
    greg_t* r = uc ? uc->uc_mcontext.gregs : NULL;
    if (r) { eip = (unsigned int)r[REG_EIP]; esp = (unsigned int)r[REG_ESP]; ebp = (unsigned int)r[REG_EBP]; }

    unsigned long tid = (unsigned long)syscall(SYS_gettid);
    unsigned int faultAddr = (info && sig != SIGABRT) ? (unsigned int)(uintptr_t)info->si_addr : 0;

    char mod[128], sym[256]; unsigned int off;
    exception_resolveFrame(eip, mod, sizeof(mod), &off, sym, sizeof(sym));

    char timeStr[32];
    time_t now = time(NULL); struct tm tmNow; localtime_r(&now, &tmNow);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmNow);

    char fault[256];
    snprintf(fault, sizeof(fault), "%s (code %d) at address 0x%08x%s",
        exception_signalName(sig), info ? info->si_code : 0, faultAddr, faultAddr && faultAddr < 0x10000 ? " (near NULL)" : "");

    char exePath[256] = {0};
    ssize_t el = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (el > 0) exePath[el] = '\0'; else exePath[0] = '\0';

    jreset();
    jraw("{");
    jkv("version", APP_VERSION);
    jkv("type", "linux-server");
    jkv("installId", "");

    jraw("\"crashInfo\":{");
    jkv("kind", "crash");
    jraw("\"exceptionCode\":\"0x%08x\",", (unsigned int)sig);
    jkv("exceptionName", exception_signalName(sig));
    jraw("\"exceptionAddress\":\"0x%08x\",", eip);
    jkv("moduleName", mod[0] ? mod : "<unknown>");
    jraw("\"fileOffset\":\"0x%08x\",", off);
    jkv("faultDescription", fault);
    jraw("\"crashThreadId\":\"%lu\",", tid);
    jraw("\"currentThreadId\":\"%lu\",", tid);
    jkv("time", timeStr);
    jraw("\"test\":%d,", exception_testMode);
    jraw("\"signalCode\":%d", info ? info->si_code : 0);
    jraw("},");

    jraw("\"crashContext\":{");
    jraw("\"registers\":{");
    if (r) {
        jraw("\"EIP\":\"0x%08x\",", eip);
        jraw("\"EAX\":\"0x%08x\",", (unsigned int)r[REG_EAX]);
        jraw("\"EBX\":\"0x%08x\",", (unsigned int)r[REG_EBX]);
        jraw("\"ECX\":\"0x%08x\",", (unsigned int)r[REG_ECX]);
        jraw("\"EDX\":\"0x%08x\",", (unsigned int)r[REG_EDX]);
        jraw("\"ESI\":\"0x%08x\",", (unsigned int)r[REG_ESI]);
        jraw("\"EDI\":\"0x%08x\",", (unsigned int)r[REG_EDI]);
        jraw("\"EBP\":\"0x%08x\",", ebp);
        jraw("\"ESP\":\"0x%08x\",", esp);
        jraw("\"EFLAGS\":\"0x%08x\"", (unsigned int)r[REG_EFL]);
    }
    jraw("},");
    jraw("\"memory\":[");
    exception_jsonMemory("stack", esp, 256); jputc(',');
    exception_jsonMemory("code", eip >= 32 ? eip - 32 : 0, 64);
    jraw("],\"threads\":[],\"stackCodePointers\":[]");
    jraw("},");

    jraw("\"stacks\":[");
    exception_jsonStackByEbp(eip, ebp, tid);
    jputc(',');
    exception_jsonStackByBacktrace(tid);
    jraw("],");

    // --- system / memory (uname + sysinfo) ---
    struct utsname un; memset(&un, 0, sizeof(un));
    uname(&un);
    char host[128] = {0}; gethostname(host, sizeof(host) - 1);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    struct sysinfo si; memset(&si, 0, sizeof(si));
    sysinfo(&si);
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    unsigned long totalMB = (unsigned long)(((unsigned long long)si.totalram * unit) / (1024 * 1024));
    unsigned long freeMB  = (unsigned long)(((unsigned long long)si.freeram  * unit) / (1024 * 1024));

    // --- command line (/proc/self/cmdline: NUL-separated args) ---
    char cmdline[1024] = {0};
    { int cfd = open("/proc/self/cmdline", O_RDONLY);
      if (cfd >= 0) { ssize_t cn = read(cfd, cmdline, sizeof(cmdline) - 1); close(cfd);
        if (cn > 0) { for (ssize_t k = 0; k < cn - 1; k++) if (cmdline[k] == '\0') cmdline[k] = ' '; cmdline[cn] = '\0'; } } }

    // --- process RSS (/proc/self/statm: pages) ---
    unsigned long rssMB = 0;
    { char sm[128] = {0}; int sfd = open("/proc/self/statm", O_RDONLY);
      if (sfd >= 0) { ssize_t sn = read(sfd, sm, sizeof(sm) - 1); close(sfd);
        if (sn > 0) { unsigned long total = 0, rss = 0; if (sscanf(sm, "%lu %lu", &total, &rss) == 2)
            rssMB = (rss * (unsigned long)sysconf(_SC_PAGESIZE)) / (1024 * 1024); } } }

    // --- swap + dynamic memory / thread state ---
    unsigned long swapTotalMB = (unsigned long)(((unsigned long long)si.totalswap * unit) / (1024 * 1024));
    unsigned long swapFreeMB  = (unsigned long)(((unsigned long long)si.freeswap  * unit) / (1024 * 1024));
    long vmPeakMB = -1, vmHWMMB = -1, threadCount = -1;
    { int stfd = open("/proc/self/status", O_RDONLY);
      if (stfd >= 0) { static char stbuf[8192]; ssize_t sn = read(stfd, stbuf, sizeof(stbuf) - 1); close(stfd);
        if (sn > 0) { stbuf[sn] = '\0';
          long vp = exception_readProcStatusKB(stbuf, "VmPeak:"); if (vp > 0) vmPeakMB = vp / 1024;
          long vh = exception_readProcStatusKB(stbuf, "VmHWM:");  if (vh > 0) vmHWMMB  = vh / 1024;
          long th = exception_readProcStatusKB(stbuf, "Threads:"); if (th > 0) threadCount = th; } } }

    jraw("\"systemInfo\":{");
    jraw("\"system\":{");
    char osStr[256]; snprintf(osStr, sizeof(osStr), "%s %s %s", un.sysname, un.release, un.machine);
    jkv("os", osStr);
    jkv("distro", sys_distro);
    jkv("kernel", sys_kernel[0] ? sys_kernel : un.release);
    jkv("arch", sys_arch[0] ? sys_arch : un.machine);
    jkv("libc", sys_libc);
    jkv("cpuModel", sys_cpuModel);
    jkv("hostname", host);
    jraw("\"cpuCores\":\"%ld\",", cores > 0 ? cores : 0);
    jraw("\"cpuCoresPhysical\":\"%d\",", sys_cpuPhysical);
    jkv("cpuAffinity", sys_affinity);
    jkv("virtualization", sys_virt);
    jraw("\"isRoot\":%s,", sys_isRoot ? "true" : "false");
    jraw("\"ulimitStackKB\":\"%ld\",", sys_ulimitStackKB);
    jraw("\"ulimitCoreKB\":\"%ld\",", sys_ulimitCoreKB);
    jraw("\"ulimitNoFile\":\"%ld\",", sys_ulimitNoFile);
    jraw("\"loadAverage\":\"%.2f\",", si.loads[0] / 65536.0);
    jraw("\"uptimeSeconds\":\"%ld\",", (long)si.uptime);
    jraw("\"platform\":\"cod2_lnxded\"}"); jputc(',');

    jraw("\"memory\":{");
    jraw("\"physicalTotalMB\":\"%lu\",", totalMB);
    jraw("\"physicalAvailableMB\":\"%lu\",", freeMB);
    jraw("\"swapTotalMB\":\"%lu\",", swapTotalMB);
    jraw("\"swapFreeMB\":\"%lu\"}", swapFreeMB); jputc(',');

    jraw("\"processMemory\":{");
    jraw("\"workingSetMB\":\"%lu\",", rssMB);
    jraw("\"peakWorkingSetMB\":\"%ld\",", vmHWMMB);
    jraw("\"virtualPeakMB\":\"%ld\",", vmPeakMB);
    jraw("\"threadCount\":\"%ld\",", threadCount);
    jraw("\"processUptimeSeconds\":\"%ld\"}", (long)(time(NULL) - sys_procStart)); jputc(',');

    jraw("\"process\":{");
    jraw("\"processId\":\"%d\",", (int)getpid());
    jraw("\"executablePath\":"); jstr(exePath); jputc(',');
    jkv("reportFile", reportPath);
    jraw("\"commandLine\":"); jstr(cmdline);
    jraw("}"); jputc(',');
    exception_jsonModules(); jputc(',');
    exception_jsonCvars();
    jraw("},");

    static char loggerBuf[LOG_CAPACITY * (LOG_MSG_LEN + 32)];
    logger_get_recent(loggerBuf, sizeof(loggerBuf));
    jlinesArray("loggerLines", loggerBuf);
    jputc(',');
    jlinesArray("logLines", "");
    jraw("}");
}

// Writes exception_json[] to "<base>.pending.json" (open/write only - safe in the signal handler).
static void exception_writePending(const char* reportPath) {
    char jpath[256];
    snprintf(jpath, sizeof(jpath), "%s.pending.json", reportPath);
    int fd = open(jpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, exception_json, exception_jsonPos) < 0) { /* best effort */ }
    close(fd);
}

static void exception_handler(int sig, siginfo_t* info, void* ucontextRaw) {
    // A second fault inside the handler: let the default action end the process
    if (exception_inHandler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    exception_inHandler = 1;

    char path[256];
    time_t now = time(NULL);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    snprintf(path, sizeof(path), "cod2x_crash.%04d%02d%02d_%02d%02d%02d.%d.log",
        tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec, (int)getpid());
    exception_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    char desc[512];

    exception_write("\n=== COD2X CRASH ===\n");
    exception_write("Version:      %s\n", APP_VERSION);
    exception_write("Signal:       %d %s, code %d\n", sig, exception_signalName(sig), info ? info->si_code : 0);
    if (info && sig != SIGABRT) {
        unsigned int faultAddr = (unsigned int)(uintptr_t)info->si_addr;
        exception_write("Fault addr:   0x%08x%s\n", faultAddr, faultAddr < 0x10000 ? " (near NULL)" : "");
    }

    ucontext_t* uc = (ucontext_t*)ucontextRaw;
    unsigned int eip = 0, esp = 0, ebp = 0;
    if (uc) {
        greg_t* r = uc->uc_mcontext.gregs;
        eip = (unsigned int)r[REG_EIP];
        esp = (unsigned int)r[REG_ESP];
        ebp = (unsigned int)r[REG_EBP];
        exception_describeAddress(eip, desc, sizeof(desc));
        exception_write("Address:      0x%08x  %s\n", eip, desc);
        exception_write("\n=== REGISTERS ===\n");
        exception_write("EIP=0x%08x  EAX=0x%08x  EBX=0x%08x  ECX=0x%08x\n",
            eip, (unsigned int)r[REG_EAX], (unsigned int)r[REG_EBX], (unsigned int)r[REG_ECX]);
        exception_write("EDX=0x%08x  ESI=0x%08x  EDI=0x%08x  EBP=0x%08x\n",
            (unsigned int)r[REG_EDX], (unsigned int)r[REG_ESI], (unsigned int)r[REG_EDI], ebp);
        exception_write("ESP=0x%08x  EFL=0x%08x\n", esp, (unsigned int)r[REG_EFL]);
    }

    // EBP chain: [EBP+0] = previous EBP, [EBP+4] = return address
    exception_write("\n=== STACK (EBP chain) ===\n");
    exception_describeAddress(eip, desc, sizeof(desc));
    exception_write("  0  0x%08x  %s\n", eip, desc);
    unsigned int frame = ebp;
    for (int i = 1; i < 64 && frame != 0 && (frame & 3) == 0; i++) {
        unsigned int words[2];
        if (!exception_safeRead(frame, words, sizeof(words)))
            break;
        if (words[1] == 0)
            break;
        exception_describeAddress(words[1], desc, sizeof(desc));
        exception_write("%3d  0x%08x  %s\n", i, words[1], desc);
        if (words[0] <= frame)
            break;
        frame = words[0];
    }

    // glibc backtrace (follows the signal frame, uses unwind info where available)
    exception_write("\n=== STACK (backtrace) ===\n");
    void* frames[64];
    int count = backtrace(frames, 64);
    for (int i = 0; i < count; i++) {
        exception_describeAddress((unsigned int)(uintptr_t)frames[i], desc, sizeof(desc));
        exception_write("%3d  0x%08x  %s\n", i, (unsigned int)(uintptr_t)frames[i], desc);
    }

    // Raw stack
    exception_write("\n=== RAW STACK ===\n");
    unsigned int words[128];
    if (esp && exception_safeRead(esp, words, sizeof(words))) {
        for (int i = 0; i < 128; i++)
            exception_write("%s%08x", (i % 8 == 0) ? (i ? "\n0x" : "0x") : " ", words[i]);
        exception_write("\n");
    }

    // Memory map, so the addresses of all libraries can be resolved later
    exception_write("\n=== MAPS ===\n");
    int maps = open("/proc/self/maps", O_RDONLY);
    if (maps >= 0) {
        char buf[1024];
        ssize_t n;
        while ((n = read(maps, buf, sizeof(buf))) > 0) {
            if (exception_fd >= 0 && write(exception_fd, buf, n) < 0) break;
        }
        close(maps);
        exception_write("(written to %s only)\n", path);
    }

    exception_write("\nCrash report saved to %s\n", path);

    if (exception_fd >= 0) {
        close(exception_fd);
        exception_fd = -1;
    }

    // Build the rich JSON report and queue it for delivery. It is sent later by exception_resendPending()
    // on the next server start - we never run HTTP/OpenSSL inside the crashing process.
    exception_buildJson(sig, info, (ucontext_t*)ucontextRaw, path);
    exception_writePending(path);

    // Default action (core dump / exit with the signal)
    signal(sig, SIG_DFL);
    raise(sig);
}

/** Called before the entry point is called. Used to patch the memory. */
void exception_patch() {
    // Gather the static system environment once now (cheap /proc + libc reads that never change),
    // so the signal handler only has to read the dynamic state.
    exception_gatherStaticSystemInfo();

    // The faulting stack may be exhausted (stack overflow), handle signals on an own stack
    stack_t ss;
    ss.ss_sp = exception_altStack;
    ss.ss_size = sizeof(exception_altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    // backtrace() loads libgcc on its first use, which must not happen inside the signal handler
    void* dummy[1];
    backtrace(dummy, 1);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = exception_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    const int signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        sigaction(signals[i], &sa, NULL);
}

// ---------------------------------------------------------------------------
// Delivery: send queued "<name>.pending.json" reports on the next server start.
//
// A dedicated server auto-restarts under Docker/systemd, so a crash is reported shortly after it
// comes back up. This runs on a normal thread (heap/OpenSSL are fine here), never in the handler.
// ---------------------------------------------------------------------------

// Reports older than this are deleted instead of being sent again.
#define EXCEPTION_PENDING_MAX_AGE_DAYS 14

static bool exception_readFile(const char* path, std::string& out, size_t maxSize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > maxSize) { close(fd); return false; }
    out.resize((size_t)st.st_size);
    ssize_t got = read(fd, &out[0], out.size());
    close(fd);
    return got == (ssize_t)out.size();
}

// Sends one report synchronously; returns true when delivered or permanently rejected (so it can be deleted).
static bool exception_sendOne(const std::string& json) {
    struct Result { bool done = false; bool deletable = false; } result;

    HttpClient client;
    client.postJson(
        TELEMETRY_ENDPOINT_URL,
        json.c_str(),
        [&result](const HttpClient::Response& res) {
            result.done = true;
            // Delivered (2xx) or permanently rejected (4xx except 408/429) -> stop retrying.
            result.deletable = (res.status >= 200 && res.status < 300) ||
                               (res.status >= 400 && res.status < 500 && res.status != 408 && res.status != 429);
            if (res.status >= 200 && res.status < 300)
                logger_add("[EXCEPTION] Report sent successfully (HTTP %d)", res.status);
            else
                logger_add("[EXCEPTION] Report rejected by endpoint (HTTP %d)", res.status);
        },
        [&result](const std::string& err) {
            result.done = true; // transient: keep the file for next start
            logger_add("[EXCEPTION] Failed to send report: %s", err.c_str());
        },
        5000);

    client.poll_max(10000);
    return result.deletable;
}

static void exception_resendPending() {
    DIR* dir = opendir(".");
    if (!dir) return;

    time_t now = time(NULL);
    struct dirent* ent;
    int sent = 0;
    while ((ent = readdir(dir)) != NULL && sent < 20) {
        const char* n = ent->d_name;
        // Match "cod2x_crash.*.pending.json"
        size_t len = strlen(n);
        if (strncmp(n, "cod2x_crash.", 12) != 0) continue;
        const char* suffix = ".pending.json";
        size_t slen = strlen(suffix);
        if (len < slen || strcmp(n + len - slen, suffix) != 0) continue;

        // Too old to be useful -> delete without sending.
        struct stat st;
        if (stat(n, &st) == 0 && now > st.st_mtime &&
            (now - st.st_mtime) > EXCEPTION_PENDING_MAX_AGE_DAYS * 86400) {
            unlink(n);
            continue;
        }

        std::string json;
        if (!exception_readFile(n, json, 8 * 1024 * 1024) || json.empty() || json[0] != '{') {
            unlink(n);
            continue;
        }

        logger_add("[EXCEPTION] Sending pending report %s", n);
        if (exception_sendOne(json))
            unlink(n);
        sent++;
    }
    closedir(dir);
}

// ---------------------------------------------------------------------------
// Crash-handler self test (Linux)
//
// EXCEPTION_TEST=1 compiles the self test into the build (shipping builds use 0). It is NOT a mode.
// With a test build, choose a scenario at launch:  COD2X_EXCEPTION_TEST=<mode> LD_PRELOAD=... ./cod2_lnxded ...
// The scenario fires EXCEPTION_TEST_DELAY_MS after startup; the handler writes the .log + .pending.json and
// the process dies via the default action. In test mode the pending queue is NOT sent, so test crashes
// never reach production - a runner (tools/test/run_exception_tests_linux.sh) inspects the files directly.
// ---------------------------------------------------------------------------
#ifndef EXCEPTION_TEST
#define EXCEPTION_TEST 0
#endif

#if EXCEPTION_TEST
#define EXCEPTION_TEST_DELAY_MS 3000

// 1 - NULL write on the test thread. Expect SIGSEGV, fault at 0x0, crash in libCoD2x.so.
#define EXCEPTION_TEST_SIGSEGV          1
// 2 - abort(). Expect SIGABRT.
#define EXCEPTION_TEST_SIGABRT          2
// 3 - integer divide by zero. Expect SIGFPE.
#define EXCEPTION_TEST_SIGFPE           3
// 4 - illegal instruction (ud2). Expect SIGILL.
#define EXCEPTION_TEST_SIGILL           4
// 5 - unbounded recursion. Expect SIGSEGV via the stack guard page (verifies the sigaltstack path).
#define EXCEPTION_TEST_STACK_OVERFLOW   5
// 6 - NULL write on a second thread. Expect SIGSEGV with crashThreadId != main thread.
#define EXCEPTION_TEST_THREAD           6

static volatile int exception_testZero = 0; // read to defeat constant folding in the divide-by-zero test

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"            // older GCC (18.04 toolchain) doesn't know the next option
#pragma GCC diagnostic ignored "-Winfinite-recursion" // the recursion is intentional (GCC 12+)
static void exception_testRecurse(volatile unsigned int depth) {
    volatile char buffer[8192];
    buffer[0] = (char)depth;
    buffer[sizeof(buffer) - 1] = (char)depth;
    exception_testRecurse(depth + 1);
    (void)buffer[0];
}
#pragma GCC diagnostic pop

static void exception_testTrigger(int mode) {
    switch (mode) {
        case EXCEPTION_TEST_SIGSEGV: {
            volatile int* p = nullptr; *p = 0xDEAD; break;
        }
        case EXCEPTION_TEST_SIGABRT:
            abort();
            break;
        case EXCEPTION_TEST_SIGFPE: {
            volatile int x = 1 / exception_testZero; (void)x; break;
        }
        case EXCEPTION_TEST_SIGILL:
            __builtin_trap();
            break;
        case EXCEPTION_TEST_STACK_OVERFLOW:
            exception_testRecurse(0);
            break;
        case EXCEPTION_TEST_THREAD:
            std::thread([]() {
                volatile int* p = nullptr; *p = 0xDEAD;
            }).join();
            break;
        default:
            logger_add("[EXCEPTION_TEST] unknown mode %d", mode);
            break;
    }
}

// Marker so the runner can confirm exception_init ran and which mode it resolved, even if nothing crashed.
static void exception_testWriteMarker(int mode) {
    char line[128];
    int n = snprintf(line, sizeof(line), "exception_init ran; mode=%d\n", mode);
    int fd = open("cod2x_crash_test_init.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { if (write(fd, line, n) < 0) {} close(fd); }
}
#endif

/** Called once on server start, after the network is up. Sends any crash reports queued from earlier runs. */
void exception_init() {
#if EXCEPTION_TEST
    const char* env = getenv("COD2X_EXCEPTION_TEST");
    int mode = env ? atoi(env) : 0;
    exception_testMode = mode;
    exception_testWriteMarker(mode);
    if (mode > 0) {
        logger_add("[EXCEPTION_TEST] mode %d will trigger in %d ms", mode, EXCEPTION_TEST_DELAY_MS);
        std::thread([mode]() {
            // sigaltstack is per-thread; give this thread its own alternate stack so a stack-overflow
            // crash here can still be reported (the main thread gets one in exception_patch).
            static char testAltStack[64 * 1024];
            stack_t ss; ss.ss_sp = testAltStack; ss.ss_size = sizeof(testAltStack); ss.ss_flags = 0;
            sigaltstack(&ss, NULL);
            usleep(EXCEPTION_TEST_DELAY_MS * 1000);
            exception_testTrigger(mode);
        }).detach();
        return; // do NOT send the pending queue in test mode - test crashes must not reach production
    }
#endif
    std::thread([]() {
        // Give the server a moment to finish starting before using the network.
        sleep(10);
        exception_resendPending();
    }).detach();
}
