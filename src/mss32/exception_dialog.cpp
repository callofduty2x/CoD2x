#include "../shared/http_client.h"

#include "exception.h"
#include "exception_internal.h"

#include <stdio.h>

#include "shared.h"

// ---------------------------------------------------------------------------
// Custom crash dialog - looks like a standard Windows dialog (MessageBox / TaskDialog):
// white content area, gray footer band with the buttons
// ---------------------------------------------------------------------------

#define CLR_WIN_BG          RGB(255, 255, 255)   // content area
#define CLR_FOOTER_BG       RGB(240, 240, 240)   // button band (COLOR_3DFACE)
#define CLR_FOOTER_LINE     RGB(223, 223, 223)
#define CLR_TEXT            RGB(0,   0,   0)
#define CLR_TEXT_MUTED      RGB(90,  90,  90)
#define CLR_HEADING         RGB(0,   51,  153)   // main instruction of TaskDialog
#define CLR_LINK            RGB(0,   102, 204)
#define CLR_EDIT_BG         RGB(246, 246, 246)
#define CLR_EDIT_FG         RGB(30,  30,  30)
#define CLR_BTN_PRIMARY_N   RGB(0,   95,  184)   // Windows accent blue (default button)
#define CLR_BTN_PRIMARY_P   RGB(0,   75,  150)
#define CLR_BTN_PRIMARY_TXT RGB(255, 255, 255)
#define CLR_BTN_N           RGB(253, 253, 253)
#define CLR_BTN_P           RGB(229, 229, 229)
#define CLR_BTN_BORDER      RGB(204, 204, 204)
#define CLR_BTN_TXT         RGB(0,   0,   0)
#define CLR_BTN_DISABLED    RGB(245, 245, 245)
#define CLR_BTN_DISABLED_BORDER RGB(220, 220, 220)
#define CLR_BTN_DISABLED_TXT    RGB(160, 160, 160)

// Buttons and closing are blocked for the first moments, so a key or mouse button the player was pressing
// when the game crashed (Enter, a click where the buttons appear) does not restart or close the game
#define CRASH_INPUT_GUARD_MS 3000

#define IDC_CRASH_EDIT      2001
#define IDC_BTN_RESTART     2002
#define IDC_BTN_EXIT        2003
#define IDC_BTN_DUMP        2004
#define IDC_CRASH_LABEL     2005  // subtitle under heading
#define IDC_CRASH_ICON      2006
#define IDC_CRASH_HEADING   2007
#define IDC_CRASH_SAVEPATH  2008  // saved-file path under subtitle
#define IDC_LNK_DETAILS     2009
#define IDC_CRASH_SENDSTATUS 2010
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
    HBRUSH            hBrushFooter;
    int               footerTop;        // client y where the gray button band starts
    DWORD             inputEnableTime;  // GetTickCount when the buttons and closing are allowed
    bool              inputEnabled;
};


volatile LONG g_crashSendState = CRASH_SEND_IDLE;
LONG g_crashSendSpinner = 0;
char g_crashSendError[256] = {0};

static void exception_updateSendLabel(HWND hwnd, CrashDialogData* data) {
    (void)data;

    HWND hSendStatus = GetDlgItem(hwnd, IDC_CRASH_SENDSTATUS);
    if (!hSendStatus)
        return;

    char status[512] = {0};
    LONG state = InterlockedCompareExchange(&g_crashSendState, 0, 0);
    switch (state) {
        case CRASH_SEND_SENDING: {
            static const char* dots[] = {"", ".", "..", "..."};
            LONG spin = InterlockedIncrement(&g_crashSendSpinner);
            const char* suffix = dots[spin % 4];
            snprintf(status, sizeof(status),
                "Crash report is being sent to CoD2x developers for analysis%s",
                suffix);
            break;
        }
        case CRASH_SEND_SUCCESS:
            snprintf(status, sizeof(status),
                "Crash report sent to CoD2x developers for analysis.");
            break;
        case CRASH_SEND_FAILED:
            snprintf(status, sizeof(status),
                "Failed to send crash report: %s\nIt will be sent again on the next game start. You can also report it on Discord - https://cod2x.me/",
                g_crashSendError[0] ? g_crashSendError : "unknown error");
            break;
        default:
            #if DEBUG && !EXCEPTION_TEST
            snprintf(status, sizeof(status),
                "Debug build: the crash report is NOT sent to CoD2x developers, it is only saved to the file above.\n"
                "(Build with EXCEPTION_TEST to test the sending.)");
            #else
            snprintf(status, sizeof(status),
                "Crash report will be sent to CoD2x developers for analysis.");
            #endif
            break;
    }

    SetWindowTextA(hSendStatus, status);
}

