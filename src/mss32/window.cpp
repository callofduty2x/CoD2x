#include "window.h"

#include <windows.h>
#include <stdio.h>
#include <cstdint>

#include "shared.h"
#include "rinput.h"
#include "demo.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"



#define vid_xpos                    (*((dvar_t**)0x00d77158))
#define vid_ypos                    (*((dvar_t**)0x00d77150))
#define r_fullscreen                (*((dvar_t**)0x00d77128))
#define r_autopriority              (*((dvar_t**)0x00d77130))
#define in_mouse                    (*((dvar_t**)0x00d52a4c))
#define cl_bypassMouseInput         (*((dvar_t**)0x006067d0))   // 1 = control ingame mouse movement even if menu is opened

#define r_mode                      (*((dvar_t**)(gfx_module_addr + 0x001ce688)))
#define r_displayRefresh            (*((dvar_t**)(gfx_module_addr + 0x001ce804)))
#define r_fullscreen_rendered       (*((dvar_t**)(gfx_module_addr + 0x001ce60c)))
#define r_monitor                   (*((dvar_t**)(gfx_module_addr + 0x001ce610)))
#define r_gamma                     (*((dvar_t**)(gfx_module_addr + 0x001ce62c)))

#define R_GetMonitorFromCvars       ((HMONITOR (__cdecl*)())(gfx_module_addr + 0x000127d0))

#define mouse_center_x              (*((int*)0x00d52a5c))
#define mouse_center_y              (*((int*)0x00d52a54))
#define mouse_windowIsActive        (*((int*)0x00d52a60)) // true when window is activated (in foreground, not minimized)
#define mouse_ingameMouseActive     (*((bool*)0x00d52a68))
#define mouse_isEnabled             (*((bool*)0x00d52a69))
#define mouse_offset_x_arr          (((int*)0x0098fda8))
#define mouse_offset_y_arr          (((int*)0x0098fdb0))
#define mouse_offset_index          (*((int*)0x0098fdb8))

#define menu_cursorX                (*((int*)0x01978034))
#define menu_cursorY                (*((int*)0x01978038))
#define menu_opened                 (*((bool*)0x01978254))

#define win_hwnd                    (*((HWND*)0x00d7713c))
#define win_isActivated             (*((int*)0x00d77144))
#define win_isMinimized             (*((int*)0x00d77148))
#define win_wheelRool               (*((UINT*)0x00d7712c))

#define input_mode                  (*((int*)0x0096b654))
#define clientState                 (*((clientState_e*)0x00609fe0))

dvar_t* m_debug;
dvar_t* m_enable = NULL;    // 1 = allow moving ingame and menu mouse cursor; 0 = bypass mouse movement, but still process the system cursor and mouse events

int minWidth = 0;
int minHeight = 0;

bool in_menu_last = true;
int window_clientStateLast = -1;

// Global state for gamma handling.
static HDC gamma_currentDC = NULL;
static char gamma_currentDeviceName[32] = "";
static WORD gamma_originalRamp[3][256] = { 0 };
static bool gamma_modified = false;
static bool gamma_warningShowed = false;
static float gamma_previous = 1.0f; // Previous gamma value to detect changes



// Creates a gamma ramp from the provided gamma value.
// A gamma of 1.0 results in an identity ramp.
static void gamma_createRamp(float gamma, WORD ramp[3][256])
{
    for (int i = 0; i < 256; i++)
    {
        double normalized = i / 255.0;
        double value = pow(normalized, 1.0 / gamma);
        int rampValue = (int)(value * 65535.0 + 0.5);
        if (rampValue > 65535)
            rampValue = 65535;
        ramp[0][i] = ramp[1][i] = ramp[2][i] = (WORD)rampValue;
    }
}

// Restores the original gamma ramp on the current device and cleans up.
void gamma_restore()
{
    if (gamma_currentDC && gamma_modified)
    {
        // Restore the saved gamma ramp.
        SetDeviceGammaRamp(gamma_currentDC, gamma_originalRamp);
        gamma_modified = false;
    }
    if (gamma_currentDC)
    {
        DeleteDC(gamma_currentDC);
        gamma_currentDC = NULL;
        gamma_currentDeviceName[0] = '\0';
    }
}

