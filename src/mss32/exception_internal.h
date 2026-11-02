#ifndef EXCEPTION_INTERNAL_H
#define EXCEPTION_INTERNAL_H

// Parts of the crash handler that use each other, only for exception*.cpp:
//   exception.cpp          handlers, stages of a crash, files, freeze report, self test
//   exception_stack.cpp    symbols and stack walking
//   exception_report.cpp   data collection, report as text and JSON
//   exception_dialog.cpp   crash dialog

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

class HttpClient;


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

// State of the report being sent while the crash dialog is open
enum CrashSendState {
    CRASH_SEND_IDLE = 0,
    CRASH_SEND_SENDING = 1,
    CRASH_SEND_SUCCESS = 2,
    CRASH_SEND_FAILED = 3
};


// ---------------------------------------------------------------------------
// exception.cpp
// ---------------------------------------------------------------------------

extern HttpClient* g_crashDialogHttpClient;
extern bool exception_initDone;
extern CRITICAL_SECTION exception_reportLock;
extern bool exception_reportLockReady;

void exception_appendf(std::string& out, const char* fmt, ...);
void exception_bufAppend(char* buf, size_t size, size_t* pos, const char* fmt, ...);
size_t exception_safeRead(uintptr_t addr, void* out, size_t len);
bool exception_getModuleForAddress(unsigned int addr, char* name, size_t nameSize, unsigned int* base);
const char* exception_getText(DWORD exceptionCode);
void exception_describeFault(const EXCEPTION_RECORD* rec, char* out, size_t outSize);


// ---------------------------------------------------------------------------
// exception_stack.cpp
// ---------------------------------------------------------------------------

void exception_initSymbols();
size_t exception_describeAddress(unsigned int addr, char* out, size_t outSize);
bool exception_isReturnAddress(unsigned int addr);
void exception_describeCallSite(HANDLE hProcess, unsigned int retAddr, DWORD frameEbp, char* out, size_t outSize);
std::string exception_getThreadName(HANDLE hThread);
void exception_prependCrashFrame0(HANDLE hProcess, StackCollectionResult& result, const char* moduleName, unsigned int moduleBase,
                                  unsigned int exceptionAddress, unsigned int fileOffset);
void exception_walkStackWalk64(HANDLE hProcess, const ExceptionThreadTarget& target, unsigned int exceptionAddress, StackCollectionResult& result);
void exception_walkEbpChain(HANDLE hProcess, const ExceptionThreadTarget& target, unsigned int exceptionAddress, StackCollectionResult& result);
void exception_collectTargetThreads(DWORD crashThreadId, EXCEPTION_POINTERS* pEx, std::vector<ExceptionThreadTarget>& out);


// ---------------------------------------------------------------------------
// exception_report.cpp
// ---------------------------------------------------------------------------

void exception_collectReport(const ExceptionReportInput& in, CrashReportData& r);
std::string exception_buildReportText(const CrashReportData& r);
std::string exception_buildReportJson(const CrashReportData& r);


// ---------------------------------------------------------------------------
// exception_dialog.cpp
// ---------------------------------------------------------------------------

extern volatile LONG g_crashSendState;
extern LONG g_crashSendSpinner;
extern char g_crashSendError[256];

bool exception_showCrashDialog(const char* content, EXCEPTION_POINTERS* pExInfo, DWORD crashThreadId, const char* savePath);

#endif
