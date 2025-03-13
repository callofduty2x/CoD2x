#include "server.h"
#include "shared.h"
#include "common.h"

dvar_t* sv_masterPort;
dvar_t* sv_masterServer;

#define sv_masterAddress (*((netaddr_s*)( ADDR(0x019a6fe8, 0x0849fbe0) )))




void SV_DirectConnect(netadrtype_e type, int32_t ip, uint32_t port, int32_t ipx1, int32_t ipx2)
{
    Com_DPrintf("SV_DirectConnect(ip = %i.%i.%i.%i, port: %i)\n", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, port);

    netaddr_s addr;
    addr.type = type;
    *(int*)&addr.ip = ip;
    addr.port = port;
    addr.ipx_data = ipx1;
    addr.ipx_data2 = ipx2;

    const char *infoKeyValue = Cmd_Argv(1);

    char str[1024];
    strncpy(str, infoKeyValue, 1023);
    str[1023] = '\0';

    int32_t protocolNum = atol(Info_ValueForKey(str, "protocol"));

    if (protocolNum != 118)
    {   
        const char* msg;
        switch (protocolNum)
        {
            case 115: msg = "Your version is 1.0\nYou need version 1.3 and CoD2x"; break;
            case 116: msg = "Your version is 1.1\nYou need version 1.3 and CoD2x"; break;
            case 117: msg = "Your version is 1.2\nYou need version 1.3 and CoD2x"; break;
            default: msg = "Your CoD2 version is unknown"; break;
        }
        NET_OutOfBandPrint(va("error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s%s\n\x00", APP_VERSION "\n\n", msg), 1, addr);
        Com_DPrintf("    rejected connect from protocol version %i (should be %i)\n", protocolNum, 118);
        return;
    }

    int32_t cod2xNum = atol(Info_ValueForKey(str, "protocol_cod2x"));

    // CoD2x is not installed on 1.3 client
    if (cod2xNum == 0) {
        NET_OutOfBandPrint(va(
            "error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s\n\x00", 
            APP_VERSION "\n\n" "Download CoD2x at:\n" APP_URL ""), 
        1, addr);
        Com_DPrintf("    rejected connect from non-CoD2x version\n");
        return;
    }

    // CoD2x is installed but the version is different
    if (cod2xNum < APP_VERSION_PROTOCOL) { // Older client can not connect newer server
        const char* msg = va(
            "error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s\n\x00", 
            APP_VERSION "\n\n" "Update CoD2x to version " APP_VERSION " or above");
        NET_OutOfBandPrint(msg, 1, addr);
        Com_DPrintf("    rejected connect from CoD2x version %i (should be %i)\n", cod2xNum, APP_VERSION_PROTOCOL);
        return;
    }

    // Call the original function
    ((void (*)(int32_t type, int32_t ip, int32_t port, int32_t ipx1, int32_t ipx2))ADDR(0x00453c20, 0x0808e2aa))(type, ip, port, ipx1, ipx2);
}




// Function called when a client fully connects to the server, original function calls "begin" to gsc script
// Its called on client connection and on map_restart (even on soft restart on next round)
void SV_ClientBegin(int clientNum) {
    
    // Set client cvar g_cod2x
    // This will ensure that the same client side bug fixes are applied
    SV_SetClientCvar(clientNum, "g_cod2x", TOSTRING(APP_VERSION_PROTOCOL));

}

void SV_ClientBegin_Win32() {
    // Get num in eax
    int clientNum;
    ASM( movr, clientNum, "eax" );
    // Call the original function
    const void* original_func = (void*)0x004fe460;
    ASM( mov, "eax", clientNum );
    ASM( call, original_func );
    // Call our function
    SV_ClientBegin(clientNum);
}

void SV_ClientBegin_Linux(int clientNum) {
    // Call the original function
    ((void (*)(int))0x080f90ae)(clientNum);
    // Call our function
    SV_ClientBegin(clientNum);
}





// Called when the server is started via /map or /devmap, or /map_restart
void SV_SpawnServer(char* mapname) {

    // Fix animation time from crouch to stand
    common_fix_clip_bug(true);

    // Call the original function
    ((void (*)(char* mapname))ADDR(0x00458a40, 0x08093520))(mapname);
}




