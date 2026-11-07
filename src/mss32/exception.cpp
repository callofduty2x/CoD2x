#include "../shared/http_client.h"

#include "exception.h"
#include "exception_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <stdexcept>
#include <dbghelp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <exception>

#include "shared.h"
#include "main.h"
#include "../shared/cod2_shared.h"
#include "../shared/logger.h"
#include "../shared/exception.h"

// Global HttpClient pointer for polling the sent report during crash dialog
HttpClient* g_crashDialogHttpClient = nullptr;

// ---------------------------------------------------------------------------
// Crash-handler self test
//
// EXCEPTION_TEST=1 compiles the self test into the build (shipping builds use 0). It is meant for a
// release test build: `make build_win_test` / `cmake -DEXCEPTION_TEST=ON`. It is NOT a specific mode.
//
// With a test build, choose a scenario at launch with the command line:  +set exception_test <mode>
// e.g.  CoD2MP_s.exe +set exception_test 4
// The selected scenario fires EXCEPTION_TEST_DELAY_MS after start. In test mode the crash handler writes
// the report files, then exits without the dialog, telemetry or restart, so a test runner can catch them.
// Each mode value and the report it should produce are documented at its EXCEPTION_TEST_* #define below.
// ---------------------------------------------------------------------------

// Compiled in only when requested; overridable from the compiler command line (-DEXCEPTION_TEST=1).
#ifndef EXCEPTION_TEST
#define EXCEPTION_TEST 0
#endif

// 1 - NULL write patched into CL_Frame. The game's main thread runs the patched code.
//     Expect: ACCESS_VIOLATION, "Write to address 0x00000000". Crashing thread = "CoD2 MainThread",
//     fault in CoD2MP_s.exe at CL_Frame+0x2; stacks show CL_Frame <- hook_CL_Frame <- Com_Frame.
#define EXCEPTION_TEST_NULL_WRITE_MAIN      1

// 2 - NULL write executed directly in the CoD2x test thread (not a game thread).
//     Expect: ACCESS_VIOLATION, "Write to address 0x00000000". Crashing thread = "CoD2x ExceptionTest",
//     fault in mss32.dll inside exception_testThread. Verifies reporting of a crash on a non-game thread.
#define EXCEPTION_TEST_NULL_WRITE_DLL       2

// 3 - NULL write patched into RB_EndFrame. The renderer's backend thread runs the patched code.
//     Expect: ACCESS_VIOLATION. Crashing thread = "CoD2 RenderBackend", fault in gfx_d3d_mp_x86_s.dll.
//     Only works on the client with the renderer loaded (dedicated 0). Verifies a crash in the renderer module.
#define EXCEPTION_TEST_NULL_WRITE_RENDERER  3

// 4 - NULL write patched into a ddraw.dll export, then called from the test thread.
//     Expect: ACCESS_VIOLATION. Crashing thread = "CoD2x ExceptionTest", fault in DDRAW.dll (nearest export shown).
//     Verifies a crash located in a system / graphics module and that it is attributed to that module.
#define EXCEPTION_TEST_NULL_WRITE_DDRAW     4

// 5 - Unbounded recursion on the test thread.
//     Expect: EXCEPTION_STACK_OVERFLOW handled by the vectored handler (the report runs on a fresh thread);
//     crashing thread labelled as "CoD2x ExceptionTest". Verifies the stack-overflow hand-off path.
#define EXCEPTION_TEST_STACK_OVERFLOW       5

// 6 - Deterministic heap fast-fail (int 29h) on the test thread. (A real heap overflow is unreliable -
//     the allocator usually tolerates it silently, so the process would not crash at all.)
//     Expect: NO CoD2x report. On Windows 10/11 a heap-metadata fast-fail bypasses SEH and vectored
//     handlers and terminates the process directly - it cannot be caught in-process (only out-of-process,
//     e.g. WER LocalDumps). This mode documents and guards that limit: the process must terminate promptly
//     without the handler hanging or producing a false report. The vectored handler still lists these
//     codes for older Windows / Wine, where they may be delivered.
#define EXCEPTION_TEST_HEAP_CORRUPTION      6

// 7 - abort() called from CoD2x code on the test thread.
//     Expect: COD2X_FATAL_ERROR, fault description "abort() called in CoD2x". Verifies the SIGABRT handler.
#define EXCEPTION_TEST_CRT_ABORT            7

// 8 - Uncaught C++ exception thrown from CoD2x code on the test thread.
//     Expect: COD2X_FATAL_ERROR, "std::terminate called in CoD2x ...". Verifies the std::terminate handler.
#define EXCEPTION_TEST_CPP_TERMINATE        8

// 9 - NULL write (as mode 1), and the handler itself faults once near the end of reporting.
//     Expect: a normal crash report, followed by a "*** Secondary exception ... stage: exit ***" line
//     appended to the .txt. Verifies the re-entry guard / secondary-exception note.
#define EXCEPTION_TEST_SECONDARY            9

#define EXCEPTION_TEST_DELAY_MS 5000

// Scenario selected at launch via "+set exception_test <mode>"; 0 = no test. Set in exception_init.
static volatile int exception_testMode = 0;

#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION 0xC0000374
#endif
#ifndef STATUS_STACK_BUFFER_OVERRUN
#define STATUS_STACK_BUFFER_OVERRUN 0xC0000409
#endif
#define EXCEPTION_MSVC_CPP 0xE06D7363

// How long the crash handler keeps sending the report after the dialog was closed.
#define EXCEPTION_SEND_WAIT_MS 5000

// How many old crash / freeze files are kept in the game folder.
#define EXCEPTION_KEEP_DUMPS   5
#define EXCEPTION_KEEP_REPORTS 20

// Value 0 = no crash in progress; >= 1 = crash handler is running.
volatile long exception_processCrashed = 0;

// Thread that is running the crash handler. A fault on this thread is a secondary exception inside the handler,
// a fault on another thread is parked until the report is finished.
static volatile LONG exception_ownerThreadId = 0;

// Current step of the crash handler, written to the report when the handler itself crashes.
static const char* volatile exception_stage = "";

// Timestamp captured once at the start of the crash handler; shared by the .txt, .dmp and .pending.json filenames.
static SYSTEMTIME exception_crashTime;

