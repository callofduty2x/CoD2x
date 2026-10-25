#include "../shared/http_client.h"
#include "../shared/json.h"

#include "exception.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <stdexcept>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <string>
#include <vector>
#include <algorithm>
#include <exception>
#include <winternl.h>

#include "shared.h"
#include "gamma.h"
#include "error.h"
#include "system.h"
#include "main.h"
#include "console.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_shared.h"
#include "../shared/cod2_client.h"
#include "../shared/logger.h"
#include "symbols_cod2mp_s.h"
#include "symbols_gfx_d3d_mp_x86_s.h"
#include "symbols_mss32.h"

#define TELEMETRY_ENDPOINT_URL "https://master.cod2x.me/api/telemetry/cod2x-exceptions"
//#define TELEMETRY_ENDPOINT_URL "http://localhost:8080/api/telemetry/cod2x-exceptions"

// Global HttpClient pointer for telemetry polling during crash dialog
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
#define EXCEPTION_TELEMETRY_WAIT_MS 5000

// How many old crash / freeze files are kept in the game folder.
#define EXCEPTION_KEEP_DUMPS   5
#define EXCEPTION_KEEP_REPORTS 20

// Unsent reports older than this are deleted instead of being sent again.
#define EXCEPTION_PENDING_MAX_AGE_DAYS 14

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
static bool exception_initDone = false;

static PVOID exception_vectoredHandle = NULL;
static CRITICAL_SECTION exception_reportLock;
static bool exception_reportLockReady = false;

LONG WINAPI exception_handler(EXCEPTION_POINTERS* pExceptionInfo);

extern char hwid_regid[33];
extern dvar_t* cl_hwid2;


// ---------------------------------------------------------------------------
// Custom crash dialog - modern dark theme
// ---------------------------------------------------------------------------

#define CLR_WIN_BG          RGB(30,  30,  30)   // Neutral dark
#define CLR_EDIT_BG         RGB(18,  18,  18)   // Neutral dark keeps text readable
#define CLR_EDIT_FG         RGB(212, 212, 212)
#define CLR_BTN_RESTART_N   RGB(0,   120, 212)   // Windows blue
#define CLR_BTN_RESTART_P   RGB(0,   90,  160)
#define CLR_BTN_DUMP_N      RGB(68,  68,  68)    // Neutral gray
#define CLR_BTN_DUMP_P      RGB(45,  45,  45)
#define CLR_BTN_EXIT_N      RGB(180, 40,  40)    // Red
#define CLR_BTN_EXIT_P      RGB(140, 20,  20)    // Darker red
#define CLR_BTN_TEXT        RGB(255, 255, 255)

#define IDC_CRASH_EDIT      2001
#define IDC_BTN_RESTART     2002
#define IDC_BTN_EXIT        2003
#define IDC_BTN_DUMP        2004
#define IDC_CRASH_LABEL     2005  // subtitle under heading
#define IDC_CRASH_ICON      2006
#define IDC_CRASH_HEADING   2007
#define IDC_CRASH_SAVEPATH  2008  // saved-file path under subtitle
#define IDC_LNK_DETAILS     2009
#define IDC_CRASH_TELEMETRY 2010
#define CRASH_DLG_CLASS     "CoD2x_CrashDialog"

#define HDR_ICON_SIZE   48
#define HDR_H           100
#define CRASH_COMPACT_CLIENT_W  860
#define CRASH_COMPACT_CLIENT_H  215
#define CRASH_EXPANDED_CLIENT_W 1200
#define CRASH_EXPANDED_CLIENT_H 820
#define CRASH_MIN_CLIENT_W      760
#define CRASH_MIN_CLIENT_H      195

struct CrashDialogData {
    const char*       text;
    const char*       savePath;   // full path to the saved crash .txt file
    EXCEPTION_POINTERS* pExInfo;
    DWORD             crashThreadId;
    bool              wantRestart;
    HFONT             hFont;
    HFONT             hBtnFont;
    HFONT             hLinkFont;
    HFONT             hHeadingFont;
    HBRUSH            hBrushBg;
    HBRUSH            hBrushEdit;
    HCURSOR           hLinkCursor;
    WNDPROC           hEditOrigProc;
    bool              detailsVisible;
    int               compactWidth;
    int               compactHeight;
    int               expandedWidth;
    int               expandedHeight;
};

enum CrashTelemetryState {
    CRASH_TELEMETRY_IDLE = 0,
    CRASH_TELEMETRY_SENDING = 1,
    CRASH_TELEMETRY_SUCCESS = 2,
    CRASH_TELEMETRY_FAILED = 3
};

static volatile LONG g_crashTelemetryState = CRASH_TELEMETRY_IDLE;
static LONG g_crashTelemetrySpinner = 0;
static char g_crashTelemetryError[256] = {0};

// Forward declaration (defined after exception_resizeDialogClient)
static void exception_layoutCrashDlg(HWND hwnd, int w, int h);

static void exception_updateTelemetryLabel(HWND hwnd, CrashDialogData* data) {
    (void)data;

    HWND hTelemetry = GetDlgItem(hwnd, IDC_CRASH_TELEMETRY);
    if (!hTelemetry)
        return;

    char status[512] = {0};
    LONG state = InterlockedCompareExchange(&g_crashTelemetryState, 0, 0);
    switch (state) {
        case CRASH_TELEMETRY_SENDING: {
            static const char* dots[] = {"", ".", "..", "..."};
            LONG spin = InterlockedIncrement(&g_crashTelemetrySpinner);
            const char* suffix = dots[spin % 4];
            snprintf(status, sizeof(status),
                "Crash report is being sent to CoD2x developers for analysis%s",
                suffix);
            break;
        }
        case CRASH_TELEMETRY_SUCCESS:
            snprintf(status, sizeof(status),
                "Crash report sent to CoD2x developers for analysis.");
            break;
        case CRASH_TELEMETRY_FAILED:
            snprintf(status, sizeof(status),
                "Failed to send crash report: %s\nIt will be sent again on the next game start. You can also report it on Discord - https://cod2x.me/",
                g_crashTelemetryError[0] ? g_crashTelemetryError : "unknown error");
            break;
        default:
            snprintf(status, sizeof(status),
                "Crash report will be sent to CoD2x developers for analysis.");
            break;
    }

    SetWindowTextA(hTelemetry, status);
}


// ---------------------------------------------------------------------------
// Collected report data
// ---------------------------------------------------------------------------

struct JsonStackFrameData {
    int frameNumber = 0;
    unsigned int instructionAddress = 0;
    unsigned int moduleOffset = 0;
    std::string moduleName;
    std::string sectionName;
    std::string resolvedSymbol;
    std::string instruction;
};

struct JsonStackDiagnosticsData {
    std::string method;
    int unwindFrames = 0;
    bool contextRepaired = false;
    bool ebpFallbackAttempted = false;
    int ebpFallbackFrames = 0;
    bool rawEspScanAttempted = false;
    int rawEspScanFrames = 0;
    bool stackSnapshot = false;   // stack of a running thread was copied while the thread was suspended
    std::string stopReason;       // why the EBP chain ended
    std::string symInitializeStatus;
    unsigned long symInitializeCode = 0;
    std::string symInitializeMessage;
    std::string stackWalkStatus;
    unsigned long stackWalkCode = 0;
    std::string stackWalkMessage;
};

// One collected stack trace. Identifies which thread it belongs to and which method
// produced it, and holds both the formatted text (LF-separated frame lines, without the
// column header) and the structured JSON frames + diagnostics. The crash report contains
// an array of these: up to 3 threads (crashing, CoD2 MainThread, CoD2 RenderBackend) x
// 2 methods (StackWalk64, EBP chain).
struct StackCollectionResult {
    std::string threadRole;   // "crashing thread", "CoD2 MainThread", "CoD2 RenderBackend"
    std::string threadName;   // OS thread description (may be empty)
    unsigned long threadId = 0;
    std::string method;       // "StackWalk64" or "EBP chain"
    std::string text;         // formatted frame lines (LF)
    std::vector<JsonStackFrameData> frames;
    JsonStackDiagnosticsData diagnostics;
};

struct JsonModuleInfoData {
    std::string baseAddress;
    std::string size;
    std::string moduleName;
    std::string path;
    std::string version;
    std::string timestamp;    // PE header TimeDateStamp (identifies the exact build)
};

struct JsonNamedValueData {
    std::string name;
    std::string value;
};

struct JsonMainFolderFileInfoData {
    std::string entryType;
    std::string size;
    std::string name;
    std::string message;
};

struct JsonThreadInfoData {
    unsigned long threadId = 0;
    std::string startAddress;
    std::string startSymbol;
    std::string threadName;
};

struct JsonMemoryBlockData {
    std::string name;         // "stack", "code", "EAX", ...
    unsigned int address = 0;
    std::vector<unsigned char> bytes;
    std::string note;         // why the block is empty
};

struct JsonSystemInfoData {
    std::string cpuCores;
    std::string cpuAffinity;
    std::string screenResolution;

    std::string memoryLoadPercent;
    std::string physicalTotalMB;
    std::string physicalAvailableMB;
    std::string virtualTotalMB;
    std::string virtualAvailableMB;

    std::string privateMB;
    std::string workingSetMB;
    std::string peakWorkingSetMB;
    std::string largestFreeBlockMB;
    std::string totalFreeAddressSpaceMB;
    bool largeAddressAware = false;
    std::string handleCount;
    std::string gdiObjects;
    std::string userObjects;

    std::string processId;
    std::string executablePath;
    std::string commandLine;
    std::string workingDirectory;
    std::string uptimeSeconds;

    std::string clientStateName;
    bool demoPlaying = false;
    unsigned int frameCount = 0;
    bool insideComFrame = false;

    std::vector<JsonModuleInfoData> modules;
    std::vector<JsonNamedValueData> cvars;
    std::vector<JsonMainFolderFileInfoData> mainFolderFiles;
};

struct JsonCrashContextData {
    bool hasRegisters = false;
    std::string eip;
    std::string eax;
    std::string ebx;
    std::string ecx;
    std::string edx;
    std::string esi;
    std::string edi;
    std::string ebp;
    std::string esp;
    std::string eflags;

    std::vector<JsonThreadInfoData> threads;
    std::vector<JsonMemoryBlockData> memory;
    std::vector<std::string> stackCodePointers;   // values on the stack pointing into executable code (raw ESP scan)
};

struct CrashReportData {
    std::string kind;                   // "crash" or "freeze"
    unsigned int exceptionCode = 0;
    std::string exceptionName;
    unsigned int exceptionAddress = 0;
    std::string moduleName;
    unsigned int fileOffset = 0;
    std::string faultDescription;
    std::vector<std::string> exceptionParameters;
    std::vector<std::string> nestedExceptions;
    unsigned long crashThreadId = 0;
    unsigned long currentThreadId = 0;
    std::string freezeReason;
    unsigned long frozenMs = 0;
    std::string minidumpFile;
    std::string time;
    int testMode = 0;                   // non-zero when produced by the crash-handler self test