// ---------------------------------------------------------------------------
// Crash dialog
// ---------------------------------------------------------------------------

// Work area (without the taskbar) of the monitor, or of the primary monitor
static RECT exception_monitorWorkArea(HMONITOR hMonitor) {
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (hMonitor && GetMonitorInfoA(hMonitor, &mi))
        return mi.rcWork;
    RECT rc = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return rc;
}

// Moves (and shrinks if needed) the window so it is fully inside the work area of one monitor - the given one, or the
// monitor the window is mostly on. Also keeps it topmost (the fullscreen game window is topmost too).
static void exception_fitToMonitor(HWND hwnd, HMONITOR hMonitor) {
    RECT wr;
    if (!GetWindowRect(hwnd, &wr))
        return;
    if (!hMonitor)
        hMonitor = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
    RECT wa = exception_monitorWorkArea(hMonitor);

    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;

    int x = wr.left;
    int y = wr.top;
    if (x + w > wa.right)  x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void exception_resizeDialogClient(HWND hwnd, int clientW, int clientH, bool keepCentered) {
    if (clientW < CRASH_MIN_CLIENT_W) clientW = CRASH_MIN_CLIENT_W;
    if (clientH < CRASH_MIN_CLIENT_H) clientH = CRASH_MIN_CLIENT_H;

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
        newX = (oldWr.left + oldWr.right) / 2 - newW / 2;
        newY = (oldWr.top + oldWr.bottom) / 2 - newH / 2;
    } else {
        flags |= SWP_NOMOVE;
    }

    // The monitor the dialog is on now, the new size is fitted into it below
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    SetWindowPos(hwnd, NULL, newX, newY, newW, newH, flags);
    exception_fitToMonitor(hwnd, hMonitor);
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

    // Fixed 2-line height reserved for the send status area
    const int sendStatusH = 36;

    const int usableW = w - margin * 2;
    int btnW = (usableW - gap * 2) / 3;
    if (btnW > 220) btnW = 220;
    if (btnW < 150) btnW = 150;

    int totalW = btnW * 3 + gap * 2;
    int btnX   = (w - totalW) / 2;

    int sendStatusY = headerBottom + 8;
    int btnY = 0;
    int editY = 0;
    int editH = 0;
    if (showDetails) {
        btnY = h - margin - btnH;
        editY = sendStatusY + sendStatusH + gap;
        editH = btnY - margin - gap - editY; // ends above the footer band
        if (editH < 120) {
            editH = 120;
            btnY = editY + editH + gap + margin;
        }
    } else {
        // Buttons at the bottom, in the footer band under the send status
        btnY = h - margin - btnH;
        if (btnY < sendStatusY + sendStatusH + 24)
            btnY = sendStatusY + sendStatusH + 24;
    }
    if (data)
        data->footerTop = btnY - margin;

    HWND hIcon     = GetDlgItem(hwnd, IDC_CRASH_ICON);
    HWND hHeading  = GetDlgItem(hwnd, IDC_CRASH_HEADING);
    HWND hSavePath = GetDlgItem(hwnd, IDC_CRASH_SAVEPATH);
    HWND hLnkDetails = GetDlgItem(hwnd, IDC_LNK_DETAILS);
    HWND hSendStatus = GetDlgItem(hwnd, IDC_CRASH_SENDSTATUS);
    HWND hEdit     = GetDlgItem(hwnd, IDC_CRASH_EDIT);
    HWND hBtnR     = GetDlgItem(hwnd, IDC_BTN_RESTART);
    HWND hBtnD     = GetDlgItem(hwnd, IDC_BTN_DUMP);
    HWND hBtnE     = GetDlgItem(hwnd, IDC_BTN_EXIT);

    if (hIcon)     SetWindowPos(hIcon,     NULL, iconX,  iconY,     HDR_ICON_SIZE, HDR_ICON_SIZE, SWP_NOZORDER);
    if (hHeading)  SetWindowPos(hHeading,  NULL, textX,  headingY,  textW, headingH,  SWP_NOZORDER);
    if (hSavePath) SetWindowPos(hSavePath, NULL, textX,  savepathY, textW, savepathH, SWP_NOZORDER);
    if (hLnkDetails) SetWindowPos(hLnkDetails, NULL, textX, linkY, textW, linkH, SWP_NOZORDER);
    if (hSendStatus) SetWindowPos(hSendStatus, NULL, textX, sendStatusY, textW, sendStatusH, SWP_NOZORDER);

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
        data->hBrushFooter    = CreateSolidBrush(CLR_FOOTER_BG);
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

        HWND hSendStatus = CreateWindowEx(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
            margin, HDR_H + 10, rc.right - margin * 2, 36,
            hwnd, (HMENU)IDC_CRASH_SENDSTATUS, GetModuleHandle(NULL), NULL);
        SendMessage(hSendStatus, WM_SETFONT, (WPARAM)data->hBtnFont, FALSE);
        exception_updateSendLabel(hwnd, data);

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
        // Disabled until CRASH_INPUT_GUARD_MS passed (see WM_TIMER), a click or Enter from the game does not press them
        CreateWindowEx(0, "BUTTON", "Restart Game",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
            btnX, btnY, btnW, btnH,
            hwnd, (HMENU)IDC_BTN_RESTART, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, "BUTTON", "Generate Full Dump",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
            btnX + (btnW + gap), btnY, btnW, btnH,
            hwnd, (HMENU)IDC_BTN_DUMP, GetModuleHandle(NULL), NULL);
        CreateWindowEx(0, "BUTTON", "Exit",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
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

        // Gray footer band with a separator line, like the button area of a Windows dialog
        if (data->footerTop > 0 && data->footerTop < rc2.bottom) {
            RECT footer = rc2;
            footer.top = data->footerTop;
            FillRect(hdc, &footer, data->hBrushFooter);
            RECT line = footer;
            line.bottom = line.top + 1;
            HBRUSH hLine = CreateSolidBrush(CLR_FOOTER_LINE);
            FillRect(hdc, &line, hLine);
            DeleteObject(hLine);
        }
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
        // Heading: main instruction blue, like TaskDialog
        if (ctlId == IDC_CRASH_HEADING) {
            SetBkColor(hdc, CLR_WIN_BG);
            SetTextColor(hdc, CLR_HEADING);
            return (LRESULT)data->hBrushBg;
        }
        if (ctlId == IDC_LNK_DETAILS) {
            SetBkColor(hdc, CLR_WIN_BG);
            SetTextColor(hdc, CLR_LINK);
            return (LRESULT)data->hBrushBg;
        }
        if (ctlId == IDC_CRASH_SENDSTATUS) {
            SetBkColor(hdc, CLR_WIN_BG);
            LONG sendState = InterlockedCompareExchange(&g_crashSendState, 0, 0);
            if (sendState == CRASH_SEND_SUCCESS)
                SetTextColor(hdc, RGB(16, 124, 16));   // green: sent ok
            else if (sendState == CRASH_SEND_FAILED)
                SetTextColor(hdc, RGB(196, 43, 28));   // red: failed
            else if (sendState == CRASH_SEND_SENDING)
                SetTextColor(hdc, RGB(157, 93, 0));    // amber: in progress
            else
                SetTextColor(hdc, CLR_TEXT_MUTED);     // grey: idle (not sent in debug build)
            return (LRESULT)data->hBrushBg;
        }
        // Save path: regular text
        SetBkColor(hdc, CLR_WIN_BG);
        SetTextColor(hdc, CLR_TEXT);
        return (LRESULT)data->hBrushBg;
    }
    case WM_DRAWITEM: {
        if (!data) break;
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        bool primary = dis->CtlID == IDC_BTN_RESTART;

        // Flat buttons like Windows 11: "Restart Game" is the accent (default) button, the others light with a border
        COLORREF clrBg, clrBorder, clrText;
        if (disabled) {
            clrBg = CLR_BTN_DISABLED;
            clrBorder = CLR_BTN_DISABLED_BORDER;
            clrText = CLR_BTN_DISABLED_TXT;
        } else if (primary) {
            clrBg = pressed ? CLR_BTN_PRIMARY_P : CLR_BTN_PRIMARY_N;
            clrBorder = clrBg;
            clrText = CLR_BTN_PRIMARY_TXT;
        } else {
            clrBg = pressed ? CLR_BTN_P : CLR_BTN_N;
            clrBorder = CLR_BTN_BORDER;
            clrText = CLR_BTN_TXT;
        }

        HBRUSH hBrush = CreateSolidBrush(clrBg);
        FillRect(dis->hDC, &dis->rcItem, hBrush);
        DeleteObject(hBrush);
        HBRUSH hBorder = CreateSolidBrush(clrBorder);
        FrameRect(dis->hDC, &dis->rcItem, hBorder);
        DeleteObject(hBorder);

        char text[64] = {0};
        GetWindowTextA(dis->hwndItem, text, sizeof(text));

        // Seconds until the buttons work, "Restart Game (3)"
        if (disabled && !data->inputEnabled) {
            LONG left = (LONG)(data->inputEnableTime - GetTickCount());
            if (left > 0) {
                size_t len = strlen(text);
                snprintf(text + len, sizeof(text) - len, " (%ld)", (left + 999) / 1000);
            }
        }

        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, clrText);
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
            InvalidateRect(hwnd, NULL, TRUE); // the footer band moved
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
        } else if (data && !data->inputEnabled) {
            // Buttons are not allowed yet (they are disabled, this only guards any other way to send the command)
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
            exception_updateSendLabel(hwnd, data);

            // Input guard: enable the buttons once CRASH_INPUT_GUARD_MS passed, until then redraw the countdown.
            // No button gets the focus, so a held Enter does not press one either.
            if (!data->inputEnabled) {
                const int buttons[] = { IDC_BTN_RESTART, IDC_BTN_DUMP, IDC_BTN_EXIT };
                bool enable = (LONG)(GetTickCount() - data->inputEnableTime) >= 0;
                if (enable)
                    data->inputEnabled = true;
                for (int id : buttons) {
                    HWND hBtn = GetDlgItem(hwnd, id);
                    if (!hBtn) continue;
                    if (enable)
                        EnableWindow(hBtn, TRUE);
                    InvalidateRect(hBtn, NULL, TRUE);
                }
            }
        }
        return 0;
    case WM_CLOSE:
        // Input guard: Alt+F4 or the close button right after the crash are ignored
        if (data && !data->inputEnabled)
            return 0;
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
            if (data->hBrushFooter) { DeleteObject(data->hBrushFooter); data->hBrushFooter = NULL; }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * Show the crash dialog (Windows dialog look, the report in a scrollable box under "Show crash details"),
 * fully on the monitor of the game, topmost, with buttons and closing blocked for CRASH_INPUT_GUARD_MS.
 * Returns true if the user chose to restart the game.
 */