// Full path of the crash .txt report (known after the first stage of the handler).
static char exception_reportPath[MAX_PATH * 2] = {0};

// Fatal exceptions that can not be handled on the faulting thread (stack overflow, heap corruption) are handed off
// to a new thread. The ID of the faulting thread is stored here so the report labels it correctly.
static volatile DWORD exception_handoffThreadId = 0;
static volatile LONG exception_handoffCaught = 0;

// Set in exception_init (Com_Init). Before that dvars are not registered yet.
bool exception_initDone = false;

static PVOID exception_vectoredHandle = NULL;
CRITICAL_SECTION exception_reportLock;
bool exception_reportLockReady = false;



// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void exception_appendf(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) {
        va_end(argsCopy);
        return;
    }
    if ((size_t)n < sizeof(buf)) {
        out.append(buf, (size_t)n);
    } else {
        std::vector<char> big((size_t)n + 1);
        vsnprintf(big.data(), big.size(), fmt, argsCopy);
        out.append(big.data(), (size_t)n);
    }
    va_end(argsCopy);
}

// Append into a fixed buffer without heap allocations (used by the first, minimal stage of the crash handler).
void exception_bufAppend(char* buf, size_t size, size_t* pos, const char* fmt, ...) {
    if (*pos + 1 >= size)
        return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, size - *pos, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    *pos += (size_t)n;
    if (*pos >= size)
        *pos = size - 1;
}

// Read memory of this process without faulting. Reads page by page, so a block crossing into an
// unreadable page still returns the readable beginning. Returns the number of bytes read.
size_t exception_safeRead(uintptr_t addr, void* out, size_t len) {
    size_t done = 0;
    while (done < len) {
        uintptr_t cur = addr + done;
        size_t pageLeft = 0x1000 - (cur & 0xFFF);
        size_t chunk = (len - done < pageLeft) ? (len - done) : pageLeft;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)cur, (BYTE*)out + done, chunk, &got) || got != chunk)
            break;
        done += chunk;
    }
    return done;
}

// Module containing addr: short file name and base address. Returns false when the address is not inside a module.
bool exception_getModuleForAddress(unsigned int addr, char* name, size_t nameSize, unsigned int* base) {
    if (name && nameSize) name[0] = '\0';
    if (base) *base = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)(uintptr_t)addr, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == NULL)
        return false;
    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path)) == 0)
        return false;
    const char* shortName = strrchr(path, '\\');
    shortName = shortName ? shortName + 1 : path;
    if (name && nameSize)
        snprintf(name, nameSize, "%s", shortName);
    if (base)
        *base = (unsigned int)(uintptr_t)mbi.AllocationBase;
    return true;
}

// Builds "<game folder>\CoD2MP_s.exe.<kind>.YYYYMMDD_HHMMSS<suffix>"
static void exception_buildFilePath(char* out, size_t outSize, const char* kind, const SYSTEMTIME& st, const char* suffix) {
    snprintf(out, outSize, "%s%sCoD2MP_s.exe.%s.%04d%02d%02d_%02d%02d%02d%s",
        EXE_DIRECTORY_PATH, EXE_DIRECTORY_PATH[0] ? "\\" : "",
        kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, suffix);
}

static bool exception_writeFile(const char* path, const char* data, size_t len, bool append) {
    HANDLE hFile = CreateFileA(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, NULL,
        append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data, (DWORD)len, &written, NULL);
    CloseHandle(hFile);
    return ok && written == (DWORD)len;
}

// Convert LF to CRLF so the Win32 EDIT control and Notepad render newlines correctly
static std::string exception_toCrlf(const std::string& in) {
    std::string out;
    out.reserve(in.size() + in.size() / 16 + 16);
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r'))
            out += '\r';
        out += in[i];
    }
    return out;
}


// ---------------------------------------------------------------------------
// Exception description
// ---------------------------------------------------------------------------

/*
 * Determine the exception name based on the exception code
 */
const char* exception_getText(DWORD exceptionCode) {
    switch (exceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
        case EXCEPTION_INVALID_HANDLE:           return "INVALID_HANDLE";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case STATUS_HEAP_CORRUPTION:             return "HEAP_CORRUPTION";
        case STATUS_STACK_BUFFER_OVERRUN:        return "STACK_BUFFER_OVERRUN";
        case EXCEPTION_MSVC_CPP:                 return "CPP_EXCEPTION";
        case EXCEPTION_COD2X_CRT_RUNTIME_ERROR:  return "CRT_RUNTIME_ERROR";
        case EXCEPTION_COD2X_CRT_SECURITY_ERROR: return "CRT_BUFFER_OVERRUN";
        case EXCEPTION_COD2X_FATAL:              return "COD2X_FATAL_ERROR";
        case 0:                                  return "APPLICATION_FREEZE";
        default:                                 return "Unknown Exception";
    }
}

// Copies a CRT / CoD2x message string from an exception parameter, replacing line breaks.
static void exception_copyMessageParam(ULONG_PTR ptr, char* out, size_t outSize) {
    out[0] = '\0';
    if (!ptr)
        return;
    size_t got = exception_safeRead(ptr, out, outSize - 1);
    out[got] = '\0';
    for (char* p = out; *p; p++) {
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    }
}

