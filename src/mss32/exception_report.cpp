// json.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "../shared/json.h"

#include "exception.h"
#include "exception_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <winternl.h>

#include "shared.h"
#include "system.h"
#include "main.h"
#include "console.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"
#include "../shared/logger.h"
#include "../shared/exception.h"

extern char hwid_regid[33];
extern dvar_t* cl_hwid2;

static std::string exception_jsonString(const std::string& value) {
    return "\"" + json_escape_string(value) + "\"";
}

static std::string exception_hex32(unsigned int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", value);
    return buf;
}


// ---------------------------------------------------------------------------
// Data collection
// ---------------------------------------------------------------------------

static void exception_formatDvar(dvar_t* dv, char* out, size_t outSize) {
    if (!dv) {
        snprintf(out, outSize, "<not registered>");
        return;
    }
    switch (dv->type) {
        case DVAR_TYPE_BOOL:
            snprintf(out, outSize, "%s", dv->value.boolean ? "true" : "false");
            break;
        case DVAR_TYPE_INT:
            snprintf(out, outSize, "%d", dv->value.integer);
            break;
        case DVAR_TYPE_FLOAT:
            snprintf(out, outSize, "%.4f", dv->value.decimal);
            break;
        case DVAR_TYPE_ENUM:
            if (dv->limits.enumeration.strings &&
                dv->value.integer >= 0 &&
                dv->value.integer < dv->limits.enumeration.stringCount) {
                snprintf(out, outSize, "%s (%d)", dv->limits.enumeration.strings[dv->value.integer], dv->value.integer);
            } else {
                snprintf(out, outSize, "%d", dv->value.integer);
            }
            break;
        case DVAR_TYPE_STRING:
            snprintf(out, outSize, "%s", dv->value.string ? dv->value.string : "<null>");
            break;
        default:
            snprintf(out, outSize, "<type %d>", (int)dv->type);
            break;
    }
}

// Version from the VS_FIXEDFILEINFO resource of a loaded module (no file access, no version.dll).
static bool exception_getModuleVersion(HMODULE hMod, char* out, size_t outSize) {
    out[0] = '\0';
    HRSRC res = FindResourceA(hMod, MAKEINTRESOURCEA(1), (LPCSTR)RT_VERSION);
    if (!res)
        return false;
    HGLOBAL hRes = LoadResource(hMod, res);
    DWORD size = SizeofResource(hMod, res);
    const BYTE* p = hRes ? (const BYTE*)LockResource(hRes) : NULL;
    if (!p)
        return false;
    for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        const VS_FIXEDFILEINFO* fi = (const VS_FIXEDFILEINFO*)(p + i);
        if (fi->dwSignature == 0xFEEF04BD) {
            snprintf(out, outSize, "%u.%u.%u.%u",
                HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            return true;
        }
    }
    return false;
}

