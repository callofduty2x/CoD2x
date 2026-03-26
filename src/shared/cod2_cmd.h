#ifndef COD2_CMD_H
#define COD2_CMD_H

#include "shared.h"
#include "assembly.h"


#define cmd_argc (*(int*)ADDR(0x00b1a480, 0x0819f100))
#define cmd_argv ((char**)ADDR(0x00b17a80, 0x0819f180))

// Get command argument count
inline int Cmd_Argc( void )
{
	return cmd_argc;
}

// Get command argument at index
inline const char *Cmd_Argv( int arg )
{
	if ( arg >= cmd_argc )
	{
		return "";
	}

	return cmd_argv[arg];
}

/**
 * Adds command to execute. Text is added at the end of the buffer. Must contain a newline.
 * Example:
 * Cbuf_AddText("quit\n");
 * Cbuf_AddText("exec %s\n", "config_mp.cfg");
 * https://github.com/id-Software/Enemy-Territory/blob/40342a9e3690cb5b627a433d4d5cbf30e3c57698/src/qcommon/cmd.c#L94
 */
inline void Cbuf_AddText(const char* text) {
    #if COD2X_WIN32
        const void* original_func = (void*)0x00420ad0;
        ASM( mov,   "eax", text     );
        ASM( call,  original_func   );
    #endif
    #if COD2X_LINUX
        ((void(__cdecl*)(const char*))(0x0805fd0a))(text);
    #endif
}

/** Immediately executes a command string. */
inline void Cmd_ExecuteString(const char *text){
    ASM_CALL(RETURN_VOID, ADDR(0x004214c0, 0x080608a6), 1, PUSH(text));
}

inline void Cmd_TokenizeString(const char *text_in){
    #if COD2X_WIN32
        const void* original_func = (void*)0x00421270;
        ASM( mov,   "ecx", text_in      ); 
        ASM( call,  original_func       );
    #endif
    #if COD2X_LINUX
        ((void*(__cdecl*)(const char*))(0x0806064c))(text_in);
    #endif
}


// Register a new command
inline void Cmd_AddCommand(const char *command, void (*func)()) {
    #if COD2X_WIN32
        ((void(*)(const char *, void (*)()))0x004212f0)(command, func);
    #endif
    #if COD2X_LINUX
        ((void(*)(const char *, void (*)()))0x80606b6)(command, func);
    #endif
}


// Register a new command
inline void Cmd_RemoveCommand(const char *command) {
    #if COD2X_WIN32
        ((void(*)(const char *))0x00421370)(command);
    #endif
    #if COD2X_LINUX
        ((void(*)(const char *))0x0806072e)(command);
    #endif
}




#endif