// Human-readable description of what went wrong, without heap allocations.
// Example: "Read of address 0x00000010 (NULL pointer + 0x10)".
void exception_describeFault(const EXCEPTION_RECORD* rec, char* out, size_t outSize) {
    out[0] = '\0';
    if (!rec)
        return;

    switch (rec->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR: {
            if (rec->NumberParameters < 2)
                break;
            ULONG_PTR type = rec->ExceptionInformation[0];
            unsigned int addr = (unsigned int)rec->ExceptionInformation[1];
            const char* op = (type == 0) ? "Read of" : (type == 1) ? "Write to" : (type == 8) ? "Execution (DEP) of" : "Access to";
            char nearNull[48] = {0};
            if (addr < 0x10000)
                snprintf(nearNull, sizeof(nearNull), " (NULL pointer + 0x%X)", addr);
            if (rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && rec->NumberParameters >= 3)
                snprintf(out, outSize, "%s address 0x%08X%s failed to page in, NTSTATUS 0x%08lX",
                    op, addr, nearNull, (unsigned long)rec->ExceptionInformation[2]);
            else
                snprintf(out, outSize, "%s address 0x%08X%s", op, addr, nearNull);
            break;
        }
        case EXCEPTION_COD2X_CRT_RUNTIME_ERROR: {
            char msg[256];
            exception_copyMessageParam(rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0, msg, sizeof(msg));
            snprintf(out, outSize, "Microsoft Visual C++ Runtime error %lu: %s",
                rec->NumberParameters >= 1 ? (unsigned long)rec->ExceptionInformation[0] : 0UL,
                msg[0] ? msg : "<unknown>");
            break;
        }
        case EXCEPTION_COD2X_CRT_SECURITY_ERROR:
            snprintf(out, outSize, "Buffer overrun detected by the game (/GS security check, code %lu)",
                rec->NumberParameters >= 1 ? (unsigned long)rec->ExceptionInformation[0] : 0UL);
            break;
        case EXCEPTION_COD2X_FATAL: {
            char msg[256];
            exception_copyMessageParam(rec->NumberParameters >= 1 ? rec->ExceptionInformation[0] : 0, msg, sizeof(msg));
            snprintf(out, outSize, "%s", msg[0] ? msg : "CoD2x fatal error");
            break;
        }
        case EXCEPTION_MSVC_CPP: {
            // [0] = magic, [1] = thrown object, [2] = ThrowInfo*
            // ThrowInfo+0x0C -> CatchableTypeArray, +0x04 -> CatchableType[0], +0x04 -> TypeDescriptor, +0x08 -> decorated name
            char typeName[128] = {0};
            if (rec->NumberParameters >= 3) {
                DWORD throwInfo = (DWORD)rec->ExceptionInformation[2];
                DWORD catchableArray = 0, catchableType = 0, typeDescriptor = 0;
                if (exception_safeRead(throwInfo + 0x0C, &catchableArray, 4) == 4 &&
                    exception_safeRead(catchableArray + 0x04, &catchableType, 4) == 4 &&
                    exception_safeRead(catchableType + 0x04, &typeDescriptor, 4) == 4) {
                    size_t got = exception_safeRead(typeDescriptor + 0x08, typeName, sizeof(typeName) - 1);
                    typeName[got] = '\0';
                }
            }
            snprintf(out, outSize, "Uncaught C++ exception of type %s", typeName[0] ? typeName : "<unknown>");
            break;
        }
        case STATUS_HEAP_CORRUPTION:
            snprintf(out, outSize, "The process heap is corrupted (memory was written out of bounds or freed twice)");
            break;
        case EXCEPTION_STACK_OVERFLOW:
            snprintf(out, outSize, "The thread used all of its stack (infinite recursion or a huge local array)");
            break;
        default:
            break;
    }
}


// ---------------------------------------------------------------------------
// Minidump
// ---------------------------------------------------------------------------

struct MiniDumpThreadArgs {
    EXCEPTION_POINTERS* pEx;
    DWORD threadId;
    bool fullMemory;
    const char* path;
    bool ok;
};

// Small dump: global variables (data sections) only of the game modules. The data sections of all modules
// would add tens of MB of driver data (nvgpucomp32.dll alone has 50 MB).
static BOOL CALLBACK exception_miniDumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) {
    (void)param;
    if (!input || !output || input->CallbackType != ModuleCallback || !(output->ModuleWriteFlags & ModuleWriteDataSeg))
        return TRUE;

    const wchar_t* path = input->Module.FullPath;
    const wchar_t* name = path ? wcsrchr(path, L'\\') : NULL;
    name = name ? name + 1 : (path ? path : L"");
    if (_wcsicmp(name, L"CoD2MP_s.exe") != 0 && _wcsicmp(name, L"gfx_d3d_mp_x86_s.dll") != 0 && _wcsicmp(name, L"mss32.dll") != 0)
        output->ModuleWriteFlags &= ~ModuleWriteDataSeg;
    return TRUE;
}