// PE header of a loaded module, read without faulting.
static bool exception_getModuleNtHeaders(HMODULE hMod, IMAGE_NT_HEADERS* nt) {
    IMAGE_DOS_HEADER dos;
    if (exception_safeRead((uintptr_t)hMod, &dos, sizeof(dos)) != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    if (exception_safeRead((uintptr_t)hMod + dos.e_lfanew, nt, sizeof(*nt)) != sizeof(*nt) || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    return true;
}

static void exception_collectProcessMemory(JsonSystemInfoData* out) {
    char temp[64];

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    typedef BOOL (WINAPI *K32GetProcessMemoryInfoFn)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    K32GetProcessMemoryInfoFn pGetProcessMemoryInfo =
        (K32GetProcessMemoryInfoFn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    if (pGetProcessMemoryInfo && pGetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc))) {
        snprintf(temp, sizeof(temp), "%lu", (unsigned long)(pmc.PrivateUsage / (1024 * 1024)));
        out->privateMB = temp;
        snprintf(temp, sizeof(temp), "%lu", (unsigned long)(pmc.WorkingSetSize / (1024 * 1024)));
        out->workingSetMB = temp;
        snprintf(temp, sizeof(temp), "%lu", (unsigned long)(pmc.PeakWorkingSetSize / (1024 * 1024)));
        out->peakWorkingSetMB = temp;
    }

    // Address space fragmentation: a 32-bit process can fail allocations long before memory is used up
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t maxAddr = (uintptr_t)si.lpMaximumApplicationAddress;
    unsigned long long totalFree = 0, largestFree = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < maxAddr && VirtualQuery((void*)addr, &mbi, sizeof(mbi)) != 0) {
        if (mbi.State == MEM_FREE) {
            totalFree += mbi.RegionSize;
            if (mbi.RegionSize > largestFree)
                largestFree = mbi.RegionSize;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr)
            break;
        addr = next;
    }
    snprintf(temp, sizeof(temp), "%llu", largestFree / (1024 * 1024));
    out->largestFreeBlockMB = temp;
    snprintf(temp, sizeof(temp), "%llu", totalFree / (1024 * 1024));
    out->totalFreeAddressSpaceMB = temp;

    IMAGE_NT_HEADERS nt;
    out->largeAddressAware = exception_getModuleNtHeaders(GetModuleHandleA(NULL), &nt) &&
                             (nt.FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE);

    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
        snprintf(temp, sizeof(temp), "%lu", handles);
        out->handleCount = temp;
    }
    snprintf(temp, sizeof(temp), "%lu", GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    out->gdiObjects = temp;
    snprintf(temp, sizeof(temp), "%lu", GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    out->userObjects = temp;
}

static void exception_collectSystemInfo(JsonSystemInfoData* out) {
    char temp[512] = {0};
    SYSTEM_INFO si = {};

    GetSystemInfo(&si);
    snprintf(temp, sizeof(temp), "%lu", si.dwNumberOfProcessors);
    out->cpuCores = temp;

    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
        // Count bits set in procMask
        int affinityCores = 0;
        for (DWORD_PTR m = procMask; m; m &= m - 1) ++affinityCores;
        snprintf(temp, sizeof(temp), "%d (mask=0x%lX)", affinityCores, (unsigned long)procMask);
    } else {
        snprintf(temp, sizeof(temp), "unknown");
    }
    out->cpuAffinity = temp;

    snprintf(temp, sizeof(temp), "%d x %d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    out->screenResolution = temp;

    // Memory
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    snprintf(temp, sizeof(temp), "%lu", ms.dwMemoryLoad);
    out->memoryLoadPercent = temp;
    snprintf(temp, sizeof(temp), "%llu", ms.ullTotalPhys / (1024 * 1024));
    out->physicalTotalMB = temp;
    snprintf(temp, sizeof(temp), "%llu", ms.ullAvailPhys / (1024 * 1024));
    out->physicalAvailableMB = temp;
    snprintf(temp, sizeof(temp), "%llu", ms.ullTotalVirtual / (1024 * 1024));
    out->virtualTotalMB = temp;
    snprintf(temp, sizeof(temp), "%llu", ms.ullAvailVirtual / (1024 * 1024));
    out->virtualAvailableMB = temp;

    exception_collectProcessMemory(out);

    // Process
    snprintf(temp, sizeof(temp), "%lu", GetCurrentProcessId());
    out->processId = temp;
    out->executablePath = EXE_PATH;
    out->commandLine = EXE_COMMAND_LINE;

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    out->workingDirectory = cwd;

    FILETIME creation, exitTime, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER c, n;
        c.LowPart = creation.dwLowDateTime; c.HighPart = creation.dwHighDateTime;
        n.LowPart = now.dwLowDateTime;      n.HighPart = now.dwHighDateTime;
        snprintf(temp, sizeof(temp), "%llu", (n.QuadPart - c.QuadPart) / 10000000ULL);
        out->uptimeSeconds = temp;
    }

    // Game state (globals of CoD2MP_s.exe, always mapped)
    out->clientStateName = get_client_state_name(clientState);
    out->demoPlaying = demo_isPlaying != 0;
    out->frameCount = hook_comFrameCount;
    out->insideComFrame = hook_insideComFrame;

    // Modules
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = {};
        me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) {
            do {
                JsonModuleInfoData module;
                module.baseAddress = exception_hex32((unsigned int)(uintptr_t)me.modBaseAddr);
                module.size = exception_hex32((unsigned int)me.modBaseSize);
                module.moduleName = me.szModule;
                module.path = me.szExePath;
                if (exception_getModuleVersion(me.hModule, temp, sizeof(temp)))
                    module.version = temp;
                IMAGE_NT_HEADERS nt;
                if (exception_getModuleNtHeaders(me.hModule, &nt))
                    module.timestamp = exception_hex32((unsigned int)nt.FileHeader.TimeDateStamp);
                out->modules.push_back(module);
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
    }

    // CVARs (only after Com_Init started, before that the dvar system is not ready)
    if (exception_initDone) {
        const char* cvarNames[] = {
            "cl_hwid2", "name", "fs_game", "mapname", "g_gametype", "sv_running", "dedicated", "developer",
            "com_maxfps", "r_mode", "r_displayRefresh", "r_fullscreen", "r_monitor", "r_aaSamples",
            "r_multiGpu", "r_gpuSync", "r_rendererPreference", "vid_xpos", "vid_ypos",
            "cl_maxpackets", "rate", "snaps", "fs_basepath", "fs_homepath"
        };
        for (size_t i = 0; i < sizeof(cvarNames) / sizeof(cvarNames[0]); ++i) {
            JsonNamedValueData item;
            item.name = cvarNames[i];
            exception_formatDvar(Dvar_GetDvarByName(cvarNames[i]), temp, sizeof(temp));
            item.value = temp;
            out->cvars.push_back(item);
        }
    }

    // Main folder files
    char mainDir[MAX_PATH * 2] = {0};
    snprintf(mainDir, sizeof(mainDir), "%s\\main", EXE_DIRECTORY_PATH);

    DIR* dir = opendir(mainDir);
    if (!dir) {
        JsonMainFolderFileInfoData note;
        note.entryType = "note";
        snprintf(temp, sizeof(temp), "<failed to open '%s'>", mainDir);
        note.message = temp;
        out->mainFolderFiles.push_back(note);
        return;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char* name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        char fullPath[MAX_PATH * 2] = {0};
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", mainDir, name);

        JsonMainFolderFileInfoData entry;
        entry.name = name;
        struct stat st;
        if (stat(fullPath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                entry.entryType = "directory";
            } else {
                entry.entryType = "file";
                snprintf(temp, sizeof(temp), "%lld", (long long)st.st_size);
                entry.size = temp;
            }
        } else {
            entry.entryType = "file";
            entry.size = "?";
        }

        out->mainFolderFiles.push_back(entry);
    }
    closedir(dir);
}

static void exception_addMemoryBlock(JsonCrashContextData* out, const char* name, unsigned int address, size_t length) {
    JsonMemoryBlockData block;
    block.name = name;
    block.address = address;
    if (address < 0x10000) {
        block.note = "near NULL";
    } else {
        block.bytes.resize(length);
        block.bytes.resize(exception_safeRead(address, block.bytes.data(), length));
        if (block.bytes.empty())
            block.note = "unreadable";
    }
    out->memory.push_back(block);
}

