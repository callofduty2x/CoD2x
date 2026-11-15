#ifndef WINDOW_H
#define WINDOW_H

#include <windows.h>

void Mouse_SetMenuCursorPos(int x, int y);

void window_frame();
void window_rendered();
void window_init();
void window_patch();

#endif