static DWORD WINAPI exception_miniDumpThread(LPVOID param) {
    MiniDumpThreadArgs* args = (MiniDumpThreadArgs*)param;
    args->ok = false;

    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = args->threadId;
    mdei.ExceptionPointers = args->pEx;
    mdei.ClientPointers = FALSE;

    MINIDUMP_CALLBACK_INFORMATION callback;
    callback.CallbackRoutine = exception_miniDumpCallback;
    callback.CallbackParam = NULL;

    // Small dump: stacks + memory referenced from them + globals of all modules (CoD2 keeps its whole state in globals)
    MINIDUMP_TYPE types[2];
    if (args->fullMemory) {
        types[0] = (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        types[1] = MiniDumpWithFullMemory;
    } else {
        types[0] = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                   MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
        types[1] = MiniDumpNormal; // old dbghelp.dll does not know the newer flags
    }

    for (int i = 0; i < 2 && !args->ok; i++) {
        HANDLE hFile = CreateFileA(args->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return 0;
        args->ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, types[i],
                                     args->pEx ? &mdei : NULL, NULL, args->fullMemory ? NULL : &callback) != FALSE;
        CloseHandle(hFile);
    }
    if (!args->ok)
        DeleteFileA(args->path);
    return 0;
}

/**
 * Writes a minidump into the game folder: "CoD2MP_s.exe.<kind>.<time>.dmp" (or ".full.dmp").
 * The dump is written from a helper thread (fresh stack, the dump of the faulting thread is not disturbed)
 * with a timeout, so a deadlock inside DbgHelp can not hang the crash handler forever.
 * threadId is the faulting thread (pExceptionInfo belongs to it), 0 = current thread.
 */
bool exception_createMiniDump(EXCEPTION_POINTERS* pExceptionInfo, DWORD threadId, bool fullMemory, const char* kind, char* pathOut, size_t pathOutSize) {
    SYSTEMTIME st;
    if (strcmp(kind, "crash") == 0 && exception_crashTime.wYear != 0)
        st = exception_crashTime;
    else
        GetLocalTime(&st);

    char path[MAX_PATH * 2];
    exception_buildFilePath(path, sizeof(path), kind, st, fullMemory ? ".full.dmp" : ".dmp");

    MiniDumpThreadArgs args = { pExceptionInfo, threadId ? threadId : GetCurrentThreadId(), fullMemory, path, false };

    HANDLE hThread = CreateThread(NULL, 0, exception_miniDumpThread, &args, 0, NULL);
    if (hThread) {
        DWORD wait = WaitForSingleObject(hThread, fullMemory ? 180000 : 30000);
        if (wait != WAIT_OBJECT_0) {
            // The thread still uses 'args' on our stack; it is never safe to return while it runs
            TerminateThread(hThread, 1);
            args.ok = false;
        }
        CloseHandle(hThread);
    } else {
        exception_miniDumpThread(&args);
    }

    if (args.ok && pathOut && pathOutSize > 0)
        snprintf(pathOut, pathOutSize, "%s", path);
    return args.ok;
}


// ---------------------------------------------------------------------------
// Old report files and resending
// ---------------------------------------------------------------------------

// Lists "<game folder>\<pattern>" sorted by name (the timestamp in the name sorts chronologically).
static std::vector<std::string> exception_listGameFolderFiles(const char* pattern) {
    std::vector<std::string> names;
    char search[MAX_PATH * 2];
    snprintf(search, sizeof(search), "%s\\%s", EXE_DIRECTORY_PATH, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.push_back(fd.cFileName);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    std::sort(names.begin(), names.end());
    return names;
}

// Keeps only the newest 'keep' files matching the pattern.
static void exception_cleanupOldFiles(const char* pattern, size_t keep) {
    std::vector<std::string> names = exception_listGameFolderFiles(pattern);
    for (size_t i = 0; i + keep < names.size(); i++) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s\\%s", EXE_DIRECTORY_PATH, names[i].c_str());
        DeleteFileA(path);
    }
}

// Sends reports that could not be delivered last time (offline, firewall, process killed while sending).
static DWORD WINAPI exception_resendPendingThread(LPVOID) {
    thread_setName(GetCurrentThread(), L"CoD2x CrashReportResend");

    // Do not compete with the game start for the network and disk
    Sleep(10000);

    exception_resendPending(EXE_DIRECTORY_PATH, "CoD2MP_s.exe.", true);
    return 0;
}


// ---------------------------------------------------------------------------
// First stage of the crash handler: minimal report without heap allocations
// ---------------------------------------------------------------------------

static char exception_minimalBuffer[16 * 1024];

static void exception_writeMinimalReport(EXCEPTION_POINTERS* pEx, DWORD crashThreadId) {
    char* buf = exception_minimalBuffer;
    const size_t size = sizeof(exception_minimalBuffer);
    size_t pos = 0;

    const EXCEPTION_RECORD* rec = pEx->ExceptionRecord;
    unsigned int addr = (unsigned int)(uintptr_t)rec->ExceptionAddress;
    char moduleName[MAX_PATH] = {0};
    unsigned int moduleBase = 0;
    exception_getModuleForAddress(addr, moduleName, sizeof(moduleName), &moduleBase);
    char fault[512];
    exception_describeFault(rec, fault, sizeof(fault));

    exception_bufAppend(buf, size, &pos,
        "=== CRASH INFO (minimal report, the full report was not completed) ===\r\n"
        "Code:         0x%08lX  (%s)\r\n"
        "Address:      0x%08X  (%s)\r\n"
        "File Offset:  0x%08X\r\n"
        "Fault:        %s\r\n"
        "Version:      %s\r\n"
        "Crash Thread: %lu\r\n",
        (unsigned long)rec->ExceptionCode, exception_getText(rec->ExceptionCode),
        addr, moduleName[0] ? moduleName : "unknown module",
        moduleBase ? addr - moduleBase : 0,
        fault, APP_VERSION, (unsigned long)crashThreadId);

    const CONTEXT* ctx = pEx->ContextRecord;
    if (ctx) {
        exception_bufAppend(buf, size, &pos,
            "\r\n=== REGISTERS ===\r\n"
            "EIP=0x%08lX  EAX=0x%08lX  EBX=0x%08lX  ECX=0x%08lX\r\n"
            "EDX=0x%08lX  ESI=0x%08lX  EDI=0x%08lX  EBP=0x%08lX\r\n"
            "ESP=0x%08lX  EFL=0x%08lX\r\n",
            ctx->Eip, ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp, ctx->EFlags);

        DWORD words[128];
        size_t got = exception_safeRead(ctx->Esp, words, sizeof(words)) / 4;
        exception_bufAppend(buf, size, &pos, "\r\n=== RAW STACK ===\r\n");
        for (size_t i = 0; i < got; i++) {
            if (i % 8 == 0)
                exception_bufAppend(buf, size, &pos, "%s0x%08lX:", i ? "\r\n" : "", (unsigned long)(ctx->Esp + i * 4));
            exception_bufAppend(buf, size, &pos, " %08lX", (unsigned long)words[i]);
        }
        exception_bufAppend(buf, size, &pos, "\r\n");
    }

    exception_writeFile(exception_reportPath, buf, pos, false);
}

// The crash handler itself crashed: note where in the report, then end the process.
static void exception_writeSecondaryNote(EXCEPTION_POINTERS* pEx) {
    char note[512];
    int n = snprintf(note, sizeof(note),
        "\r\n*** Secondary exception 0x%08lX at 0x%08X inside the crash handler, stage: %s ***\r\n",
        (unsigned long)pEx->ExceptionRecord->ExceptionCode,
        (unsigned int)(uintptr_t)pEx->ExceptionRecord->ExceptionAddress,
        exception_stage);
    if (n > 0 && exception_reportPath[0])
        exception_writeFile(exception_reportPath, note, (size_t)n, true);
}


// ---------------------------------------------------------------------------
// Crash handler
// ---------------------------------------------------------------------------

static LONG exception_handleCrash(EXCEPTION_POINTERS* pExceptionInfo, DWORD crashThreadId) {
    InterlockedIncrement(&exception_processCrashed);

    // In self-test mode the report files are written but the dialog, telemetry and restart are skipped,
    // so the test runner gets the files and the process exits on its own.
    const bool testMode = exception_testMode != 0;

    // Capture the crash timestamp once so the .txt, .dmp and .pending.json files share the same name.
    GetLocalTime(&exception_crashTime);
    exception_buildFilePath(exception_reportPath, sizeof(exception_reportPath), "crash", exception_crashTime, ".txt");
    char pendingPath[MAX_PATH * 2];
    exception_buildFilePath(pendingPath, sizeof(pendingPath), "crash", exception_crashTime, EXCEPTION_PENDING_SUFFIX);

    // Stage 1: the essentials, without heap allocations, in case the rest of the handler fails
    exception_stage = "minimal report";
    exception_writeMinimalReport(pExceptionInfo, crashThreadId);

    // Stage 2: small minidump, before collecting changes anything
    exception_stage = "minidump";
    char dumpPath[MAX_PATH * 2] = {0};
    exception_createMiniDump(pExceptionInfo, crashThreadId, false, "crash", dumpPath, sizeof(dumpPath));

    // Stage 3: full report
    exception_stage = "collect report";
    CrashReportData report;
    ExceptionReportInput input;
    input.pEx = pExceptionInfo;
    input.crashThreadId = crashThreadId;
    input.kind = "crash";
    input.minidumpFile = dumpPath[0] ? dumpPath : nullptr;
    input.time = &exception_crashTime;
    exception_collectReport(input, report);

    report.testMode = exception_testMode;

    exception_stage = "build report";
    std::string crashJson = exception_buildReportJson(report);
    std::string displayText = exception_toCrlf(exception_buildReportText(report) + "\n=== JSON ===\n" + crashJson + "\n");

    exception_stage = "save report";
    exception_writeFile(exception_reportPath, displayText.c_str(), displayText.size(), false);

    // Stage 4: send the report. The JSON is saved first and deleted once delivered, so a report that could not be sent
    // (offline, process killed while sending) is sent on the next game start.
    // The report is sent for real crashes and for self-test crashes alike (self-test reports carry
    // crashInfo.test and the backend acks them without storing). Only real crashes are queued offline.
    exception_stage = "send report";
    #if !DEBUG || EXCEPTION_TEST
      {
        static HttpClient httpClient;
        if (!testMode)
            exception_writeFile(pendingPath, crashJson.c_str(), crashJson.size(), false);

        InterlockedExchange(&g_crashSendSpinner, 0);
        InterlockedExchange(&g_crashSendState, CRASH_SEND_SENDING);
        g_crashSendError[0] = '\0';
        g_crashDialogHttpClient = &httpClient;  // Allow crash dialog to poll for the sent report
        exception_postReport(httpClient, crashJson, pendingPath, EXCEPTION_SEND_WAIT_MS,
            [](bool delivered, const std::string& error) {
                snprintf(g_crashSendError, sizeof(g_crashSendError), "%s", error.c_str());
                InterlockedExchange(&g_crashSendState, delivered ? CRASH_SEND_SUCCESS : CRASH_SEND_FAILED);
            });
      }
    #endif

    exception_stage = "dialog";
    bool wantRestart = false;
    if (testMode)
        logger_add("[EXCEPTION_TEST] report written to %s, exiting without dialog", exception_reportPath);
    else
        wantRestart = exception_showCrashDialog(displayText.c_str(), pExceptionInfo, crashThreadId, exception_reportPath);

    // The dialog is often closed before the report is delivered, give it a moment to finish
    exception_stage = "send report wait";
    if (g_crashDialogHttpClient) {
        DWORD waitStart = GetTickCount();
        while (InterlockedCompareExchange(&g_crashSendState, 0, 0) == CRASH_SEND_SENDING &&
               GetTickCount() - waitStart < EXCEPTION_SEND_WAIT_MS) {
            g_crashDialogHttpClient->poll(20);
        }
        g_crashDialogHttpClient = nullptr;
    }

    // In self-test mode there is no dialog; record the delivery outcome so the E2E runner can verify it.
    if (testMode) {
        LONG st = InterlockedCompareExchange(&g_crashSendState, 0, 0);
        char line[384];
        snprintf(line, sizeof(line), "\r\nTelemetry: %s%s%s\r\n",
            st == CRASH_SEND_SUCCESS ? "sent" : st == CRASH_SEND_FAILED ? "failed" : "pending",
            (st == CRASH_SEND_FAILED && g_crashSendError[0]) ? " - " : "",
            (st == CRASH_SEND_FAILED && g_crashSendError[0]) ? g_crashSendError : "");
        exception_writeFile(exception_reportPath, line, strlen(line), true);
    }

    if (wantRestart) {
        exception_stage = "restart";
        // Relaunch the game with the same command-line arguments
        char cmdLine[MAX_PATH * 2];
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" %s", EXE_PATH, EXE_COMMAND_LINE);
        STARTUPINFOA startInfo = {};
        startInfo.cb = sizeof(startInfo);
        PROCESS_INFORMATION procInfo = {};
        if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL,
                           EXE_DIRECTORY_PATH, &startInfo, &procInfo)) {
            CloseHandle(procInfo.hProcess);
            CloseHandle(procInfo.hThread);
        }
    }

    exception_stage = "exit";

