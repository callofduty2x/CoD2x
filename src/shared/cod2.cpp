#include "cod2.h"

#if COD2X_LINUX
Cmd_AddCommand_t Cmd_AddCommand = (Cmd_AddCommand_t)0x80606b6;
#endif

#if COD2X_WIN32
Cmd_AddCommand_t Cmd_AddCommand = (Cmd_AddCommand_t)0x004212f0;
#endif