// Updates the gamma ramp for the monitor on which 'hWnd' is located.
// When in fullscreen (r_fullscreen true), the gamma is restored.
// Otherwise, if gamma != 1.0 the new ramp is applied.
// Call this function on WM_MOVE or whenever the window location changes.
bool gamma_update()
{
    HWND hWnd = win_hwnd;
    
    // Get dvar by name to avoid error when renderer is not loaded (server)
    dvar_t* gammaCvar = Dvar_GetDvarByName("r_gamma");
    if (gammaCvar == nullptr) {
        return true;
    }
    float gamma = gammaCvar->value.decimal;


    // Check window mode. If fullscreen, restore gamma and do nothing.
    if (r_fullscreen->value.boolean || win_hwnd == NULL)
    {
        gamma_restore();
        return true;
    }

    // Determine which monitor the window is on.
    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX mi;
    ZeroMemory(&mi, sizeof(MONITORINFOEX));
    mi.cbSize = sizeof(MONITORINFOEX);
    if (!GetMonitorInfo(hMonitor, &mi))
        return false;

    // If we are already using this monitor, then update or restore.
    if (gamma_currentDC && strcmp(gamma_currentDeviceName, mi.szDevice) == 0)
    {
        if (gamma == 1.0f)
        {
            gamma_restore();
        }
        else
        {
            WORD newRamp[3][256] = { 0 };
            gamma_createRamp(gamma, newRamp);
            if (SetDeviceGammaRamp(gamma_currentDC, newRamp))
            {
                gamma_modified = true;
            }
        }
        return true;
    }
    else
    {
        // New monitor: restore gamma on the old monitor before switching.
        gamma_restore();

        // Create a device context for the new monitor by its device name.
        gamma_currentDC = CreateDC(NULL, mi.szDevice, NULL, NULL);
        if (!gamma_currentDC)
            return false;
        strcpy(gamma_currentDeviceName, mi.szDevice);

        // Save the original gamma ramp.
        if (!GetDeviceGammaRamp(gamma_currentDC, gamma_originalRamp))
        {
            DeleteDC(gamma_currentDC);
            gamma_currentDC = NULL;
            gamma_currentDeviceName[0] = '\0';
            return false;
        }

        // If gamma is 1.0, no change is needed.
        if (gamma == 1.0f)
            return true;

        // Create and set the new gamma ramp.
        WORD newRamp[3][256] = { 0 };
        gamma_createRamp(gamma, newRamp);
        if (!SetDeviceGammaRamp(gamma_currentDC, newRamp))
        {
            if (!gamma_warningShowed)
                MessageBoxA(NULL, "Failed to apply gamma for the monitor.", "Gamma Warning", MB_ICONWARNING | MB_OK);
            gamma_warningShowed = true;
            gamma_restore();
            return false;
        }
        gamma_modified = true;
        return true;
    }
}





// 004648e0
void Mouse_DeactivateIngameCursor()
{
    if (!mouse_isEnabled || !mouse_ingameMouseActive)
        return;

    mouse_ingameMouseActive = 0;
    ReleaseCapture();
    
	while (ShowCursor(TRUE) < 0); // show system cursor
}

// 00464750
void Mouse_ActivateIngameCursor()
{
    SetCapture(win_hwnd);
    
    struct tagRECT rect;
    GetWindowRect(win_hwnd, &rect);

    mouse_center_x = (rect.right + rect.left) / 2;
    mouse_center_y = (rect.bottom + rect.top) / 2;

	while (ShowCursor(FALSE) >= 0); // hide system cursor
}


void Mouse_SetMenuCursorPos(int x, int y) {

    menu_cursorX = x;
    menu_cursorY = y;

    if (menu_opened) {
        // Original function to process mouse movement
        ((void (__cdecl*)(void* data, void* arg2, int32_t x, int32_t y))0x00542d60)(*((void**)0x005dcb10), nullptr, x, y);                  
    }
}