#if EXCEPTION_TEST
    if (exception_testMode == EXCEPTION_TEST_SECONDARY) {
        // Intentionally crash inside the handler AFTER the report, to verify that the
        // secondary-exception path writes a note and ends the process.
        volatile int* nullPtr = nullptr;
        *nullPtr = 0xDEAD;
    }
#endif

    // Return EXCEPTION_EXECUTE_HANDLER to let the app exit
    return EXCEPTION_EXECUTE_HANDLER;
}

/** Called when exception happend */
LONG WINAPI exception_handler(EXCEPTION_POINTERS* pExceptionInfo) {
    const DWORD self = GetCurrentThreadId();

    // Only one thread reports a crash. The previous game filter (MSVC CRT __CxxUnhandledExceptionFilter) is not
    // chained: it calls terminate() for uncaught C++ exceptions and does nothing useful otherwise.
    LONG owner = InterlockedCompareExchange(&exception_ownerThreadId, (LONG)self, 0);
    if (owner != 0) {
        if ((DWORD)owner == self) {
            // A secondary exception occurred inside the handler. Note it and end the process.
            exception_writeSecondaryNote(pExceptionInfo);
            TerminateProcess(GetCurrentProcess(), pExceptionInfo->ExceptionRecord->ExceptionCode);
        }
        // Another thread crashed while the first crash is being reported (e.g. main thread after the render
        // thread crashed). Returning would terminate the process and kill the crash dialog, so park this thread;
        // the process ends when the report is finished.
        for (;;)
            Sleep(INFINITE);
    }

    // When a fatal exception was handed off from the vectored handler, the faulting thread ID is stored
    // in exception_handoffThreadId; otherwise the current thread IS the faulting thread.
    const DWORD crashThreadId = exception_handoffThreadId ? (DWORD)exception_handoffThreadId : self;

    return exception_handleCrash(pExceptionInfo, crashThreadId);
}

