#include "../shared/http_client.h"
#include "../shared/json.h"

#include "exception.h"

#include <stdio.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <string>
#include <vector>
#include <winternl.h>

#include "shared.h"
#include "gamma.h"
#include "error.h"
#include "system.h"
#include "main.h"
#include "console.h"
#include "../shared/cod2_dvars.h"
#include "../shared/logger.h"
#include "symbols_cod2mp_s.h"
#include "symbols_gfx_d3d_mp_x86_s.h"
#include "symbols_mss32.h"

#define TELEMETRY_ENDPOINT_URL "https://master.cod2x.me/api/telemetry/cod2x-crashes"
//#define TELEMETRY_ENDPOINT_URL "http://localhost:8080/api/telemetry/cod2x-crashes"

// Global HttpClient pointer for telemetry polling during crash dialog
HttpClient* g_crashDialogHttpClient = nullptr;

#define EXCEPTION_TEST 0
#define EXCEPTION_TEST_DELAY_MS 5000

// Value 0 = no crash in progress; >= 1 = handler already running (re-entry guard).
volatile long exception_processCrashed = 0;

// Timestamp captured once at the start of exception_handler; shared by the .txt and .dmp filenames.
static SYSTEMTIME exception_crashTime;

// When a stack-overflow crash is handled via the vectored handler, the actual faulting
// thread ID is stored here so exception_handler can label it correctly (it runs on a
// separate thread with a fresh stack in that case).
static volatile DWORD exception_stackOverflowThreadId = 0;

// Prevents double-handling if multiple threads overflow simultaneously.
static volatile long exception_stackOverflowCaught = 0;

// The unhandled exception filter that was installed before ours (the game's own handler,
// if any). We call it first so that game-side recovery (EXCEPTION_CONTINUE_EXECUTION)
// is never silently discarded when it would let the game continue running.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUnhandledFilter = nullptr;

LONG WINAPI exception_handler(EXCEPTION_POINTERS* pExceptionInfo);

extern char hwid_regid[33];


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
                "Failed to send crash report: %s\nPlease report this crash manually on Discord - https://cod2x.me/",
                g_crashTelemetryError[0] ? g_crashTelemetryError : "unknown error");
            break;
        default:
            snprintf(status, sizeof(status),
                "Crash report will be sent to CoD2x developers for analysis.");
            break;
    }

    SetWindowTextA(hTelemetry, status);
}

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
    std::string moduleName;
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
    std::string threadName;
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

    std::string processId;
    std::string executablePath;
    std::string commandLine;
    std::string workingDirectory;

    std::vector<JsonModuleInfoData> modules;
    std::vector<JsonNamedValueData> cvars;
    std::vector<JsonMainFolderFileInfoData> mainFolderFiles;
};

struct JsonCrashContextData {
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
};

static const char* exception_jsonBoolText(bool value) {
    return value ? "true" : "false";
}

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
            out += "\"";
            out += json_escape_string(std::string(lineStart, lineLen));
            out += "\"";
        }

        while (*p == '\n' || *p == '\r') {
            ++p;
        }
    }

    out += "]";
}

static void exception_jsonAppendNamedValuesObject(std::string& out, const std::vector<JsonNamedValueData>& values) {
    out += "{";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "\"" + json_escape_string(values[i].name) + "\":\"" + json_escape_string(values[i].value) + "\"";
    }
    out += "}";
}

static void exception_jsonAppendSystemInfoObject(std::string& out, const JsonSystemInfoData& systemInfo) {
    out += "\"systemInfo\":{";

    out += "\"system\":{";
    out += "\"cpuCores\":\"" + json_escape_string(systemInfo.cpuCores) + "\",";
    out += "\"cpuAffinity\":\"" + json_escape_string(systemInfo.cpuAffinity) + "\",";
    out += "\"screenResolution\":\"" + json_escape_string(systemInfo.screenResolution) + "\",";
    out += "\"winBuild\":\"" + json_escape_string(SYS_WIN_BUILD) + "\",";
    out += "\"winEdition\":\"" + json_escape_string(SYS_WIN_EDITION) + "\",";
    out += "\"winDisplayVersion\":\"" + json_escape_string(SYS_WIN_DISPLAY_VERSION) + "\",";
    out += "\"language\":\"" + json_escape_string(SYS_LANGUAGE_CODE) + "\",";
    out += "\"virtualization\":\"" + json_escape_string(SYS_VIRTUALIZATION) + "\",";
    out += "\"compatibilityFlags\":\"" + json_escape_string(SYS_COMPATIBILITY_FLAGS) + "\",";
    out += "\"isElevated\":" + std::string(SYS_IS_ELEVATED ? "true" : "false");
    out += "},";
    out += "\"memory\":{";
    out += "\"memoryLoadPercent\":\"" + json_escape_string(systemInfo.memoryLoadPercent) + "\",";
    out += "\"physicalTotalMB\":\"" + json_escape_string(systemInfo.physicalTotalMB) + "\",";
    out += "\"physicalAvailableMB\":\"" + json_escape_string(systemInfo.physicalAvailableMB) + "\",";
    out += "\"virtualTotalMB\":\"" + json_escape_string(systemInfo.virtualTotalMB) + "\",";
    out += "\"virtualAvailableMB\":\"" + json_escape_string(systemInfo.virtualAvailableMB) + "\"";
    out += "},";

    out += "\"process\":{";
    out += "\"processId\":\"" + json_escape_string(systemInfo.processId) + "\",";
    out += "\"executablePath\":\"" + json_escape_string(systemInfo.executablePath) + "\",";
    out += "\"commandLine\":\"" + json_escape_string(systemInfo.commandLine) + "\",";
    out += "\"workingDirectory\":\"" + json_escape_string(systemInfo.workingDirectory) + "\"";
    out += "},";

    out += "\"modules\":[";
    for (size_t i = 0; i < systemInfo.modules.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "{";
        out += "\"baseAddress\":\"" + json_escape_string(systemInfo.modules[i].baseAddress) + "\",";
        out += "\"moduleName\":\"" + json_escape_string(systemInfo.modules[i].moduleName) + "\"";
        out += "}";
    }
    out += "],";

    out += "\"cvars\":";
    exception_jsonAppendNamedValuesObject(out, systemInfo.cvars);
    out += ",";

    out += "\"mainFolderFiles\":[";
    for (size_t i = 0; i < systemInfo.mainFolderFiles.size(); ++i) {
        const JsonMainFolderFileInfoData& entry = systemInfo.mainFolderFiles[i];
        if (i > 0) {
            out += ",";
        }
        out += "{";
        out += "\"entryType\":\"" + json_escape_string(entry.entryType) + "\"";
        if (!entry.size.empty()) {
            out += ",\"size\":\"" + json_escape_string(entry.size) + "\"";
        }
        if (!entry.name.empty()) {
            out += ",\"name\":\"" + json_escape_string(entry.name) + "\"";
        }
        if (!entry.message.empty()) {
            out += ",\"message\":\"" + json_escape_string(entry.message) + "\"";
        }
        out += "}";
    }
    out += "]";

    out += "}";
}

static void exception_jsonAppendCrashContextObject(std::string& out, const JsonCrashContextData& crashContext) {
    out += "\"crashContext\":{";
    out += "\"registers\":{";
    out += "\"EIP\":\"" + json_escape_string(crashContext.eip) + "\",";
    out += "\"EAX\":\"" + json_escape_string(crashContext.eax) + "\",";
    out += "\"EBX\":\"" + json_escape_string(crashContext.ebx) + "\",";
    out += "\"ECX\":\"" + json_escape_string(crashContext.ecx) + "\",";
    out += "\"EDX\":\"" + json_escape_string(crashContext.edx) + "\",";
    out += "\"ESI\":\"" + json_escape_string(crashContext.esi) + "\",";
    out += "\"EDI\":\"" + json_escape_string(crashContext.edi) + "\",";
    out += "\"EBP\":\"" + json_escape_string(crashContext.ebp) + "\",";
    out += "\"ESP\":\"" + json_escape_string(crashContext.esp) + "\",";
    out += "\"EFLAGS\":\"" + json_escape_string(crashContext.eflags) + "\"";
    out += "},";

    out += "\"threads\":[";
    for (size_t i = 0; i < crashContext.threads.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        const JsonThreadInfoData& thread = crashContext.threads[i];
        out += "{";
        out += "\"threadId\":\"" + std::to_string(thread.threadId) + "\",";
        out += "\"startAddress\":\"" + json_escape_string(thread.startAddress) + "\",";
        out += "\"threadName\":\"" + json_escape_string(thread.threadName) + "\"";
        out += "}";
    }
    out += "]";

    out += "}";
}