void Mouse_ProcessMovement() {

    POINT cursorPoint;
    GetCursorPos(&cursorPoint);

    // Get the cursor position relative to the window client area (inner window)
    POINT cursorRelativePoint = cursorPoint;
    ScreenToClient(win_hwnd, &cursorRelativePoint);

    RECT clientRect;
    GetClientRect(win_hwnd, &clientRect); // Get the inner area of the window

    // in_menu = if we control menu cursor
    bool in_menu = ((input_mode & 8) != 0 && !cl_bypassMouseInput->value.boolean && m_enable->value.boolean) || clientState < CLIENT_STATE_PRIMED;
    bool menu_changed = in_menu != in_menu_last;
    in_menu_last = in_menu;

    // Windowed menu
    if (in_menu && !r_fullscreen->value.boolean) {

        if (menu_changed) {
            // Set cursor position to top left corner of the window
            cursorPoint.x -= cursorRelativePoint.x;
            cursorPoint.y -= cursorRelativePoint.y;
            // If the cursor is outside the window, set it to the center
            if (menu_cursorX < 0 || menu_cursorX > 640 || menu_cursorY < 0 || menu_cursorY > 480) {
                menu_cursorX = 320;
                menu_cursorY = 240;
            }
            // Get last menu cursor and translate it to monitor coordinates
            cursorPoint.x += clientRect.left + (menu_cursorX * (clientRect.right - clientRect.left)) / 640;
            cursorPoint.y += clientRect.top + (menu_cursorY * (clientRect.bottom - clientRect.top)) / 480;

            SetCursorPos(cursorPoint.x, cursorPoint.y);
            return;
        }

        // Check if rect has some size (might return 0 if the window is minimized)
        if (clientRect.right - clientRect.left <= 0 || clientRect.bottom - clientRect.top <= 0) {
            Mouse_DeactivateIngameCursor();
            return;
        }

        // Scale the cursor position to the game window resolution
        // So if width is 1280 and cursor is at 1280, it will be 640 in 640x480 resolution
        int newMenuX = (cursorRelativePoint.x * 640) / (clientRect.right - clientRect.left);
        int newMenuY = (cursorRelativePoint.y * 480) / (clientRect.bottom - clientRect.top);

        // Cursor is outside window
        bool isInsideWindow = newMenuX >= 0 && newMenuX <= 640 && newMenuY >= 0 && newMenuY <= 480;
        if (isInsideWindow && !mouse_ingameMouseActive) {
            Mouse_ActivateIngameCursor(); // activate ingame cursor, hide system cursor
            mouse_ingameMouseActive = 1;
            if (m_debug->value.boolean)
                Com_Printf("Hiding system cursor\n");
        }
        else if (!isInsideWindow && mouse_ingameMouseActive) {
            Mouse_DeactivateIngameCursor(); // deactivate ingame cursor, show system cursor
            mouse_ingameMouseActive = 0;
            if (m_debug->value.boolean)
                Com_Printf("Show system cursor\n");
        }

        bool isCloseToWindow = newMenuX >= -20 && newMenuX <= 660 && newMenuY >= -20 && newMenuY <= 500;
        if (m_debug->value.boolean && (menu_cursorX != newMenuX || menu_cursorY != newMenuY) && isCloseToWindow) {
            Com_Printf("New menu cursor: (%d %d) (source: system)\n", newMenuX, newMenuY);
        }

        Mouse_SetMenuCursorPos(newMenuX, newMenuY);


    // Fullscreen menu or windowed game, use centering offset method
    } else {

        // Menu has been opened or closed
        if (menu_changed) {
            // In windowed mode the system cursor is not in center, avoid artificial movement
            if (!r_fullscreen->value.boolean) {
                cursorPoint.x = mouse_center_x;
                cursorPoint.y = mouse_center_y;
            }
        }

        // If the window is not active, deactivate the ingame cursor if it's active
        if (!mouse_windowIsActive || GetForegroundWindow() != win_hwnd)
        {
            Mouse_DeactivateIngameCursor();
            return;
        }

        // Activate the ingame cursor if it's not active yet
        if (mouse_ingameMouseActive == 0) {

            // Cursor is outside the inner area of the window
            if (r_fullscreen->value.boolean == false) {
                bool cursorIsOutside = 
                    cursorRelativePoint.x <= clientRect.left || cursorRelativePoint.x >= clientRect.right || 
                    cursorRelativePoint.y <= clientRect.top || cursorRelativePoint.y >= clientRect.bottom;
                if (cursorIsOutside)
                    return;
            }

            Mouse_ActivateIngameCursor();

            SetCursorPos(mouse_center_x, mouse_center_y);

            mouse_ingameMouseActive = 1;
            return;
        }

        int32_t x_offset = (cursorPoint.x - mouse_center_x);
        int32_t y_offset = (cursorPoint.y - mouse_center_y);

        // When raw input is enabled, use the raw input offsets
        if (rinput_is_enabled())
        {
            long rinput_x, rinput_y;
            rinput_get_last_offset(&rinput_x, &rinput_y);
            x_offset = rinput_x;
            y_offset = rinput_y;
        }
        
        SetCursorPos(mouse_center_x, mouse_center_y);

        if (m_debug->value.boolean && (x_offset != 0 || y_offset != 0))
            Com_Printf("Mouse move offset: (%d %d) (source: %s)\n", x_offset, y_offset, rinput_is_enabled() ? "rinput" : "system");

        // Mouse movement is bypassed
        if (!m_enable->value.boolean) {
            return;
        }

        if (in_menu) {

            int newMenuX = menu_cursorX + x_offset;
            int newMenuY = menu_cursorY + y_offset;
            
            // Make sure the cursor is inside the window
            if (newMenuX < 0) newMenuX = 0;
            else if (newMenuX > 640) newMenuX = 640;
            if (newMenuY < 0) newMenuY = 0;
            else if (newMenuY > 480) newMenuY = 480;

            Mouse_SetMenuCursorPos(newMenuX, newMenuY);

        // In game
        } else {
            // 2 dimensional array of mouse offsets used for mouse filtering and game movement
            // index is being swapped between 0 and 1
            mouse_offset_x_arr[mouse_offset_index] += x_offset;
            mouse_offset_y_arr[mouse_offset_index] += y_offset;
        }
    }
}