/**
 * Reports a fatal error that is not a CPU exception (CRT runtime error, std::terminate, abort) through the
 * crash handler, with the context of the caller. Never returns.
 */
static void exception_reportFatal(DWORD code, DWORD numberParameters, const ULONG_PTR* parameters) {
    CONTEXT context;
    RtlCaptureContext(&context);

    EXCEPTION_RECORD record = {};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = (PVOID)(uintptr_t)context.Eip;
    record.NumberParameters = numberParameters;
    for (DWORD i = 0; i < numberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        record.ExceptionInformation[i] = parameters[i];

    EXCEPTION_POINTERS pointers = { &record, &context };
    exception_handler(&pointers);

    TerminateProcess(GetCurrentProcess(), 3);
    for (;;)
        Sleep(INFINITE);
}


// ---------------------------------------------------------------------------
// Vectored handler for exceptions that can not be handled on the faulting thread
//
// SetUnhandledExceptionFilter is never reached for EXCEPTION_STACK_OVERFLOW
// because the Windows exception dispatcher itself faults when it tries to build
// its dispatch frame on the already-exhausted stack (secondary 0xc0000005).
// A vectored handler is invoked earlier, while the CPU's guard-page guarantee
// still has a few hundred bytes remaining. Heap corruption is handled the same
// way, since the corrupted heap must not be used on the faulting thread - but note
// that modern Windows heap corruption uses __fastfail, which bypasses vectored
// handlers entirely; this path only catches it on older Windows / Wine.
// The exception records are copied to static memory (no heap) and a fresh thread
// runs the full crash handler.
// ---------------------------------------------------------------------------

struct ExceptionHandoffArgs {
    EXCEPTION_RECORD   exRecord;
    CONTEXT            exContext;
    EXCEPTION_POINTERS exPtrs;   // points into the two fields above
};
static ExceptionHandoffArgs exception_handoffArgs;

static DWORD WINAPI exception_handoffThread(LPVOID lpParam) {
    ExceptionHandoffArgs* args = (ExceptionHandoffArgs*)lpParam;
    exception_handler(&args->exPtrs);
    TerminateProcess(GetCurrentProcess(), args->exRecord.ExceptionCode);
    return 0;
}

static LONG WINAPI exception_vectoredHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_STACK_OVERFLOW && code != STATUS_HEAP_CORRUPTION && code != STATUS_STACK_BUFFER_OVERRUN)
        return EXCEPTION_CONTINUE_SEARCH;

    // Fatal exception inside the crash handler itself
    if ((DWORD)exception_ownerThreadId == GetCurrentThreadId()) {
        exception_writeSecondaryNote(pExceptionInfo);
        TerminateProcess(GetCurrentProcess(), code);
    }

    // Only the first one is handed off; park any concurrent one (the process ends after the report).
    if (InterlockedCompareExchange(&exception_handoffCaught, 1, 0) != 0) {
        for (;;)
            SuspendThread(GetCurrentThread());
    }

    // Record the faulting thread so exception_handler labels the thread list correctly.
    InterlockedExchange((volatile LONG*)(void*)&exception_handoffThreadId, (LONG)GetCurrentThreadId());

    ExceptionHandoffArgs* args = &exception_handoffArgs;
    args->exRecord               = *pExceptionInfo->ExceptionRecord;
    args->exRecord.ExceptionRecord = NULL;
    args->exContext              = *pExceptionInfo->ContextRecord;
    args->exPtrs.ExceptionRecord = &args->exRecord;
    args->exPtrs.ContextRecord   = &args->exContext;

    HANDLE hCrash = CreateThread(NULL, 0, exception_handoffThread, args, 0, NULL);
    if (hCrash) {
        CloseHandle(hCrash);
        // Suspend the faulting thread so the crash handler can safely read its stack.
        // The crash thread will TerminateProcess when done.
        for (;;)
            SuspendThread(GetCurrentThread());
    }

    // Fallback: if CreateThread failed, terminate immediately.
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH; // unreachable
}


// ---------------------------------------------------------------------------
// Fatal errors that are not exceptions
// ---------------------------------------------------------------------------

// 00585c56    void _NMSG_WRITE(int32_t rterrnum)
// Statically linked MSVC 7.1 CRT of CoD2MP_s.exe. Hooked in _amsg_exit, __amsg_exit and abort, where it is called
// right before the CRT ends the process (R6025 pure virtual function call, R6016 not enough space for thread data, ...).
static void _NMSG_WRITE(int rterrnum)
{
    // CoD2x: report the runtime error through the crash handler instead of the CRT message box.
    // exception_reportFatal never returns, the original function is not called.
    ULONG_PTR params[2] = { (ULONG_PTR)rterrnum, (ULONG_PTR)_GET_RTERRMSG(rterrnum) };
    exception_reportFatal(EXCEPTION_COD2X_CRT_RUNTIME_ERROR, 2, params);
    // CoD2x: End
}

// 0058b6cb    void __security_error_handler(int32_t code, void* data) __noreturn
// Statically linked MSVC 7.1 CRT of CoD2MP_s.exe. Called when the /GS cookie check detects a buffer overrun.
static void __security_error_handler(int code, void* data)
{
    (void)data;

    // CoD2x: report the buffer overrun through the crash handler instead of the CRT message box.
    // exception_reportFatal never returns, the original function is not called.
    ULONG_PTR params[1] = { (ULONG_PTR)code };
    exception_reportFatal(EXCEPTION_COD2X_CRT_SECURITY_ERROR, 1, params);
    // CoD2x: End
}

// std::terminate in CoD2x code (uncaught C++ exception, pure virtual call in libstdc++)
static void exception_terminateHandler() {
    static const char message[] = "std::terminate called in CoD2x (uncaught C++ exception or pure virtual call)";
    ULONG_PTR params[1] = { (ULONG_PTR)message };
    exception_reportFatal(EXCEPTION_COD2X_FATAL, 1, params);
}