bool exception_showCrashDialog(const char* content, EXCEPTION_POINTERS* pExInfo, DWORD crashThreadId, const char* savePath) {
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = exception_crashDlgProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   // handled in WM_ERASEBKGND
    wc.lpszClassName = CRASH_DLG_CLASS;
    wc.hIcon         = LoadIcon(NULL, IDI_ERROR);
    RegisterClassEx(&wc);

    // The dialog is shown on the monitor of the window that was in front - the game - fully inside its work area
    HMONITOR hMonitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    RECT workArea = exception_monitorWorkArea(hMonitor);
    int screenW = workArea.right - workArea.left;
    int screenH = workArea.bottom - workArea.top;
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
    int dlgX    = workArea.left + (screenW - dlgW) / 2;
    int dlgY    = workArea.top + (screenH - dlgH) / 2;

    CrashDialogData data = {
        content, savePath, pExInfo, crashThreadId, false,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
        false,
        compactClientW, compactClientH,
        expandedClientW, expandedClientH,
        NULL, 0,
        GetTickCount() + CRASH_INPUT_GUARD_MS, false
    };

    HWND hwnd = CreateWindowEx(
        dlgExStyle,
        CRASH_DLG_CLASS, "Call of Duty 2 - Application Crash",
        dlgStyle,
        dlgX, dlgY, dlgW, dlgH,
        NULL, NULL, GetModuleHandle(NULL), &data);

    if (hwnd) {
        // Fully on the monitor and above the game (its fullscreen window is topmost too)
        exception_fitToMonitor(hwnd, hMonitor);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        MSG wmsg;
        while (GetMessage(&wmsg, NULL, 0, 0) > 0) {
            TranslateMessage(&wmsg);
            DispatchMessage(&wmsg);
            // Poll HTTP client to allow the report to be sent while dialog is open
            if (g_crashDialogHttpClient) {
                g_crashDialogHttpClient->poll(10);  // 10ms poll
            }
        }
        DestroyWindow(hwnd);
        UnregisterClass(CRASH_DLG_CLASS, GetModuleHandle(NULL));
    }

    return data.wantRestart;
}