// 00464b30
void Mouse_Loop()
{
    if (!win_hwnd)
        return;

    rinput_mouse_loop();

    if (mouse_isEnabled)
    {
        Mouse_ProcessMovement();
    }

    // Get dvar by name to avoid error when renderer is not loaded (server)
    dvar_t* gammaCvar = Dvar_GetDvarByName("r_gamma");
    if (gammaCvar == nullptr)
        return;

    float gamma = gammaCvar->value.decimal;
    if (gamma != gamma_previous) {
        gamma_previous = gamma;

        gamma_update();
    }
}




// 00468db0
LRESULT CALLBACK CoD2WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    bool callOriginal = true;

    switch (uMsg)
    {
        case WM_CREATE: {
            win_hwnd = hwnd;
            win_wheelRool = RegisterWindowMessageA("MSWHEEL_ROLLMSG");

            rinput_on_main_window_create();

            callOriginal = false;        
            break;
        }


        case WM_CLOSE: {
            // Prevent the window from closing
            if (demo_getDemoForUpload()) {
                demo_scheduleCloseAfterUpload();
                return 0;
            }
            break; // allow default handling
        }


        case WM_DESTROY: {
            
            rinput_on_main_window_destory();

            gamma_restore();

            win_hwnd = NULL;

            callOriginal = false;
            break;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: 
        case WM_RBUTTONDOWN: 
        case WM_RBUTTONUP: 
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: 
        {
            // In windowed borderless mode the menu cursor is based on the system cursor.
            // Since we are hiding the system cursor, it wont automatically activate the window when clicking, so do it manually
            if (r_fullscreen->value.boolean == 0 && !win_isActivated) {
                SetForegroundWindow(hwnd);
            }
            break;
        }


        case WM_GETMINMAXINFO: {
            MINMAXINFO* pMinMaxInfo = (MINMAXINFO*)lParam;

            // Set minimum width and height for the client area
            RECT rect = {0, 0, minWidth, minHeight};
            AdjustWindowRect(&rect, WS_GROUP | WS_CAPTION | WS_SYSMENU | WS_OVERLAPPEDWINDOW, FALSE);

            pMinMaxInfo->ptMinTrackSize.x = rect.right - rect.left; // Minimum width
            pMinMaxInfo->ptMinTrackSize.y = rect.bottom - rect.top; // Minimum height

            return 0;
        }

        // Called when the window is moved
        case WM_MOVE: {
            if (r_fullscreen->value.boolean == 0) // Windowed mode
            {
                RECT lpRect;
                lpRect.left = 0;
                lpRect.top = 0;
                lpRect.right = 1;
                lpRect.bottom = 1;
                // Get window X and Y - lpRect.left and lpRect.top will contain negative values representing the size of the window borders and title bar.
                AdjustWindowRect(&lpRect, GetWindowLongA(hwnd, GWL_STYLE), 0);

                int xPos = (int)(short)LOWORD(lParam); // X-coordinate
                int yPos = (int)(short)HIWORD(lParam); // Y-coordinate
                RECT rect;
                if (GetWindowRect(hwnd, &rect)) { // Full 32-bit coordinate possible on high-DPI screens
                    xPos = rect.left; 
                    yPos = rect.top;
                }

                Dvar_SetInt(vid_xpos, xPos);
                Dvar_SetInt(vid_ypos, yPos);

                vid_xpos->modified = false;
                vid_ypos->modified = false;

                if (win_isActivated)
                    mouse_windowIsActive = 1;

                gamma_update();
            }

            // Original function also contained logic to handle hiding the cursor, thats removed now

            callOriginal = false;

            break;
        }

        // Called when the window is activated or deactivated with minimized state
        case WM_ACTIVATE: {
			bool bActivated = LOWORD(wParam) != WA_INACTIVE; // was activated (WA_ACTIVE or WA_CLICKACTIVE, not WA_INACTIVE)
			bool bMinimized = HIWORD(wParam) != 0;

            win_isMinimized = bMinimized;

            // Something with bindlist
            ((void (*)())0x0040bd50)();

            if (bActivated && bMinimized == 0) {
                win_isActivated = 1;              
                ((void (*)())0x0042c600)(); // Com_TouchMemory();

                logger_add("Window activated");

                gamma_update();
            } else {
                win_isActivated = 0;

                logger_add("Window deactivated / minimized");

                gamma_restore();
            }

            mouse_windowIsActive = win_isActivated;

            if (!win_isActivated)
                Mouse_DeactivateIngameCursor();

            // Original function also contained logic to handle hiding the cursor, thats removed now as its handled in Mouse_Loop

            callOriginal = false;

            break;
        }

        case WM_INPUT: {
            rinput_wm_input(lParam);
            break;
        }

    }

    if (!callOriginal) {
        return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }

    // Call the original function
	LRESULT ret = ((LRESULT (CALLBACK *)(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam))0x00468db0)(hwnd, uMsg, wParam, lParam);

    return ret;
}