// abort() in CoD2x code (msvcrt.dll)
static void exception_abortSignalHandler(int sig) {
    (void)sig;
    static const char message[] = "abort() called in CoD2x";
    ULONG_PTR params[1] = { (ULONG_PTR)message };
    exception_reportFatal(EXCEPTION_COD2X_FATAL, 1, params);
}


// ---------------------------------------------------------------------------
// Freeze report
// ---------------------------------------------------------------------------

/**
 * Reports a freeze: saves "CoD2MP_s.exe.freeze.<time>.txt" with the stacks of CoD2 MainThread and
 * CoD2 RenderBackend and sends it to the telemetry endpoint (kind "freeze"). Blocks for a few seconds.
 */
void exception_reportFreeze(const char* reason, DWORD frozenMs) {
    static volatile LONG reporting = 0;
    static int reportsSent = 0;

    if (exception_processCrashed || reportsSent >= 3)
        return;
    if (InterlockedCompareExchange(&reporting, 1, 0) != 0)
        return;
    reportsSent++;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char txtPath[MAX_PATH * 2], pendingPath[MAX_PATH * 2];
    exception_buildFilePath(txtPath, sizeof(txtPath), "freeze", st, ".txt");
    exception_buildFilePath(pendingPath, sizeof(pendingPath), "freeze", st, EXCEPTION_PENDING_SUFFIX);

    CrashReportData report;
    ExceptionReportInput input;
    input.kind = "freeze";
    input.freezeReason = reason;
    input.frozenMs = frozenMs;
    input.time = &st;
    exception_collectReport(input, report);

    std::string json = exception_buildReportJson(report);
    std::string text = exception_toCrlf(exception_buildReportText(report) + "\n=== JSON ===\n" + json + "\n");
    exception_writeFile(txtPath, text.c_str(), text.size(), false);
    logger_add("Freeze report saved to %s", txtPath);

    #if !DEBUG
        exception_writeFile(pendingPath, json.c_str(), json.size(), false);
        exception_sendReport(json, pendingPath, 7000);
    #endif

    InterlockedExchange(&reporting, 0);
}


// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

#if EXCEPTION_TEST

// Machine code that writes to address 0 and faults at the "mov [eax], eax" (offset +2).
// Patched over a function so that executing that function raises an access violation inside its module.
static const unsigned char EXCEPTION_TEST_CRASH_CODE[5] = {
    0x31, 0xC0,       // xor eax, eax
    0x89, 0x00,       // mov [eax], eax   <- faults here
    0xC3              // ret (not reached)
};

// Unbounded recursion for the stack-overflow test. volatile + the touch after the call keep the
// recursion from being turned into a loop or a tail call (also built with -O0).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static void exception_testRecurse(volatile unsigned int depth) {
    volatile char buffer[8192];
    buffer[0] = (char)depth;
    buffer[sizeof(buffer) - 1] = (char)depth;
    exception_testRecurse(depth + 1);
    (void)buffer[0];
}
#pragma GCC diagnostic pop

static DWORD WINAPI exception_testThread(LPVOID) {
    thread_setName(GetCurrentThread(), L"CoD2x ExceptionTest");
    Sleep(EXCEPTION_TEST_DELAY_MS);

    switch (exception_testMode) {

    case EXCEPTION_TEST_NULL_WRITE_MAIN:
    case EXCEPTION_TEST_SECONDARY:
        // Patch CL_Frame so the main thread faults inside CoD2MP_s.exe.
        patch_copy(0x0040f850, (void*)EXCEPTION_TEST_CRASH_CODE, sizeof(EXCEPTION_TEST_CRASH_CODE));
        break;

    case EXCEPTION_TEST_NULL_WRITE_DLL: {
        // Fault here, on this thread, inside mss32.dll.
        volatile int* nullPtr = nullptr;
        *nullPtr = 0xDEAD;
        break;
    }

    case EXCEPTION_TEST_NULL_WRITE_RENDERER:
        // Patch RB_EndFrame so the render backend thread faults inside gfx_d3d_mp_x86_s.dll.
        if (gfx_module_addr == 0) {
            logger_add("[EXCEPTION_TEST] renderer not loaded, cannot run EXCEPTION_TEST_NULL_WRITE_RENDERER");
            break;
        }
        patch_copy(gfx_module_addr + 0x00033ed0, (void*)EXCEPTION_TEST_CRASH_CODE, sizeof(EXCEPTION_TEST_CRASH_CODE));
        break;

    case EXCEPTION_TEST_NULL_WRITE_DDRAW: {
        // Patch a ddraw.dll export and call it, so the fault is inside DDRAW.dll.
        HMODULE hDDraw = GetModuleHandleA("ddraw.dll");
        void* fn = hDDraw ? (void*)GetProcAddress(hDDraw, "DirectDrawCreate") : nullptr;
        if (!fn) {
            logger_add("[EXCEPTION_TEST] ddraw.dll / DirectDrawCreate not found");
            break;
        }
        patch_copy((unsigned int)(uintptr_t)fn, (void*)EXCEPTION_TEST_CRASH_CODE, sizeof(EXCEPTION_TEST_CRASH_CODE));
        ((void (*)())fn)();
        break;
    }

    case EXCEPTION_TEST_STACK_OVERFLOW:
        exception_testRecurse(0);
        break;

    case EXCEPTION_TEST_HEAP_CORRUPTION:
        // A real heap overflow is unreliable - the allocator often tolerates it silently. Instead raise the
        // exact fast-fail the Windows heap uses on detected metadata corruption: int 29h with the fast-fail
        // code in ECX. This deterministically terminates the process and (on Win10/11) bypasses SEH and
        // vectored handlers, modelling the uncatchable crash class this mode documents.
        __asm__ volatile("int $0x29" : : "c"(2 /*FAST_FAIL_HEAP_METADATA_CORRUPTION*/));
        break;

    case EXCEPTION_TEST_CRT_ABORT:
        abort();
        break;

    case EXCEPTION_TEST_CPP_TERMINATE:
        throw std::runtime_error("EXCEPTION_TEST uncaught C++ exception");

    default:
        logger_add("[EXCEPTION_TEST] unknown mode %d", exception_testMode);
        break;
    }

    return 0;
}