netaddr_s * custom_SV_MasterAddress(void)
{
	if ( sv_masterAddress.type == NA_BOT )
	{
		Com_Printf("Resolving %s\n", sv_masterServer->value.string);
		if ( !NET_StringToAdr(sv_masterServer->value.string, &sv_masterAddress) )
		{
			Com_Printf("Couldn't resolve address: %s\n", sv_masterServer->value.string);
		}
		else
		{
			if ( !strstr(":", sv_masterServer->value.string) )
			{
				sv_masterAddress.port = BigShort(sv_masterPort->value.integer);
			}
			Com_Printf("%s resolved to %i.%i.%i.%i:%i\n",
						sv_masterServer->value.string,
						sv_masterAddress.ip[0],
						sv_masterAddress.ip[1],
						sv_masterAddress.ip[2],
						sv_masterAddress.ip[3],
						BigShort(sv_masterAddress.port));
		}
	}
	return &sv_masterAddress;
}


// Called after all is initialized on game start
void server_init()
{
    for (int i = 0; i <= 1; i++)
    {
        dvarFlags_e flags = i == 0 ? 
            (dvarFlags_e)(DVAR_LATCH | DVAR_CHANGEABLE_RESET) : // allow the value to be changed via cmd when starting the game
            (dvarFlags_e)(DVAR_ROM | DVAR_CHANGEABLE_RESET);    // then make it read-only to avoid changes

        sv_masterServer = Dvar_RegisterString("sv_masterServer", "cod2master.activision.com", flags);
        sv_masterPort = Dvar_RegisterInt("sv_masterPort", 20710, 0, 65535, flags);
    }

    #if COD2X_WIN32
        // After the cvars are loaded, change the master server address
        patch_string_ptr(0x004b3fa5 + 1, sv_masterServer->value.string);
        patch_int32(0x004b3fb4 + 1, sv_masterPort->value.integer);

        // Change the protocol used when requesting list of servers from master server
        //patch_byte(0x00539727 + 1, PROTOCOL_VERSION);
        patch_byte(0x00539727 + 1, 118);
    #endif

    hwid_server_init();
}




// Server side hooks
// The hooked functions are the same for both Windows and Linux
void server_patch()
{
    // Patch the protocol version check - now its being done in SV_DirectConnect
    patch_byte(ADDR(0x00453ce3, 0x0808e34d), 0xeb); // from '7467 je' to 'eb67 jmp'

    // Remove the Com_DPrintf("SV_DirectConnect()\n") call - now its being done in SV_DirectConnect
    patch_nop(ADDR(0x00453c87, 0x0808e2f7), 5);

    // Hook the SV_DirectConnect function
    patch_call(ADDR(0x0045b8cd, 0x08095d6c), (unsigned int)SV_DirectConnect);


    // Hook the SV_SpawnServer function
    patch_call(ADDR(0x00451c7f, 0x0808be22), (unsigned int)SV_SpawnServer); // map / devmap
    patch_call(ADDR(0x00451f1c, 0x0808befa), (unsigned int)SV_SpawnServer); // map_restart


    // Hook the SV_ClientBegin function
    patch_call(ADDR(0x00454d12, 0x0808f6ee), (unsigned int)ADDR(SV_ClientBegin_Win32, SV_ClientBegin_Linux));

    
    patch_call(ADDR(0x00452c13, 0x0808cf46), (unsigned int)custom_SV_MasterAddress); // in SV_UpdateLastTimeMasterServerCommunicated
    patch_call(ADDR(0x00453038, 0x0808d44b), (unsigned int)custom_SV_MasterAddress); // in SV_GetChallenge
    patch_call(ADDR(0x004b888f, 0x08096f14), (unsigned int)custom_SV_MasterAddress); // in SV_MasterHeartbeat
    patch_call(ADDR(0x004b88f6, 0x08096f94), (unsigned int)custom_SV_MasterAddress); // in SV_MasterHeartbeat
    patch_call(ADDR(0x004b8940, 0x08096fea), (unsigned int)custom_SV_MasterAddress); // in SV_MasterGameCompleteStatus




    // Fix "+smoke" bug
    // When player holds smoke or grenade button but its not available, the player will be able to shoot
    // The problem is that when holding the frag/smoke key, the server sends an EV_EMPTY_OFFHAND event every client frame. 
    // These events are buffered into an array of 4 items, overwriting any older events that were previously buffered. 
    // Since the server frame is slower than the client frame, with sv_fps set to 30, snaps to 30, and cl_maxpackets to 100, 
    // there are approximately 8 events the server might want to send to other players, but only 4 of them can actually 
    // be sent through the network.

    #if COD2X_WIN32
        // replace jmp to ret (5 bytes)
        // orig: 004f4f5f  e99cfeffff         jmp     PM_SendEmtpyOffhandEvent
        // new:  004f4f5f  c390909090         ret
        patch_copy(0x004f4f5f, (void*)"\xc3\x90\x90\x90\x90", 5); 
    #endif
    #if COD2X_LINUX
        patch_nop(0x080efe12, 5); // remove call to PM_SendEmtpyOffhandEvent
    #endif
}