struct GfxWindowParms
{
    HWND hwnd;
    int32_t hz;
    int32_t fullscreen;
    int32_t x;
    int32_t y;
    int32_t renderWidth;
    int32_t renderHeight;
    int32_t unk;
    int32_t aaSamples;
};



bool window_doBorderless(int renderWidth, int renderHeight, LONG *windowX, LONG *windowY) {
    bool doBorderless = false;

    // Call original function to determine on which monitor the game should be displayed according to r_monitor / vid_xpos / vid_ypos
    HMONITOR hMonitor = R_GetMonitorFromCvars();

    MONITORINFO monitorInfo;
    ZeroMemory(&monitorInfo, sizeof(MONITORINFO));
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoA(hMonitor, &monitorInfo)) {
        Com_Printf("Couldn't get monitor info for borderless window.");
    } else {
        int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
        int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

        // Check if monitor resolutions match the required game resolution
        if (monitorWidth == renderWidth && monitorHeight == renderHeight) {
            *windowX = monitorInfo.rcMonitor.left;
            *windowY = monitorInfo.rcMonitor.top;
            doBorderless = true;
        }
    }
    return doBorderless;
}


// 10012a80
// Function called in R_Init
int R_CreateWindow()
{
    // Load data from esi register using assembly
    struct GfxWindowParms* windowInfo;
    __asm__ ("movl %%esi, %0\n\t" : "=m"(windowInfo));

    int wsExStyle;
    int wsStyle;
    RECT rect;

    if (r_fullscreen_rendered->value.boolean) // Fullscreen
    {
        Com_Printf("Attempting %i x %i fullscreen with 32 bpp at %i hz\n", windowInfo->renderWidth, windowInfo->renderHeight, windowInfo->hz);
        wsExStyle = WS_EX_TOPMOST;
        wsStyle = WS_POPUP;
        windowInfo->fullscreen = 1; 
    }
    else
    {
        POINT borderlessPoint = {0, 0};
        bool doBorderless = window_doBorderless(windowInfo->renderWidth, windowInfo->renderHeight, &borderlessPoint.x, &borderlessPoint.y);

        if (doBorderless) {
            Com_Printf("Attempting %i x %i borderless window at (%i, %i)\n", windowInfo->renderWidth, windowInfo->renderHeight, borderlessPoint.x, borderlessPoint.y);               
            wsExStyle = 0;
            wsStyle = WS_POPUP; // hide the border
            windowInfo->x = borderlessPoint.x;
            windowInfo->y = borderlessPoint.y;
        } else {
            Com_Printf("Attempting %i x %i window at (%i, %i)\n", windowInfo->renderWidth, windowInfo->renderHeight, windowInfo->x, windowInfo->y);
            wsExStyle = 0;
            wsStyle = WS_CAPTION | WS_SYSMENU | WS_OVERLAPPEDWINDOW;
            // WS_CAPTION: Adds a title bar to the window.
            // WS_SYSMENU: Adds a window menu on the title bar. (Restore, Move, Size, Minimize, Maximize, Close)
            // WS_OVERLAPPEDWINDOW: Add resizing, minimizing, maximizing, and other standard features.
        }      
        windowInfo->fullscreen = 0;
    }

    minWidth = windowInfo->renderWidth;
    minHeight = windowInfo->renderHeight;

    // Calculates the required size of the window rectangle, based on the desired size of the client rectangle.
    rect.left = 0;
    rect.right = windowInfo->renderWidth;
    rect.top = 0;
    rect.bottom = windowInfo->renderHeight;
    AdjustWindowRectEx(&rect, wsStyle, 0, wsExStyle); // we provide required size, it returns the size of the full window rectangle including borders

    windowInfo->hwnd = CreateWindowExA(wsExStyle, "CoD2", "Call of Duty 2x Multiplayer", wsStyle, windowInfo->x, windowInfo->y,
                           rect.right - rect.left, rect.bottom - rect.top, 0, 0, GetModuleHandleA(nullptr), 0);

    if (!windowInfo->hwnd)
    {
        Com_Printf("Couldn't create a window.\n");
        return 0;
    }

    Com_Printf("Game window successfully created.\n");
    
    return 1;
}




