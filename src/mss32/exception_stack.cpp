#include "exception.h"
#include "exception_internal.h"

#include <stdio.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <winternl.h>

#include "shared.h"
#include "symbols_cod2mp_s.h"
#include "symbols_gfx_d3d_mp_x86_s.h"
#include "symbols_mss32.h"
#include "../shared/exception.h"

static bool exception_isExecutable(unsigned int addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (addr == 0 || VirtualQuery((void*)(uintptr_t)addr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
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
bool exception_isReturnAddress(unsigned int addr) {
    ExceptionCallSite cs;
    return exception_isExecutable(addr) && exception_decodeCallSite(addr, 0, &cs);
}

/**
 * Describes the CALL instruction that produced return address 'retAddr', with the resolved callee
 * when it is known: "call 0x00434F70  (Com_Frame_Try_Block_Function)", "call [ebp-0xC] -> 0x0040F850  (CL_Frame)".
 */
void exception_describeCallSite(HANDLE hProcess, unsigned int retAddr, DWORD frameEbp,
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

void exception_initSymbols() {
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
std::string exception_getThreadName(HANDLE hThread) {
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
void exception_prependCrashFrame0(HANDLE hProcess, StackCollectionResult& result,
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
void exception_walkStackWalk64(HANDLE hProcess, const ExceptionThreadTarget& target,
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
void exception_walkEbpChain(HANDLE hProcess, const ExceptionThreadTarget& target,
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
void exception_collectTargetThreads(DWORD crashThreadId, EXCEPTION_POINTERS* pEx,
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


