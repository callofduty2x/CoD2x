#ifndef COD2_H
#define COD2_H

#include "shared.h"
#include "cod2_dvars.h"
#include <stdio.h>


/*
__asm__ (
    "assembly_code"            // The actual assembly code
    : output_operands           // Output constraints
    : input_operands            // Input constraints
    : clobbers                  // Clobbered registers or flags
);

Common constraint types:
    r: Use a general-purpose register.
    m: Use memory (the variable's address in memory).
    i: Use an immediate value.
    =r: Writable register for output.
    +r: A register that's both an input and an output.

Clobbers specify which registers, memory, or flags are modified by the assembly code but are not part of the inputs or outputs. 
The compiler uses this information to avoid using those resources for other purposes.

If the assembly code modifies global memory (e.g., via pointers), add "memory" to the clobber list to tell the compiler that memory content has changed:

*/


/*
    Helper macros for inline assembly
*/

#define ASM(op, ...) ASM__##op(__VA_ARGS__)

#define ASM__push(arg) \
    __asm__ volatile("push %0\n" : : "m"(arg) : "memory")

#define ASM__mov(dest, src) \
    __asm__ volatile("movl %0, %%" dest "\n" : : "m"(src) : "memory", dest)

#define ASM__call(func) \
    __asm__ volatile("call *%0\n" : : "m"(func) : "memory")

#define ASM__add(dest, imm) \
    __asm__ volatile("addl %0, %%" dest "\n" : : "i"(imm) : "memory", dest)

#define ASM__add_esp(bytes) \
    __asm__ volatile("addl %0, %%esp\n" : : "i"(bytes) : "memory")

#define ASM__movr(dest, src) \
    __asm__ volatile("movl %%" src ", %0\n" : "=m"(dest) : : "memory", src)





enum errorParm_e
{
    // Show message box, close window, show console window
	ERR_FATAL = 0x0,
    // Show ingame error
	ERR_DROP = 0x1,
	ERR_SERVERDISCONNECT = 0x2,
	ERR_DISCONNECT = 0x3,
	ERR_SCRIPT = 0x4,
	ERR_SCRIPT_DROP = 0x5,
	ERR_LOCALIZATION = 0x6,
	ERR_MAPLOADERRORSUMMARY = 0x7,
};

enum netadrtype_e
{
    NA_BOT = 0x0,
    NA_BAD = 0x1,
    NA_LOOPBACK = 0x2,
    NA_BROADCAST = 0x3,
    NA_IP = 0x4,
    NA_IPX = 0x5,
    NA_BROADCAST_IPX = 0x6
};

struct netaddr_s
{
    enum netadrtype_e type;
    uint8_t ip[0x4];
    uint16_t port;
    int ipx_data;
    int ipx_data2;
};


// https://github.com/id-Software/Enemy-Territory/blob/40342a9e3690cb5b627a433d4d5cbf30e3c57698/src/game/q_shared.h#L1621
enum clientState_e{
	CLIENT_STATE_DISCONNECTED,    // not talking to a server
	CLIENT_STATE_CINEMATIC,       // playing a cinematic or a static pic, not connected to a server
	CLIENT_STATE_AUTHORIZING,     // not used any more, was checking cd key
	CLIENT_STATE_CONNECTING,      // sending request packets to the server
	CLIENT_STATE_CHALLENGING,     // sending challenge packets to the server
	CLIENT_STATE_CONNECTED,       // netchan_t established, getting gamestate
	CLIENT_STATE_LOADING,         // only during cgame initialization, never during main loop
	CLIENT_STATE_PRIMED,          // got gamestate, waiting for first frame
	CLIENT_STATE_ACTIVE,          // game views should be displayed       
};

inline const char* get_client_state_name(int state) {
    switch (state) {
        case CLIENT_STATE_DISCONNECTED: return "DISCONNECTED";
        case CLIENT_STATE_CINEMATIC: return "CINEMATIC";
        case CLIENT_STATE_AUTHORIZING: return "AUTHORIZING";
        case CLIENT_STATE_CONNECTING: return "CONNECTING";
        case CLIENT_STATE_CHALLENGING: return "CHALLENGING";
        case CLIENT_STATE_CONNECTED: return "CONNECTED";
        case CLIENT_STATE_LOADING: return "LOADING";
        case CLIENT_STATE_PRIMED: return "PRIMED";
        case CLIENT_STATE_ACTIVE: return "ACTIVE";
        default: return "UNKNOWN";
    }
}