/** Called every frame at the start of the frame. */
void window_frame() {

    // Player disconnected from the server
    if (clientState != window_clientStateLast && clientState == CLIENT_STATE_CONNECTED) {
        Dvar_SetBool(m_enable, true); // enable mouse movement
    }

    // First frame
    if (window_clientStateLast == -1) {
        window_clientStateLast = clientState; // set it now to avoid running this code again (Com_Error jumps to exception handler)
      
        // Check if sound is initialized properly
        if (dedicated->value.integer == 0 && *(int*)0xc94ed4 == 0) {
            Com_Error(ERR_DROP, "Sound system failed to initialize.\n\nThe game might crash at any time.\nPlease check your sound drivers.");
        }
    }

    window_clientStateLast = clientState;
}


/**
 * Hook for the call to R_UsedCachedStaticModelSurface at gfx_d3d_mp_x86_s.dll+0x0001f53f.
 *
 * The function performs a doubly-linked LRU move-to-front for a Static Model Cache.
 * 
 * arg1 (eax): pointer/offset somewhere within a cache array entry
 * data_108eab00: base address of the cache array (array of 0x188-byte entries)
 *
 * The function normalizes arg1 to the start of its cache entry:
 *   index = (arg1 - data_108eab00) / 0x188       (using magic multiply 0x5397829d)
 *   normalized_ptr = index * 0x188 + data_108eab00
 *
 * Cache entry structure (0x188 = 392 bytes total):
 *   +0x0  prev       (SMCNode* - LRU list previous)
 *   +0x4  next       (SMCNode* - LRU list next)
 *   +0x8  frameData  (int - frame counter from frontEndDataOut)
 *   +0xC  ...        (380 more bytes of cached surface data)
 *
 * Crash at 0x0002bd23 (since 1.4.3.2):
 *   mov ecx, [eax+4]   ; ecx = node->next
 *   mov [ecx], edx     ; ACCESS_VIOLATION because ecx (next) == NULL
 *
 * Root cause: R_CacheStaticModelSurface can return a cache entry whose next/prev
 * pointers are still zero (never linked into the LRU list), so unlink crashes.
 *
 * Fix: normalize arg1, check next/prev for NULL, skip if unlinked.
 */

