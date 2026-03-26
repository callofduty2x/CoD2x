#include "exception.h"

#include <windows.h>
#include <stdio.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "shared.h"
#include "window.h"
#include "error.h"
#include "main.h"
#include "console.h"
#include "../shared/cod2_dvars.h"
#include "../shared/logger.h"
#include "symbols_cod2mp_s.h"
#include "symbols_gfx_d3d_mp_x86_s.h"
#include "symbols_mss32.h"

#define EXCEPTION_TEST 0
#define EXCEPTION_TEST_DELAY_MS 5000

// Value 0 = no crash in progress; >= 1 = handler already running (re-entry guard).
volatile long exception_processCrashed = 0;

// Timestamp captured once at the start of exception_handler; shared by the .txt and .dmp filenames.
static SYSTEMTIME exception_crashTime;




// ---------------------------------------------------------------------------
// Custom crash dialog - modern dark theme
// ---------------------------------------------------------------------------

#define CLR_WIN_BG          RGB(48,  22,  22)   // Subtle red tint - signals a crash
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
#define CRASH_DLG_CLASS     "CoD2x_CrashDialog"

#define HDR_ICON_SIZE   48
#define HDR_H           100

struct CrashDialogData {
    const char*       text;
    const char*       savePath;   // full path to the saved crash .txt file
    EXCEPTION_POINTERS* pExInfo;
    bool              wantRestart;
    HFONT             hFont;
    HFONT             hBtnFont;
    HFONT             hHeadingFont;
    HBRUSH            hBrushBg;
    HBRUSH            hBrushEdit;
    WNDPROC           hEditOrigProc;
};

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

