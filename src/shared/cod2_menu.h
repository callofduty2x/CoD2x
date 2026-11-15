#ifndef COD2_MENU_H
#define COD2_MENU_H

// UI menus of the Windows client (menuDef / itemDef / displayContextDef of Quake 3 - the Windows build has no names).
// Only the fields CoD2x reads, by offset. Windows only.

#include <stdint.h>

#include "assembly.h"


// ======================================================================================================
// Display context
// ======================================================================================================

// Display context of the UI menus (the HUD menus have their own)
#define ui_displayContext           (*((char**)0x005dcb10))
#define DISPLAYCONTEXT_REALTIME(dc) (*((int32_t*)((dc) + 0x4)))     // time of the frame in ms
#define DISPLAYCONTEXT_CURSOR_X(dc) (*((int32_t*)((dc) + 0xc)))     // cursor in units of the 640x480 screen
#define DISPLAYCONTEXT_CURSOR_Y(dc) (*((int32_t*)((dc) + 0x10)))
#define DISPLAYCONTEXT_MENUS(dc)        ((char**)((dc) + 0x2c))             // all loaded menus
#define DISPLAYCONTEXT_MENUCOUNT(dc)    (*((int32_t*)((dc) + 0x22c)))       // Q3 Menu_Count()
#define DISPLAYCONTEXT_MENUSTACK(dc)    ((char**)((dc) + 0x230))            // open menus, the last one is on top
#define DISPLAYCONTEXT_OPENMENUCOUNT(dc) (*((int32_t*)((dc) + 0x270)))

// Scale of the cursor (units of the 640x480 screen) to real screen pixels, x and y - the item hit test (0053a460) multiplies the cursor by it
#define scrPlace_scaleVirtualToFull     ((float*)0x00c94c10)


// ======================================================================================================
// Window header - menuDef and itemDef start with it
// ======================================================================================================

#define WINDOWDEF_RECT(w)           ((float*)(w))                                       // x, y, w, h in units of the 640x480 screen
#define WINDOWDEF_HORZALIGN(w)      (*((int32_t*)((char*)(w) + 0x10)))                  // horizontalAlign_e of the rect
#define WINDOWDEF_VERTALIGN(w)      (*((int32_t*)((char*)(w) + 0x14)))                  // verticalAlign_e of the rect
#define WINDOWDEF_NAME(w)           (*((const char**)((char*)(w) + 0xc0)))              // "name", NULL if none
#define WINDOWDEF_FLAGS(w)          (*((int32_t*)((char*)(w) + 0xe8)))                  // WINDOW_*

#define WINDOW_HASFOCUS             0x2
#define WINDOW_VISIBLE              0x4


// ======================================================================================================
// Menu (menuDef) and menu item (itemDef)
// ======================================================================================================

// Menu item (itemDef)
#define ITEMDEF_TEXT(item)          (*((const char**)((char*)(item) + 0x294)))  // "text", "@KEY" = localized string
#define ITEMDEF_PARENT(item)        (*((char**)((char*)(item) + 0x29c)))        // menu of the item
#define ITEMDEF_MOUSEENTER(item)    (*((const char**)((char*)(item) + 0x2a8)))  // script "mouseEnter"
#define ITEMDEF_ACTION(item)        (*((const char**)((char*)(item) + 0x2b0)))  // script "action"
#define ITEMDEF_DVAR(item)          (*((const char**)((char*)(item) + 0x2c0)))  // name of the dvar, "dvar"
#define ITEMDEF_CVARFLAGS(item)     (*((int32_t*)((char*)(item) + 0x2d0)))      // CVAR_* conditions of the item

// Dvar conditions of an item (ITEMDEF_CVARFLAGS), checked with Item_EnableShowViaCvar
#define CVAR_ENABLE                 0x1     // "enableDvar"
#define CVAR_DISABLE                0x2     // "disableDvar"
#define CVAR_SHOW                   0x4     // "showDvar"
#define CVAR_HIDE                   0x8     // "hideDvar"

// Menu (menuDef)
#define MENUDEF_ITEMCOUNT(menu)     (*((int32_t*)((menu) + 0x218)))
#define MENUDEF_FOCUSCOLOR(menu)    ((const float*)((menu) + 0x25c))            // "focusColor"
#define MENUDEF_ITEMS(menu)         (*((char***)((menu) + 0x27c)))


// ======================================================================================================
// Keys of the menus
// ======================================================================================================

#define K_PGDN                      163
#define K_PGUP                      164
#define K_HOME                      165
#define K_END                       166
#define K_MOUSE1                    200
#define K_MWHEELDOWN                205
#define K_MWHEELUP                  206


// ======================================================================================================
// Functions
// ======================================================================================================

/**
 * Returns the menu that gets the keys: the last opened menu of the display context that is visible and has the focus
 * (a popup over the main menu, not the main menu), NULL when no menu is open.
 */
// 00542120    void* Menu_GetFocused(void* dc)
inline void* Menu_GetFocused(void* dc) {
    void* menu;
    ASM_CALL(RETURN(menu), 0x00542120, 1, PUSH(dc));
    return menu;
}

/**
 * Runs a script of a menu item (the text of its action, mouseEnter, onFocus, ...): the commands separated by ';' are
 * looked up in the command list of the menus (play, open, close, setdvar, exec, ...) and executed with the item,
 * an unknown command is passed to the uiScript handler. Does nothing when the item or the script is NULL or empty.
 * dc is the display context of the menu (UI or HUD). Named after Quake 3, the function has no name in the Windows build.
 */
// 0053bd30    void Item_RunScript(void* dc, void* item, char* script)
inline void Item_RunScript(void* dc, void* item, const char* script) {
    ASM_CALL(RETURN_VOID, 0x0053bd30, 3, PUSH(dc), PUSH(item), PUSH(script));
}

/**
 * Checks the dvar condition of a menu item (ITEMDEF_CVARFLAGS): flag CVAR_ENABLE = enableDvar / disableDvar (the item reacts to
 * the mouse), CVAR_SHOW = showDvar / hideDvar (the item is drawn). Returns nonzero if the condition allows it. Call only when the
 * item has such a condition (CVAR_ENABLE | CVAR_DISABLE, CVAR_SHOW | CVAR_HIDE), like the item hit test Menu_HandleMouseMove
 * (00542330) does. Named after Quake 3, the function has no name in the Windows build.
 */
// 00546c50    int32_t Item_EnableShowViaCvar(struct itemDef_t* item, int32_t flag)
inline int Item_EnableShowViaCvar(void* item, int flag) {
    ASM_CALL_RETURN(int, 0x00546c50, 2, PUSH(item), PUSH(flag));
}

/**
 * Hover of the menus at the cursor x y (units of the 640x480 screen): the open menus of the display context, the focused one
 * first, update the item under the cursor (focus, mouseEnter / mouseExit scripts). With an item, moves that item instead.
 * Called by UI_MouseEvent (00538490) when a menu is loaded. Named after Quake 3, the function has no name in the Windows build.
 */
// 00542d60    void Display_MouseMove(void* dc, void* item, int32_t x, int32_t y)
inline void Display_MouseMove(void* dc, void* item, int32_t x, int32_t y) {
    ASM_CALL(RETURN_VOID, 0x00542d60, 4, PUSH(dc), PUSH(item), PUSH(x), PUSH(y));
}

#endif