struct SMCNode {
    SMCNode* prev;      // +0x0 LRU list previous pointer
    SMCNode* next;      // +0x4 LRU list next pointer
    int      frameData; // +0x8 Frame counter
    char cachedData[0x17c];
};

// VA 0x108eab00 - 0x10000000 (preferred base) = RVA 0x008eab00
#define SMC_CACHE_BASE      (gfx_module_addr + 0x008eab00u)
#define SMC_ENTRY_SIZE      0x188  // 392 bytes per cache entry

static void* hook_R_UsedCachedStaticModelSurface()
{
    uintptr_t arg1;  // Pointer passed in eax (may point anywhere within a cache entry)
    ASM(movr, arg1, "eax");

    // Normalize arg1 to the start of its 0x188-byte cache entry.
    // Original uses: index = (arg1 - base) / 0x188 via magic multiply 0x5397829d.
    // We use simple integer division (slower but correct, only runs on NULL check path).
    uintptr_t offset = arg1 - SMC_CACHE_BASE;
    uintptr_t index = offset / SMC_ENTRY_SIZE;
    SMCNode* node = (SMCNode*)(SMC_CACHE_BASE + index * SMC_ENTRY_SIZE);

    // If the cache entry isn't linked into the LRU list yet, skip to avoid crash.
    if (node->next == NULL || node->prev == NULL)
        return node;

    // Call the original function for the normal path (pass original arg1).
    void* ret;
    ASM_CALL(RETURN(ret), (gfx_module_addr + 0x0002bcf0u), 0, EAX(arg1));
    return ret;
}

#undef SMC_CACHE_BASE
#undef SMC_ENTRY_SIZE


// Called when the game loaded the renderer DLL
void window_rendered() {

    // Patch the function that creates the window
    patch_call(gfx_module_addr + 0x00012d69, (unsigned int)R_CreateWindow);

    // Fix crash in R_UsedCachedStaticModelSurface when node->next/prev is NULL
    // (ACCESS_VIOLATION at gfx+0x0002bd23, call site at gfx+0x0001f53f, since 1.4.3.2)
    patch_call(gfx_module_addr + 0x0001f53f, (unsigned int)hook_R_UsedCachedStaticModelSurface);

    // Change flags of r_fullscreen cvar - removed original DVAR_ROM flag - now user can change the value
    patch_int32(gfx_module_addr + 0x000ba43 + 1, (DVAR_ARCHIVE | DVAR_LATCH | DVAR_CHANGEABLE_RESET | DVAR_RENDERER));

}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void window_init() {
    // We need to register cvars here to fix the issue caused by registering cvars in renderer that are also referenced in CoD2MP_s.exe
    // Cvars that get first registered in renderer DLL have strings allocated in renderer DLL, so when vid_restart is called, the renderer DLL is unloaded and the strings are freed.
    // So we need to register the cvars in this DLL where the strings will be allocated for the whole time of the game.
    vid_xpos = Dvar_RegisterInt("vid_xpos", 3, -100000, 100000, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    vid_ypos = Dvar_RegisterInt("vid_ypos", 22, -100000, 100000, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    r_fullscreen = Dvar_RegisterBool("r_fullscreen", true, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_LATCH | DVAR_CHANGEABLE_RESET));
    r_autopriority = Dvar_RegisterBool("r_autopriority", false, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_CHANGEABLE_RESET));
    
    m_enable = Dvar_RegisterBool("m_enable", true, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
    m_debug = Dvar_RegisterBool("m_debug", false, (enum dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void window_patch() {
    // Hook the game window procedure
    patch_int32(0x004663D1 + 4, (unsigned int)&CoD2WindowProc);

    // Hook calls of functions processing mouse movement
    patch_call(0x0040932e, (unsigned int)&Mouse_Loop);
    patch_call(0x0040f787, (unsigned int)&Mouse_Loop);
    patch_call(0x0043512e, (unsigned int)&Mouse_Loop);
}