static void exception_jsonAppendFramesArray(std::string& out, const std::vector<JsonStackFrameData>& frames) {
    out += "[";
    for (size_t i = 0; i < frames.size(); ++i) {
        const JsonStackFrameData& frame = frames[i];
        char addrHex[16];
        char offHex[16];
        snprintf(addrHex, sizeof(addrHex), "0x%08X", frame.instructionAddress);
        snprintf(offHex, sizeof(offHex), "0x%08X", frame.moduleOffset);

        if (i > 0) {
            out += ",";
        }
        out += "{";
        out += "\"frameNumber\":" + std::to_string(frame.frameNumber) + ",";
        out += "\"instructionAddress\":\"" + json_escape_string(addrHex) + "\",";
        out += "\"moduleOffset\":\"" + json_escape_string(offHex) + "\",";
        out += "\"moduleName\":\"" + json_escape_string(frame.moduleName) + "\",";
        out += "\"sectionName\":\"" + json_escape_string(frame.sectionName) + "\",";
        out += "\"resolvedSymbol\":\"" + json_escape_string(frame.resolvedSymbol) + "\",";
        out += "\"instruction\":\"" + json_escape_string(frame.instruction) + "\"";
        out += "}";
    }
    out += "]";
}

static void exception_jsonAppendDiagnosticsObject(std::string& out, const JsonStackDiagnosticsData& d) {
    out += "{";
    out += "\"method\":\"" + json_escape_string(d.method) + "\",";
    out += "\"unwindFrames\":" + std::to_string(d.unwindFrames) + ",";
    out += "\"contextRepaired\":" + std::string(exception_jsonBoolText(d.contextRepaired)) + ",";
    out += "\"ebpFallbackAttempted\":" + std::string(exception_jsonBoolText(d.ebpFallbackAttempted)) + ",";
    out += "\"ebpFallbackFrames\":" + std::to_string(d.ebpFallbackFrames) + ",";
    out += "\"rawEspScanAttempted\":" + std::string(exception_jsonBoolText(d.rawEspScanAttempted)) + ",";
    out += "\"rawEspScanFrames\":" + std::to_string(d.rawEspScanFrames) + ",";
    out += "\"symInitializeStatus\":\"" + json_escape_string(d.symInitializeStatus) + "\",";
    out += "\"symInitializeCode\":" + std::to_string(d.symInitializeCode) + ",";
    out += "\"symInitializeMessage\":\"" + json_escape_string(d.symInitializeMessage) + "\",";
    out += "\"stackWalkStatus\":\"" + json_escape_string(d.stackWalkStatus) + "\",";
    out += "\"stackWalkCode\":" + std::to_string(d.stackWalkCode) + ",";
    out += "\"stackWalkMessage\":\"" + json_escape_string(d.stackWalkMessage) + "\"";
    out += "}";
}

static void exception_jsonAppendStacksArray(std::string& out, const std::vector<StackCollectionResult>& stacks) {
    out += "\"stacks\":[";
    for (size_t s = 0; s < stacks.size(); ++s) {
        const StackCollectionResult& st = stacks[s];
        if (s > 0) {
            out += ",";
        }
        out += "{";
        out += "\"threadRole\":\"" + json_escape_string(st.threadRole) + "\",";
        out += "\"threadName\":\"" + json_escape_string(st.threadName) + "\",";
        out += "\"threadId\":\"" + std::to_string(st.threadId) + "\",";
        out += "\"method\":\"" + json_escape_string(st.method) + "\",";
        out += "\"frames\":";
        exception_jsonAppendFramesArray(out, st.frames);
        out += ",";
        out += "\"diagnostics\":";
        exception_jsonAppendDiagnosticsObject(out, st.diagnostics);
        out += "}";
    }
    out += "]";
}

static std::string exception_buildCrashJson(
    unsigned int exceptionCode,
    const char* exceptionName,
    unsigned int exceptionAddress,
    const char* moduleName,
    unsigned int fileOffset,
    unsigned long crashThreadId,
    unsigned long currentThreadId,
    const char* version,
    const std::vector<StackCollectionResult>& stacks,
    const JsonCrashContextData& crashContext,
    const JsonSystemInfoData& systemInfo,
    const char* logger,
    const char* logs)
{
    char exceptionCodeHex[16];
    char exceptionAddressHex[16];
    char fileOffsetHex[16];

    snprintf(exceptionCodeHex, sizeof(exceptionCodeHex), "0x%08X", exceptionCode);
    snprintf(exceptionAddressHex, sizeof(exceptionAddressHex), "0x%08X", exceptionAddress);
    snprintf(fileOffsetHex, sizeof(fileOffsetHex), "0x%08X", fileOffset);

    const char* safeExceptionName = exceptionName ? exceptionName : "";
    const char* safeModuleName = moduleName ? moduleName : "";
    const char* safeVersion = version ? version : "";
    const char* safeLogger = logger ? logger : "";
    const char* safeLogs = logs ? logs : "";

    std::string json;
    json.reserve(1024 + strlen(safeLogger) + strlen(safeLogs));

    json += "{";
    json += "\"version\":\"" + json_escape_string(safeVersion) + "\",";
    json += "\"type\":\"" + json_escape_string("win-client") + "\",";
    json += "\"installId\":\"" + json_escape_string(hwid_regid) + "\",";
    json += "\"crashInfo\":{";
    json += "\"exceptionCode\":\"" + json_escape_string(exceptionCodeHex) + "\",";
    json += "\"exceptionName\":\"" + json_escape_string(safeExceptionName) + "\",";
    json += "\"exceptionAddress\":\"" + json_escape_string(exceptionAddressHex) + "\",";
    json += "\"moduleName\":\"" + json_escape_string(safeModuleName) + "\",";
    json += "\"fileOffset\":\"" + json_escape_string(fileOffsetHex) + "\",";
    json += "\"crashThreadId\":\"" + std::to_string(crashThreadId) + "\",";
    json += "\"currentThreadId\":\"" + std::to_string(currentThreadId) + "\"";
    json += "},";

    exception_jsonAppendCrashContextObject(json, crashContext);
    json += ",";

    exception_jsonAppendStacksArray(json, stacks);
    json += ",";

    exception_jsonAppendSystemInfoObject(json, systemInfo);
    json += ",";
    exception_jsonAppendLinesArray(json, "loggerLines", safeLogger);
    json += ",";
    exception_jsonAppendLinesArray(json, "logLines", safeLogs);
    json += "}";

    return json;
}

// Convert LF to CRLF so the Win32 EDIT control renders newlines correctly
static size_t exception_lf_to_crlf(const char* src, char* dst, size_t dstSize) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 2 < dstSize; si++) {
        if (src[si] == '\n' && (si == 0 || src[si - 1] != '\r'))
            dst[di++] = '\r';
        dst[di++] = src[si];
    }
    if (di < dstSize) dst[di] = '\0';
    return di;
}

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




/*
 * Determine the exception name based on the exception code
 */
const char* exception_getText(DWORD exceptionCode) {
    const char *exceptionName;
    switch (exceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
            exceptionName = "ACCESS_VIOLATION";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            exceptionName = "EXCEPTION_STACK_OVERFLOW";
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            exceptionName = "FLT_DIVIDE_BY_ZERO";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            exceptionName = "INT_DIVIDE_BY_ZERO";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            exceptionName = "ILLEGAL_INSTRUCTION";
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            exceptionName = "PRIV_INSTRUCTION";
            break;
        default:
            exceptionName = "Unknown Exception";
            break;
    }
    return exceptionName;
}