static void exception_collectCrashContext(JsonCrashContextData* out, EXCEPTION_POINTERS* pEx) {

    if (pEx && pEx->ContextRecord) {
        CONTEXT* ctx = pEx->ContextRecord;
        out->hasRegisters = true;
        out->eip = exception_hex32(ctx->Eip);
        out->eax = exception_hex32(ctx->Eax);
        out->ebx = exception_hex32(ctx->Ebx);
        out->ecx = exception_hex32(ctx->Ecx);
        out->edx = exception_hex32(ctx->Edx);
        out->esi = exception_hex32(ctx->Esi);
        out->edi = exception_hex32(ctx->Edi);
        out->ebp = exception_hex32(ctx->Ebp);
        out->esp = exception_hex32(ctx->Esp);
        out->eflags = exception_hex32(ctx->EFlags);

        // Raw memory: allows manual unwinding and disassembly when both stack walks fail
        exception_addMemoryBlock(out, "stack", ctx->Esp, 1024);
        exception_addMemoryBlock(out, "code", ctx->Eip >= 32 ? ctx->Eip - 32 : 0, 64);
        const struct { const char* name; DWORD value; } regs[] = {
            {"EAX", ctx->Eax}, {"EBX", ctx->Ebx}, {"ECX", ctx->Ecx}, {"EDX", ctx->Edx},
            {"ESI", ctx->Esi}, {"EDI", ctx->Edi}, {"EBP", ctx->Ebp}
        };
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
            exception_addMemoryBlock(out, regs[i].name, regs[i].value, 64);

        // Raw ESP scan: every stack value that points right after a CALL instruction is a return address candidate.
        // Shows frames both walks miss (callers without frame pointer); old values left on the stack appear too.
        DWORD stackWords[2048];
        size_t got = exception_safeRead(ctx->Esp, stackWords, sizeof(stackWords)) / 4;
        for (size_t i = 0; i < got && out->stackCodePointers.size() < 64; i++) {
            if (!exception_isReturnAddress(stackWords[i]))
                continue;
            char desc[400];
            exception_describeAddress(stackWords[i], desc, sizeof(desc));
            char callSite[200];
            exception_describeCallSite(GetCurrentProcess(), stackWords[i], 0, callSite, sizeof(callSite));
            std::string line;
            exception_appendf(line, "ESP+0x%04X  0x%08X  %-60s  %s", (unsigned int)(i * 4), stackWords[i], desc, callSite);
            out->stackCodePointers.push_back(line);
        }
    }

    HANDLE threadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnap != INVALID_HANDLE_VALUE) {
        typedef LONG (NTAPI *NtQueryInformationThreadFn)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
        NtQueryInformationThreadFn pNtQueryInformationThread = (NtQueryInformationThreadFn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");

        THREADENTRY32 te = {};
        te.dwSize = sizeof(te);
        if (Thread32First(threadSnap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) {
                    continue;
                }
                JsonThreadInfoData thread;
                thread.threadId = (unsigned long)te.th32ThreadID;

                HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (hThread) {
                    if (pNtQueryInformationThread) {
                        void* startAddr = nullptr;
                        if (pNtQueryInformationThread(hThread, (THREADINFOCLASS)9 /*ThreadQuerySetWin32StartAddress*/, &startAddr, sizeof(startAddr), nullptr) == 0 && startAddr) {
                            thread.startAddress = exception_hex32((unsigned int)(uintptr_t)startAddr);
                            char desc[400];
                            exception_describeAddress((unsigned int)(uintptr_t)startAddr, desc, sizeof(desc));
                            thread.startSymbol = desc;
                        }
                    }
                    thread.threadName = exception_getThreadName(hThread);
                    CloseHandle(hThread);
                }

                out->threads.push_back(thread);
            } while (Thread32Next(threadSnap, &te));
        }
        CloseHandle(threadSnap);
    }
}

static bool exception_lockReport() {
    if (!exception_reportLockReady)
        return false;
    // Do not wait forever, the other report could be stuck
    for (int i = 0; i < 200; i++) {
        if (TryEnterCriticalSection(&exception_reportLock))
            return true;
        Sleep(50);
    }
    return false;
}

static void exception_unlockReport(bool locked) {
    if (locked)
        LeaveCriticalSection(&exception_reportLock);
}