    JsonCrashContextData crashContext;
    std::vector<StackCollectionResult> stacks;
    JsonSystemInfoData systemInfo;
    std::string logger;
    std::string logs;
};

struct ExceptionReportInput {
    EXCEPTION_POINTERS* pEx = nullptr;   // NULL for a freeze report
    DWORD crashThreadId = 0;             // 0 for a freeze report
    const char* kind = "crash";
    const char* freezeReason = nullptr;
    DWORD frozenMs = 0;
    const char* minidumpFile = nullptr;
    const SYSTEMTIME* time = nullptr;
};


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void exception_appendf(std::string& out, const char* fmt, ...) {
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
static void exception_bufAppend(char* buf, size_t size, size_t* pos, const char* fmt, ...) {
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

static std::string exception_jsonString(const std::string& value) {
    return "\"" + json_escape_string(value) + "\"";
}

static std::string exception_hex32(unsigned int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", value);
    return buf;
}

// Read memory of this process without faulting. Reads page by page, so a block crossing into an
// unreadable page still returns the readable beginning. Returns the number of bytes read.
static size_t exception_safeRead(uintptr_t addr, void* out, size_t len) {
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

static bool exception_isExecutable(unsigned int addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (addr == 0 || VirtualQuery((void*)(uintptr_t)addr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Module containing addr: short file name and base address. Returns false when the address is not inside a module.
static bool exception_getModuleForAddress(unsigned int addr, char* name, size_t nameSize, unsigned int* base) {
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

static void exception_formatWin32Error(DWORD error, char* out, size_t outSize) {
    if (!out || outSize == 0)
        return;

    out[0] = '\0';
    if (error == ERROR_SUCCESS) {
        snprintf(out, outSize, "The operation completed successfully.");
        return;
    }

    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        0,
        out,
        (DWORD)outSize,
        NULL);

    if (len == 0) {
        snprintf(out, outSize, "Unknown Win32 error");
        return;
    }

    while (len > 0 && (out[len - 1] == '\r' || out[len - 1] == '\n' ||
                       out[len - 1] == ' ' || out[len - 1] == '\t')) {
        out[len - 1] = '\0';
        --len;
    }
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
static void exception_describeFault(const EXCEPTION_RECORD* rec, char* out, size_t outSize) {
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
// Symbols
// ---------------------------------------------------------------------------

/**
 * Returns the PE section name (.text, .rdata, …) whose virtual address range
 * covers 'rva' (offset from image base). Returns false if not found.
 */
static bool exception_getPeSection(HMODULE hMod, unsigned int rva,
                                   char* outName, size_t nameSize) {
    if (!hMod || !nameSize) return false;
    BYTE* base = (BYTE*)hMod;
    if (IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER))) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (IsBadReadPtr(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    WORD nsec = nt->FileHeader.NumberOfSections;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    if (IsBadReadPtr(sec, sizeof(IMAGE_SECTION_HEADER) * nsec)) return false;
    for (WORD i = 0; i < nsec; ++i) {
        DWORD start = sec[i].VirtualAddress;
        DWORD vsz   = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                               : sec[i].SizeOfRawData;
        if (rva >= start && rva < start + vsz) {
            snprintf(outName, nameSize, "%.8s", (char*)sec[i].Name);
            return true;
        }
    }
    return false;
}

/**
 * Scans the PE export table of 'hMod' for the exported function closest to
 * (but not past) 'rva'. Fills outSym with "Name+0xDISP".
 * Returns false if no suitable export is found.
 */
static bool exception_getNearestExport(HMODULE hMod, unsigned int rva,
                                       char* outSym, size_t symSize) {
    if (!hMod || !symSize) return false;
    BYTE* base = (BYTE*)hMod;
    if (IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER))) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (IsBadReadPtr(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    DWORD expRva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    DWORD expSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (!expRva) return false;
    if (IsBadReadPtr(base + expRva, sizeof(IMAGE_EXPORT_DIRECTORY))) return false;
    IMAGE_EXPORT_DIRECTORY* expDir = (IMAGE_EXPORT_DIRECTORY*)(base + expRva);
    if (IsBadReadPtr(base + expDir->AddressOfFunctions,  expDir->NumberOfFunctions * sizeof(DWORD))) return false;
    if (IsBadReadPtr(base + expDir->AddressOfNames,      expDir->NumberOfNames     * sizeof(DWORD))) return false;
    if (IsBadReadPtr(base + expDir->AddressOfNameOrdinals, expDir->NumberOfNames   * sizeof(WORD)))  return false;
    DWORD* funcs    = (DWORD*)(base + expDir->AddressOfFunctions);
    DWORD* names    = (DWORD*)(base + expDir->AddressOfNames);
    WORD*  ordinals = (WORD* )(base + expDir->AddressOfNameOrdinals);
    DWORD       bestRva  = 0;
    const char* bestName = nullptr;
    DWORD       nextRva  = MAXDWORD; // closest export RVA *above* bestRva (upper bound)

    for (DWORD i = 0; i < expDir->NumberOfNames; ++i) {
        DWORD fRva = funcs[ordinals[i]];
        if (!fRva) continue;
        if (fRva >= expRva && fRva < expRva + expSize) continue; // forwarded

        if (fRva <= rva && fRva > bestRva) {
            // New best lower-bound: the old bestRva becomes a candidate upper bound
            if (bestRva != 0 && bestRva < nextRva)
                nextRva = bestRva;
            const char* nm = (const char*)(base + names[i]);
            if (!IsBadReadPtr(nm, 1)) {
                bestRva  = fRva;
                bestName = nm;
            }
        } else if (fRva > rva && fRva < nextRva) {
            // Export sits above the target — track as upper bound
            nextRva = fRva;
        }
    }

    if (!bestName) return false;

    DWORD disp = rva - bestRva;

    // Reject if the address is beyond the next known export (the function can't be that long)
    if (nextRva != MAXDWORD && rva >= nextRva) return false;

    // Fallback safety cap when there is no upper bound (last export in the table)
    if (nextRva == MAXDWORD && disp >= 0x10000u) return false;

    snprintf(outSym, symSize, "%s+0x%lX", bestName, (unsigned long)disp);
    return true;
}

/**
 * Resolves an address to "Name+0xDISP". Sources in order: DbgHelp (system DLLs with public symbols),
 * the CoD2MP_s.exe / gfx_d3d_mp_x86_s.dll / mss32.dll symbol tables, the nearest PE export.
 * hMod is the module containing addr (may be NULL), rva is addr - module base.
 */
static bool exception_resolveSymbol(HANDLE hProcess, HMODULE hMod, unsigned int addr, unsigned int rva,
                                    char* outSym, size_t symSize) {
    struct { SYMBOL_INFO sym; char buf[256]; } sb;
    memset(&sb, 0, sizeof(sb));
    sb.sym.SizeOfStruct = sizeof(SYMBOL_INFO);
    sb.sym.MaxNameLen   = 256;
    DWORD64 disp = 0;
    if (SymFromAddr(hProcess, (DWORD64)addr, &disp, &sb.sym)
            && sb.sym.Name[0] != '\0' && disp < 0x10000u) {
        if (disp == 0)
            snprintf(outSym, symSize, "%s", sb.sym.Name);
        else
            snprintf(outSym, symSize, "%s+0x%llX", sb.sym.Name, (unsigned long long)disp);
        return true;
    }
    if (!hMod)
        return false;

    // CoD2MP_s.exe symbols lookup
    if (hMod == GetModuleHandleA("CoD2MP_s.exe")) {
        if (const Cod2mpSymbol* sym = symbols_cod2mp_s_find(addr)) {
            uint32_t d = addr - sym->addr;
            if (d == 0)
                snprintf(outSym, symSize, "%s", sym->name);
            else
                snprintf(outSym, symSize, "%s+0x%X", sym->name, d);
            return true;
        }
    }
    // gfx_d3d_mp_x86_s.dll symbols lookup
    if (hMod == GetModuleHandleA("gfx_d3d_mp_x86_s.dll")) {
        if (const GfxD3dSymbol* sym = symbols_gfx_d3d_mp_x86_s_find(addr)) {
            uint32_t d = addr - sym->addr;
            if (d == 0)
                snprintf(outSym, symSize, "%s", sym->name);
            else
                snprintf(outSym, symSize, "%s+0x%X", sym->name, d);
            return true;
        }
    }
    // mss32.dll symbols lookup (RVA-based; populated one build behind by generate_mss32_symbols_h.ps1)
    if (hMod == GetModuleHandleA("mss32.dll")) {
        if (const Mss32Symbol* sym = symbols_mss32_find(rva)) {
            uint32_t d = rva - sym->rva;
            if (d == 0)
                snprintf(outSym, symSize, "%s", sym->name);
            else
                snprintf(outSym, symSize, "%s+0x%X", sym->name, d);
            return true;
        }
    }
    // Export-table path
    return exception_getNearestExport(hMod, rva, outSym, symSize);
}

/**
 * Describes an address as "module+0xOFFSET (Symbol+0xDISP)", or "<unknown>" when it is not inside a module.
 */
size_t exception_describeAddress(unsigned int addr, char* out, size_t outSize) {
    if (!out || !outSize)
        return 0;
    char modName[MAX_PATH] = {0};
    unsigned int base = 0;
    if (!exception_getModuleForAddress(addr, modName, sizeof(modName), &base))
        return (size_t)snprintf(out, outSize, "<unknown>");
    char sym[300] = {0};
    if (exception_resolveSymbol(GetCurrentProcess(), (HMODULE)(uintptr_t)base, addr, addr - base, sym, sizeof(sym)))
        return (size_t)snprintf(out, outSize, "%s+0x%X (%s)", modName, addr - base, sym);
    return (size_t)snprintf(out, outSize, "%s+0x%X", modName, addr - base);
}

// Start address of the function containing addr, from the symbol tables / DbgHelp. False when unknown.
static bool exception_getSymbolStart(unsigned int addr, unsigned int* start) {
    char modName[MAX_PATH];
    unsigned int base = 0;
    if (!exception_getModuleForAddress(addr, modName, sizeof(modName), &base))
        return false;
    HMODULE hMod = (HMODULE)(uintptr_t)base;

    if (hMod == GetModuleHandleA("CoD2MP_s.exe")) {
        if (const Cod2mpSymbol* sym = symbols_cod2mp_s_find(addr)) { *start = sym->addr; return true; }
    }
    if (hMod == GetModuleHandleA("gfx_d3d_mp_x86_s.dll")) {
        if (const GfxD3dSymbol* sym = symbols_gfx_d3d_mp_x86_s_find(addr)) { *start = sym->addr; return true; }
    }
    if (hMod == GetModuleHandleA("mss32.dll")) {
        if (const Mss32Symbol* sym = symbols_mss32_find(addr - base)) { *start = base + sym->rva; return true; }
    }

    struct { SYMBOL_INFO sym; char buf[256]; } sb;
    memset(&sb, 0, sizeof(sb));
    sb.sym.SizeOfStruct = sizeof(SYMBOL_INFO);
    sb.sym.MaxNameLen   = 256;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)addr, &disp, &sb.sym) && disp < 0x10000u) {
        *start = addr - (unsigned int)disp;
        return true;
    }
    return false;
}

// Reads memory, served from the stack snapshot of the thread being walked when it covers the address.
// Defined with the stack walking.
static size_t exception_readThreadMemory(DWORD addr, void* out, size_t len);

// A decoded CALL instruction that ends right before a return address.
struct ExceptionCallSite {
    bool direct = false;          // E8 rel32
    bool targetKnown = false;
    unsigned int target = 0;
    char text[96] = {0};          // "call 0x00434F70", "call eax", "call [ebp-0xC]", ...
};

// Length of an "FF /2" (call r/m32) instruction from its ModRM / SIB bytes, 0 if the bytes are not one.
static int exception_callRmLength(const BYTE* insn) {
    if (insn[0] != 0xFF || ((insn[1] >> 3) & 7) != 2)
        return 0;
    int mod = insn[1] >> 6;
    int rm = insn[1] & 7;
    int len = 2;
    if (mod == 3)
        return len;
    if (rm == 4) {
        len += 1; // SIB
        if (mod == 0 && (insn[2] & 7) == 5)
            len += 4;
    } else if (mod == 0 && rm == 5) {
        len += 4; // [disp32]
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/**
 * Decodes the CALL instruction that produced return address 'retAddr' (the bytes right before it).
 * frameEbp is the EBP of the calling function (0 = unknown); it resolves "call [ebp+disp]", the form
 * used by ASM_CALL in CoD2x code. Returns false when the bytes before retAddr are not a call.
 */
static bool exception_decodeCallSite(unsigned int retAddr, DWORD frameEbp, ExceptionCallSite* cs) {
    static const char* const regNames[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

    if (retAddr < 8)
        return false;
    BYTE b[7];
    if (exception_safeRead(retAddr - 7, b, sizeof(b)) != sizeof(b))
        return false;
    // b[7 - n] is the byte at retAddr - n

    // E8 rel32: direct near call
    if (b[2] == 0xE8) {
        int32_t rel;
        memcpy(&rel, b + 3, 4);
        cs->direct = true;
        cs->targetKnown = true;
        cs->target = retAddr + (unsigned int)rel;
        snprintf(cs->text, sizeof(cs->text), "call 0x%08X", cs->target);
        return true;
    }

    // FF /2: call r/m32 (register, [disp32], [reg+disp], SIB forms)
    static const int lengths[] = { 2, 3, 6, 7, 4 };
    for (int len : lengths) {
        const BYTE* insn = b + 7 - len;
        if (exception_callRmLength(insn) != len)
            continue;

        int mod = insn[1] >> 6;
        int rm = insn[1] & 7;
        uint32_t pointerAddr = 0;
        bool pointerKnown = false;

        if (mod == 3) {
            snprintf(cs->text, sizeof(cs->text), "call %s", regNames[rm]);
            return true;
        }
        if (rm == 4) {
            // SIB: [base + index*scale + disp], e.g. "call [esp+0x4]" or a jump table "call [eax*4+0x005CD000]"
            BYTE sib = insn[2];
            int scale = 1 << (sib >> 6);
            int index = (sib >> 3) & 7;
            int base = sib & 7;
            int32_t disp = 0;
            if (mod == 1) disp = (int8_t)insn[3];
            else if (mod == 2 || (mod == 0 && base == 5)) memcpy(&disp, insn + 3, 4);

            char expr[64] = {0};
            size_t len = 0;
            if (!(mod == 0 && base == 5))
                len += snprintf(expr + len, sizeof(expr) - len, "%s", regNames[base]);
            if (index != 4) // index 4 = no index
                len += snprintf(expr + len, sizeof(expr) - len, "%s%s*%d", len ? "+" : "", regNames[index], scale);
            if (disp != 0 || len == 0)
                snprintf(expr + len, sizeof(expr) - len, "%s0x%X", disp < 0 ? "-" : (len ? "+" : ""), disp < 0 ? -disp : disp);
            snprintf(cs->text, sizeof(cs->text), "call [%s]", expr);
        } else if (mod == 0 && rm == 5) {
            memcpy(&pointerAddr, insn + 2, 4);
            pointerKnown = true;
            snprintf(cs->text, sizeof(cs->text), "call [0x%08X]", pointerAddr);
        } else {
            int32_t disp = 0;
            if (mod == 1) disp = (int8_t)insn[2];
            else if (mod == 2) memcpy(&disp, insn + 2, 4);
            if (disp == 0)
                snprintf(cs->text, sizeof(cs->text), "call [%s]", regNames[rm]);
            else
                snprintf(cs->text, sizeof(cs->text), "call [%s%s0x%X]", regNames[rm], disp < 0 ? "-" : "+", disp < 0 ? -disp : disp);
            if (rm == 5 && frameEbp != 0) {
                pointerAddr = frameEbp + disp;
                pointerKnown = true;
            }
        }

        // Only trust a target that points to code (a wrong frame EBP would read garbage)
        uint32_t target = 0;
        if (pointerKnown && exception_readThreadMemory(pointerAddr, &target, 4) == 4 && exception_isExecutable(target)) {
            cs->target = target;
            cs->targetKnown = true;
        }
        return true;
    }
    return false;
}

// True when addr looks like a return address: executable and right after a CALL instruction.
static bool exception_isReturnAddress(unsigned int addr) {
    ExceptionCallSite cs;
    return exception_isExecutable(addr) && exception_decodeCallSite(addr, 0, &cs);
}

/**
 * Describes the CALL instruction that produced return address 'retAddr', with the resolved callee
 * when it is known: "call 0x00434F70  (Com_Frame_Try_Block_Function)", "call [ebp-0xC] -> 0x0040F850  (CL_Frame)".
 */
static void exception_describeCallSite(HANDLE hProcess, unsigned int retAddr, DWORD frameEbp,
                                       char* out, size_t outSize) {
    if (!out || !outSize) return;
    out[0] = '\0';

    ExceptionCallSite cs;
    if (!exception_decodeCallSite(retAddr, frameEbp, &cs))
        return;

    if (!cs.targetKnown) {
        snprintf(out, outSize, "%s", cs.text);
        return;
    }

    char symBuf[256] = {0};
    unsigned int base = 0;
    char modName[MAX_PATH];
    bool hasSymbol = exception_getModuleForAddress(cs.target, modName, sizeof(modName), &base) &&
        exception_resolveSymbol(hProcess, (HMODULE)(uintptr_t)base, cs.target, cs.target - base, symBuf, sizeof(symBuf));

    if (cs.direct)
        snprintf(out, outSize, hasSymbol ? "%s  (%s)" : "%s", cs.text, symBuf);
    else if (hasSymbol)
        snprintf(out, outSize, "%s -> 0x%08X  (%s)", cs.text, cs.target, symBuf);
    else
        snprintf(out, outSize, "%s -> 0x%08X", cs.text, cs.target);
}

// DbgHelp is initialized once; it is not thread safe, callers hold exception_reportLock.
static BOOL exception_symInitialized = FALSE;
static DWORD exception_symInitializeError = ERROR_SUCCESS;
static bool exception_symInitAttempted = false;

static void exception_initSymbols() {
    if (exception_symInitAttempted)
        return;
    exception_symInitAttempted = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    exception_symInitialized = SymInitialize(GetCurrentProcess(), NULL, TRUE);
    exception_symInitializeError = exception_symInitialized ? ERROR_SUCCESS : GetLastError();
}

/**
 * Formats the call stack of the current thread (skipping framesToSkip frames of the caller) as lines
 * "  N  0xADDRESS  module+0xOFFSET (Symbol)". Used for fatal errors that are not exceptions (Com_Error / Sys_Error).
 */
size_t exception_captureCallerStack(char* out, size_t outSize, int framesToSkip) {
    if (!out || !outSize)
        return 0;
    out[0] = '\0';

    PVOID frames[32] = {0};
    USHORT count = RtlCaptureStackBackTrace((ULONG)(framesToSkip + 1), 32, frames, NULL);

    size_t pos = 0;
    for (USHORT i = 0; i < count; i++) {
        char desc[400];
        exception_describeAddress((unsigned int)(uintptr_t)frames[i], desc, sizeof(desc));
        exception_bufAppend(out, outSize, &pos, "%3d  0x%08X  %s\n", (int)i, (unsigned int)(uintptr_t)frames[i], desc);
    }
    return pos;
}


// ---------------------------------------------------------------------------
// Stack walking
// ---------------------------------------------------------------------------

// Maximum number of frames captured per stack (per thread, per method).
static const size_t EXCEPTION_MAX_STACK_FRAMES = 64;

// How much of a running thread's stack is copied while it is suspended.
static const size_t EXCEPTION_STACK_SNAPSHOT_SIZE = 256 * 1024;

// One target thread whose stack we want to capture. The CONTEXT and the stack memory are captured up front
// while the thread is briefly suspended, then the thread is resumed. Walking the copy gives a consistent stack
// even though the thread keeps running, and nothing that could take a lock (heap, DbgHelp) runs while it is suspended.
struct ExceptionThreadTarget {
    std::string role;
    std::string name;
    DWORD       threadId = 0;
    CONTEXT     context = {};
    bool        hasContext = false;
    bool        isCrashing = false;
    std::vector<BYTE> stackCopy;
    DWORD       stackCopyAddress = 0;
    DWORD       stackBase = 0;          // highest address of the thread's stack, 0 = unknown
};

// The thread whose stack is being walked (read by the StackWalk64 memory callback).
static const ExceptionThreadTarget* exception_walkTarget = nullptr;

static size_t exception_readThreadMemory(DWORD addr, void* out, size_t len) {
    const ExceptionThreadTarget* t = exception_walkTarget;
    if (t && !t->stackCopy.empty() && addr >= t->stackCopyAddress &&
        (size_t)(addr - t->stackCopyAddress) + len <= t->stackCopy.size()) {
        memcpy(out, t->stackCopy.data() + (addr - t->stackCopyAddress), len);
        return len;
    }
    return exception_safeRead(addr, out, len);
}

static BOOL CALLBACK exception_stackWalkReadMemory(HANDLE hProcess, DWORD64 baseAddress, PVOID buffer, DWORD size, LPDWORD bytesRead) {
    (void)hProcess;
    size_t got = exception_readThreadMemory((DWORD)baseAddress, buffer, size);
    if (bytesRead)
        *bytesRead = (DWORD)got;
    return got == size;
}

// Returns the OS thread description (set via SetThreadDescription) for a thread handle.
// Empty string if unavailable.
static std::string exception_getThreadName(HANDLE hThread) {
    std::string out;
    if (!hThread)
        return out;
    typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR*);
    static GetThreadDescriptionFn pGetThreadDescription =
        (GetThreadDescriptionFn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    if (!pGetThreadDescription)
        return out;
    PWSTR wideName = nullptr;
    if (SUCCEEDED(pGetThreadDescription(hThread, &wideName)) && wideName) {
        char nameUtf8[256] = {0};
        if (WideCharToMultiByte(CP_UTF8, 0, wideName, -1, nameUtf8, (int)sizeof(nameUtf8), nullptr, nullptr) > 0)
            out = nameUtf8;
        LocalFree(wideName);
    }
    return out;
}

// Highest address of the thread's stack (NT_TIB.StackBase from the TEB), 0 if unknown.
static DWORD exception_getThreadStackBase(HANDLE hThread) {
    typedef LONG (NTAPI *NtQueryInformationThreadFn)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
    static NtQueryInformationThreadFn pNtQueryInformationThread =
        (NtQueryInformationThreadFn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!pNtQueryInformationThread)
        return 0;

    struct {
        LONG      ExitStatus;
        PVOID     TebBaseAddress;
        PVOID     UniqueProcess;
        PVOID     UniqueThread;
        ULONG_PTR AffinityMask;
        LONG      Priority;
        LONG      BasePriority;
    } tbi = {};
    if (pNtQueryInformationThread(hThread, (THREADINFOCLASS)0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), NULL) != 0 || !tbi.TebBaseAddress)
        return 0;

    DWORD stackBase = 0;
    if (exception_safeRead((uintptr_t)tbi.TebBaseAddress + 4 /*NT_TIB.StackBase*/, &stackBase, 4) != 4)
        return 0;
    return stackBase;
}

// Resolves a single return/instruction address into a frame line (text) + JSON entry,
// appending both to 'result'. frameEbp is the EBP of the function containing addr (0 = unknown),
// used to resolve "call [ebp+disp]". Returns false when the address is not backed by readable memory.
static bool exception_appendFrameToResult(HANDLE hProcess, StackCollectionResult& result,
                                          int frameIndex, unsigned int addr, DWORD frameEbp) {
    MEMORY_BASIC_INFORMATION mbi2;
    if (VirtualQuery((void*)(uintptr_t)addr, &mbi2, sizeof(mbi2)) == 0)
        return false;

    char modName2[MAX_PATH] = {0};
    unsigned int offset2;
    if (mbi2.AllocationBase == NULL) {
        // AllocationBase=NULL means the address isn't backed by any module.
        strncpy(modName2, "<unknown>", sizeof(modName2) - 1);
        offset2 = addr; // no base to subtract; display raw address as offset
    } else {
        GetModuleFileNameA((HMODULE)mbi2.AllocationBase, modName2, sizeof(modName2));
        char* p2 = strrchr(modName2, '\\');
        if (p2) memmove(modName2, p2 + 1, strlen(p2 + 1) + 1);
        if (modName2[0] == '\0')
            strncpy(modName2, "<unknown>", sizeof(modName2) - 1);
        offset2 = addr - (unsigned int)(uintptr_t)mbi2.AllocationBase;
    }

    char section2[16] = {0};
    if (!exception_getPeSection((HMODULE)mbi2.AllocationBase, offset2, section2, sizeof(section2)))
        strncpy(section2, "?", sizeof(section2) - 1);

    char resolvedSymbol2[300] = {0};
    bool hasSymbol = exception_resolveSymbol(hProcess, (HMODULE)mbi2.AllocationBase,
                                 addr, offset2, resolvedSymbol2, sizeof(resolvedSymbol2));
    char callSite2[200] = {0};
    exception_describeCallSite(hProcess, addr, frameEbp, callSite2, sizeof(callSite2));

    JsonStackFrameData frameData;
    frameData.frameNumber = frameIndex;
    frameData.instructionAddress = addr;
    frameData.moduleOffset = offset2;
    frameData.moduleName = modName2;
    frameData.sectionName = section2;
    frameData.resolvedSymbol = hasSymbol ? resolvedSymbol2 : "<no symbol>";
    frameData.instruction = callSite2;
    result.frames.push_back(frameData);

    exception_appendf(result.text,
        "%3d  %-24s  0x%08X  0x%08X  %-40s  %s\n",
        frameIndex, modName2, addr, offset2,
        hasSymbol ? resolvedSymbol2 : "<no symbol>", callSite2);
    return true;
}

// Inserts frame 0 (the actual exception address) at the top of a crashing-thread stack.
static void exception_prependCrashFrame0(HANDLE hProcess, StackCollectionResult& result,
                                         const char* moduleName, unsigned int moduleBase,
                                         unsigned int exceptionAddress, unsigned int fileOffset) {
    if (!moduleName || moduleName[0] == '\0')
        return;

    // Use GetModuleHandleA so we get the same handle Windows uses for the PE,
    // rather than a raw integer cast of AllocationBase which can mismatch.
    HMODULE hMod0 = GetModuleHandleA(moduleName);
    if (!hMod0) hMod0 = (HMODULE)(uintptr_t)moduleBase;

    char section0[16] = {0};
    if (!exception_getPeSection(hMod0, fileOffset, section0, sizeof(section0)))
        strncpy(section0, "?", sizeof(section0) - 1);

    char symbol0[300] = {0};
    if (!exception_resolveSymbol(hProcess, hMod0, exceptionAddress, fileOffset, symbol0, sizeof(symbol0)))
        strncpy(symbol0, "<no symbol>", sizeof(symbol0) - 1);

    std::string line;
    exception_appendf(line,
        "  0  %-24s  0x%08X  0x%08X  %-40s  %s\n",
        moduleName, exceptionAddress, fileOffset, symbol0, "");
    result.text.insert(0, line);

    JsonStackFrameData frameZero;
    frameZero.frameNumber = 0;
    frameZero.instructionAddress = exceptionAddress;
    frameZero.moduleOffset = fileOffset;
    frameZero.moduleName = moduleName;
    frameZero.sectionName = section0;
    frameZero.resolvedSymbol = symbol0;
    frameZero.instruction = "";
    result.frames.insert(result.frames.begin(), frameZero);
}

static void exception_fillSymDiagnostics(JsonStackDiagnosticsData& d) {
    char symInitErrMsg[256] = {0};
    exception_formatWin32Error(exception_symInitializeError, symInitErrMsg, sizeof(symInitErrMsg));
    d.symInitializeStatus = exception_symInitialized ? "ok" : "failed";
    d.symInitializeCode = exception_symInitializeError;
    d.symInitializeMessage = symInitErrMsg;
}

/**
 * For the crashing thread: finds a return address that both walks would miss, because the frame of the
 * crashing function does not exist (yet). Must be called with exception_walkTarget set.
 *  - EIP is not executable (call through a NULL / bad pointer): the CPU already pushed the return address to [ESP].
 *  - The crash is at the entry of a function, before "push ebp / mov ebp, esp": the return address is at [ESP]
 *    (or at [ESP+4] after "push ebp") and EBP still belongs to the caller, so [EBP+4] skips the caller.
 * Returns the return address and ESP after popping it.
 */
static bool exception_findMissingReturnAddress(const CONTEXT& ctx, DWORD* retAddr, DWORD* espAfter, const char** reason) {
    if (ctx.Esp == 0)
        return false;

    if (!exception_isExecutable(ctx.Eip)) {
        DWORD value = 0;
        if (exception_readThreadMemory(ctx.Esp, &value, 4) == 4 && exception_isExecutable(value)) {
            *retAddr = value;
            *espAfter = ctx.Esp + 4;
            *reason = "EIP not executable, return address from [ESP]";
            return true;
        }
        return false;
    }

    DWORD callerReturn = 0;
    exception_readThreadMemory(ctx.Ebp + 4, &callerReturn, 4);

    for (int k = 0; k < 2; k++) {
        DWORD value = 0;
        if (exception_readThreadMemory(ctx.Esp + 4 * k, &value, 4) != 4)
            continue;
        if (k == 1) {
            // After "push ebp" the saved EBP (still equal to EBP) is at [ESP]
            DWORD top = 0;
            if (exception_readThreadMemory(ctx.Esp, &top, 4) != 4 || top != ctx.Ebp)
                continue;
        }
        if (value == callerReturn)
            continue; // the caller is already reachable through EBP

        ExceptionCallSite cs;
        if (!exception_isExecutable(value) || !exception_decodeCallSite(value, 0, &cs))
            continue;

        // The call must lead into the crashing function: a direct call to just before EIP, or EIP at the start of a known function
        bool callsCrashingFunction = false;
        unsigned int functionStart = 0;
        if (cs.direct)
            callsCrashingFunction = cs.target <= ctx.Eip && ctx.Eip - cs.target < 0x40;
        else
            callsCrashingFunction = exception_getSymbolStart(ctx.Eip, &functionStart) && ctx.Eip - functionStart < 0x10;
        if (!callsCrashingFunction)
            continue;

        *retAddr = value;
        *espAfter = ctx.Esp + 4 * (k + 1);
        *reason = "crash at function entry, return address from the stack";
        return true;
    }
    return false;
}

// Method 1: DbgHelp StackWalk64. Walks 'walkContext' (a copy is taken so it can be mutated).
// For the crashing thread it skips the frame that equals exceptionAddress (re-added later as
// frame 0) and starts from the return address on the stack when the crashing function has no frame.
static void exception_walkStackWalk64(HANDLE hProcess, const ExceptionThreadTarget& target,
                                      unsigned int exceptionAddress, StackCollectionResult& result) {
    CONTEXT walkContext = target.context;
    const bool isCrashing = target.isCrashing;
    exception_walkTarget = &target;

    result.method = "StackWalk64";
    walkContext.ContextFlags = CONTEXT_FULL;

    bool contextRepaired = false;
    const char* repairReason = "";
    DWORD missingReturn = 0, espAfter = 0;
    if (isCrashing && exception_findMissingReturnAddress(walkContext, &missingReturn, &espAfter, &repairReason)) {
        walkContext.Eip = missingReturn;
        walkContext.Esp = espAfter;
        contextRepaired = true;
    }

    STACKFRAME64 stackFrame = {};
    stackFrame.AddrPC.Offset    = walkContext.Eip;
    stackFrame.AddrPC.Mode      = AddrModeFlat;
    stackFrame.AddrFrame.Offset = walkContext.Ebp;
    stackFrame.AddrFrame.Mode   = AddrModeFlat;
    stackFrame.AddrStack.Offset = walkContext.Esp;
    stackFrame.AddrStack.Mode   = AddrModeFlat;
    stackFrame.AddrReturn.Mode  = AddrModeFlat;

    DWORD stackWalkError = ERROR_SUCCESS;
    int unwindFrameCount = 0;
    int frameN = isCrashing ? 1 : 0; // crashing thread reserves frame 0 for the exception address
    bool skippedExceptionFrame = !isCrashing;

    // Garbage-frame rollback: on old dbghelp.dll StackWalk64 falls back to a permissive raw-stack
    // scan after losing the EBP chain, producing fake frames whose AllocationBase is NULL. If two
    // consecutive frames are not backed by a real module, roll back to the last good position.
    size_t textAtLastGood = result.text.size();
    size_t framesAtLastGood = result.frames.size();
    int frameNAtLastGood = frameN;
    int unwindCountAtLastGood = 0;
    int consecutiveBadFrames = 0;

    for (size_t i = 0; i < EXCEPTION_MAX_STACK_FRAMES; ++i) {
        BOOL walked = StackWalk64(
            IMAGE_FILE_MACHINE_I386,
            hProcess,
            GetCurrentThread(),
            &stackFrame,
            &walkContext,
            exception_stackWalkReadMemory,
            SymFunctionTableAccess64,
            SymGetModuleBase64,
            NULL);
        if (!walked || stackFrame.AddrPC.Offset == 0) {
            stackWalkError = GetLastError();
            break;
        }

        unsigned int addr2 = (unsigned int)stackFrame.AddrPC.Offset;

        if (!skippedExceptionFrame && addr2 == exceptionAddress) {
            skippedExceptionFrame = true;
            continue;
        }
        skippedExceptionFrame = true;

        if (exception_appendFrameToResult(hProcess, result, frameN, addr2, (DWORD)stackFrame.AddrFrame.Offset)) {
            ++frameN;
            ++unwindFrameCount;

            MEMORY_BASIC_INFORMATION mbiQ;
            bool isGoodFrame = (VirtualQuery((void*)(uintptr_t)addr2, &mbiQ, sizeof(mbiQ)) != 0 &&
                                mbiQ.AllocationBase != NULL);
            if (isGoodFrame) {
                consecutiveBadFrames = 0;
                textAtLastGood        = result.text.size();
                framesAtLastGood      = result.frames.size();
                frameNAtLastGood      = frameN;
                unwindCountAtLastGood = unwindFrameCount;
            } else {
                if (++consecutiveBadFrames >= 2) {
                    result.text.resize(textAtLastGood);
                    result.frames.resize(framesAtLastGood);
                    frameN = frameNAtLastGood;
                    unwindFrameCount = unwindCountAtLastGood;
                    break;
                }
            }
        }

        if (stackFrame.AddrReturn.Offset == 0)
            break;
    }

    exception_walkTarget = nullptr;

    char stackWalkErrMsg[256] = {0};
    if (stackWalkError != ERROR_SUCCESS)
        exception_formatWin32Error(stackWalkError, stackWalkErrMsg, sizeof(stackWalkErrMsg));

    result.diagnostics.method = contextRepaired
        ? std::string("StackWalk64 (repaired CONTEXT: ") + repairReason + ")" : "StackWalk64 (original CONTEXT)";
    result.diagnostics.unwindFrames = unwindFrameCount;
    result.diagnostics.contextRepaired = contextRepaired;
    result.diagnostics.stackSnapshot = !target.stackCopy.empty();
    exception_fillSymDiagnostics(result.diagnostics);
    result.diagnostics.stackWalkStatus = (stackWalkError != ERROR_SUCCESS) ? "failed" : "ok";
    result.diagnostics.stackWalkCode = stackWalkError;
    result.diagnostics.stackWalkMessage = stackWalkErrMsg;
}

// Method 2: legacy EBP-chain walk. [EBP+0]=prev EBP, [EBP+4]=return address. Works through code compiled
// with frame pointers (CoD2x, most of CoD2MP_s.exe); it stops at the first caller that uses EBP as a general
// register (frame pointer omission, e.g. Com_Frame_Try_Block_Function), StackWalk64 continues past those.
static void exception_walkEbpChain(HANDLE hProcess, const ExceptionThreadTarget& target,
                                   unsigned int exceptionAddress, StackCollectionResult& result) {
    const bool isCrashing = target.isCrashing;
    exception_walkTarget = &target;

    result.method = "EBP chain";
    int frameN = isCrashing ? 1 : 0;
    int count = 0;
    DWORD ebp = target.context.Ebp;
    const char* stopReason = "end of chain";

    if (!isCrashing) {
        // Frame 0 of a running thread: where it is now (the crashing thread gets frame 0 from the exception address)
        if (exception_appendFrameToResult(hProcess, result, frameN, target.context.Eip, ebp))
            ++frameN;
    } else {
        // Caller of a crashing function that has no frame yet
        DWORD missingReturn = 0, espAfter = 0;
        const char* reason = "";
        if (exception_findMissingReturnAddress(target.context, &missingReturn, &espAfter, &reason) &&
            exception_appendFrameToResult(hProcess, result, frameN, missingReturn, ebp)) {
            ++frameN;
            ++count;
            result.diagnostics.contextRepaired = true;
        }
    }

    for (size_t i = 0; i < EXCEPTION_MAX_STACK_FRAMES; ++i) {
        // EBP must be non-null and 4-byte aligned
        if (ebp == 0 || (ebp & 3) != 0) {
            stopReason = ebp == 0 ? "end of chain" : "EBP not aligned (caller without frame pointer)";
            break;
        }
        // EBP must point into the thread's stack
        if (target.stackBase != 0 && (ebp < target.context.Esp || ebp + 8 > target.stackBase)) {
            stopReason = "EBP outside of the stack (caller without frame pointer)";
            break;
        }

        DWORD frame[2];
        if (exception_readThreadMemory(ebp, frame, sizeof(frame)) != sizeof(frame)) {
            stopReason = "frame not readable";
            break;
        }
        DWORD nextEbp = frame[0];
        DWORD retAddr = frame[1];

        // EBP chain must grow toward higher addresses (stack grows down)
        if (nextEbp != 0 && nextEbp <= ebp) {
            stopReason = "EBP chain does not grow (caller without frame pointer)";
            break;
        }

        if (retAddr != 0 && !(isCrashing && retAddr == exceptionAddress) && exception_isExecutable(retAddr)) {
            if (exception_appendFrameToResult(hProcess, result, frameN, retAddr, nextEbp)) {
                ++frameN;
                ++count;
            }
        }

        if (nextEbp == 0)
            break;
        ebp = nextEbp;
    }

    exception_walkTarget = nullptr;

    result.diagnostics.method = "EBP chain (frame pointer unwind)";
    result.diagnostics.stopReason = stopReason;
    result.diagnostics.unwindFrames = count;
    result.diagnostics.ebpFallbackAttempted = true;
    result.diagnostics.ebpFallbackFrames = count;
    result.diagnostics.stackSnapshot = !target.stackCopy.empty();
    exception_fillSymDiagnostics(result.diagnostics);
    result.diagnostics.stackWalkStatus = "n/a";
}

// Builds the list of target threads: the crashing thread (from pEx, when crashThreadId != 0) plus the named
// "CoD2 MainThread" and "CoD2 RenderBackend" threads. Other threads are suspended only to snapshot their CONTEXT
// and stack memory; the current thread captures its own context.
static void exception_collectTargetThreads(DWORD crashThreadId, EXCEPTION_POINTERS* pEx,
                                           std::vector<ExceptionThreadTarget>& out) {
    // 1. Crashing thread
    if (crashThreadId != 0) {
        ExceptionThreadTarget t;
        t.role = "crashing thread";
        t.threadId = crashThreadId;
        t.isCrashing = true;
        if (pEx && pEx->ContextRecord) {
            t.context = *pEx->ContextRecord;
            t.hasContext = true;
        }
        HANDLE hNamed = OpenThread(THREAD_QUERY_INFORMATION, FALSE, crashThreadId);
        if (hNamed) {
            t.name = exception_getThreadName(hNamed);
            t.stackBase = exception_getThreadStackBase(hNamed);
            CloseHandle(hNamed);
        }
        out.push_back(t);
    }

    // 2. Named threads (CoD2 MainThread / CoD2 RenderBackend)
    HANDLE threadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnap == INVALID_HANDLE_VALUE)
        return;

    const DWORD currentThreadId = GetCurrentThreadId();

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(threadSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId())
                continue;
            if (te.th32ThreadID == crashThreadId)
                continue; // already covered by the crashing-thread entry

            HANDLE hT = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                FALSE, te.th32ThreadID);
            if (!hT)
                continue;

            std::string nm = exception_getThreadName(hT);
            if (nm != "CoD2 MainThread" && nm != "CoD2 RenderBackend") {
                CloseHandle(hT);
                continue;
            }

            ExceptionThreadTarget t;
            t.role = nm;
            t.name = nm;
            t.threadId = te.th32ThreadID;

            DWORD stackBase = exception_getThreadStackBase(hT);
            t.stackBase = stackBase;

            // Allocate before suspending, nothing may allocate while the thread is suspended
            // (it could hold the heap lock).
            t.stackCopy.resize(EXCEPTION_STACK_SNAPSHOT_SIZE);

            if (te.th32ThreadID == currentThreadId) {
                // Our own thread (freeze report from the render thread): capture the context here. The frames
                // below this function are overwritten by the walk itself, so the stack is copied as well.
                RtlCaptureContext(&t.context);
                t.hasContext = true;
            } else if (SuspendThread(hT) != (DWORD)-1) {
                t.context.ContextFlags = CONTEXT_FULL;
                t.hasContext = GetThreadContext(hT, &t.context) != FALSE;
                if (!t.hasContext)
                    ResumeThread(hT);
            }

            if (t.hasContext) {
                size_t want = EXCEPTION_STACK_SNAPSHOT_SIZE;
                if (stackBase > t.context.Esp && stackBase - t.context.Esp < want)
                    want = stackBase - t.context.Esp;
                size_t got = exception_safeRead(t.context.Esp, t.stackCopy.data(), want);
                t.stackCopyAddress = t.context.Esp;
                t.stackCopy.resize(got); // shrinking does not reallocate
                if (te.th32ThreadID != currentThreadId)
                    ResumeThread(hT);
            } else {
                t.stackCopy.clear();
            }
            CloseHandle(hT);
            out.push_back(t);
        } while (Thread32Next(threadSnap, &te));
    }
    CloseHandle(threadSnap);
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
static void exception_collectReport(const ExceptionReportInput& in, CrashReportData& r) {
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

static std::string exception_buildReportText(const CrashReportData& r) {
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

static void exception_jsonAppendLinesArray(std::string& out, const char* fieldName, const char* text) {
    out += "\"";
    out += fieldName ? fieldName : "";
    out += "\":[";

    const char* p = text ? text : "";
    bool first = true;

    while (*p) {
        const char* lineStart = p;
        while (*p && *p != '\n' && *p != '\r') {
            ++p;
        }

        size_t lineLen = (size_t)(p - lineStart);
        if (lineLen > 0) {
            if (!first) {
                out += ",";
            }
            first = false;
            out += exception_jsonString(std::string(lineStart, lineLen));
        }

        while (*p == '\n' || *p == '\r') {
            ++p;
        }
    }

    out += "]";
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
    out += "\"gpu\":" + exception_jsonString(SYS_GPU_INFO) + ",";
    out += "\"cpuModel\":" + exception_jsonString(SYS_CPU_BRAND) + ",";
    out += "\"winBuild\":" + exception_jsonString(SYS_WIN_BUILD) + ",";
    out += "\"winEdition\":" + exception_jsonString(SYS_WIN_EDITION) + ",";
    out += "\"winDisplayVersion\":" + exception_jsonString(SYS_WIN_DISPLAY_VERSION) + ",";
    out += "\"language\":" + exception_jsonString(SYS_LANGUAGE_CODE) + ",";
    out += "\"virtualization\":" + exception_jsonString(SYS_VIRTUALIZATION) + ",";
    out += "\"compatibilityFlags\":" + exception_jsonString(SYS_COMPATIBILITY_FLAGS) + ",";
    out += "\"versionInfo\":" + exception_jsonString(SYS_VERSION_INFO) + ",";
    out += "\"isElevated\":" + std::string(SYS_IS_ELEVATED ? "true" : "false");
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

static std::string exception_buildReportJson(const CrashReportData& r) {
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
    exception_jsonAppendLinesArray(json, "loggerLines", r.logger.c_str());
    json += ",";
    exception_jsonAppendLinesArray(json, "logLines", r.logs.c_str());
    json += "}";

    return json;
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
// Telemetry with offline queue
// ---------------------------------------------------------------------------

// Extracts a short message from a JSON error body ("error":"..." or "message":"...").
static void exception_formatHttpError(const HttpClient::Response& res, char* out, size_t outSize) {
    char jsonMsgBuf[128] = {0};
    if (!res.body.empty() && res.body[0] == '{') {
        for (const char* key : {"\"error\"", "\"message\""}) {
            const char* p = strstr(res.body.c_str(), key);
            if (!p) continue;
            p = strchr(p + strlen(key), '"');
            if (!p) continue;
            p++; // skip opening quote
            size_t len = 0;
            while (p[len] && p[len] != '"' && len < sizeof(jsonMsgBuf) - 1)
                len++;
            if (len > 0) {
                memcpy(jsonMsgBuf, p, len);
                jsonMsgBuf[len] = '\0';
                break;
            }
        }
    }
    if (jsonMsgBuf[0])
        snprintf(out, outSize, "HTTP %d: %s", res.status, jsonMsgBuf);
    else
        snprintf(out, outSize, "HTTP %d", res.status);
}

// A rejected report (4xx) will never be accepted, other failures are worth retrying later.
static bool exception_isPermanentHttpFailure(int status) {
    return status >= 400 && status < 500 && status != 408 && status != 429;
}

// Sends one report synchronously. Deletes pendingPath when the report was delivered or permanently rejected.
static bool exception_sendReportSync(const std::string& json, const char* pendingPath, int maxWaitMs) {
    struct Result { bool done = false; bool ok = false; bool permanent = false; } result;

    HttpClient client;
    client.postJson(
        TELEMETRY_ENDPOINT_URL,
        json.c_str(),
        [&result](const HttpClient::Response& res) {
            result.done = true;
            result.ok = res.status >= 200 && res.status < 300;
            result.permanent = exception_isPermanentHttpFailure(res.status);
            if (result.ok) {
                logger_add("[TELEMETRY] Report sent successfully (HTTP %d)", res.status);
            } else {
                char err[256];
                exception_formatHttpError(res, err, sizeof(err));
                logger_add("[TELEMETRY] Report rejected by endpoint: %s", err);
            }
        },
        [&result](const std::string& err) {
            result.done = true;
            logger_add("[TELEMETRY] Failed to send report: %s", err.c_str());
        },
        5000);

    DWORD start = GetTickCount();
    while (!result.done && (int)(GetTickCount() - start) < maxWaitMs)
        client.poll(20);

    if (pendingPath && (result.ok || result.permanent))
        DeleteFileA(pendingPath);
    return result.ok;
}

static bool exception_readWholeFile(const char* path, std::string& out, size_t maxSize) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    DWORD size = GetFileSize(hFile, NULL);
    bool ok = false;
    if (size != INVALID_FILE_SIZE && size > 0 && size <= maxSize) {
        out.resize(size);
        DWORD got = 0;
        ok = ReadFile(hFile, &out[0], size, &got, NULL) && got == size;
    }
    CloseHandle(hFile);
    return ok;
}

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

    std::vector<std::string> names = exception_listGameFolderFiles("CoD2MP_s.exe.*.pending.json");
    int sent = 0;
    for (size_t i = 0; i < names.size() && sent < 10; i++) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s\\%s", EXE_DIRECTORY_PATH, names[i].c_str());

        // Too old to be useful
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &attr)) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER w, n;
            w.LowPart = attr.ftLastWriteTime.dwLowDateTime; w.HighPart = attr.ftLastWriteTime.dwHighDateTime;
            n.LowPart = now.dwLowDateTime;                  n.HighPart = now.dwHighDateTime;
            if (n.QuadPart > w.QuadPart && (n.QuadPart - w.QuadPart) / 10000000ULL > EXCEPTION_PENDING_MAX_AGE_DAYS * 86400ULL) {
                DeleteFileA(path);
                continue;
            }
        }

        std::string json;
        if (!exception_readWholeFile(path, json, 8 * 1024 * 1024) || json[0] != '{') {
            DeleteFileA(path);
            continue;
        }
        json.insert(1, "\"resent\":true,");

        logger_add("[TELEMETRY] Sending pending report %s", names[i].c_str());
        exception_sendReportSync(json, path, 15000);
        sent++;
    }
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
// Crash dialog
// ---------------------------------------------------------------------------

static void exception_resizeDialogClient(HWND hwnd, int clientW, int clientH, bool keepCentered) {
    if (clientW < CRASH_MIN_CLIENT_W) clientW = CRASH_MIN_CLIENT_W;
    if (clientH < CRASH_MIN_CLIENT_H) clientH = CRASH_MIN_CLIENT_H;

    const int maxClientW = GetSystemMetrics(SM_CXSCREEN) - 80;
    const int maxClientH = GetSystemMetrics(SM_CYSCREEN) - 120;
    if (clientW > maxClientW) clientW = maxClientW;
    if (clientH > maxClientH) clientH = maxClientH;

    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    RECT wr = {0, 0, clientW, clientH};
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    int newW = wr.right - wr.left;
    int newH = wr.bottom - wr.top;

    UINT flags = SWP_NOZORDER;
    int newX = 0;
    int newY = 0;
    if (keepCentered) {
        RECT oldWr = {};
        GetWindowRect(hwnd, &oldWr);
        int centerX = (oldWr.left + oldWr.right) / 2;
        int centerY = (oldWr.top + oldWr.bottom) / 2;
        newX = centerX - newW / 2;
        newY = centerY - newH / 2;

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (newX < 0) newX = 0;
        if (newY < 0) newY = 0;
        if (newX + newW > screenW) newX = screenW - newW;
        if (newY + newH > screenH) newY = screenH - newH;
        if (newX < 0) newX = 0;
        if (newY < 0) newY = 0;
    } else {
        flags |= SWP_NOMOVE;
    }

    SetWindowPos(hwnd, NULL, newX, newY, newW, newH, flags);
}

static void exception_layoutCrashDlg(HWND hwnd, int w, int h) {
    CrashDialogData* data = (CrashDialogData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    const int margin = 14, btnH = 42, gap = 10;

    // Header area: icon + heading + subtitle + save path
    int iconX     = margin;
    int textX     = iconX + HDR_ICON_SIZE + 12;
    int textW     = w - textX - margin;
    int headingY  = margin;
    int headingH  = 26;
    int savepathY = headingY + headingH + 6;
    int savepathH = 16;
    int linkY     = savepathY + savepathH + 4;
    int linkH     = 16;
    int headerBottom = linkY + linkH;
    // Center icon vertically within the actual text block
    int iconY     = headingY + (headerBottom - headingY - HDR_ICON_SIZE) / 2;

    const bool showDetails = (data && data->detailsVisible);

    // Fixed 2-line height reserved for the telemetry status area
    const int telemetryH = 36;

    const int usableW = w - margin * 2;
    int btnW = (usableW - gap * 2) / 3;
    if (btnW > 220) btnW = 220;
    if (btnW < 150) btnW = 150;

    int totalW = btnW * 3 + gap * 2;
    int btnX   = (w - totalW) / 2;

    int telemetryY = headerBottom + 8;
    int btnY = 0;
    int editY = 0;
    int editH = 0;
    if (showDetails) {
        btnY = h - margin - btnH;
        editY = telemetryY + telemetryH + gap;
        editH = btnY - gap - editY;
        if (editH < 120) {
            editH = 120;
            btnY = editY + editH + gap;
        }
    } else {
        btnY = telemetryY + telemetryH + 10;
    }

    HWND hIcon     = GetDlgItem(hwnd, IDC_CRASH_ICON);
    HWND hHeading  = GetDlgItem(hwnd, IDC_CRASH_HEADING);
    HWND hSavePath = GetDlgItem(hwnd, IDC_CRASH_SAVEPATH);
    HWND hLnkDetails = GetDlgItem(hwnd, IDC_LNK_DETAILS);
    HWND hTelemetry = GetDlgItem(hwnd, IDC_CRASH_TELEMETRY);
    HWND hEdit     = GetDlgItem(hwnd, IDC_CRASH_EDIT);
    HWND hBtnR     = GetDlgItem(hwnd, IDC_BTN_RESTART);
    HWND hBtnD     = GetDlgItem(hwnd, IDC_BTN_DUMP);
    HWND hBtnE     = GetDlgItem(hwnd, IDC_BTN_EXIT);

    if (hIcon)     SetWindowPos(hIcon,     NULL, iconX,  iconY,     HDR_ICON_SIZE, HDR_ICON_SIZE, SWP_NOZORDER);
    if (hHeading)  SetWindowPos(hHeading,  NULL, textX,  headingY,  textW, headingH,  SWP_NOZORDER);
    if (hSavePath) SetWindowPos(hSavePath, NULL, textX,  savepathY, textW, savepathH, SWP_NOZORDER);
    if (hLnkDetails) SetWindowPos(hLnkDetails, NULL, textX, linkY, textW, linkH, SWP_NOZORDER);
    if (hTelemetry) SetWindowPos(hTelemetry, NULL, textX, telemetryY, textW, telemetryH, SWP_NOZORDER);

    if (hEdit) {
        if (showDetails) {
            ShowWindow(hEdit, SW_SHOW);
            SetWindowPos(hEdit, NULL, margin, editY, usableW, editH, SWP_NOZORDER);
        } else {
            ShowWindow(hEdit, SW_HIDE);
        }
    }

    if (hBtnR)       SetWindowPos(hBtnR,       NULL, btnX,                    btnY, btnW, btnH, SWP_NOZORDER);
    if (hBtnD)       SetWindowPos(hBtnD,       NULL, btnX + (btnW + gap),     btnY, btnW, btnH, SWP_NOZORDER);
    if (hBtnE)       SetWindowPos(hBtnE,       NULL, btnX + (btnW + gap) * 2, btnY, btnW, btnH, SWP_NOZORDER);
}

// Subclass proc for the edit control: adds Ctrl+A select-all support
static LRESULT CALLBACK exception_editSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessage(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    CrashDialogData* data = (CrashDialogData*)GetWindowLongPtr(GetParent(hwnd), GWLP_USERDATA);
    if (data && data->hEditOrigProc)
        return CallWindowProc(data->hEditOrigProc, hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK exception_crashDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CrashDialogData* data = (CrashDialogData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        data = (CrashDialogData*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)data);

        RECT rc;
        GetClientRect(hwnd, &rc);
        const int margin = 14, btnH = 42, btnW = 200, gap = 10;

        data->hBrushBg        = CreateSolidBrush(CLR_WIN_BG);
        data->hBrushEdit      = CreateSolidBrush(CLR_EDIT_BG);
        data->hLinkCursor     = LoadCursor(NULL, IDC_HAND);
        data->hEditOrigProc   = NULL;
        data->hLinkFont       = NULL;
        data->hHeadingFont    = NULL;
        data->detailsVisible  = false;
        if (data->compactWidth <= 0)  data->compactWidth = rc.right - rc.left;
        if (data->compactHeight <= 0) data->compactHeight = rc.bottom - rc.top;
        if (data->expandedWidth <= 0) data->expandedWidth = CRASH_EXPANDED_CLIENT_W;
        if (data->expandedHeight <= 0) data->expandedHeight = CRASH_EXPANDED_CLIENT_H;

        // Play the system error sound to signal a crash
        MessageBeep(MB_ICONERROR);

        // --- Fonts ---
        data->hFont = CreateFont(
            15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

        // Button / label font: Segoe UI Semibold
        data->hBtnFont = CreateFont(
            14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        data->hLinkFont = CreateFont(
            14, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        // Heading font: larger, bold
        data->hHeadingFont = CreateFont(
            22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        // --- Header: icon + heading + subtitle + save path ---
        int iconX     = margin;
        int iconY     = (HDR_H - HDR_ICON_SIZE) / 2;
        int textX     = iconX + HDR_ICON_SIZE + 12;
        int textW     = rc.right - textX - margin;
        int headingY  = margin;
        int headingH  = 26;
        int savepathY = headingY + headingH + 6;
        int savepathH = 16;

        // System error icon (48x48)
        HICON hErrIcon = (HICON)LoadImage(NULL, IDI_ERROR, IMAGE_ICON,
            HDR_ICON_SIZE, HDR_ICON_SIZE, LR_SHARED);
        HWND hIconCtl = CreateWindowEx(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE,
            iconX, iconY, HDR_ICON_SIZE, HDR_ICON_SIZE,
            hwnd, (HMENU)IDC_CRASH_ICON, GetModuleHandle(NULL), NULL);
        SendMessage(hIconCtl, STM_SETICON, (WPARAM)hErrIcon, 0);

        // Heading
        HWND hHeading = CreateWindowEx(0, "STATIC", "Call of Duty 2 has crashed.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            textX, headingY, textW, headingH,
            hwnd, (HMENU)IDC_CRASH_HEADING, GetModuleHandle(NULL), NULL);
        SendMessage(hHeading, WM_SETFONT, (WPARAM)data->hHeadingFont, FALSE);

        // Save path
        char savePathText[MAX_PATH * 2 + 32] = {0};
        snprintf(savePathText, sizeof(savePathText), "The crash report was saved to: %s",
            (data->savePath && data->savePath[0]) ? data->savePath : "(unknown)");
        HWND hSavePath = CreateWindowEx(0, "STATIC", savePathText,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
            textX, savepathY, textW, savepathH,
            hwnd, (HMENU)IDC_CRASH_SAVEPATH, GetModuleHandle(NULL), NULL);
        SendMessage(hSavePath, WM_SETFONT, (WPARAM)data->hBtnFont, FALSE);

        HWND hDetailsLink = CreateWindowEx(0, "STATIC", "Show crash details",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
            textX, savepathY + savepathH + 4, textW, 16,
            hwnd, (HMENU)IDC_LNK_DETAILS, GetModuleHandle(NULL), NULL);
        SendMessage(hDetailsLink, WM_SETFONT, (WPARAM)data->hLinkFont, FALSE);

        HWND hTelemetry = CreateWindowEx(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
            margin, HDR_H + 10, rc.right - margin * 2, 36,
            hwnd, (HMENU)IDC_CRASH_TELEMETRY, GetModuleHandle(NULL), NULL);
        SendMessage(hTelemetry, WM_SETFONT, (WPARAM)data->hBtnFont, FALSE);
        exception_updateTelemetryLabel(hwnd, data);

        // --- Edit box (below header) ---
        int editY = HDR_H + 6;
        int editH = rc.bottom - editY - gap - btnH - margin;
        int btnY  = editY + editH + gap;
        int totalW2 = btnW * 3 + gap * 2;
        int btnX  = (rc.right - totalW2) / 2;

        HWND hEdit = CreateWindowEx(
            0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            margin, editY, rc.right - margin * 2, editH,
            hwnd, (HMENU)IDC_CRASH_EDIT, GetModuleHandle(NULL), NULL);

        // Subclass the edit control to enable Ctrl+A select-all
        data->hEditOrigProc = (WNDPROC)SetWindowLongPtr(hEdit, GWLP_WNDPROC, (LONG_PTR)exception_editSubclassProc);

        SendMessage(hEdit, WM_SETFONT, (WPARAM)data->hFont, FALSE);
        SendMessage(hEdit, EM_SETLIMITTEXT, 0, 0); // the report is larger than the default 32K limit
        SetWindowTextA(hEdit, data->text);
        ShowWindow(hEdit, SW_HIDE);

        // --- Buttons ---
        CreateWindowEx(0, "BUTTON", "Restart Game",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            btnX, btnY, btnW, btnH,
            hwnd, (HMENU)IDC_BTN_RESTART, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, "BUTTON", "Generate Full Dump",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            btnX + (btnW + gap), btnY, btnW, btnH,
            hwnd, (HMENU)IDC_BTN_DUMP, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, "BUTTON", "Exit",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            btnX + (btnW + gap) * 2, btnY, btnW, btnH,
            hwnd, (HMENU)IDC_BTN_EXIT, GetModuleHandle(NULL), NULL);

        SetTimer(hwnd, 1, 250, NULL);
        exception_layoutCrashDlg(hwnd, rc.right, rc.bottom);
        return 0;
    }
    case WM_ERASEBKGND: {
        if (!data) break;
        HDC hdc = (HDC)wParam;
        RECT rc2;
        GetClientRect(hwnd, &rc2);
        FillRect(hdc, &rc2, data->hBrushBg);
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        if (!data) break;
        HDC  hdc  = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        int ctlId = GetDlgCtrlID(hCtl);
        // ES_READONLY edit controls receive WM_CTLCOLORSTATIC on Windows,
        // so check the control ID to distinguish the log box from the header controls.
        if (ctlId == IDC_CRASH_EDIT) {
            SetBkColor(hdc, CLR_EDIT_BG);
            SetTextColor(hdc, CLR_EDIT_FG);
            return (LRESULT)data->hBrushEdit;
        }
        // Heading: bright white text on dark red tint background
        if (ctlId == IDC_CRASH_HEADING) {
            SetBkColor(hdc, CLR_WIN_BG);
            SetTextColor(hdc, RGB(255, 255, 255));
            return (LRESULT)data->hBrushBg;
        }
        if (ctlId == IDC_LNK_DETAILS) {
            SetBkColor(hdc, CLR_WIN_BG);
            SetTextColor(hdc, RGB(120, 180, 255));
            return (LRESULT)data->hBrushBg;
        }
        if (ctlId == IDC_CRASH_TELEMETRY) {
            SetBkColor(hdc, CLR_WIN_BG);
            LONG telState = InterlockedCompareExchange(&g_crashTelemetryState, 0, 0);
            if (telState == CRASH_TELEMETRY_SUCCESS)
                SetTextColor(hdc, RGB(90, 210, 90));   // green: sent ok
            else if (telState == CRASH_TELEMETRY_FAILED)
                SetTextColor(hdc, RGB(255, 90, 70));   // red: failed
            else if (telState == CRASH_TELEMETRY_SENDING)
                SetTextColor(hdc, RGB(220, 190, 70));  // amber: in progress
            else
                SetTextColor(hdc, RGB(160, 140, 140)); // grey: idle
            return (LRESULT)data->hBrushBg;
        }
        // Subtitle / icon / save path: muted light text
        SetBkColor(hdc, CLR_WIN_BG);
        SetTextColor(hdc, RGB(170, 170, 170));
        return (LRESULT)data->hBrushBg;
    }
    case WM_DRAWITEM: {
        if (!data) break;
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        COLORREF clrBg;
        if (dis->CtlID == IDC_BTN_RESTART)
            clrBg = pressed ? CLR_BTN_RESTART_P : CLR_BTN_RESTART_N;
        else if (dis->CtlID == IDC_BTN_DUMP)
            clrBg = pressed ? CLR_BTN_DUMP_P    : CLR_BTN_DUMP_N;
        else
            clrBg = pressed ? CLR_BTN_EXIT_P    : CLR_BTN_EXIT_N;

        HBRUSH hBrush = CreateSolidBrush(clrBg);
        FillRect(dis->hDC, &dis->rcItem, hBrush);
        DeleteObject(hBrush);

        char text[64] = {0};
        GetWindowTextA(dis->hwndItem, text, sizeof(text));
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, CLR_BTN_TEXT);
        HFONT oldFont = (HFONT)SelectObject(dis->hDC, data->hBtnFont);
        DrawTextA(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldFont);

        if (dis->itemState & ODS_FOCUS) {
            RECT fr = dis->rcItem;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(dis->hDC, &fr);
        }
        return TRUE;
    }
    case WM_SIZE:
        if (data) {
            int clientW = LOWORD(lParam);
            int clientH = HIWORD(lParam);
            if (data->detailsVisible) {
                data->expandedWidth = clientW;
                data->expandedHeight = clientH;
            } else {
                data->compactWidth = clientW;
                data->compactHeight = clientH;
            }
            exception_layoutCrashDlg(hwnd, clientW, clientH);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        RECT minRect = {0, 0, CRASH_MIN_CLIENT_W, CRASH_MIN_CLIENT_H};
        AdjustWindowRectEx(&minRect, style, FALSE, exStyle);
        mmi->ptMinTrackSize = {
            minRect.right - minRect.left,
            minRect.bottom - minRect.top
        };
        return 0;
    }
    case WM_SETCURSOR: {
        if (data && LOWORD(lParam) == HTCLIENT && data->hLinkCursor) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            HWND hChild = ChildWindowFromPoint(hwnd, pt);
            if (hChild && GetDlgCtrlID(hChild) == IDC_LNK_DETAILS) {
                SetCursor(data->hLinkCursor);
                return TRUE;
            }
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_LNK_DETAILS && HIWORD(wParam) == STN_CLICKED) {
            if (data) {
                data->detailsVisible = !data->detailsVisible;

                HWND hLink = GetDlgItem(hwnd, IDC_LNK_DETAILS);
                SetWindowTextA(hLink,
                    data->detailsVisible ? "Hide crash details" : "Show crash details");

                RECT clientRc = {};
                GetClientRect(hwnd, &clientRc);
                int currentClientW = clientRc.right - clientRc.left;
                int currentClientH = clientRc.bottom - clientRc.top;

                if (data->detailsVisible) {
                    data->compactWidth = currentClientW;
                    data->compactHeight = currentClientH;
                    int desiredW = data->expandedWidth > 0 ? data->expandedWidth : CRASH_EXPANDED_CLIENT_W;
                    int desiredH = data->expandedHeight > 0 ? data->expandedHeight : CRASH_EXPANDED_CLIENT_H;
                    exception_resizeDialogClient(hwnd, desiredW, desiredH, true);
                } else {
                    data->expandedWidth = currentClientW;
                    data->expandedHeight = currentClientH;
                    int desiredW = data->compactWidth > 0 ? data->compactWidth : CRASH_COMPACT_CLIENT_W;
                    int desiredH = data->compactHeight > 0 ? data->compactHeight : CRASH_COMPACT_CLIENT_H;
                    exception_resizeDialogClient(hwnd, desiredW, desiredH, true);
                }

                RECT rc;
                GetClientRect(hwnd, &rc);
                exception_layoutCrashDlg(hwnd, rc.right, rc.bottom);
            }
        } else if (LOWORD(wParam) == IDC_BTN_RESTART) {
            if (data) data->wantRestart = true;
            PostQuitMessage(0);
        } else if (LOWORD(wParam) == IDC_BTN_DUMP) {
            if (data) {
                char dmpPath[MAX_PATH * 2] = {0};
                HCURSOR oldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
                bool ok = exception_createMiniDump(data->pExInfo, data->crashThreadId, true, "crash", dmpPath, sizeof(dmpPath));
                SetCursor(oldCursor);
                if (ok) {
                    char dmpMsg[MAX_PATH * 2 + 256];
                    snprintf(dmpMsg, sizeof(dmpMsg),
                        "Full diagnostic file was saved to:\n%s\n"
                        "\n"
                        "Please send this file to the developers.\n"
                        "You can reach them on the Discord - more info at https://cod2x.me/.\n"
                        "\n"
                        "Without this file we would not have a chance to fix this problem. Thank you!",
                        dmpPath);
                    MessageBoxA(hwnd, dmpMsg, "Crash Dump Saved", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                } else {
                    MessageBoxA(hwnd,
                        "Failed to create crash dump file.",
                        "Crash Dump Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
                }
            }
        } else if (LOWORD(wParam) == IDC_BTN_EXIT) {
            if (data) data->wantRestart = false;
            PostQuitMessage(0);
        }
        return 0;
    case WM_TIMER:
        if (wParam == 1 && data) {
            exception_updateTelemetryLabel(hwnd, data);
        }
        return 0;
    case WM_CLOSE:
        if (data) data->wantRestart = false;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        if (data) {
            KillTimer(hwnd, 1);
            if (data->hFont)        { DeleteObject(data->hFont);        data->hFont = NULL; }
            if (data->hBtnFont)     { DeleteObject(data->hBtnFont);     data->hBtnFont = NULL; }
            if (data->hLinkFont)    { DeleteObject(data->hLinkFont);    data->hLinkFont = NULL; }
            if (data->hHeadingFont) { DeleteObject(data->hHeadingFont); data->hHeadingFont = NULL; }
            if (data->hBrushBg)     { DeleteObject(data->hBrushBg);     data->hBrushBg = NULL; }
            if (data->hBrushEdit)   { DeleteObject(data->hBrushEdit);   data->hBrushEdit = NULL; }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * Show the crash dialog with a dark-themed scrollable report.
 * Returns true if the user chose to restart the game.
 */
static bool exception_showCrashDialog(const char* content, EXCEPTION_POINTERS* pExInfo, DWORD crashThreadId, const char* savePath) {
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = exception_crashDlgProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   // handled in WM_ERASEBKGND
    wc.lpszClassName = CRASH_DLG_CLASS;
    wc.hIcon         = LoadIcon(NULL, IDI_ERROR);
    RegisterClassEx(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int compactClientW = (CRASH_COMPACT_CLIENT_W < screenW - 80) ? CRASH_COMPACT_CLIENT_W : (screenW - 80);
    int compactClientH = (CRASH_COMPACT_CLIENT_H < screenH - 120) ? CRASH_COMPACT_CLIENT_H : (screenH - 120);
    int expandedClientW = (CRASH_EXPANDED_CLIENT_W < screenW - 80) ? CRASH_EXPANDED_CLIENT_W : (screenW - 80);
    int expandedClientH = (CRASH_EXPANDED_CLIENT_H < screenH - 120) ? CRASH_EXPANDED_CLIENT_H : (screenH - 120);

    DWORD dlgStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    DWORD dlgExStyle = WS_EX_TOPMOST;
    RECT compactRect = {0, 0, compactClientW, compactClientH};
    AdjustWindowRectEx(&compactRect, dlgStyle, FALSE, dlgExStyle);

    int dlgW = compactRect.right - compactRect.left;
    int dlgH = compactRect.bottom - compactRect.top;
    int dlgX    = (screenW - dlgW) / 2;
    int dlgY    = (screenH - dlgH) / 2;

    CrashDialogData data = {
        content, savePath, pExInfo, crashThreadId, false,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
        false,
        compactClientW, compactClientH,
        expandedClientW, expandedClientH
    };

    HWND hwnd = CreateWindowEx(
        dlgExStyle,
        CRASH_DLG_CLASS, "Call of Duty 2 - Application Crash",
        dlgStyle,
        dlgX, dlgY, dlgW, dlgH,
        NULL, NULL, GetModuleHandle(NULL), &data);

    if (hwnd) {
        SetForegroundWindow(hwnd);
        MSG wmsg;
        while (GetMessage(&wmsg, NULL, 0, 0) > 0) {
            TranslateMessage(&wmsg);
            DispatchMessage(&wmsg);
            // Poll HTTP client to allow telemetry requests to complete while dialog is open
            if (g_crashDialogHttpClient) {
                g_crashDialogHttpClient->poll(10);  // 10ms poll
            }
        }
        DestroyWindow(hwnd);
        UnregisterClass(CRASH_DLG_CLASS, GetModuleHandle(NULL));
    }

    return data.wantRestart;
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
    exception_buildFilePath(pendingPath, sizeof(pendingPath), "crash", exception_crashTime, ".pending.json");

    exception_stage = "restore gamma";
    gamma_restore();

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

    // Stage 4: telemetry. The JSON is saved first and deleted once delivered, so a report that could not be sent
    // (offline, process killed while sending) is sent on the next game start.
    // Telemetry is sent for real crashes and for self-test crashes alike (self-test reports carry
    // crashInfo.test and the backend acks them without storing). Only real crashes are queued offline.
    exception_stage = "telemetry";
    #if !DEBUG || EXCEPTION_TEST
      {
        static HttpClient httpClient;
        static char pendingPathForCallback[MAX_PATH * 2];
        snprintf(pendingPathForCallback, sizeof(pendingPathForCallback), "%s", pendingPath);
        if (!testMode)
            exception_writeFile(pendingPath, crashJson.c_str(), crashJson.size(), false);

        InterlockedExchange(&g_crashTelemetrySpinner, 0);
        InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_SENDING);
        g_crashTelemetryError[0] = '\0';
        g_crashDialogHttpClient = &httpClient;  // Allow crash dialog to poll for telemetry completion
        httpClient.postJson(
            TELEMETRY_ENDPOINT_URL,
            crashJson.c_str(),
            [](const HttpClient::Response& res) {
                if (res.status >= 200 && res.status < 300) {
                    logger_add("[TELEMETRY] Crash report sent successfully (HTTP %d)", res.status);
                    DeleteFileA(pendingPathForCallback);
                    InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_SUCCESS);
                    return;
                }
                exception_formatHttpError(res, g_crashTelemetryError, sizeof(g_crashTelemetryError));
                logger_add("[TELEMETRY] Crash report rejected by endpoint: %s", g_crashTelemetryError);
                if (exception_isPermanentHttpFailure(res.status))
                    DeleteFileA(pendingPathForCallback);
                InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_FAILED);
            },
            [](const std::string& error) {
                logger_add("[TELEMETRY] Failed to send crash report: %s", error.c_str());
                snprintf(g_crashTelemetryError, sizeof(g_crashTelemetryError), "%s", error.c_str());
                InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_FAILED);
            },
            5000
        );
      }
    #endif

    exception_stage = "dialog";
    bool wantRestart = false;
    if (testMode)
        logger_add("[EXCEPTION_TEST] report written to %s, exiting without dialog", exception_reportPath);
    else
        wantRestart = exception_showCrashDialog(displayText.c_str(), pExceptionInfo, crashThreadId, exception_reportPath);

    // The dialog is often closed before the report is delivered, give it a moment to finish
    exception_stage = "telemetry wait";
    if (g_crashDialogHttpClient) {
        DWORD waitStart = GetTickCount();
        while (InterlockedCompareExchange(&g_crashTelemetryState, 0, 0) == CRASH_TELEMETRY_SENDING &&
               GetTickCount() - waitStart < EXCEPTION_TELEMETRY_WAIT_MS) {
            g_crashDialogHttpClient->poll(20);
        }
        g_crashDialogHttpClient = nullptr;
    }

    // In self-test mode there is no dialog; record the delivery outcome so the E2E runner can verify it.
    if (testMode) {
        LONG st = InterlockedCompareExchange(&g_crashTelemetryState, 0, 0);
        char line[384];
        snprintf(line, sizeof(line), "\r\nTelemetry: %s%s%s\r\n",
            st == CRASH_TELEMETRY_SUCCESS ? "sent" : st == CRASH_TELEMETRY_FAILED ? "failed" : "pending",
            (st == CRASH_TELEMETRY_FAILED && g_crashTelemetryError[0]) ? " - " : "",
            (st == CRASH_TELEMETRY_FAILED && g_crashTelemetryError[0]) ? g_crashTelemetryError : "");
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
    exception_buildFilePath(pendingPath, sizeof(pendingPath), "freeze", st, ".pending.json");

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
        exception_sendReportSync(json, pendingPath, 7000);
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