bool exception_createMiniDump(EXCEPTION_POINTERS* pExceptionInfo, char* pathOut, size_t pathOutSize) {
    const SYSTEMTIME& st = exception_crashTime;
    char dmpFilename[MAX_PATH];
    snprintf(dmpFilename, sizeof(dmpFilename), "CoD2MP_s.exe.crash.%04d%02d%02d_%02d%02d%02d.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    if (pathOut && pathOutSize > 0)
        snprintf(pathOut, pathOutSize, "%s", dmpFilename);

    HANDLE hFile = CreateFile(dmpFilename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, "Failed to create crash dump file.", "Application error", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = pExceptionInfo;
    mdei.ClientPointers = FALSE;

    BOOL bSuccess = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                      hFile, MiniDumpWithFullMemory,
                                      (pExceptionInfo ? &mdei : NULL), NULL, NULL);
    CloseHandle(hFile);

    if (!bSuccess) {
        MessageBox(NULL, "Failed to create crash dump file.", "Application error", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return false;
    }

    return true;
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
        SetWindowTextA(hEdit, data->text);
        // Scroll to bottom so the most recent log lines are immediately visible
        SendMessage(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessage(hEdit, EM_SCROLLCARET, 0, 0);
        ShowWindow(hEdit, SW_HIDE);

        // --- Buttons ---
        CreateWindowEx(0, "BUTTON", "Restart Game",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            btnX, btnY, btnW, btnH,
            hwnd, (HMENU)IDC_BTN_RESTART, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, "BUTTON", "Generate Dump",
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
                char dmpPath[MAX_PATH] = {0};
                bool ok = exception_createMiniDump(data->pExInfo, dmpPath, sizeof(dmpPath));
                if (ok) {
                    char dmpMsg[MAX_PATH + 256];
                    snprintf(dmpMsg, sizeof(dmpMsg),
                        "Diagnostic file '%s' saved into the CoD2 folder.\n"
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

static size_t exception_buildCvarDump(char* buf, size_t bufSize) {
    size_t pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "=== CVARS ===\r\n");

    const char* cvarNames[] = {
        "cl_hwid2", "name", "fs_game", "r_mode", "r_displayRefresh",
        "r_fullscreen", "r_monitor", "vid_xpos", "vid_ypos"
    };

    for (size_t i = 0; i < sizeof(cvarNames) / sizeof(cvarNames[0]); ++i) {
        if (pos + 128 >= bufSize) break;
        dvar_t* dv = Dvar_GetDvarByName(cvarNames[i]);
        if (!dv) {
            pos += snprintf(buf + pos, bufSize - pos, "%-20s = <not registered>\r\n", cvarNames[i]);
            continue;
        }
        switch (dv->type) {
            case DVAR_TYPE_BOOL:
                pos += snprintf(buf + pos, bufSize - pos, "%-20s = %s\r\n",
                    cvarNames[i], dv->value.boolean ? "true" : "false");
                break;
            case DVAR_TYPE_INT:
                pos += snprintf(buf + pos, bufSize - pos, "%-20s = %d\r\n",
                    cvarNames[i], dv->value.integer);
                break;
            case DVAR_TYPE_FLOAT:
                pos += snprintf(buf + pos, bufSize - pos, "%-20s = %.4f\r\n",
                    cvarNames[i], dv->value.decimal);
                break;
            case DVAR_TYPE_ENUM:
                if (dv->limits.enumeration.strings &&
                    dv->value.integer >= 0 &&
                    dv->value.integer < dv->limits.enumeration.stringCount) {
                    pos += snprintf(buf + pos, bufSize - pos, "%-20s = %s (%d)\r\n",
                        cvarNames[i],
                        dv->limits.enumeration.strings[dv->value.integer],
                        dv->value.integer);
                } else {
                    pos += snprintf(buf + pos, bufSize - pos, "%-20s = %d\r\n",
                        cvarNames[i], dv->value.integer);
                }
                break;
            case DVAR_TYPE_STRING:
                pos += snprintf(buf + pos, bufSize - pos, "%-20s = %s\r\n",
                    cvarNames[i], dv->value.string ? dv->value.string : "<null>");
                break;
            default:
                pos += snprintf(buf + pos, bufSize - pos, "%-20s = <type %d>\r\n",
                    cvarNames[i], (int)dv->type);
                break;
        }
    }
    return pos;
}

/**
 * List files in the main folder (name + size in bytes).
 */
static size_t exception_buildMainFolderFiles(char* buf, size_t bufSize) {
    size_t pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "=== MAIN FOLDER FILES ===\r\n");

    dvar_t* fs_basePath = Dvar_GetDvarByName("fs_basepath");
    if (!fs_basePath || !fs_basePath->value.string || fs_basePath->value.string[0] == '\0') {
        pos += snprintf(buf + pos, bufSize - pos, "<fs_basepath not set>\r\n");
        return pos;
    }

    char mainDir[MAX_PATH * 2] = {0};
    snprintf(mainDir, sizeof(mainDir), "%s\\main", fs_basePath->value.string);

    DIR* dir = opendir(mainDir);
    if (!dir) {
        pos += snprintf(buf + pos, bufSize - pos, "<failed to open '%s'>\r\n", mainDir);
        return pos;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char* name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (pos + 256 >= bufSize) break;

        char fullPath[MAX_PATH * 2] = {0};
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", mainDir, name);

        struct stat st;
        if (stat(fullPath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                pos += snprintf(buf + pos, bufSize - pos, "       [DIR]  %s\r\n", name);
            } else {
                pos += snprintf(buf + pos, bufSize - pos, "  %10lld  %s\r\n", (long long)st.st_size, name);
            }
        } else {
            pos += snprintf(buf + pos, bufSize - pos, "  %10s  %s\r\n", "?", name);
        }
    }
    closedir(dir);
    return pos;
}

/**
 * Save crash text to a timestamped file in the game root directory.
 * Filename format: CoD2MP_s.exe.crash.YYYYMMDD.txt
 * Writes the full absolute path into pathOut (if non-null).
 * Content is expected to already use CRLF line endings.
 */
static void exception_saveToFile(const char* content, char* pathOut, size_t pathOutSize) {
    const SYSTEMTIME& st = exception_crashTime;

    char filename[MAX_PATH];
    snprintf(filename, sizeof(filename), "CoD2MP_s.exe.crash.%04d%02d%02d_%02d%02d%02d.txt",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // Build full absolute path
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    char fullPath[MAX_PATH * 2];
    snprintf(fullPath, sizeof(fullPath), "%s\\%s", cwd, filename);

    if (pathOut && pathOutSize > 0)
        snprintf(pathOut, pathOutSize, "%s", fullPath);

    // Open in binary mode so CRLF is written as-is
    FILE* f = fopen(fullPath, "wb");
    if (!f) return;

    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/**
 * Append extra diagnostics (system, memory, process, modules)
 * to buf. Returns number of bytes written.
 */
static size_t exception_buildSysInfo(char* buf, size_t bufSize, EXCEPTION_POINTERS* pEx) {
    size_t pos = 0;
     (void)pEx;

    // --- System ---
    pos += snprintf(buf + pos, bufSize - pos, "=== SYSTEM ===\r\n");
    // RtlGetVersion gives the true OS version (GetVersionEx lies on Win10+)
    typedef LONG (WINAPI* RtlGetVer_t)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW osv = {};
    osv.dwOSVersionInfoSize = sizeof(osv);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    FARPROC fnRtlRaw = ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    RtlGetVer_t fnRtl = (RtlGetVer_t)(void*)fnRtlRaw;
    if (fnRtl) fnRtl(&osv);
    pos += snprintf(buf + pos, bufSize - pos,
        "Windows:        %lu.%lu (build %lu)\r\n",
        osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    pos += snprintf(buf + pos, bufSize - pos,
        "CPU Cores:      %lu\r\n", si.dwNumberOfProcessors);
    pos += snprintf(buf + pos, bufSize - pos,
        "Screen:         %d x %d\r\n",
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    // --- Memory ---
    pos += snprintf(buf + pos, bufSize - pos, "\r\n=== MEMORY ===\r\n");
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    pos += snprintf(buf + pos, bufSize - pos, "Memory Load:    %lu%%\r\n",  ms.dwMemoryLoad);
    pos += snprintf(buf + pos, bufSize - pos, "Physical Total: %llu MB\r\n", ms.ullTotalPhys    / (1024*1024));
    pos += snprintf(buf + pos, bufSize - pos, "Physical Avail: %llu MB\r\n", ms.ullAvailPhys    / (1024*1024));
    pos += snprintf(buf + pos, bufSize - pos, "Virtual  Total: %llu MB\r\n", ms.ullTotalVirtual / (1024*1024));
    pos += snprintf(buf + pos, bufSize - pos, "Virtual  Avail: %llu MB\r\n", ms.ullAvailVirtual / (1024*1024));

    // --- Process ---
    pos += snprintf(buf + pos, bufSize - pos, "\r\n=== PROCESS ===\r\n");
    pos += snprintf(buf + pos, bufSize - pos, "PID:            %lu\r\n",  GetCurrentProcessId());
    pos += snprintf(buf + pos, bufSize - pos, "Executable:     %s\r\n",  EXE_PATH);
    pos += snprintf(buf + pos, bufSize - pos, "Command:        %s\r\n",  EXE_COMMAND_LINE);
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    pos += snprintf(buf + pos, bufSize - pos, "WorkDir:        %s\r\n",  cwd);

    // --- Loaded Modules ---
    pos += snprintf(buf + pos, bufSize - pos, "\r\n=== MODULES ===\r\n");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = {};
        me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) {
            do {
                if (pos + 128 >= bufSize) break;
                pos += snprintf(buf + pos, bufSize - pos,
                    "0x%08X  %s\r\n",
                    (unsigned int)(uintptr_t)me.modBaseAddr, me.szModule);
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
    }

    // --- CVARs ---
    pos += snprintf(buf + pos, bufSize - pos, "\r\n");
    pos += exception_buildCvarDump(buf + pos, bufSize - pos);

    // --- Main folder file listing ---
    pos += snprintf(buf + pos, bufSize - pos, "\r\n");
    pos += exception_buildMainFolderFiles(buf + pos, bufSize - pos);

    return pos;
}

static size_t exception_buildCrashContextDump(char* buf, size_t bufSize, EXCEPTION_POINTERS* pEx, DWORD crashThreadId) {
    size_t pos = 0;
    DWORD currentThreadId = GetCurrentThreadId();

    pos += snprintf(buf + pos, bufSize - pos, "=== CRASH CONTEXT ===\r\n");
    pos += snprintf(buf + pos, bufSize - pos, "Crash Thread ID:   %lu\r\n", (unsigned long)crashThreadId);
    pos += snprintf(buf + pos, bufSize - pos, "Current Thread ID: %lu\r\n", (unsigned long)currentThreadId);

    if (pEx && pEx->ContextRecord) {
        CONTEXT* ctx = pEx->ContextRecord;
        pos += snprintf(buf + pos, bufSize - pos, "\r\n=== REGISTERS ===\r\n");
        pos += snprintf(buf + pos, bufSize - pos,
            "EIP=0x%08lX  EAX=0x%08lX  EBX=0x%08lX  ECX=0x%08lX\r\n",
            ctx->Eip, ctx->Eax, ctx->Ebx, ctx->Ecx);
        pos += snprintf(buf + pos, bufSize - pos,
            "EDX=0x%08lX  ESI=0x%08lX  EDI=0x%08lX  EBP=0x%08lX\r\n",
            ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp);
        pos += snprintf(buf + pos, bufSize - pos,
            "ESP=0x%08lX  EFL=0x%08lX\r\n",
            ctx->Esp, ctx->EFlags);
    }

    return pos;
}

static size_t exception_buildThreadsDump(char* buf, size_t bufSize, const JsonCrashContextData& crashContext, DWORD crashThreadId, DWORD currentThreadId) {
    size_t pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "=== THREADS ===\r\n");

    for (size_t i = 0; i < crashContext.threads.size(); ++i) {
        const JsonThreadInfoData& thread = crashContext.threads[i];
        const bool isCrash = (thread.threadId == (unsigned long)crashThreadId);
        const bool isCurrent = (thread.threadId == (unsigned long)currentThreadId);
        pos += snprintf(buf + pos, bufSize - pos,
            "TID=%lu%s%s  START=%s  NAME=%s\r\n",
            thread.threadId,
            isCrash ? " [CRASH]" : "",
            isCurrent ? " [CURRENT]" : "",
            thread.startAddress.empty() ? "<unknown>" : thread.startAddress.c_str(),
            thread.threadName.empty() ? "<unnamed>" : thread.threadName.c_str());
        if (pos + 256 >= bufSize) {
            break;
        }
    }

    return pos;
}

static void exception_collectSystemInfoJson(JsonSystemInfoData* out, EXCEPTION_POINTERS* pEx) {
    if (!out) {
        return;
    }

    char temp[256] = {0};
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

    // Process
    snprintf(temp, sizeof(temp), "%lu", GetCurrentProcessId());
    out->processId = temp;
    out->executablePath = EXE_PATH;
    out->commandLine = EXE_COMMAND_LINE;

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    out->workingDirectory = cwd;

    (void)pEx;

    // Modules
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me = {};
        me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) {
            do {
                JsonModuleInfoData module;
                snprintf(temp, sizeof(temp), "0x%08X", (unsigned int)(uintptr_t)me.modBaseAddr);
                module.baseAddress = temp;
                module.moduleName = me.szModule;
                out->modules.push_back(module);
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
    }

    // CVARs
    const char* cvarNames[] = {
        "cl_hwid2", "name", "fs_game", "r_mode", "r_displayRefresh",
        "r_fullscreen", "r_monitor", "vid_xpos", "vid_ypos"
    };

    for (size_t i = 0; i < sizeof(cvarNames) / sizeof(cvarNames[0]); ++i) {
        JsonNamedValueData item;
        item.name = cvarNames[i];

        dvar_t* dv = Dvar_GetDvarByName(cvarNames[i]);
        if (!dv) {
            item.value = "<not registered>";
            out->cvars.push_back(item);
            continue;
        }

        switch (dv->type) {
            case DVAR_TYPE_BOOL:
                item.value = dv->value.boolean ? "true" : "false";
                break;
            case DVAR_TYPE_INT:
                snprintf(temp, sizeof(temp), "%d", dv->value.integer);
                item.value = temp;
                break;
            case DVAR_TYPE_FLOAT:
                snprintf(temp, sizeof(temp), "%.4f", dv->value.decimal);
                item.value = temp;
                break;
            case DVAR_TYPE_ENUM:
                if (dv->limits.enumeration.strings &&
                    dv->value.integer >= 0 &&
                    dv->value.integer < dv->limits.enumeration.stringCount) {
                    snprintf(temp, sizeof(temp), "%s (%d)",
                        dv->limits.enumeration.strings[dv->value.integer],
                        dv->value.integer);
                    item.value = temp;
                } else {
                    snprintf(temp, sizeof(temp), "%d", dv->value.integer);
                    item.value = temp;
                }
                break;
            case DVAR_TYPE_STRING:
                item.value = dv->value.string ? dv->value.string : "<null>";
                break;
            default:
                snprintf(temp, sizeof(temp), "<type %d>", (int)dv->type);
                item.value = temp;
                break;
        }
        out->cvars.push_back(item);
    }

    // Main folder files
    dvar_t* fs_basePath = Dvar_GetDvarByName("fs_basepath");
    if (!fs_basePath || !fs_basePath->value.string || fs_basePath->value.string[0] == '\0') {
        JsonMainFolderFileInfoData note;
        note.entryType = "note";
        note.message = "<fs_basepath not set>";
        out->mainFolderFiles.push_back(note);
        return;
    }

    char mainDir[MAX_PATH * 2] = {0};
    snprintf(mainDir, sizeof(mainDir), "%s\\main", fs_basePath->value.string);

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
        struct stat st;
        if (stat(fullPath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                entry.entryType = "directory";
                entry.name = name;
            } else {
                entry.entryType = "file";
                snprintf(temp, sizeof(temp), "%lld", (long long)st.st_size);
                entry.size = temp;
                entry.name = name;
            }
        } else {
            entry.entryType = "file";
            entry.size = "?";
            entry.name = name;
        }

        out->mainFolderFiles.push_back(entry);
    }
    closedir(dir);
}

static void exception_collectCrashContextJson(JsonCrashContextData* out, EXCEPTION_POINTERS* pEx, DWORD crashThreadId) {
    if (!out) {
        return;
    }

    char temp[64] = {0};
    const DWORD currentThreadId = GetCurrentThreadId();

    if (pEx && pEx->ContextRecord) {
        CONTEXT* ctx = pEx->ContextRecord;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Eip); out->eip = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Eax); out->eax = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Ebx); out->ebx = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Ecx); out->ecx = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Edx); out->edx = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Esi); out->esi = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Edi); out->edi = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Ebp); out->ebp = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->Esp); out->esp = temp;
        snprintf(temp, sizeof(temp), "0x%08lX", ctx->EFlags); out->eflags = temp;
    }

    HANDLE threadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnap != INVALID_HANDLE_VALUE) {
        typedef LONG NTSTATUS;
        typedef NTSTATUS (NTAPI *NtQueryInformationThreadFn)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
        NtQueryInformationThreadFn pNtQueryInformationThread = (NtQueryInformationThreadFn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");

        typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR*);
        GetThreadDescriptionFn pGetThreadDescription = (GetThreadDescriptionFn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");

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
                        if (pNtQueryInformationThread(hThread, (THREADINFOCLASS)9, &startAddr, sizeof(startAddr), nullptr) == 0 && startAddr) {
                            snprintf(temp, sizeof(temp), "0x%p", startAddr);
                            thread.startAddress = temp;
                        }
                    }

                    if (pGetThreadDescription) {
                        PWSTR wideName = nullptr;
                        if (SUCCEEDED(pGetThreadDescription(hThread, &wideName)) && wideName) {
                            char nameUtf8[256] = {0};
                            int wrote = WideCharToMultiByte(CP_UTF8, 0, wideName, -1, nameUtf8, (int)sizeof(nameUtf8), nullptr, nullptr);
                            if (wrote > 0) {
                                thread.threadName = nameUtf8;
                            }
                            LocalFree(wideName);
                        }
                    }

                    CloseHandle(hThread);
                }

                out->threads.push_back(thread);
            } while (Thread32Next(threadSnap, &te));
        }
        CloseHandle(threadSnap);
    }

    (void)crashThreadId;
    (void)currentThreadId;
}