// Reads the self-test mode from the command line: "+set exception_test <n>" or "exception_test <n>".
// Parsed directly from the command line because exception_init runs before the dvar system is ready.
static int exception_parseTestModeFromCmdLine(const char* cmdLine) {
    if (!cmdLine)
        return 0;
    const char* p = strstr(cmdLine, "exception_test");
    if (!p)
        return 0;
    p += strlen("exception_test");
    while (*p == ' ' || *p == '\t')
        ++p;
    return atoi(p);
}

// Resolves the self-test mode. The environment variable COD2X_EXCEPTION_TEST (set by the test runner)
// wins, because it is inherited reliably; otherwise the command line is used.
static int exception_resolveTestMode() {
    const char* env = getenv("COD2X_EXCEPTION_TEST");
    if (env && env[0])
        return atoi(env);
    return exception_parseTestModeFromCmdLine(EXE_COMMAND_LINE);
}

// Writes a one-line marker next to the game so the test runner can confirm exception_init ran and what
// it resolved, even when no crash follows (e.g. the mode was 0 or the command line was not captured).
static void exception_writeTestInitMarker(int mode) {
    const char* env = getenv("COD2X_EXCEPTION_TEST");
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s%sCoD2MP_s.exe.exception_test_init.log",
        EXE_DIRECTORY_PATH, EXE_DIRECTORY_PATH[0] ? "\\" : "");
    char line[MAX_PATH * 3];
    int n = snprintf(line, sizeof(line),
        "exception_init ran; resolved mode=%d; env COD2X_EXCEPTION_TEST=%s; EXE_COMMAND_LINE=\"%s\"\r\n",
        mode, env ? env : "(unset)", EXE_COMMAND_LINE);
    if (n > 0)
        exception_writeFile(path, line, (size_t)n, false);
}
#endif

/** Called every frame. The unhandled exception filter is checked every few seconds, other DLLs (overlays, drivers) may replace it. */
void exception_frame() {
    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (now - lastCheck < 5000)
        return;
    lastCheck = now;

    LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(exception_handler);
    if (current != exception_handler) {
        char desc[400];
        exception_describeAddress((unsigned int)(uintptr_t)current, desc, sizeof(desc));
        logger_add("Unhandled exception filter was replaced by 0x%08X %s, restored", (unsigned int)(uintptr_t)current, desc);
        Com_DPrintf("Unhandled exception filter was replaced by 0x%08X %s, restored\n", (unsigned int)(uintptr_t)current, desc);
    }
}

/** Called before the hot-reloaded DLL is unloaded. */
void exception_unload() {
    if (exception_vectoredHandle) {
        RemoveVectoredExceptionHandler(exception_vectoredHandle);
        exception_vectoredHandle = NULL;
    }
    SetUnhandledExceptionFilter(NULL);
    signal(SIGABRT, SIG_DFL);
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void exception_init() {
    exception_initDone = true;

    // The game CRT installed __CxxUnhandledExceptionFilter on top of our filter during its startup, replace it.
    SetUnhandledExceptionFilter(exception_handler);

    // Remove old crash files, keep the newest ones
    exception_cleanupOldFiles("CoD2MP_s.exe.crash.*.dmp", EXCEPTION_KEEP_DUMPS);
    exception_cleanupOldFiles("CoD2MP_s.exe.freeze.*.dmp", EXCEPTION_KEEP_DUMPS);
    exception_cleanupOldFiles("CoD2MP_s.exe.crash.*.txt", EXCEPTION_KEEP_REPORTS);
    exception_cleanupOldFiles("CoD2MP_s.exe.freeze.*.txt", EXCEPTION_KEEP_REPORTS);

    // Send reports that could not be delivered last time
    HANDLE hResend = CreateThread(NULL, 0, exception_resendPendingThread, NULL, 0, NULL);
    if (hResend)
        CloseHandle(hResend);

#if EXCEPTION_TEST
    // Self test: "+set exception_test <mode>" (or the COD2X_EXCEPTION_TEST env var) spawns a thread that
    // triggers the chosen crash scenario after EXCEPTION_TEST_DELAY_MS ms, to verify the crash handler.
    exception_testMode = exception_resolveTestMode();
    exception_writeTestInitMarker(exception_testMode);
    if (exception_testMode > 0) {
        logger_add("[EXCEPTION_TEST] mode %d will trigger in %d ms", exception_testMode, EXCEPTION_TEST_DELAY_MS);
        Com_Printf("[EXCEPTION_TEST] mode %d will trigger in %d ms\n", exception_testMode, EXCEPTION_TEST_DELAY_MS);
        HANDLE hThread = CreateThread(NULL, 0, exception_testThread, NULL, 0, NULL);
        if (hThread)
            CloseHandle(hThread);
    }
#endif
}

/** Called before the entry point is called. Used to patch the memory. */
void exception_patch() {
    if (!exception_reportLockReady) {
        InitializeCriticalSection(&exception_reportLock);
        exception_reportLockReady = true;
    }

    // Vectored handler: catches EXCEPTION_STACK_OVERFLOW and heap corruption before the normal SEH/unhandled
    // filter path, which is unreachable (or unsafe) for them.
    exception_vectoredHandle = AddVectoredExceptionHandler(1 /*CALL_FIRST*/, exception_vectoredHandler);

    // Installed as early as possible so crashes during the game start are reported too.
    // exception_init installs it again after the game CRT replaced it.
    SetUnhandledExceptionFilter(exception_handler);

    // Game CRT runtime errors (would show "Microsoft Visual C++ Runtime Library" message box and exit)
    patch_call(0x0057dac7, (unsigned int)_NMSG_WRITE); // in _amsg_exit
    patch_call(0x0057daec, (unsigned int)_NMSG_WRITE); // in __amsg_exit
    patch_call(0x0057dd92, (unsigned int)_NMSG_WRITE); // in abort

    // Game CRT /GS buffer overrun (would show "Buffer overrun detected!" message box and exit)
    patch_call(0x0058599c, (unsigned int)__security_error_handler); // in report_failure
    patch_call(0x0058b827, (unsigned int)__security_error_handler); // in sub_58b823 (__security_check_cookie failure path)

    // Fatal errors in CoD2x code
    std::set_terminate(exception_terminateHandler);
    signal(SIGABRT, exception_abortSignalHandler);
}
