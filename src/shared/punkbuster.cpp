#include "punkbuster.h"

#include "shared.h"

// PunkBuster is dead (no master servers), but the game still loads pb\pbcl.dll / pb\pbsv.dll (Linux: pb/pbsv.so)
// and calls into them every frame. Its DLLs crash the game, so CoD2x never loads them.
//
// The DLLs are loaded only by stPbCl::LoadClientDll / stPbSv::LoadServerDll. Every caller does
// "if (loaded) call PB; else if (!LoadDll()) call PB;" and ignores the returned error string, so the game
// behaves exactly as if the pb folder was missing:
// - PbClientInitialize / PbServerInitialize (WinMain) call set_cl_punkbuster("0") / set_sv_punkbuster("0"),
//   so cl_punkbuster and sv_punkbuster are always 0 (both are DVAR_NOWRITE, the menu or config can not change them)
// - pb_* commands, PB packets, setPbClStatus and the per-frame processing only try to load the DLL again
// - the PB function pointers checked for NULL (PbPassConnectString, PbAuthClient, ...) stay NULL


// 004bb730    char const* stPbCl_LoadClientDll(void* pbcl @ eax)
const char* stPbCl_LoadClientDll()
{
    // CoD2x: never load pb\pbcl.dll, fail like a missing DLL (the error string is not printed by any caller)
    return "PB Error: Client DLL Load Failure";

    // if (!pbcl->hLibModule) {
    //     ...rename pbclnew.dll -> pbcl.dll -> pbclold.dll...
    //     pbcl->hLibModule = LoadLibraryA(pbcl->makefn("pbcl.dll"));
    //     if (!pbcl->hLibModule)
    //         return "PB Error: Client DLL Load Failure";
    //     ...GetProcAddress x2...
    // }
    // return NULL;
    // CoD2x: End
}

// 004bd3e0    char const* stPbSv_LoadServerDll(void* pbsv @ eax)
// 0813ca88    char const* stPbSv_LoadServerDll(void* pbsv)
const char* stPbSv_LoadServerDll()
{
    // CoD2x: never load pb\pbsv.dll (Linux: pb/pbsv.so), fail like a missing DLL (the error string is not printed by any caller)
    return "PB Error: Server DLL Load Failure";

    // if (!pbsv->hLibModule) {
    //     ...rename pbsvnew.dll -> pbsv.dll -> pbsvold.dll...
    //     pbsv->hLibModule = LoadLibraryA(pbsv->makefn("pbsv.dll"));   // Linux: dlopen("pbsv.so")
    //     if (!pbsv->hLibModule)
    //         return "PB Error: Server DLL Load Failure";
    //     ...GetProcAddress x2...
    // }
    // return NULL;
    // CoD2x: End
}


void punkbuster_patch()
{
    // Replace the whole loaders, the pbcl / pbsv state is passed in eax on Windows and on the stack on Linux,
    // both functions ignore it, so the same cdecl function without arguments fits every caller
    patch_jump(ADDR(0x004bd3e0, 0x0813ca88), (unsigned int)stPbSv_LoadServerDll);

    #if COD2X_WIN32
        patch_jump(0x004bb730, (unsigned int)stPbCl_LoadClientDll);

        // UI_RunMenuScript "JoinServer": a server that reports sv_punkbuster 1 while cl_punkbuster is 0 opens
        // joinpb_popmenu ("enable PunkBuster?") instead of connecting - PB can not be enabled anymore, so connect
        patch_byte(0x0053593f, 0xeb); // 7521 jne 0x535962 => EB21 jmp 0x535962 (if (LAN_GetServerPunkBuster(...) != 1))
    #endif
}