/**
 * Show the crash dialog with a dark-themed scrollable report.
 * Returns true if the user chose to restart the game.
 */
static bool exception_showCrashDialog(const char* content, EXCEPTION_POINTERS* pExInfo, const char* savePath) {
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
        content, savePath, pExInfo, false,
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
        // Declare external httpClient for polling (defined in exception_handler scope)
        extern HttpClient* g_crashDialogHttpClient;
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
// Stack analysis helpers
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

/**
 * Describes the CALL instruction that produced return address 'retAddr'.
 * The return address sits immediately after the CALL opcode, so we inspect
 * the bytes that precede it to reconstruct the instruction text and, for
 * direct/indirect calls, resolve the callee symbol when possible.
 */
static void exception_describeCallSite(HANDLE hProcess, unsigned int retAddr,
                                       char* out, size_t outSize) {
    if (!out || !outSize) return;
    out[0] = '\0';

    if (retAddr < 6) return;
    static const char* const regNames[8] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
    };
    const BYTE* p = (const BYTE*)(uintptr_t)retAddr;
    if (IsBadReadPtr(p - 6, 6)) return;

    // Helper: resolve a target address to a symbol string.
    auto resolveTarget = [&](unsigned int target, char* symBuf, size_t symBufSize) -> bool {
        // DbgHelp
        struct { SYMBOL_INFO sym; char buf[256]; } sb;
        memset(&sb, 0, sizeof(sb));
        sb.sym.SizeOfStruct = sizeof(SYMBOL_INFO);
        sb.sym.MaxNameLen   = 256;
        DWORD64 disp = 0;
        if (SymFromAddr(hProcess, (DWORD64)target, &disp, &sb.sym)
                && sb.sym.Name[0] != '\0' && disp < 0x10000u) {
            if (disp == 0)
                snprintf(symBuf, symBufSize, "%s", sb.sym.Name);
            else
                snprintf(symBuf, symBufSize, "%s+0x%llX", sb.sym.Name, (unsigned long long)disp);
            return true;
        }
        // Determine which module the target belongs to
        MEMORY_BASIC_INFORMATION mbi3;
        HMODULE targetMod = nullptr;
        if (VirtualQuery((void*)(uintptr_t)target, &mbi3, sizeof(mbi3)) != 0)
            targetMod = (HMODULE)mbi3.AllocationBase;
        // CoD2MP_s.exe symbols lookup
        if (targetMod && targetMod == GetModuleHandleA("CoD2MP_s.exe")) {
            if (const Cod2mpSymbol* sym = symbols_cod2mp_s_find(target)) {
                uint32_t d = target - sym->addr;
                if (d == 0)
                    snprintf(symBuf, symBufSize, "%s", sym->name);
                else
                    snprintf(symBuf, symBufSize, "%s+0x%X", sym->name, d);
                return true;
            }
        }
        // gfx_d3d_mp_x86_s.dll symbols lookup
        if (targetMod && targetMod == GetModuleHandleA("gfx_d3d_mp_x86_s.dll")) {
            if (const GfxD3dSymbol* sym = symbols_gfx_d3d_mp_x86_s_find(target)) {
                uint32_t d = target - sym->addr;
                if (d == 0)
                    snprintf(symBuf, symBufSize, "%s", sym->name);
                else
                    snprintf(symBuf, symBufSize, "%s+0x%X", sym->name, d);
                return true;
            }
        }
        // mss32.dll symbols lookup (RVA-based; populated one build behind by generate_mss32_symbols_h.ps1)
        if (targetMod && targetMod == GetModuleHandleA("mss32.dll")) {
            uint32_t tRva = target - (uint32_t)(uintptr_t)targetMod;
            if (const Mss32Symbol* sym = symbols_mss32_find(tRva)) {
                uint32_t d = tRva - sym->rva;
                if (d == 0)
                    snprintf(symBuf, symBufSize, "%s", sym->name);
                else
                    snprintf(symBuf, symBufSize, "%s+0x%X", sym->name, d);
                return true;
            }
        }
        // Export table
        if (targetMod) {
            unsigned int tRva = target - (unsigned int)mbi3.AllocationBase;
            if (exception_getNearestExport(targetMod, tRva, symBuf, symBufSize))
                return true;
        }
        return false;
    };

    char symBuf[256];

    // --- E8 rel32: direct near call ---
    if (p[-5] == 0xE8) {
        int32_t rel;
        memcpy(&rel, p - 4, 4);
        unsigned int target = (unsigned int)retAddr + (unsigned int)rel;
        symBuf[0] = '\0';
        if (resolveTarget(target, symBuf, sizeof(symBuf)))
            snprintf(out, outSize, "call 0x%08X  (%s)", target, symBuf);
        else
            snprintf(out, outSize, "call 0x%08X", target);
        return;
    }

    // --- FF D?: call via register ---
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) {
        snprintf(out, outSize, "call %s", regNames[p[-1] & 0x07]);
        return;
    }

    // --- FF 15 imm32: call indirect via pointer (import stub / IAT) ---
    if (p[-6] == 0xFF && p[-5] == 0x15) {
        uint32_t ptrAddr;
        memcpy(&ptrAddr, p - 4, 4);
        if (!IsBadReadPtr((void*)(uintptr_t)ptrAddr, 4)) {
            uint32_t target;
            memcpy(&target, (void*)(uintptr_t)ptrAddr, 4);
            symBuf[0] = '\0';
            if (resolveTarget(target, symBuf, sizeof(symBuf)))
                snprintf(out, outSize, "call [0x%08X]  (%s)", ptrAddr, symBuf);
            else
                snprintf(out, outSize, "call [0x%08X]  -> 0x%08X", ptrAddr, target);
        } else {
            snprintf(out, outSize, "call [0x%08X]", ptrAddr);
        }
        return;
    }

    // --- FF /2 rm / disp8: other indirect call forms ---
    snprintf(out, outSize, "call (indirect)");
}

