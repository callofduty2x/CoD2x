#ifndef ERROR_H
#define ERROR_H

#include <windows.h>

int error_fatalMessageBox(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);

void error_init();
void error_patch();

#endif