// Collects everything that goes into the crash / freeze report.
void exception_collectReport(const ExceptionReportInput& in, CrashReportData& r) {
    bool locked = exception_lockReport();

    HANDLE hProcess = GetCurrentProcess();
    exception_initSymbols();

    r.kind = in.kind;
    r.crashThreadId = in.crashThreadId;
    r.currentThreadId = GetCurrentThreadId();
    if (in.freezeReason) r.freezeReason = in.freezeReason;
    r.frozenMs = in.frozenMs;
    if (in.minidumpFile) r.minidumpFile = in.minidumpFile;
    if (in.time) {
        char t[32];
        snprintf(t, sizeof(t), "%04d-%02d-%02d %02d:%02d:%02d", in.time->wYear, in.time->wMonth, in.time->wDay,
            in.time->wHour, in.time->wMinute, in.time->wSecond);
        r.time = t;
    }

    char moduleName[MAX_PATH] = {0};
    unsigned int moduleBase = 0;
    if (in.pEx && in.pEx->ExceptionRecord) {
        const EXCEPTION_RECORD* rec = in.pEx->ExceptionRecord;
        r.exceptionCode = rec->ExceptionCode;
        r.exceptionAddress = (unsigned int)(uintptr_t)rec->ExceptionAddress;
        if (exception_getModuleForAddress(r.exceptionAddress, moduleName, sizeof(moduleName), &moduleBase))
            r.fileOffset = r.exceptionAddress - moduleBase;

        char fault[512];
        exception_describeFault(rec, fault, sizeof(fault));
        r.faultDescription = fault;
        for (DWORD i = 0; i < rec->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
            r.exceptionParameters.push_back(exception_hex32((unsigned int)rec->ExceptionInformation[i]));

        const EXCEPTION_RECORD* nested = rec->ExceptionRecord;
        for (int depth = 0; nested && depth < 4; depth++) {
            EXCEPTION_RECORD copy;
            if (exception_safeRead((uintptr_t)nested, &copy, sizeof(copy)) != sizeof(copy))
                break;
            std::string line;
            exception_appendf(line, "0x%08lX %s at 0x%08X", (unsigned long)copy.ExceptionCode,
                exception_getText(copy.ExceptionCode), (unsigned int)(uintptr_t)copy.ExceptionAddress);
            r.nestedExceptions.push_back(line);
            nested = copy.ExceptionRecord;
        }
    }
    r.exceptionName = exception_getText(r.exceptionCode);
    r.moduleName = moduleName[0] ? moduleName : (in.pEx ? "unknown module" : "");

    // Stacks: crashing thread (if any) + CoD2 MainThread + CoD2 RenderBackend, each walked by both methods
    std::vector<ExceptionThreadTarget> threadTargets;
    exception_collectTargetThreads(in.crashThreadId, in.pEx, threadTargets);

    for (size_t ti = 0; ti < threadTargets.size(); ++ti) {
        const ExceptionThreadTarget& t = threadTargets[ti];
        if (!t.hasContext)
            continue;

        // Method 1: DbgHelp StackWalk64
        StackCollectionResult sw;
        sw.threadRole = t.role;
        sw.threadName = t.name;
        sw.threadId   = t.threadId;
        exception_walkStackWalk64(hProcess, t, r.exceptionAddress, sw);
        if (t.isCrashing)
            exception_prependCrashFrame0(hProcess, sw, moduleName, moduleBase, r.exceptionAddress, r.fileOffset);
        r.stacks.push_back(sw);

        // Method 2: legacy EBP-chain walk
        StackCollectionResult eb;
        eb.threadRole = t.role;
        eb.threadName = t.name;
        eb.threadId   = t.threadId;
        exception_walkEbpChain(hProcess, t, r.exceptionAddress, eb);
        if (t.isCrashing)
            exception_prependCrashFrame0(hProcess, eb, moduleName, moduleBase, r.exceptionAddress, r.fileOffset);
        r.stacks.push_back(eb);
    }

    exception_collectCrashContext(&r.crashContext, in.pEx);
    exception_collectSystemInfo(&r.systemInfo);

    std::vector<char> loggerBuf(LOG_CAPACITY * (LOG_MSG_LEN + 32) + 1, 0);
    logger_get_recent(loggerBuf.data(), loggerBuf.size());
    r.logger = loggerBuf.data();

    std::vector<char> logsBuf(CONSOLE_MIRROR_CAPACITY + 1, 0);
    console_getLogs(logsBuf.data(), logsBuf.size());
    r.logs = logsBuf.data();

    exception_unlockReport(locked);
}


// ---------------------------------------------------------------------------
// Report text (LF, converted to CRLF when saved / shown)
// ---------------------------------------------------------------------------

static void exception_appendHexDump(std::string& out, unsigned int address, const std::vector<unsigned char>& bytes, unsigned int marker) {
    for (size_t row = 0; row < bytes.size(); row += 16) {
        exception_appendf(out, "  0x%08X%s ", (unsigned int)(address + row),
            (marker >= address + row && marker < address + row + 16) ? ">" : " ");
        for (size_t i = 0; i < 16; i++) {
            if (row + i < bytes.size())
                exception_appendf(out, " %02X", bytes[row + i]);
            else
                out += "   ";
        }
        out += "  ";
        for (size_t i = 0; i < 16 && row + i < bytes.size(); i++) {
            unsigned char c = bytes[row + i];
            out += (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        out += "\n";
    }
}

static void exception_appendDiagnosticsText(std::string& out, const JsonStackDiagnosticsData& d) {
    out += "    [diagnostics]\n";
    exception_appendf(out, "      method: %s\n", d.method.c_str());
    exception_appendf(out, "      unwindFrames: %d\n", d.unwindFrames);
    exception_appendf(out, "      contextRepaired: %s\n", d.contextRepaired ? "yes" : "no");
    exception_appendf(out, "      ebpFallbackFrames: %d\n", d.ebpFallbackFrames);
    exception_appendf(out, "      stackSnapshot: %s\n", d.stackSnapshot ? "yes" : "no");
    if (!d.stopReason.empty())
        exception_appendf(out, "      stopReason: %s\n", d.stopReason.c_str());
    exception_appendf(out, "      symInitialize: %s (code=%lu, message=\"%s\")\n",
        d.symInitializeStatus.c_str(), d.symInitializeCode, d.symInitializeMessage.c_str());
    if (d.stackWalkStatus == "failed")
        exception_appendf(out, "      stackWalk64: failed (code=%lu, message=\"%s\")\n",
            d.stackWalkCode, d.stackWalkMessage.c_str());
    else
        exception_appendf(out, "      stackWalk64: %s\n", d.stackWalkStatus.c_str());
}

std::string exception_buildReportText(const CrashReportData& r) {
    std::string out;
    out.reserve(256 * 1024);
    const bool isFreeze = (r.kind == "freeze");

    // Crash info
    out += isFreeze ? "=== FREEZE INFO ===\n" : "=== CRASH INFO ===\n";
    if (isFreeze) {
        exception_appendf(out, "Reason:       %s\n", r.freezeReason.c_str());
        exception_appendf(out, "Frozen for:   %lu ms\n", r.frozenMs);
    } else {
        exception_appendf(out, "Code:         0x%08X  (%s)\n", r.exceptionCode, r.exceptionName.c_str());
        exception_appendf(out, "Address:      0x%08X  (%s)\n", r.exceptionAddress, r.moduleName.c_str());
        exception_appendf(out, "File Offset:  0x%08X\n", r.fileOffset);
        if (!r.faultDescription.empty())
            exception_appendf(out, "Fault:        %s\n", r.faultDescription.c_str());
        if (!r.exceptionParameters.empty()) {
            out += "Parameters:  ";
            for (const std::string& p : r.exceptionParameters)
                out += " " + p;
            out += "\n";
        }
        for (const std::string& n : r.nestedExceptions)
            exception_appendf(out, "Nested:       %s\n", n.c_str());
    }
    exception_appendf(out, "Version:      %s\n", APP_VERSION);
    exception_appendf(out, "Time:         %s\n", r.time.c_str());
    if (r.testMode)
        exception_appendf(out, "Test mode:    %d\n", r.testMode);
    if (!r.minidumpFile.empty())
        exception_appendf(out, "Minidump:     %s\n", r.minidumpFile.c_str());

    // Crash context
    out += isFreeze ? "\n=== FREEZE CONTEXT ===\n" : "\n=== CRASH CONTEXT ===\n";
    if (!isFreeze)
        exception_appendf(out, "Crash Thread ID:   %lu\n", r.crashThreadId);
    exception_appendf(out, "Current Thread ID: %lu\n", r.currentThreadId);
    const JsonCrashContextData& c = r.crashContext;
    if (c.hasRegisters) {
        out += "\n=== REGISTERS ===\n";
        exception_appendf(out, "EIP=%s  EAX=%s  EBX=%s  ECX=%s\n", c.eip.c_str(), c.eax.c_str(), c.ebx.c_str(), c.ecx.c_str());
        exception_appendf(out, "EDX=%s  ESI=%s  EDI=%s  EBP=%s\n", c.edx.c_str(), c.esi.c_str(), c.edi.c_str(), c.ebp.c_str());
        exception_appendf(out, "ESP=%s  EFL=%s\n", c.esp.c_str(), c.eflags.c_str());
    }

    // Stacks
    static const char colHeader[] =
        "  #  Module                    Address     Offset      Symbol                                    Instruction\n";
    out += "\n=== STACKS ===\n";
    for (const StackCollectionResult& s : r.stacks) {
        exception_appendf(out, "\n--- [%s]  TID %lu  \"%s\"  method: %s ---\n",
            s.threadRole.c_str(), s.threadId, s.threadName.c_str(), s.method.c_str());
        out += colHeader;
        if (s.frames.empty())
            out += "    Stack unavailable\n";
        else
            out += s.text;
        exception_appendDiagnosticsText(out, s.diagnostics);
    }

    // Raw stack scan + memory
    if (!c.stackCodePointers.empty()) {
        out += "\n=== CODE POINTERS ON STACK (raw ESP scan) ===\n";
        for (const std::string& line : c.stackCodePointers)
            out += "  " + line + "\n";
    }
    if (!c.memory.empty()) {
        out += "\n=== MEMORY ===\n";
        unsigned int eip = (unsigned int)strtoul(c.eip.c_str(), NULL, 16);
        for (const JsonMemoryBlockData& m : c.memory) {
            exception_appendf(out, "[%s] 0x%08X%s%s\n", m.name.c_str(), m.address,
                m.note.empty() ? "" : "  ", m.note.c_str());
            exception_appendHexDump(out, m.address, m.bytes, m.name == "code" ? eip : 0xFFFFFFFF);
        }
    }

    // Threads
    out += "\n=== THREADS ===\n";
    for (const JsonThreadInfoData& t : c.threads) {
        exception_appendf(out, "TID=%lu%s%s  START=%s %s  NAME=%s\n",
            t.threadId,
            (!isFreeze && t.threadId == r.crashThreadId) ? " [CRASH]" : "",
            t.threadId == r.currentThreadId ? " [CURRENT]" : "",
            t.startAddress.empty() ? "<unknown>" : t.startAddress.c_str(),
            t.startSymbol.c_str(),
            t.threadName.empty() ? "<unnamed>" : t.threadName.c_str());
    }

    // System
    const JsonSystemInfoData& s = r.systemInfo;
    out += "\n=== SYSTEM ===\n";
    exception_appendf(out, "Windows:        %s %s (build %s)\n", SYS_WIN_EDITION, SYS_WIN_DISPLAY_VERSION, SYS_WIN_BUILD);
    exception_appendf(out, "Virtualization: %s\n", SYS_VIRTUALIZATION);
    exception_appendf(out, "Compatibility:  %s\n", SYS_COMPATIBILITY_FLAGS);
    exception_appendf(out, "Language:       %s\n", SYS_LANGUAGE_CODE);
    exception_appendf(out, "Elevated:       %s\n", SYS_IS_ELEVATED ? "yes" : "no");
    exception_appendf(out, "CPU Cores:      %s (affinity %s)\n", s.cpuCores.c_str(), s.cpuAffinity.c_str());
    exception_appendf(out, "Screen:         %s\n", s.screenResolution.c_str());
    exception_appendf(out, "GPU:            %s\n", SYS_GPU_INFO);

    out += "\n=== MEMORY USAGE ===\n";
    exception_appendf(out, "Memory Load:    %s%%\n", s.memoryLoadPercent.c_str());
    exception_appendf(out, "Physical:       %s MB available of %s MB\n", s.physicalAvailableMB.c_str(), s.physicalTotalMB.c_str());
    exception_appendf(out, "Address space:  %s MB available of %s MB (largest free block %s MB, large address aware: %s)\n",
        s.virtualAvailableMB.c_str(), s.virtualTotalMB.c_str(), s.largestFreeBlockMB.c_str(), s.largeAddressAware ? "yes" : "no");
    exception_appendf(out, "Process:        private %s MB, working set %s MB (peak %s MB)\n",
        s.privateMB.c_str(), s.workingSetMB.c_str(), s.peakWorkingSetMB.c_str());
    exception_appendf(out, "Handles:        %s, GDI objects %s, USER objects %s\n",
        s.handleCount.c_str(), s.gdiObjects.c_str(), s.userObjects.c_str());

    out += "\n=== PROCESS ===\n";
    exception_appendf(out, "PID:            %s\n", s.processId.c_str());
    exception_appendf(out, "Executable:     %s\n", s.executablePath.c_str());
    exception_appendf(out, "Command:        %s\n", s.commandLine.c_str());
    exception_appendf(out, "WorkDir:        %s\n", s.workingDirectory.c_str());
    exception_appendf(out, "Uptime:         %s s\n", s.uptimeSeconds.c_str());

    out += "\n=== GAME STATE ===\n";
    exception_appendf(out, "Client state:   %s\n", s.clientStateName.c_str());
    exception_appendf(out, "Demo playing:   %s\n", s.demoPlaying ? "yes" : "no");
    exception_appendf(out, "Frames:         %u (inside Com_Frame: %s)\n", s.frameCount, s.insideComFrame ? "yes" : "no");

    out += "\n=== MODULES ===\n";
    for (const JsonModuleInfoData& m : s.modules) {
        exception_appendf(out, "%s  %s  %-28s  %-16s  ts=%s  %s\n",
            m.baseAddress.c_str(), m.size.c_str(), m.moduleName.c_str(),
            m.version.empty() ? "-" : m.version.c_str(),
            m.timestamp.empty() ? "?" : m.timestamp.c_str(), m.path.c_str());
    }

    out += "\n=== CVARS ===\n";
    if (s.cvars.empty())
        out += "<not available before Com_Init>\n";
    for (const JsonNamedValueData& v : s.cvars)
        exception_appendf(out, "%-20s = %s\n", v.name.c_str(), v.value.c_str());

    out += "\n=== MAIN FOLDER FILES ===\n";
    for (const JsonMainFolderFileInfoData& f : s.mainFolderFiles) {
        if (f.entryType == "note")
            exception_appendf(out, "%s\n", f.message.c_str());
        else if (f.entryType == "directory")
            exception_appendf(out, "       [DIR]  %s\n", f.name.c_str());
        else
            exception_appendf(out, "  %10s  %s\n", f.size.c_str(), f.name.c_str());
    }

    // Logger ring-buffer (newest first with age offsets)
    out += "\n=== LOGGER ===\n";
    out += r.logger;
    if (!out.empty() && out.back() != '\n')
        out += "\n";

    // Console logs (from crash-safe mirror)
    out += "\n=== LOGS ===\n";
    out += r.logs;
    if (!out.empty() && out.back() != '\n')
        out += "\n";

    return out;
}


// ---------------------------------------------------------------------------
// Report JSON
// ---------------------------------------------------------------------------

// The fields that do not need a crash to be collected, last field of a JSON object
static void exception_jsonAppendSystemFields(std::string& out) {
    out += "\"gpu\":" + exception_jsonString(SYS_GPU_INFO) + ",";
    out += "\"winBuild\":" + exception_jsonString(SYS_WIN_BUILD) + ",";
    out += "\"winEdition\":" + exception_jsonString(SYS_WIN_EDITION) + ",";
    out += "\"winDisplayVersion\":" + exception_jsonString(SYS_WIN_DISPLAY_VERSION) + ",";
    out += "\"language\":" + exception_jsonString(SYS_LANGUAGE_CODE) + ",";
    out += "\"virtualization\":" + exception_jsonString(SYS_VIRTUALIZATION) + ",";
    out += "\"compatibilityFlags\":" + exception_jsonString(SYS_COMPATIBILITY_FLAGS) + ",";
    out += "\"versionInfo\":" + exception_jsonString(SYS_VERSION_INFO) + ",";
    out += "\"isElevated\":" + std::string(SYS_IS_ELEVATED ? "true" : "false");
}

std::string exception_getSystemInfoJson() {
    std::string json = "{";
    exception_jsonAppendSystemFields(json);
    json += "}";
    return json;
}

static void exception_jsonAppendStringArray(std::string& out, const std::vector<std::string>& values) {
    out += "[";
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) out += ",";
        out += exception_jsonString(values[i]);
    }
    out += "]";
}

static void exception_jsonAppendSystemInfoObject(std::string& out, const JsonSystemInfoData& s) {
    out += "\"systemInfo\":{";

    out += "\"system\":{";
    out += "\"cpuCores\":" + exception_jsonString(s.cpuCores) + ",";
    out += "\"cpuAffinity\":" + exception_jsonString(s.cpuAffinity) + ",";
    out += "\"screenResolution\":" + exception_jsonString(s.screenResolution) + ",";
    out += "\"cpuModel\":" + exception_jsonString(SYS_CPU_BRAND) + ",";
    exception_jsonAppendSystemFields(out);
    out += "},";

    out += "\"memory\":{";
    out += "\"memoryLoadPercent\":" + exception_jsonString(s.memoryLoadPercent) + ",";
    out += "\"physicalTotalMB\":" + exception_jsonString(s.physicalTotalMB) + ",";
    out += "\"physicalAvailableMB\":" + exception_jsonString(s.physicalAvailableMB) + ",";
    out += "\"virtualTotalMB\":" + exception_jsonString(s.virtualTotalMB) + ",";
    out += "\"virtualAvailableMB\":" + exception_jsonString(s.virtualAvailableMB);
    out += "},";

    out += "\"processMemory\":{";
    out += "\"privateMB\":" + exception_jsonString(s.privateMB) + ",";
    out += "\"workingSetMB\":" + exception_jsonString(s.workingSetMB) + ",";
    out += "\"peakWorkingSetMB\":" + exception_jsonString(s.peakWorkingSetMB) + ",";
    out += "\"largestFreeBlockMB\":" + exception_jsonString(s.largestFreeBlockMB) + ",";
    out += "\"totalFreeAddressSpaceMB\":" + exception_jsonString(s.totalFreeAddressSpaceMB) + ",";
    out += "\"largeAddressAware\":" + std::string(s.largeAddressAware ? "true" : "false") + ",";
    out += "\"handleCount\":" + exception_jsonString(s.handleCount) + ",";
    out += "\"gdiObjects\":" + exception_jsonString(s.gdiObjects) + ",";
    out += "\"userObjects\":" + exception_jsonString(s.userObjects);
    out += "},";

    out += "\"process\":{";
    out += "\"processId\":" + exception_jsonString(s.processId) + ",";
    out += "\"executablePath\":" + exception_jsonString(s.executablePath) + ",";
    out += "\"commandLine\":" + exception_jsonString(s.commandLine) + ",";
    out += "\"workingDirectory\":" + exception_jsonString(s.workingDirectory) + ",";
    out += "\"uptimeSeconds\":" + exception_jsonString(s.uptimeSeconds);
    out += "},";

    out += "\"gameState\":{";
    out += "\"clientState\":" + exception_jsonString(s.clientStateName) + ",";
    out += "\"demoPlaying\":" + std::string(s.demoPlaying ? "true" : "false") + ",";
    out += "\"frameCount\":" + std::to_string(s.frameCount) + ",";
    out += "\"insideComFrame\":" + std::string(s.insideComFrame ? "true" : "false");
    out += "},";

    out += "\"modules\":[";
    for (size_t i = 0; i < s.modules.size(); ++i) {
        const JsonModuleInfoData& m = s.modules[i];
        if (i > 0) out += ",";
        out += "{";
        out += "\"baseAddress\":" + exception_jsonString(m.baseAddress) + ",";
        out += "\"size\":" + exception_jsonString(m.size) + ",";
        out += "\"moduleName\":" + exception_jsonString(m.moduleName) + ",";
        out += "\"path\":" + exception_jsonString(m.path) + ",";
        out += "\"version\":" + exception_jsonString(m.version) + ",";
        out += "\"timestamp\":" + exception_jsonString(m.timestamp);
        out += "}";
    }
    out += "],";

    out += "\"cvars\":{";
    for (size_t i = 0; i < s.cvars.size(); ++i) {
        if (i > 0) out += ",";
        out += exception_jsonString(s.cvars[i].name) + ":" + exception_jsonString(s.cvars[i].value);
    }
    out += "},";

    out += "\"mainFolderFiles\":[";
    for (size_t i = 0; i < s.mainFolderFiles.size(); ++i) {
        const JsonMainFolderFileInfoData& entry = s.mainFolderFiles[i];
        if (i > 0) out += ",";
        out += "{";
        out += "\"entryType\":" + exception_jsonString(entry.entryType);
        if (!entry.size.empty())
            out += ",\"size\":" + exception_jsonString(entry.size);
        if (!entry.name.empty())
            out += ",\"name\":" + exception_jsonString(entry.name);
        if (!entry.message.empty())
            out += ",\"message\":" + exception_jsonString(entry.message);
        out += "}";
    }
    out += "]";

    out += "}";
}

static void exception_jsonAppendCrashContextObject(std::string& out, const JsonCrashContextData& c) {
    out += "\"crashContext\":{";
    out += "\"registers\":{";
    out += "\"EIP\":" + exception_jsonString(c.eip) + ",";
    out += "\"EAX\":" + exception_jsonString(c.eax) + ",";
    out += "\"EBX\":" + exception_jsonString(c.ebx) + ",";
    out += "\"ECX\":" + exception_jsonString(c.ecx) + ",";
    out += "\"EDX\":" + exception_jsonString(c.edx) + ",";
    out += "\"ESI\":" + exception_jsonString(c.esi) + ",";
    out += "\"EDI\":" + exception_jsonString(c.edi) + ",";
    out += "\"EBP\":" + exception_jsonString(c.ebp) + ",";
    out += "\"ESP\":" + exception_jsonString(c.esp) + ",";
    out += "\"EFLAGS\":" + exception_jsonString(c.eflags);
    out += "},";

    out += "\"threads\":[";
    for (size_t i = 0; i < c.threads.size(); ++i) {
        const JsonThreadInfoData& thread = c.threads[i];
        if (i > 0) out += ",";
        out += "{";
        out += "\"threadId\":\"" + std::to_string(thread.threadId) + "\",";
        out += "\"startAddress\":" + exception_jsonString(thread.startAddress) + ",";
        out += "\"startSymbol\":" + exception_jsonString(thread.startSymbol) + ",";
        out += "\"threadName\":" + exception_jsonString(thread.threadName);
        out += "}";
    }
    out += "],";

    out += "\"memory\":[";
    for (size_t i = 0; i < c.memory.size(); ++i) {
        const JsonMemoryBlockData& m = c.memory[i];
        if (i > 0) out += ",";
        std::string hex;
        hex.reserve(m.bytes.size() * 2);
        for (unsigned char b : m.bytes)
            exception_appendf(hex, "%02x", b);
        out += "{";
        out += "\"name\":" + exception_jsonString(m.name) + ",";
        out += "\"address\":" + exception_jsonString(exception_hex32(m.address)) + ",";
        out += "\"hex\":\"" + hex + "\"";
        if (!m.note.empty())
            out += ",\"note\":" + exception_jsonString(m.note);
        out += "}";
    }
    out += "],";

    out += "\"stackCodePointers\":";
    exception_jsonAppendStringArray(out, c.stackCodePointers);

    out += "}";
}

static void exception_jsonAppendFramesArray(std::string& out, const std::vector<JsonStackFrameData>& frames) {
    out += "[";
    for (size_t i = 0; i < frames.size(); ++i) {
        const JsonStackFrameData& frame = frames[i];
        if (i > 0) out += ",";
        out += "{";
        out += "\"frameNumber\":" + std::to_string(frame.frameNumber) + ",";
        out += "\"instructionAddress\":" + exception_jsonString(exception_hex32(frame.instructionAddress)) + ",";
        out += "\"moduleOffset\":" + exception_jsonString(exception_hex32(frame.moduleOffset)) + ",";
        out += "\"moduleName\":" + exception_jsonString(frame.moduleName) + ",";
        out += "\"sectionName\":" + exception_jsonString(frame.sectionName) + ",";
        out += "\"resolvedSymbol\":" + exception_jsonString(frame.resolvedSymbol) + ",";
        out += "\"instruction\":" + exception_jsonString(frame.instruction);
        out += "}";
    }
    out += "]";
}

static void exception_jsonAppendDiagnosticsObject(std::string& out, const JsonStackDiagnosticsData& d) {
    out += "{";
    out += "\"method\":" + exception_jsonString(d.method) + ",";
    out += "\"unwindFrames\":" + std::to_string(d.unwindFrames) + ",";
    out += "\"contextRepaired\":" + std::string(d.contextRepaired ? "true" : "false") + ",";
    out += "\"ebpFallbackAttempted\":" + std::string(d.ebpFallbackAttempted ? "true" : "false") + ",";
    out += "\"ebpFallbackFrames\":" + std::to_string(d.ebpFallbackFrames) + ",";
    out += "\"rawEspScanAttempted\":" + std::string(d.rawEspScanAttempted ? "true" : "false") + ",";
    out += "\"rawEspScanFrames\":" + std::to_string(d.rawEspScanFrames) + ",";
    out += "\"stackSnapshot\":" + std::string(d.stackSnapshot ? "true" : "false") + ",";
    out += "\"stopReason\":" + exception_jsonString(d.stopReason) + ",";
    out += "\"symInitializeStatus\":" + exception_jsonString(d.symInitializeStatus) + ",";
    out += "\"symInitializeCode\":" + std::to_string(d.symInitializeCode) + ",";
    out += "\"symInitializeMessage\":" + exception_jsonString(d.symInitializeMessage) + ",";
    out += "\"stackWalkStatus\":" + exception_jsonString(d.stackWalkStatus) + ",";
    out += "\"stackWalkCode\":" + std::to_string(d.stackWalkCode) + ",";
    out += "\"stackWalkMessage\":" + exception_jsonString(d.stackWalkMessage);
    out += "}";
}

static void exception_jsonAppendStacksArray(std::string& out, const std::vector<StackCollectionResult>& stacks) {
    out += "\"stacks\":[";
    for (size_t s = 0; s < stacks.size(); ++s) {
        const StackCollectionResult& st = stacks[s];
        if (s > 0) out += ",";
        out += "{";
        out += "\"threadRole\":" + exception_jsonString(st.threadRole) + ",";
        out += "\"threadName\":" + exception_jsonString(st.threadName) + ",";
        out += "\"threadId\":\"" + std::to_string(st.threadId) + "\",";
        out += "\"method\":" + exception_jsonString(st.method) + ",";
        out += "\"frames\":";
        exception_jsonAppendFramesArray(out, st.frames);
        out += ",\"diagnostics\":";
        exception_jsonAppendDiagnosticsObject(out, st.diagnostics);
        out += "}";
    }
    out += "]";
}

std::string exception_buildReportJson(const CrashReportData& r) {
    const char* hwid2 = (exception_initDone && cl_hwid2 && cl_hwid2->value.string) ? cl_hwid2->value.string : "";

    std::string json;
    json.reserve(64 * 1024 + r.logger.size() + r.logs.size());

    json += "{";
    json += "\"version\":" + exception_jsonString(APP_VERSION) + ",";
    json += "\"type\":\"win-client\",";
    json += "\"installId\":" + exception_jsonString(hwid_regid) + ",";
    json += "\"hwid2\":" + exception_jsonString(hwid2) + ",";

    json += "\"crashInfo\":{";
    json += "\"kind\":" + exception_jsonString(r.kind) + ",";
    json += "\"exceptionCode\":" + exception_jsonString(exception_hex32(r.exceptionCode)) + ",";
    json += "\"exceptionName\":" + exception_jsonString(r.exceptionName) + ",";
    json += "\"exceptionAddress\":" + exception_jsonString(exception_hex32(r.exceptionAddress)) + ",";
    json += "\"moduleName\":" + exception_jsonString(r.moduleName) + ",";
    json += "\"fileOffset\":" + exception_jsonString(exception_hex32(r.fileOffset)) + ",";
    json += "\"faultDescription\":" + exception_jsonString(r.faultDescription) + ",";
    json += "\"exceptionParameters\":";
    exception_jsonAppendStringArray(json, r.exceptionParameters);
    json += ",\"nestedExceptions\":";
    exception_jsonAppendStringArray(json, r.nestedExceptions);
    json += ",\"crashThreadId\":\"" + std::to_string(r.crashThreadId) + "\",";
    json += "\"currentThreadId\":\"" + std::to_string(r.currentThreadId) + "\",";
    json += "\"freezeReason\":" + exception_jsonString(r.freezeReason) + ",";
    json += "\"frozenMs\":" + std::to_string(r.frozenMs) + ",";
    json += "\"minidumpFile\":" + exception_jsonString(r.minidumpFile) + ",";
    json += "\"time\":" + exception_jsonString(r.time) + ",";
    json += "\"test\":" + std::to_string(r.testMode);   // >0 marks a self-test report; backend acks without storing
    json += "},";

    exception_jsonAppendCrashContextObject(json, r.crashContext);
    json += ",";
    exception_jsonAppendStacksArray(json, r.stacks);
    json += ",";
    exception_jsonAppendSystemInfoObject(json, r.systemInfo);
    json += ",";
    json_appendLines(json, "loggerLines", r.logger.c_str());
    json += ",";
    json_appendLines(json, "logLines", r.logs.c_str());
    json += "}";

    return json;
}