/**
 * Tries DbgHelp SymFromAddr first (works for system DLLs with public symbols),
 * then falls back to the nearest PE export.
 * Returns false and leaves outSym untouched if nothing is found.
 */
static bool exception_getSymbolName(HANDLE hProcess, HMODULE hMod,
                                    unsigned int addr, unsigned int rva,
                                    char* outSym, size_t symSize) {
    // DbgHelp path
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

// Maximum number of frames captured per stack (per thread, per method).
static const size_t EXCEPTION_MAX_STACK_FRAMES = 64;

// One target thread whose stack we want to capture. The CONTEXT is captured up front
// (the thread is briefly suspended only long enough to read its registers, then resumed,
// to avoid deadlocking on a lock held by that thread while we allocate during the walk).
struct ExceptionThreadTarget {
    std::string role;
    std::string name;
    DWORD       threadId = 0;
    CONTEXT     context = {};
    bool        hasContext = false;
    bool        isCrashing = false;
};

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

// Resolves a single return/instruction address into a frame line (text) + JSON entry,
// appending both to 'result'. Returns false when the address is not backed by readable memory.
static bool exception_appendFrameToResult(HANDLE hProcess, StackCollectionResult& result,
                                          int frameIndex, unsigned int addr) {
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
        if (p2) strcpy(modName2, p2 + 1);
        if (modName2[0] == '\0')
            strncpy(modName2, "<unknown>", sizeof(modName2) - 1);
        offset2 = addr - (unsigned int)(uintptr_t)mbi2.AllocationBase;
    }

    char section2[16] = {0};
    if (!exception_getPeSection((HMODULE)mbi2.AllocationBase, offset2, section2, sizeof(section2)))
        strncpy(section2, "?", sizeof(section2) - 1);

    char resolvedSymbol2[300] = {0};
    bool hasSymbol = exception_getSymbolName(hProcess, (HMODULE)mbi2.AllocationBase,
                                 addr, offset2, resolvedSymbol2, sizeof(resolvedSymbol2));
    char callSite2[200] = {0};
    exception_describeCallSite(hProcess, addr, callSite2, sizeof(callSite2));

    JsonStackFrameData frameData;
    frameData.frameNumber = frameIndex;
    frameData.instructionAddress = addr;
    frameData.moduleOffset = offset2;
    frameData.moduleName = modName2;
    frameData.sectionName = section2;
    frameData.resolvedSymbol = hasSymbol ? resolvedSymbol2 : "<no symbol>";
    frameData.instruction = callSite2;
    result.frames.push_back(frameData);

    char line[256];
    snprintf(line, sizeof(line),
        "%3d  %-24s  0x%08X  0x%08X  %-40s  %s\n",
        frameIndex, modName2, addr, offset2,
        hasSymbol ? resolvedSymbol2 : "<no symbol>", callSite2);
    result.text += line;
    return true;
}

