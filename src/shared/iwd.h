#ifndef IWD_H
#define IWD_H

#include "shared.h"

#if COD2X_WIN32
void iwd_beforeMapCommand();
void iwd_afterMapCommand();
#endif

void iwd_frame();
void iwd_init();
void iwd_patch();

#endif