static void exception_layoutCrashDlg(HWND hwnd, int w, int h) {
    const int margin = 14, btnH = 42, btnW = 200, gap = 10;

    // Header area: icon + heading + subtitle + save path
    int iconX     = margin;
    int iconY     = (HDR_H - HDR_ICON_SIZE) / 2;
    int textX     = iconX + HDR_ICON_SIZE + 12;
    int textW     = w - textX - margin;
    int headingY  = margin;
    int headingH  = 26;
    int subtitleY = headingY + headingH + 4;
    int subtitleH = 16;
    int savepathY = subtitleY + subtitleH + 3;
    int savepathH = 16;

    // Edit box fills space between header and buttons
    int editY = HDR_H + 6;
    int editH = h - editY - gap - btnH - margin;
    int btnY  = editY + editH + gap;
    int totalW = btnW * 3 + gap * 2;
    int btnX   = (w - totalW) / 2;

    HWND hIcon     = GetDlgItem(hwnd, IDC_CRASH_ICON);
    HWND hHeading  = GetDlgItem(hwnd, IDC_CRASH_HEADING);
    HWND hLabel    = GetDlgItem(hwnd, IDC_CRASH_LABEL);
    HWND hSavePath = GetDlgItem(hwnd, IDC_CRASH_SAVEPATH);
    HWND hEdit     = GetDlgItem(hwnd, IDC_CRASH_EDIT);
    HWND hBtnR     = GetDlgItem(hwnd, IDC_BTN_RESTART);
    HWND hBtnD     = GetDlgItem(hwnd, IDC_BTN_DUMP);
    HWND hBtnE     = GetDlgItem(hwnd, IDC_BTN_EXIT);

    if (hIcon)     SetWindowPos(hIcon,     NULL, iconX,  iconY,     HDR_ICON_SIZE, HDR_ICON_SIZE, SWP_NOZORDER);
    if (hHeading)  SetWindowPos(hHeading,  NULL, textX,  headingY,  textW, headingH,  SWP_NOZORDER);
    if (hLabel)    SetWindowPos(hLabel,    NULL, textX,  subtitleY, textW, subtitleH, SWP_NOZORDER);
    if (hSavePath) SetWindowPos(hSavePath, NULL, textX,  savepathY, textW, savepathH, SWP_NOZORDER);
    if (hEdit)     SetWindowPos(hEdit,     NULL, margin, editY,     w - margin * 2, editH, SWP_NOZORDER);
    if (hBtnR)     SetWindowPos(hBtnR,     NULL, btnX,                    btnY, btnW, btnH, SWP_NOZORDER);
    if (hBtnD)     SetWindowPos(hBtnD,     NULL, btnX + (btnW + gap),     btnY, btnW, btnH, SWP_NOZORDER);
    if (hBtnE)     SetWindowPos(hBtnE,     NULL, btnX + (btnW + gap) * 2, btnY, btnW, btnH, SWP_NOZORDER);
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
        data->hEditOrigProc   = NULL;
        data->hHeadingFont    = NULL;

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
        int subtitleY = headingY + headingH + 4;
        int subtitleH = 16;
        int savepathY = subtitleY + subtitleH + 3;
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

        // Subtitle
        HWND hLabel = CreateWindowEx(0, "STATIC",
            "Please send the text below on Discord to get help or report a bug. More info at https://cod2x.me/.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            textX, subtitleY, textW, subtitleH,
            hwnd, (HMENU)IDC_CRASH_LABEL, GetModuleHandle(NULL), NULL);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBtnFont, FALSE);

        // Save path
        char savePathText[MAX_PATH * 2 + 32] = {0};
        snprintf(savePathText, sizeof(savePathText), "The crash report was saved to: %s",
            (data->savePath && data->savePath[0]) ? data->savePath : "(unknown)");
        HWND hSavePath = CreateWindowEx(0, "STATIC", savePathText,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            textX, savepathY, textW, savepathH,
            hwnd, (HMENU)IDC_CRASH_SAVEPATH, GetModuleHandle(NULL), NULL);
        SendMessage(hSavePath, WM_SETFONT, (WPARAM)data->hBtnFont, FALSE);

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
        // Subtitle / icon / save path: muted light text
        SetBkColor(hdc, CLR_WIN_BG);
        SetTextColor(hdc, RGB(200, 170, 170));
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
        if (data) exception_layoutCrashDlg(hwnd, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize = { 700, 500 };
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_RESTART) {
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
    case WM_CLOSE:
        if (data) data->wantRestart = false;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        if (data) {
            if (data->hFont)        { DeleteObject(data->hFont);        data->hFont = NULL; }
            if (data->hBtnFont)     { DeleteObject(data->hBtnFont);     data->hBtnFont = NULL; }
            if (data->hHeadingFont) { DeleteObject(data->hHeadingFont); data->hHeadingFont = NULL; }
            if (data->hBrushBg)     { DeleteObject(data->hBrushBg);     data->hBrushBg = NULL; }
            if (data->hBrushEdit)   { DeleteObject(data->hBrushEdit);   data->hBrushEdit = NULL; }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * Dump selected CVARs.
 */
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
 * Append extra diagnostics (system, memory, process, registers, modules)
 * to buf. Returns number of bytes written.
 */
static size_t exception_buildSysInfo(char* buf, size_t bufSize, EXCEPTION_POINTERS* pEx) {
    size_t pos = 0;

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

    // --- Registers ---
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
    int dlgW    = (1200 < screenW - 40) ? 1200 : screenW - 40;
    int dlgH    = (820  < screenH - 40) ? 820  : screenH - 40;
    int dlgX    = (screenW - dlgW) / 2;
    int dlgY    = (screenH - dlgH) / 2;

    CrashDialogData data = { content, savePath, pExInfo, false, NULL, NULL, NULL, NULL, NULL, NULL };

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST,
        CRASH_DLG_CLASS, "Call of Duty 2 - Application Crash",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        dlgX, dlgY, dlgW, dlgH,
        NULL, NULL, GetModuleHandle(NULL), &data);

    if (hwnd) {
        SetForegroundWindow(hwnd);
        MSG wmsg;
        while (GetMessage(&wmsg, NULL, 0, 0) > 0) {
            TranslateMessage(&wmsg);
            DispatchMessage(&wmsg);
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

/**
 * Returns true when the bytes immediately before 'addr' form a recognisable
 * CALL instruction, making 'addr' a plausible return address.
 * Handles the most common x86 encodings (near, indirect, absolute).
 */
static bool exception_isReturnAddress(unsigned int addr) {
    if (addr < 6) return false;
    const BYTE* p = (const BYTE*)addr;
    if (IsBadReadPtr(p - 6, 6)) return false;
    if (p[-5] == 0xE8)                             return true; // E8 rel32
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0)  return true; // FF D? (call reg)
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)  return true; // FF /2 rm
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)  return true; // FF /2 disp8
    if (p[-6] == 0xFF && p[-5] == 0x15)            return true; // FF 15 abs32
    return false;
}

/**
 * Describes the CALL instruction that produced return address 'retAddr'.
 * The return address sits immediately after the CALL opcode, so we inspect
 * the bytes that precede it to reconstruct the instruction text and, for
 * direct/indirect calls, resolve the callee symbol when possible.
 */
static void exception_describeCallSite(HANDLE hProcess, unsigned int retAddr,
                                       char* out, size_t outSize) {
    static const char* const regNames[8] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"
    };
    const BYTE* p = (const BYTE*)retAddr;

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
        snprintf(outSym, symSize, "%s+0x%llX", sb.sym.Name, (unsigned long long)disp);
        return true;
    }
    // CoD2MP_s.exe symbols lookup
    if (hMod == GetModuleHandleA("CoD2MP_s.exe")) {
        if (const Cod2mpSymbol* sym = symbols_cod2mp_s_find(addr)) {
            uint32_t d = addr - sym->addr;
            if (d == 0)
                snprintf(outSym, symSize, "0x%08X (%s)", addr, sym->name);
            else
                snprintf(outSym, symSize, "0x%08X (%s+0x%X)", addr, sym->name, d);
            return true;
        }
    }
    // gfx_d3d_mp_x86_s.dll symbols lookup
    if (hMod == GetModuleHandleA("gfx_d3d_mp_x86_s.dll")) {
        if (const GfxD3dSymbol* sym = symbols_gfx_d3d_mp_x86_s_find(addr)) {
            uint32_t d = addr - sym->addr;
            if (d == 0)
                snprintf(outSym, symSize, "0x%08X (%s)", addr, sym->name);
            else
                snprintf(outSym, symSize, "0x%08X (%s+0x%X)", addr, sym->name, d);
            return true;
        }
    }
    // mss32.dll symbols lookup (RVA-based; populated one build behind by generate_mss32_symbols_h.ps1)
    if (hMod == GetModuleHandleA("mss32.dll")) {
        if (const Mss32Symbol* sym = symbols_mss32_find(rva)) {
            uint32_t d = rva - sym->rva;
            if (d == 0)
                snprintf(outSym, symSize, "0x%08X (%s)", addr, sym->name);
            else
                snprintf(outSym, symSize, "0x%08X (%s+0x%X)", addr, sym->name, d);
            return true;
        }
    }
    // Export-table path
    return exception_getNearestExport(hMod, rva, outSym, symSize);
}