// Inserts frame 0 (the actual exception address) at the top of a crashing-thread stack,
// now that exceptionAddress has been resolved (the null-call CONTEXT repair may have run).
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
    if (!exception_getSymbolName(hProcess, hMod0, exceptionAddress, fileOffset, symbol0, sizeof(symbol0)))
        strncpy(symbol0, "<no symbol>", sizeof(symbol0) - 1);

    char line[512];
    snprintf(line, sizeof(line),
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

// Method 1: DbgHelp StackWalk64. Walks 'walkContext' (a copy is taken so it can be mutated).
// For the crashing thread it skips the frame that equals exceptionAddress (re-added later as
// frame 0) and attempts an EIP-from-ESP repair when EIP points at non-executable memory.
static void exception_walkStackWalk64(HANDLE hProcess, CONTEXT walkContext,
                                      bool isCrashing, unsigned int exceptionAddress,
                                      BOOL symInitialized, DWORD symInitializeError,
                                      StackCollectionResult& result) {
    result.method = "StackWalk64";
    walkContext.ContextFlags = CONTEXT_FULL;

    bool contextRepaired = false;
    if (isCrashing) {
        // If Eip is invalid (e.g. null-pointer call), StackWalk64 immediately fails. When a CALL
        // jumps to an invalid target, the CPU has already pushed the return address onto [Esp]
        // before faulting. Repair by reading that return address from [Esp] and adjusting Esp.
        MEMORY_BASIC_INFORMATION mbiEip;
        bool eipInvalid = (walkContext.Eip == 0) ||
            (VirtualQuery((void*)(uintptr_t)walkContext.Eip, &mbiEip, sizeof(mbiEip)) == 0) ||
            !(mbiEip.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (eipInvalid && walkContext.Esp != 0 &&
            !IsBadReadPtr((void*)(uintptr_t)walkContext.Esp, sizeof(DWORD))) {
            DWORD retAddr = *(DWORD*)(uintptr_t)walkContext.Esp;
            MEMORY_BASIC_INFORMATION mbiRet;
            bool retValid = (retAddr != 0) &&
                (VirtualQuery((void*)(uintptr_t)retAddr, &mbiRet, sizeof(mbiRet)) != 0) &&
                (mbiRet.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
            if (retValid) {
                walkContext.Eip = retAddr;
                walkContext.Esp = walkContext.Esp + 4; // pop the return address
                contextRepaired = true;
            }
        }
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
            NULL,
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

        if (exception_appendFrameToResult(hProcess, result, frameN, addr2)) {
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

    char symInitErrMsg[256] = {0};
    exception_formatWin32Error(symInitializeError, symInitErrMsg, sizeof(symInitErrMsg));
    char stackWalkErrMsg[256] = {0};
    if (stackWalkError != ERROR_SUCCESS)
        exception_formatWin32Error(stackWalkError, stackWalkErrMsg, sizeof(stackWalkErrMsg));

    result.diagnostics.method = contextRepaired
        ? "StackWalk64 (repaired CONTEXT from ESP)" : "StackWalk64 (original CONTEXT)";
    result.diagnostics.unwindFrames = unwindFrameCount;
    result.diagnostics.contextRepaired = contextRepaired;
    result.diagnostics.symInitializeStatus = symInitialized ? "ok" : "failed";
    result.diagnostics.symInitializeCode = symInitializeError;
    result.diagnostics.symInitializeMessage = symInitErrMsg;
    result.diagnostics.stackWalkStatus = (stackWalkError != ERROR_SUCCESS) ? "failed" : "ok";
    result.diagnostics.stackWalkCode = stackWalkError;
    result.diagnostics.stackWalkMessage = stackWalkErrMsg;
}

// Method 2: legacy EBP-chain walk. [EBP+0]=prev EBP, [EBP+4]=return address. Reliable for any
// x86 code compiled with frame pointers (FPO disabled), which is the case for CoD2MP_s.exe.
static void exception_walkEbpChain(HANDLE hProcess, CONTEXT ctx,
                                   bool isCrashing, unsigned int exceptionAddress,
                                   BOOL symInitialized, DWORD symInitializeError,
                                   StackCollectionResult& result) {
    result.method = "EBP chain";
    int frameN = isCrashing ? 1 : 0;
    int count = 0;
    DWORD ebp = ctx.Ebp;

    for (size_t i = 0; i < EXCEPTION_MAX_STACK_FRAMES; ++i) {
        // EBP must be non-null and 4-byte aligned
        if (ebp == 0 || (ebp & 3) != 0)
            break;

        MEMORY_BASIC_INFORMATION mbiEbp;
        if (VirtualQuery((void*)(uintptr_t)ebp, &mbiEbp, sizeof(mbiEbp)) == 0)
            break;
        if (!(mbiEbp.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_WRITECOPY)))
            break;
        if (mbiEbp.RegionSize < 8)
            break;

        DWORD nextEbp = *(DWORD*)(uintptr_t)ebp;
        DWORD retAddr = *(DWORD*)(uintptr_t)(ebp + 4);

        // EBP chain must grow toward higher addresses (stack grows down)
        if (nextEbp != 0 && nextEbp <= ebp)
            break;

        if (retAddr != 0 && !(isCrashing && retAddr == exceptionAddress)) {
            MEMORY_BASIC_INFORMATION mbiRet;
            if (VirtualQuery((void*)(uintptr_t)retAddr, &mbiRet, sizeof(mbiRet)) != 0 &&
                (mbiRet.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                if (exception_appendFrameToResult(hProcess, result, frameN, retAddr)) {
                    ++frameN;
                    ++count;
                }
            }
        }

        if (nextEbp == 0)
            break;
        ebp = nextEbp;
    }

    char symInitErrMsg[256] = {0};
    exception_formatWin32Error(symInitializeError, symInitErrMsg, sizeof(symInitErrMsg));

    result.diagnostics.method = "EBP chain (frame pointer unwind)";
    result.diagnostics.unwindFrames = count;
    result.diagnostics.ebpFallbackAttempted = true;
    result.diagnostics.ebpFallbackFrames = count;
    result.diagnostics.symInitializeStatus = symInitialized ? "ok" : "failed";
    result.diagnostics.symInitializeCode = symInitializeError;
    result.diagnostics.symInitializeMessage = symInitErrMsg;
    result.diagnostics.stackWalkStatus = "n/a";
}

// Appends the human-readable diagnostics block (LF) for one stack.
static void exception_appendDiagnosticsText(std::string& out, const JsonStackDiagnosticsData& d) {
    char line[512];
    out += "    [diagnostics]\n";
    snprintf(line, sizeof(line), "      method: %s\n", d.method.c_str()); out += line;
    snprintf(line, sizeof(line), "      unwindFrames: %d\n", d.unwindFrames); out += line;
    snprintf(line, sizeof(line), "      contextRepaired: %s\n", d.contextRepaired ? "yes" : "no"); out += line;
    snprintf(line, sizeof(line), "      ebpFallbackFrames: %d\n", d.ebpFallbackFrames); out += line;
    snprintf(line, sizeof(line), "      symInitialize: %s (code=%lu, message=\"%s\")\n",
        d.symInitializeStatus.c_str(), d.symInitializeCode, d.symInitializeMessage.c_str()); out += line;
    if (d.stackWalkStatus == "failed")
        snprintf(line, sizeof(line), "      stackWalk64: failed (code=%lu, message=\"%s\")\n",
            d.stackWalkCode, d.stackWalkMessage.c_str());
    else
        snprintf(line, sizeof(line), "      stackWalk64: %s\n", d.stackWalkStatus.c_str());
    out += line;
}

// Builds the full "=== STACKS ===" text section (LF) for the crash report textarea.
static std::string exception_buildStacksTextReport(const std::vector<StackCollectionResult>& stacks) {
    static const char colHeader[] =
        "  #  Module                    Address     Offset      Symbol                                    Instruction\n";
    std::string out;
    out += "=== STACKS ===\n";
    for (const StackCollectionResult& s : stacks) {
        char hdr[320];
        snprintf(hdr, sizeof(hdr), "\n--- [%s]  TID %lu  \"%s\"  method: %s ---\n",
            s.threadRole.c_str(), s.threadId,
            s.threadName.empty() ? "" : s.threadName.c_str(),
            s.method.c_str());
        out += hdr;
        out += colHeader;
        if (s.frames.empty())
            out += "    Stack unavailable\n";
        else
            out += s.text;
        exception_appendDiagnosticsText(out, s.diagnostics);
    }
    return out;
}

// Builds the list of target threads: the crashing thread (from pExceptionInfo) plus the named
// "CoD2 MainThread" and "CoD2 RenderBackend" threads when they are not the crashing thread.
// Named threads are briefly suspended only to snapshot their CONTEXT, then resumed.
static void exception_collectTargetThreads(DWORD crashThreadId, EXCEPTION_POINTERS* pEx,
                                           std::vector<ExceptionThreadTarget>& out) {
    // 1. Crashing thread
    {
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
            CloseHandle(hNamed);
        }
        out.push_back(t);
    }

    // 2. Named threads (CoD2 MainThread / CoD2 RenderBackend)
    HANDLE threadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnap == INVALID_HANDLE_VALUE)
        return;

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
            // Suspend only long enough to snapshot the registers, then resume immediately so we
            // never hold the thread suspended while allocating (which could deadlock on a lock it owns).
            if (SuspendThread(hT) != (DWORD)-1) {
                t.context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(hT, &t.context))
                    t.hasContext = true;
                ResumeThread(hT);
            }
            CloseHandle(hT);
            out.push_back(t);
        } while (Thread32Next(threadSnap, &te));
    }
    CloseHandle(threadSnap);
}

