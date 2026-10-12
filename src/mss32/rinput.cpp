#include "rinput.h"

#include "shared.h"
#include "../shared/cod2_dvars.h"


dvar_t*             m_rinput;
dvar_t*             m_rinput_hz;
dvar_t*             m_rinput_hz_max;

CRITICAL_SECTION    rinput_lock;
long                rinput_x = 0;
long                rinput_y = 0;
long                rinput_cnt = 0;
long                rinput_activated = 0;
LPVOID              rinput_error = NULL;
HWND                rinput_window_hwnd = NULL;
DWORD               rinput_thread_id = 0;
HANDLE              rinput_thread_handle = NULL;

// Variables for measuring input rate
#define SAMPLE_COUNT 10  // 10 intervals of 100ms = 1 second
long                rinput_inputSamples[SAMPLE_COUNT] = {0};  // Store last 10 measurements
int                 rinput_sampleIndex = 0;
long                rinput_totalInputCount = 0;  // Rolling sum of last 10 updates
long                rinput_lastTotalCount = 0;  // Tracks total input messages seen so far
LONGLONG            rinput_frequency = 0;

#define win_hwnd    (*((HWND*)0x00d7713c))



void rinput_wm_input(LPARAM lParam) {
    UINT uiSize = 40;
    static unsigned char lpb[40];

    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &uiSize, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
        RAWINPUT* raw = (RAWINPUT*)lpb;

        if (raw->header.dwType == RIM_TYPEMOUSE) {
            // Extract raw mouse data
            int deltaX = raw->data.mouse.lLastX;
            int deltaY = raw->data.mouse.lLastY;

            // Offsets for the mouse movement in loop function
            EnterCriticalSection(&rinput_lock);          
            rinput_x += deltaX;
            rinput_y += deltaY;
            rinput_cnt++;      
            LeaveCriticalSection(&rinput_lock);
        }
    }
}


LRESULT __stdcall rinput_window_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_INPUT: {
            rinput_wm_input(lParam);
			break;
		}
		default: return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}


// RInput has own dedicated thread to handle raw input messages
DWORD WINAPI rinput_thread(LPVOID lpParam) {
    thread_setName(GetCurrentThread(), L"CoD2x RawInput");

    // Register the window class
    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(WNDCLASSEX));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = rinput_window_proc;
    wcex.lpszClassName = "CoD2_Rinput";

    if (!RegisterClassEx(&wcex)) {
        InterlockedExchangePointer(&rinput_error, (PVOID)"Failed to register raw input device.\nFailed to register input class!");
        return 1;
    }

    // Create a hidden "fake" window
    rinput_window_hwnd = CreateWindowEx(0, "CoD2_Rinput", "CoD2_Rinput", 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (!rinput_window_hwnd) {
        UnregisterClassA("CoD2_Rinput", NULL);
        InterlockedExchangePointer(&rinput_error, (PVOID)"Failed to register raw input device.\nFailed to create input window!");
        return 1;
    }

    // Register raw input for this window
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;  // Generic desktop controls
    rid.usUsage = 0x02;      // Mouse
    rid.dwFlags = 0;         // Default behavior (foreground capture)
    rid.hwndTarget = rinput_window_hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        DestroyWindow(rinput_window_hwnd);
        rinput_window_hwnd = NULL;
        UnregisterClassA("CoD2_Rinput", NULL);
        InterlockedExchangePointer(&rinput_error, (PVOID)"Failed to register raw input device.");
        return 1;
    }

    InterlockedExchange(&rinput_activated, 1);

    // Message loop (dedicated to raw input)
    MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) {
            break;
        }
		DispatchMessage(&msg);
    }

    // Destroy window and unregister class
    DestroyWindow(rinput_window_hwnd);
    rinput_window_hwnd = NULL;
    UnregisterClassA("CoD2_Rinput", NULL);

    return 0;
}




void rinput_register() {

    if (m_rinput->value.integer == m_rinput->latchedValue.integer && m_rinput->value.integer > 0) {
        return;
    }

    rinput_reset_offset();

    // Detection of Rinput software
    // If the game is already running with Rinput, don't allow to register another instance
    HWND hwndRInput = FindWindow("Rinput", NULL);
    if (hwndRInput != NULL)
    {
        Dvar_SetBool(m_rinput, false);
        Com_Error(ERR_DROP, "Couldn't register raw input device.\nAnother instance of raw input is already running\n\nm_rinput was set to 0.");
        return;
    }

    // Method 1: Dedicated thread for raw input messages
    if (m_rinput->latchedValue.integer == 1) {
        InterlockedExchange(&rinput_activated, 0);
        InterlockedExchangePointer(&rinput_error, NULL);
        rinput_thread_handle = CreateThread(NULL, 0, rinput_thread, NULL, 0, &rinput_thread_id);

        // Wait for thread to activate, thread safely
        while (true) {
            Sleep(1);
            LPVOID error = InterlockedCompareExchangePointer(&rinput_error, NULL, NULL);
            if (error) {
                Dvar_SetBool(m_rinput, false);
                Com_Error(ERR_DROP, (const char*)error);
                return;
            }
            if (InterlockedCompareExchange(&rinput_activated, 0, 0) == 1) {
                break;
            }
        }

        Com_Printf("Registered raw input device (dedicated thread method)\n");


    // Method 2: Directly in the main game window
    } else if (m_rinput->latchedValue.integer == 2) {

        // Register raw input for the game window
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;  // Generic desktop controls
        rid.usUsage = 0x02;      // Mouse
        rid.dwFlags = 0;         // Default behavior (foreground capture)
        rid.hwndTarget = win_hwnd;

        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            Com_Error(ERR_DROP, "Failed to register raw input device.");
            return;
        }

        Com_Printf("Registered raw input device (main window method)\n");
    }
}