/** Called when exception happend */
LONG WINAPI exception_handler(EXCEPTION_POINTERS* pExceptionInfo) {

    // If the handler is already running, a secondary exception occurred inside it.
    // Skip everything to avoid showing a second dialog or sending an incomplete crash report.
    // Just let the process terminate.
    if (exception_processCrashed >= 1) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // In case of crash stop the watchdog thread.
    // InterlockedIncrement is used to avoid a race when two threads crash simultaneously.
    InterlockedIncrement(&exception_processCrashed);

    // Capture the crash timestamp once so the .txt and .dmp files share the same name.
    GetLocalTime(&exception_crashTime);

    gamma_restore();


    char moduleName[MAX_PATH] = {0};
    unsigned int moduleBase = 0;
    unsigned int fileOffset = 0;
    const size_t stackDumpSize = 64; // number of stack DWORDs to examine
    const size_t perLineSize   = 256; // bytes per formatted output line
    char stackDump[stackDumpSize * perLineSize];
    strcpy(stackDump, "Error reading stack");

    unsigned int exceptionCode    = pExceptionInfo->ExceptionRecord->ExceptionCode;
    unsigned int exceptionAddress = (unsigned int)pExceptionInfo->ExceptionRecord->ExceptionAddress;

    // Initialise DbgHelp so SymFromAddr can resolve symbols for loaded modules.
    HANDLE hProcess = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(hProcess, NULL, TRUE);

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

        // Get ESP at the time of the exception
        uintptr_t espValue = 0;
        if (pExceptionInfo && pExceptionInfo->ContextRecord)
            espValue = pExceptionInfo->ContextRecord->Esp;

        // Column header (always the first line)
        static const char colHeader[] =
            "  #  Address     Offset      Module                  Section   Symbol\n";
        const size_t hdrLen = sizeof(colHeader) - 1;
        memcpy(stackDump, colHeader, hdrLen + 1);
        size_t written = hdrLen;

        // Walk the stack, emitting one line per plausible return address
        if (espValue) {
            DWORD* stack  = (DWORD*)espValue;
            int    frameN = 1; // frame 0 is the exception address, prepended after the loop

            for (size_t i = 0; i < stackDumpSize && written < sizeof(stackDump) - perLineSize; ++i) {

                // Ensure the stack slot itself is readable
                if (IsBadReadPtr(&stack[i], sizeof(DWORD)))
                    break;

                unsigned int stackValue = (unsigned int)stack[i];

                // Must point to readable memory
                if (IsBadReadPtr((void*)stackValue, sizeof(DWORD)))
                    continue;

                // Must belong to an executable region
                MEMORY_BASIC_INFORMATION mbi2;
                if (VirtualQuery((void*)stackValue, &mbi2, sizeof(mbi2)) == 0)
                    continue;
                if (!(mbi2.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                    continue;

                // Must look like a return address (bytes before it form a CALL instruction)
                if (!exception_isReturnAddress(stackValue))
                    continue;

                // Resolve module name
                char modName2[MAX_PATH] = {0};
                GetModuleFileNameA((HMODULE)mbi2.AllocationBase, modName2, sizeof(modName2));
                char* p2 = strrchr(modName2, '\\');
                if (p2) strcpy(modName2, p2 + 1);
                if (modName2[0] == '\0')
                    continue;

                unsigned int addr2   = stackValue;
                unsigned int offset2 = addr2 - (unsigned int)mbi2.AllocationBase;

                // Null-pointer call recovery: fix up exception address from first matching frame
                if (exceptionAddress == 0 && strncmp(modName2, moduleName, MAX_PATH) == 0) {
                    exceptionAddress = addr2;
                    fileOffset       = offset2;
                }

                // PE section
                char section2[16] = {0};
                if (!exception_getPeSection((HMODULE)mbi2.AllocationBase, offset2, section2, sizeof(section2)))
                    strncpy(section2, "?", sizeof(section2) - 1);

                // Symbol: resolve via DbgHelp/exports, then always also describe the call
                // instruction that produced this return address (shows the callee name).
                char symbol2[300] = {0};
                bool hasSymbol = exception_getSymbolName(hProcess, (HMODULE)mbi2.AllocationBase,
                                             addr2, offset2, symbol2, sizeof(symbol2));
                char callSite2[200] = {0};
                exception_describeCallSite(hProcess, addr2, callSite2, sizeof(callSite2));
                if (!hasSymbol) {
                    strncpy(symbol2, callSite2[0] ? callSite2 : "<no symbol>", sizeof(symbol2) - 1);
                } else if (callSite2[0] != '\0') {
                    size_t symLen = strlen(symbol2);
                    snprintf(symbol2 + symLen, sizeof(symbol2) - symLen, "  ->  %s", callSite2);
                }

                written += snprintf(stackDump + written, sizeof(stackDump) - written,
                    "%3d  0x%08X  0x%08X  %-22s  %-8s  %s\n",
                    frameN++, addr2, offset2, modName2, section2, symbol2);
            }
        } else {
            written += snprintf(stackDump + written, sizeof(stackDump) - written,
                "    Stack unavailable\n");
        }

        // Build and insert frame 0 (the actual exception address) right after the header,
        // now that exceptionAddress has been resolved (null-pointer call fix-up may have run).
        if (moduleName[0] != '\0') {
            // Use GetModuleHandleA so we get the same handle Windows uses for the PE,
            // rather than a raw integer cast of AllocationBase which can mismatch.
            HMODULE hMod0 = GetModuleHandleA(moduleName);
            if (!hMod0) hMod0 = (HMODULE)(uintptr_t)moduleBase;

            char section0[16] = {0};
            if (!exception_getPeSection(hMod0, fileOffset, section0, sizeof(section0)))
                strncpy(section0, "?", sizeof(section0) - 1);

            char symbol0[300] = {0};
            if (!exception_getSymbolName(hProcess, hMod0,
                                         exceptionAddress, fileOffset, symbol0, sizeof(symbol0)))
                strncpy(symbol0, "<no symbol>", sizeof(symbol0) - 1);

            char frame0[512];
            int f0len = snprintf(frame0, sizeof(frame0),
                "  0  0x%08X  0x%08X  %-22s  %-8s  %s\n",
                exceptionAddress, fileOffset, moduleName, section0, symbol0);

            if (f0len > 0 && (size_t)f0len + written < sizeof(stackDump)) {
                // Insert after the column-header line
                memmove(stackDump + hdrLen + f0len, stackDump + hdrLen, written - hdrLen + 1);
                memcpy(stackDump + hdrLen, frame0, f0len);
                written += f0len;
            }
        }
    }

    error_sendCrashData((unsigned int)exceptionCode, exceptionAddress, moduleName, fileOffset, stackDump);

    // Build full display report: crash info → stack → sys/mem/proc/regs/modules/cvars/files → logs → logger
    // stackDump uses LF; we need CRLF for the Win32 EDIT control
    const size_t stackCrlfSize = sizeof(stackDump) * 2;
    const size_t sysInfoSize   = 48 * 1024; // sys/mem/proc/regs/modules/cvars/main-folder-files
    const size_t logsSize      = 0x4000 + 4096; // console buffer + headroom for CRLF expansion
    const size_t loggerBufSize = LOG_CAPACITY * (LOG_MSG_LEN + 32);
    const size_t displaySize   = 1024 + stackCrlfSize + sysInfoSize + logsSize + loggerBufSize * 2;
    char* displayBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, displaySize);

    if (displayBuf) {
        size_t pos = 0;

        // Crash info
        pos += snprintf(displayBuf + pos, displaySize - pos,
            "=== CRASH INFO ===\r\n"
            "Code:         0x%08X  (%s)\r\n"
            "Address:      0x%08X  (%s)\r\n"
            "File Offset:  0x%08X\r\n"
            "Version:      %s\r\n"
            "\r\n"
            "=== STACK ===\r\n",
            exceptionCode, exception_getText(exceptionCode),
            exceptionAddress, (moduleName[0] ? moduleName : "unknown module"),
            fileOffset, APP_VERSION);

        // Convert stack LF -> CRLF
        pos += exception_lf_to_crlf(stackDump, displayBuf + pos, displaySize - pos);

        // System / memory / process / registers / modules / cvars / main folder files
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");
        pos += exception_buildSysInfo(displayBuf + pos, displaySize - pos, pExceptionInfo);

        // Logger ring-buffer (newest first with age offsets)
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n=== LOGGER ===\r\n");
        {
            char* loggerBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, loggerBufSize);
            if (loggerBuf) {
                logger_get_recent(loggerBuf, loggerBufSize);
                pos += exception_lf_to_crlf(loggerBuf, displayBuf + pos, displaySize - pos);
                HeapFree(GetProcessHeap(), 0, loggerBuf);
            }
        }
        if (pos >= 2 && !(displayBuf[pos - 2] == '\r' && displayBuf[pos - 1] == '\n'))
            pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");

        // Console logs (from game's EDIT control)
        pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n=== LOGS ===\r\n");
        pos += console_getLogs(displayBuf + pos, displaySize - pos);
        if (pos >= 2 && !(displayBuf[pos - 2] == '\r' && displayBuf[pos - 1] == '\n'))
            pos += snprintf(displayBuf + pos, displaySize - pos, "\r\n");


        // Save crash report to a timestamped file in the game root before showing the dialog
        char crashFilePath[MAX_PATH * 2] = {0};
        exception_saveToFile(displayBuf, crashFilePath, sizeof(crashFilePath));

        bool wantRestart = exception_showCrashDialog(displayBuf, pExceptionInfo, crashFilePath);
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
    /*// Deliberately write to address 0 to trigger an access violation
    volatile int* nullPtr = nullptr;
    *nullPtr = 0xDEAD;*/

    // Rewrite CL_Frame
    //patch_jump(0x0040f850, 0x00);

    // R_AddCmdDrawQuadPic
    patch_jump(gfx_module_addr + 0x100218e0, 0x00);
  

    return 0;
}
#endif

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void exception_init() {
    // Set the unhandled exception filter so that our CrashHandler is called if the app crashes.
    SetUnhandledExceptionFilter(exception_handler);

#if EXCEPTION_TEST
    // Spawn a thread that will crash the process after EXCEPTION_TEST_DELAY_MS ms.
    // This allows verifying that the exception handler works correctly.
    HANDLE hThread = CreateThread(NULL, 0, exception_testThread, NULL, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
#endif
}