// ---------------------------------------------------------------------------
// Stack-overflow vectored handler
//
// SetUnhandledExceptionFilter is never reached for EXCEPTION_STACK_OVERFLOW
// because the Windows exception dispatcher itself faults when it tries to build
// its dispatch frame on the already-exhausted stack (secondary 0xc0000005).
// A vectored handler is invoked earlier, while the CPU's guard-page guarantee
// still has a few hundred bytes remaining.  We deep-copy the exception records
// to heap and hand off to a fresh thread so that the full crash handler runs
// without any stack-space pressure.
// ---------------------------------------------------------------------------

struct StackOverflowHandlerArgs {
    EXCEPTION_RECORD   exRecord;
    CONTEXT            exContext;
    EXCEPTION_POINTERS exPtrs;   // points into the two fields above
};

static DWORD WINAPI exception_stackOverflowThread(LPVOID lpParam) {
    StackOverflowHandlerArgs* args = (StackOverflowHandlerArgs*)lpParam;
    exception_handler(&args->exPtrs);
    HeapFree(GetProcessHeap(), 0, args);
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

static LONG WINAPI exception_vectoredHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    if (pExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;

    // Only the first overflow wins; ignore any concurrent ones.
    if (InterlockedCompareExchange(&exception_stackOverflowCaught, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // Record the faulting thread so exception_handler labels the thread list correctly.
    InterlockedExchange((volatile LONG*)(void*)&exception_stackOverflowThreadId,
                        (LONG)GetCurrentThreadId());

    // Deep-copy exception data to heap; the faulting thread's stack is exhausted
    // and will be gone (or unsafe to touch) once we spawn the crash thread.
    StackOverflowHandlerArgs* args = (StackOverflowHandlerArgs*)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StackOverflowHandlerArgs));
    if (args) {
        args->exRecord             = *pExceptionInfo->ExceptionRecord;
        args->exContext            = *pExceptionInfo->ContextRecord;
        args->exPtrs.ExceptionRecord = &args->exRecord;
        args->exPtrs.ContextRecord   = &args->exContext;

        HANDLE hCrash = CreateThread(NULL, 0, exception_stackOverflowThread, args, 0, NULL);
        if (hCrash) {
            CloseHandle(hCrash);
            // Suspend the faulting thread so the crash handler can safely read
            // its context.  The crash thread will TerminateProcess when done.
            SuspendThread(GetCurrentThread());
            // Should never be reached; TerminateProcess kills us.
        } else {
            HeapFree(GetProcessHeap(), 0, args);
        }
    }

    // Fallback: if allocation or CreateThread failed, terminate immediately.
    TerminateProcess(GetCurrentProcess(), 1);
    return EXCEPTION_CONTINUE_SEARCH; // unreachable
}

/** Called when exception happend */
LONG WINAPI exception_handler(EXCEPTION_POINTERS* pExceptionInfo) {
    // When a stack overflow is caught by the vectored handler, the actual crashing
    // thread ID is stored in exception_stackOverflowThreadId; otherwise use the
    // current thread (which IS the faulting thread for normal crashes).
    const DWORD crashThreadId = exception_stackOverflowThreadId
        ? (DWORD)exception_stackOverflowThreadId
        : GetCurrentThreadId();

    // Give the game's own unhandled exception filter (if any was registered before ours)
    // the first opportunity to handle the exception.  CoD2 uses __try/__except recovery
    // in several subsystems (renderer, etc.) — if its SUEF returns
    // EXCEPTION_CONTINUE_EXECUTION, the game has already recovered and we must not
    // show a crash dialog or terminate the process.
    if (g_prevUnhandledFilter && exception_processCrashed == 0) {
        LONG prevResult = g_prevUnhandledFilter(pExceptionInfo);
        if (prevResult == EXCEPTION_CONTINUE_EXECUTION)
            return EXCEPTION_CONTINUE_EXECUTION;
        // EXCEPTION_EXECUTE_HANDLER (or anything else) means the game also wants to
        // terminate — fall through so our full crash handler runs.
    }

    // If the handler is already running, a secondary exception occurred inside it.
    // Skip everything to avoid showing a second dialog or sending an incomplete crash report.
    // Just let the process terminate.
    if (exception_processCrashed >= 1) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Mark the handler as active immediately so any fault during message box or stack
    // collection does not recurse back into the crash handler.
    InterlockedIncrement(&exception_processCrashed);

    // Capture the crash timestamp once so the .txt and .dmp files share the same name.
    GetLocalTime(&exception_crashTime);

    gamma_restore();


    char moduleName[MAX_PATH] = {0};
    unsigned int moduleBase = 0;
    unsigned int fileOffset = 0;
    unsigned int exceptionCode    = pExceptionInfo->ExceptionRecord->ExceptionCode;
    unsigned int exceptionAddress = (unsigned int)pExceptionInfo->ExceptionRecord->ExceptionAddress;

    // Collected stack traces and the rendered text section. We capture up to 3 threads
    // (the crashing thread plus "CoD2 MainThread" and "CoD2 RenderBackend") and, for each,
    // run both retrieval methods (StackWalk64 and the legacy EBP-chain walk).
    std::vector<StackCollectionResult> stacks;
    std::string stacksReport;

    // Initialise DbgHelp so SymFromAddr can resolve symbols for loaded modules.
    HANDLE hProcess = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    BOOL symInitialized = SymInitialize(hProcess, NULL, FALSE);
    DWORD symInitializeError = symInitialized ? ERROR_SUCCESS : GetLastError();

    {
        // Get module info for the exception address
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(pExceptionInfo->ExceptionRecord->ExceptionAddress, &mbi, sizeof(mbi)) != 0) {
            GetModuleFileNameA((HMODULE)mbi.AllocationBase, moduleName, sizeof(moduleName));
            char* p = strrchr(moduleName, '\\');
            if (p != NULL) strcpy(moduleName, p + 1);
            moduleBase = (unsigned int)mbi.AllocationBase;
            fileOffset = exceptionAddress - moduleBase;
        }

        // Determine the target threads (crashing + named) and, for each one that has a
        // usable CONTEXT, collect a StackWalk64 stack and an EBP-chain stack.
        std::vector<ExceptionThreadTarget> threadTargets;
        exception_collectTargetThreads(crashThreadId, pExceptionInfo, threadTargets);

        for (size_t ti = 0; ti < threadTargets.size(); ++ti) {
            const ExceptionThreadTarget& t = threadTargets[ti];
            if (!t.hasContext)
                continue;

            // Method 1: DbgHelp StackWalk64
            StackCollectionResult sw;
            sw.threadRole = t.role;
            sw.threadName = t.name;
            sw.threadId   = t.threadId;
            exception_walkStackWalk64(hProcess, t.context, t.isCrashing, exceptionAddress,
                                      symInitialized, symInitializeError, sw);
            if (t.isCrashing)
                exception_prependCrashFrame0(hProcess, sw, moduleName, moduleBase, exceptionAddress, fileOffset);
            stacks.push_back(sw);

            // Method 2: legacy EBP-chain walk
            StackCollectionResult eb;
            eb.threadRole = t.role;
            eb.threadName = t.name;
            eb.threadId   = t.threadId;
            exception_walkEbpChain(hProcess, t.context, t.isCrashing, exceptionAddress,
                                   symInitialized, symInitializeError, eb);
            if (t.isCrashing)
                exception_prependCrashFrame0(hProcess, eb, moduleName, moduleBase, exceptionAddress, fileOffset);
            stacks.push_back(eb);
        }

        stacksReport = exception_buildStacksTextReport(stacks);
    }

    // Build full display report: crash info/context → stacks → threads → system info → logs/logger → json
    // stacksReport uses LF; we need CRLF for the Win32 EDIT control
    size_t totalStackFrames = 0;
    for (size_t i = 0; i < stacks.size(); ++i)
        totalStackFrames += stacks[i].frames.size();
    const size_t stacksRawSize = stacksReport.size();
    const size_t stacksCrlfSize = stacksRawSize * 2 + 1024;
    const size_t crashContextSize = 16 * 1024;
    const size_t threadsSize = 32 * 1024;
    const size_t sysInfoSize   = 48 * 1024; // sys/mem/proc/regs/modules/cvars/main-folder-files
    const size_t logsSize      = CONSOLE_MIRROR_CAPACITY + 4096; // whole console mirror + headroom
    const size_t loggerBufSize = LOG_CAPACITY * (LOG_MSG_LEN + 32);
    const size_t jsonSizeEstimate = 8192 + totalStackFrames * 1024 + crashContextSize + threadsSize + sysInfoSize + logsSize + loggerBufSize;
    const size_t displaySize   = 1024 + stacksCrlfSize + crashContextSize + threadsSize + sysInfoSize + logsSize + loggerBufSize * 2 + jsonSizeEstimate;
    char* displayBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, displaySize);

    if (displayBuf) {
        JsonCrashContextData crashContextJson;
        exception_collectCrashContextJson(&crashContextJson, pExceptionInfo, crashThreadId);

        JsonSystemInfoData systemInfoJson;
        exception_collectSystemInfoJson(&systemInfoJson, pExceptionInfo);

        char* crashContextBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, crashContextSize);
        char* threadsBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, threadsSize);
        char* systemInfoBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sysInfoSize);
        char* loggerBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, loggerBufSize);
        char* logsBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, logsSize);

        if (crashContextBuf) {
            exception_buildCrashContextDump(crashContextBuf, crashContextSize, pExceptionInfo, crashThreadId);
        }
        if (threadsBuf) {
            exception_buildThreadsDump(threadsBuf, threadsSize, crashContextJson, crashThreadId, GetCurrentThreadId());
        }

        if (systemInfoBuf) {
            exception_buildSysInfo(systemInfoBuf, sysInfoSize, pExceptionInfo);
        }
        if (loggerBuf) {
            logger_get_recent(loggerBuf, loggerBufSize);
        }
        if (logsBuf) {
            console_getLogs(logsBuf, logsSize);
        }

        const char* safeModuleName = moduleName[0] ? moduleName : "unknown module";
        std::string crashJson = exception_buildCrashJson(
            (unsigned int)exceptionCode,
            exception_getText(exceptionCode),
            (unsigned int)exceptionAddress,
            safeModuleName,
            (unsigned int)fileOffset,
            (unsigned long)crashThreadId,
            (unsigned long)GetCurrentThreadId(),
            APP_VERSION,
            stacks,
            crashContextJson,
            systemInfoJson,
            loggerBuf ? loggerBuf : "",
            logsBuf ? logsBuf : "");

        size_t pos = 0;

        // Crash info
        pos += snprintf(displayBuf + pos, displaySize - pos,
            "=== CRASH INFO ===\r\n"
            "Code:         0x%08X  (%s)\r\n"
            "Address:      0x%08X  (%s)\r\n"
            "File Offset:  0x%08X\r\n"
            "Version:      %s\r\n",
            exceptionCode, exception_getText(exceptionCode),
            exceptionAddress, safeModuleName,
            fileOffset, APP_VERSION);

        if (crashContextBuf && crashContextBuf[0]) {
            pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n%s", crashContextBuf);
        }

        // All collected stacks (one block per thread per method). Convert LF -> CRLF.
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");
        pos += exception_lf_to_crlf(stacksReport.c_str(), displayBuf + pos, displaySize - pos);

        // Thread list directly under the stacks.
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");
        if (threadsBuf && threadsBuf[0]) {
            pos += snprintf(displayBuf + pos, displaySize - pos, "%s", threadsBuf);
        }

        // System / memory / process / modules / cvars / main folder files
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");
        if (systemInfoBuf && systemInfoBuf[0]) {
            pos += snprintf(displayBuf + pos, displaySize - pos, "%s", systemInfoBuf);
        }

        // Logger ring-buffer (newest first with age offsets)
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n=== LOGGER ===\r\n");
        if (loggerBuf && loggerBuf[0]) {
            pos += exception_lf_to_crlf(loggerBuf, displayBuf + pos, displaySize - pos);
        }
        if (pos >= 2 && !(displayBuf[pos - 2] == '\r' && displayBuf[pos - 1] == '\n'))
            pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");

        // Console logs (from crash-safe mirror)
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n=== LOGS ===\r\n");
        if (logsBuf && logsBuf[0]) {
            pos += snprintf(displayBuf + pos, displaySize - pos, "%s", logsBuf);
        }
        if (pos >= 2 && !(displayBuf[pos - 2] == '\r' && displayBuf[pos - 1] == '\n'))
            pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");

        // Full JSON payload mirrored in UI for copy/paste and external tooling.
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n=== JSON ===\r\n%s\r\n", crashJson.c_str());


        // Save crash report to a timestamped file in the game root before showing the dialog
        char crashFilePath[MAX_PATH * 2] = {0};
        exception_saveToFile(displayBuf, crashFilePath, sizeof(crashFilePath));

        // Send crash telemetry to HTTP endpoint (async, non-blocking)
        static HttpClient httpClient;
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
                    InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_SUCCESS);
                    return;
                }

                // Try to extract a short human-readable message from the JSON body.
                // Fall back to just the status code if the body is absent or non-JSON.
                const char* jsonMsg = nullptr;
                char jsonMsgBuf[128] = {0};
                if (!res.body.empty() && res.body[0] == '{') {
                    // Look for "error":"..." or "message":"..." (simple scan, no full parser)
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
                            jsonMsg = jsonMsgBuf;
                            break;
                        }
                    }
                }
                if (jsonMsg)
                    snprintf(g_crashTelemetryError, sizeof(g_crashTelemetryError),
                        "HTTP %d: %s", res.status, jsonMsg);
                else
                    snprintf(g_crashTelemetryError, sizeof(g_crashTelemetryError),
                        "HTTP %d", res.status);
                logger_add("[TELEMETRY] Crash report rejected by endpoint: %s", g_crashTelemetryError);
                InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_FAILED);
            },
            [](const std::string& error) {
                logger_add("[TELEMETRY] Failed to send crash report: %s", error.c_str());
                snprintf(g_crashTelemetryError, sizeof(g_crashTelemetryError), "%s", error.c_str());
                InterlockedExchange(&g_crashTelemetryState, CRASH_TELEMETRY_FAILED);
            },
            5000
        );

        bool wantRestart = exception_showCrashDialog(displayBuf, pExceptionInfo, crashFilePath);
        g_crashDialogHttpClient = nullptr;


        if (logsBuf) {
            HeapFree(GetProcessHeap(), 0, logsBuf);
        }
        if (loggerBuf) {
            HeapFree(GetProcessHeap(), 0, loggerBuf);
        }
        if (systemInfoBuf) {
            HeapFree(GetProcessHeap(), 0, systemInfoBuf);
        }
        if (threadsBuf) {
            HeapFree(GetProcessHeap(), 0, threadsBuf);
        }
        if (crashContextBuf) {
            HeapFree(GetProcessHeap(), 0, crashContextBuf);
        }

        HeapFree(GetProcessHeap(), 0, displayBuf);

        if (wantRestart) {
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

#ifdef EXCEPTION_TEST
        {
            // Intentionally crash inside the handler AFTER the dialog (and restart), to verify
            // that the re-entry guard suppresses any second dialog.
            volatile int* nullPtr = nullptr;
            *nullPtr = 0xDEAD;
        }
#endif
    }


    // Return EXCEPTION_EXECUTE_HANDLER to let the app exit normally (or EXCEPTION_CONTINUE_SEARCH)
    return EXCEPTION_EXECUTE_HANDLER;
}