/**
 * Show error with level, for example ERR_LEVEL
 * Example:
 *  Com_Error(ERR_DROP, "Error message %s %i", "test", 1337);
 */
inline void Com_Error(enum errorParm_e type, const char* format, ...) {
    va_list va;
    va_start(va, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);
    ((void(__cdecl*)(int, const char*))(ADDR(0x004324c0, 0x08061124)))(type, buffer);
}

/**
 * Print message to console
 * Example:
 *  Com_Printf("Hello %s %i\n", "world", 1337);
 */
inline int Com_Printf(const char* format, ...) {
    va_list va;
    va_start(va, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);
    return ((int(__cdecl*)(const char*))(ADDR(0x00431ee0, 0x08060dea)))(buffer);
}

/**
 * Print message to console when developer mode is enabled
 * Example:
 *  Com_DPrintf("Hello %s %i\n", "world", 1337);
 */
inline int Com_DPrintf(const char* format, ...) {
    va_list va;
    va_start(va, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);
    return ((int(__cdecl*)(const char*))(ADDR(0x00431f30, 0x08060e3a)))(buffer);
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






#define va ((char* (*)(const char *, ...))ADDR(0x0044a990, 0x080b7fa6))




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




inline short BigShort(short l) {
    void* func = *((void**)ADDR(0x00c93bc4, 0x085bc800));
    short ret = (*(short(__cdecl*)(short))func)(l);
    return ret;
}

// Send UDP packet to a server
inline int NET_OutOfBandPrint(const char* msg, int mode, struct netaddr_s addr) {
    #if COD2X_WIN32
        const void* original_func = (void*)0x00448910;
        int result;
        ASM ( push,  addr.ipx_data2 );
        ASM ( push,  addr.ipx_data  );
        ASM ( push,  addr.port      );
        ASM ( push,  addr.ip        );
        ASM ( push,  addr.type      );
        ASM ( push,  mode           );
        ASM ( mov,   "eax", msg     );
        ASM ( call,  original_func  );
        ASM ( add_esp, 24           ); // Clean up the stack (6 arguments × 4 bytes = 24)
        ASM ( movr,  result, "eax"  ); // Store the return value in the 'result' variable
        return result;
    #endif
    #if COD2X_LINUX
        return ((int (*)(int, int, int, int, int, int, const char*))0x0806c8cc)(mode, addr.type, *(int*)&addr.ip, addr.port, addr.ipx_data, addr.ipx_data2, msg);
    #endif
}


// Send UDP packet to a server
inline int NET_OutOfBandData(const char* msg, int len, int mode, struct netaddr_s addr) {
    #if COD2X_WIN32
        const void* original_func = (void*)0x00448a70;
        int result;
        ASM ( push,  addr.ipx_data2 );
        ASM ( push,  addr.ipx_data  );
        ASM ( push,  addr.port      );
        ASM ( push,  addr.ip        );
        ASM ( push,  addr.type      );
        ASM ( push,  mode           );
        ASM ( mov,   "ecx", len     );
        ASM ( mov,   "eax", msg     );
        ASM ( call,  original_func  );
        ASM ( add_esp, 28           ); // Clean up the stack (7 arguments × 4 bytes = 28)
        ASM ( movr,  result, "eax"  ); // Store the return value in the 'result' variable
        return result;
    #endif
    #if COD2X_LINUX
        // not used
        exit(EXIT_FAILURE);
    #endif
}


inline int NET_StringToAdr(const char *updateServerUri, struct netaddr_s* a) {
    #if COD2X_WIN32
        const void* original_func = (void*)0x00448d50;
        int result;
        ASM( mov,   "eax", updateServerUri  );
        ASM( mov,   "ebx", a                );
        ASM( call,  original_func           );
        ASM( movr,  result, "eax"           ); // Store the return value in the 'result' variable
        return result;
    #endif
    #if COD2X_LINUX
        return ((int32_t (*)(const char *, struct netaddr_s*))0x0806cd98)(updateServerUri, a);
    #endif
}

#if COD2X_WIN32
    inline int Sys_IsLANAddress(struct netaddr_s addr) {
        return ((int (*)(int, int, unsigned int, int, int))0x00467100)(addr.type, (int)addr.ip, addr.port, addr.ipx_data, addr.ipx_data2);
    }
#endif





inline void Info_SetValueForKey(const char* buffer, const char* keyName, const char* value) {  
    #if COD2X_WIN32
        const void* original_func = (void*)0x0044ae10; // (char* buffer, char* keyName @ eax, int32_t value)
        ASM( push,     value          ); // 3th argument
        ASM( push,     buffer         ); // 2rd argument
        ASM( mov,      "eax", keyName ); // 1nd argument
        ASM( call,     original_func  );
        ASM( add_esp,  8              ); // Clean up the stack (2 arguments × 4 bytes = 8)
    #endif
    #if COD2X_LINUX
        ((char* (*)(const char* buffer, const char* keyName, const char* value))0x080b85ce)(buffer, keyName, value);
    #endif
}


inline char* Info_ValueForKey(const char* buffer, const char* keyName) {   
    #if COD2X_WIN32
        const void* original_func = (void*)(0x0044aa90); // char* Info_ValueForKey(char* buffer @ ecx, char* key)
        char* result;
        ASM( push,     keyName          ); // 2nd argument                    
        ASM( mov,      "ecx", buffer    ); // 1st argument
        ASM( call,     original_func    ); 
        ASM( add_esp,  4                ); // Clean up the stack (1 argument × 4 bytes = 4)     
        ASM( movr,     result, "eax"    ); // Store the return value in the 'result' variable
        return result;
    #endif
    #if COD2X_LINUX
        return ((char* (*)(const char* buffer, const char* keyName))0x080b8108)(buffer, keyName);
    #endif
}



#define SV_CMD_CAN_IGNORE 0
#define SV_CMD_RELIABLE 1

// Sends a command string to a client
inline void SV_GameSendServerCommand( int clientNum, int svscmd_type, const char *text )
{
    #if COD2X_WIN32

        #define sv_maxclients (*(dvar_t **)(0x00d52810))
        #define SV_SendServerCommand       ((void (__cdecl*)())(0x000127d0))

        if (clientNum < -1 || clientNum >= sv_maxclients->value.integer) 
            return;
        
        void* client = (clientNum == -1) ? NULL : (void*)((uint32_t)(*((void**)(0x00d3570c))) + (0xb1064 * clientNum));
        const char* ps = "%s";
        
        const void* original_func = (void*)(0x0045a670); // void* SV_SendServerCommand(int32_t* cl @ eax, int32_t type, char* msg, ...)
        ASM( push,     text             ); // 4nd argument                    
        ASM( push,     ps               ); // 3nd argument                    
        ASM( push,     svscmd_type      ); // 2nd argument                    
        ASM( mov,      "eax", client    ); // 1st argument
        ASM( call,     original_func    ); 
        ASM( add_esp,  12               ); // Clean up the stack (3 argument × 4 bytes = 12)     
        
    #endif
    #if COD2X_LINUX 
        ((void (*)(int, int, const char *))0x080917aa)(clientNum, svscmd_type, text);
    #endif
};

// Set cvar on client side
inline void SV_SetClientCvar(int clientNum, const char *cvarName, const char *cvarValue) {
    SV_GameSendServerCommand(clientNum, SV_CMD_RELIABLE, va("%c %s \"%s\"", 118, cvarName, cvarValue));
}


// Hooked version of SV_GetUserInfo function
inline char *SV_GetUserInfo_Hook(int index, char *buffer, int bufferSize)
{
#if COD2X_WIN32
    // Address of the original function in the binary (found using Binary Ninja)
    const void *original_func = (void *)0x004580b0;
    // Call the original function, passing the same parameters as the original
    ASM(mov, "edi", bufferSize); // Push bufferSize onto the stack
    ASM(mov, "ebx", buffer);     // Push the buffer pointer onto the stack
    ASM(mov, "eax", index);      // Push the client index onto the stack
    ASM(call, original_func);    // Call the original function

    return buffer; // Retorna o buffer modificado
#endif
#if COD2X_LINUX
    // Hook for the Linux version of the original function
    ((void (*)(int, char *, int))0x08092C04)(index, buffer, bufferSize);
#endif
}

typedef void (*Cmd_AddCommand_t)(const char *command, void (*func)());

#if COD2X_LINUX
extern Cmd_AddCommand_t Cmd_AddCommand;
#endif

#if COD2X_WIN32
extern Cmd_AddCommand_t Cmd_AddCommand;
#endif



#endif