void rinput_unregister() {

    if (m_rinput->value.integer == 0) {
        return;
    }

    // Method 1: Dedicated thread for raw input messages
    if (m_rinput->value.integer == 1) {
        // Post a quit message to break the message loop
        PostMessage(rinput_window_hwnd, WM_QUIT, 0, 0);

        // Wait for thread to exit
        WaitForSingleObject(rinput_thread_handle, INFINITE);

        CloseHandle(rinput_thread_handle);
        rinput_thread_handle = NULL;

        Com_Printf("Unregistered raw input device (dedicated thread method)\n");


    // Method 2: Directly in the main game window
    } else if (m_rinput->value.integer == 2) {
        
        // Unregister raw input for the game window
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;  // Generic desktop controls
        rid.usUsage = 0x02;      // Mouse
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = win_hwnd;

        RegisterRawInputDevices(&rid, 1, sizeof(rid));

        Com_Printf("Unregistered raw input device (main window method)\n");
    }
}


void rinput_on_main_window_create() {
    // Keep latched and change current to 0 to detect a change in frame loop
    m_rinput->value.integer = 0;
}

void rinput_on_main_window_destory() {
    // In method 2 the main window is tight to the raw input
    // Since the HWND changed (due to vid_restart) the raw input is automatically unregistered
    // We need to also kill external thread in method 1
    if (m_rinput->value.integer > 0) {
        rinput_unregister();
        m_rinput->latchedValue.integer = m_rinput->value.integer;
        m_rinput->value.integer = 0;
    }
}


bool rinput_is_enabled() {
    return m_rinput->value.integer > 0;
}

void rinput_get_last_offset(long* x, long* y) {
    EnterCriticalSection(&rinput_lock);          
    *x = rinput_x;
    *y = rinput_y;
    rinput_x = 0;
    rinput_y = 0;
    LeaveCriticalSection(&rinput_lock);
}

void rinput_reset_offset() {
    EnterCriticalSection(&rinput_lock);          
    rinput_x = 0;
    rinput_y = 0;
    LeaveCriticalSection(&rinput_lock);
}


void rinput_mouse_loop() {
    // If the raw input cvar has been modified
    if (m_rinput->latchedValue.integer != m_rinput->value.integer) {

        Dvar_SetInt(m_rinput_hz, 0);
        Dvar_SetInt(m_rinput_hz_max, 0);
        
        // Unregister first
        rinput_unregister();

        if (m_rinput->latchedValue.integer > 0) {
            rinput_register();
        }

        Dvar_SetInt(m_rinput, m_rinput->latchedValue.integer);
    }

    if (m_rinput->value.integer > 0) {

        static LONGLONG lastUpdateTime = 0;

        // Get current time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        long elapsedTime = (now.QuadPart - lastUpdateTime) * 1000 / rinput_frequency;

        // Check if 100ms have passed
        if (elapsedTime >= 100) {
            EnterCriticalSection(&rinput_lock);

            // Get the new raw input messages received since last update
            long newMessages = rinput_cnt - rinput_lastTotalCount;
            rinput_lastTotalCount = rinput_cnt;  // Update tracker

            // Subtract the oldest sample from the total and add the latest one
            rinput_totalInputCount -= rinput_inputSamples[rinput_sampleIndex];
            rinput_inputSamples[rinput_sampleIndex] = newMessages;  // Store new sample
            rinput_totalInputCount += newMessages;  // Update total

            // Move to next sample slot
            rinput_sampleIndex = (rinput_sampleIndex + 1) % SAMPLE_COUNT;

            // If no movement is detected, clear the buffer
            if (newMessages == 0) {
                rinput_totalInputCount = 0;  // Force zero to stop displaying old values
                memset(rinput_inputSamples, 0, sizeof(rinput_inputSamples));  // Clear stored samples
            }

            LeaveCriticalSection(&rinput_lock);

            // Display the updated input rate
            Dvar_SetInt(m_rinput_hz, rinput_totalInputCount);

            // Update max input rate
            if (rinput_totalInputCount > m_rinput_hz_max->value.integer) {
                Dvar_SetInt(m_rinput_hz_max, rinput_totalInputCount);
            }

            lastUpdateTime = now.QuadPart;
        }
    }
}


    

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void rinput_init() {
    m_rinput = Dvar_RegisterInt("m_rinput", 0, 0, 2, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_LATCH | DVAR_CHANGEABLE_RESET));

    m_rinput_hz = Dvar_RegisterInt("m_rinput_hz", 0, 0, INT32_MAX, (enum dvarFlags_e)(DVAR_ROM | DVAR_CHANGEABLE_RESET));
    m_rinput_hz_max = Dvar_RegisterInt("m_rinput_hz_max", 0, 0, INT32_MAX, (enum dvarFlags_e)(DVAR_ROM | DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void rinput_patch() {
    InitializeCriticalSection(&rinput_lock);

    // Initialize time measurement
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    rinput_frequency = freq.QuadPart;
    QueryPerformanceCounter(&now);
}