#if EXCEPTION_TEST
static DWORD WINAPI exception_testThread(LPVOID) {
    Sleep(EXCEPTION_TEST_DELAY_MS);
     
    const unsigned char crashCode[5] = {
        0x31, 0xC0,       // xor eax, eax
        0x89, 0x00,       // mov [eax], eax
        0x90              // nop
    };

    
    // Deliberately write to address 0 to trigger an access violation
    //volatile int* nullPtr = nullptr;
    //*nullPtr = 0xDEAD;

    // Rewrite CL_Frame
    patch_copy(0x0040f850, (void*)crashCode, sizeof(crashCode));

    // RB_EndFrame
    //patch_copy(gfx_module_addr + 0x00033ed0, (void*)crashCode, sizeof(crashCode));

    return 0;
}
#endif

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void exception_init() {
    // Vectored handler: catches EXCEPTION_STACK_OVERFLOW before the normal SEH/unhandled
    // filter path, which is unreachable when the stack is fully exhausted.
    AddVectoredExceptionHandler(1 /*CALL_FIRST*/, exception_vectoredHandler);

    // Set the unhandled exception filter so that our CrashHandler is called if the app crashes.
    // Save the previous filter (the game may have installed its own) so we can chain to it.
    g_prevUnhandledFilter = SetUnhandledExceptionFilter(exception_handler);

#if EXCEPTION_TEST
    // Spawn a thread that will crash the process after EXCEPTION_TEST_DELAY_MS ms.
    // This allows verifying that the exception handler works correctly.
    HANDLE hThread = CreateThread(NULL, 0, exception_testThread, NULL, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
#endif
}
