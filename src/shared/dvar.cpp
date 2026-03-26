#include "dvar.h"

#include "shared.h"
#include "cod2_dvars.h"
#include "cod2_common.h"
#include "cod2_shared.h"
#include "cod2_cmd.h"


#define MAX_DVARS 4096 // original 1280
dvar_t dvarPool[MAX_DVARS];



/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void dvar_init() {

    #if DEBUG
        Cmd_AddCommand("dvarTestLimit", []() {

            int maxDvars = MAX_DVARS;

            if (Cmd_Argc() == 2) {
                maxDvars = atoi(Cmd_Argv(1));
                if (maxDvars < 1 || maxDvars > MAX_DVARS) {
                    Com_Printf("Invalid argument, must be between 1 and %i\n", MAX_DVARS);
                    return;
                }
            }

            static char dvarNameStorage[MAX_DVARS][32];

            for (int i = 0; i < maxDvars; i++) {
                // Format the name into the static buffer
                snprintf(dvarNameStorage[i], sizeof(dvarNameStorage[i]), "ui_testDvar%i", i);
                const char* name = dvarNameStorage[i];

                Dvar_RegisterInt(name, 0, 0, 1, DVAR_CHANGEABLE_RESET);

                auto a = Dvar_GetDvarByName(name);

                if (a == NULL) {
                    Com_Error(ERR_DROP, "Failed to register dvar '%s' at index %i\n", name, i);
                    break;
                }
            }
        });
    #endif
}

/** Called before the entry point is called. Used to patch the memory. */
void dvar_patch() {

    // 00c5c9d0  struct dvar_s dvarPool[0x500] - windows   d0c9c500
    // 085abe20  struct dvar_s dvarPool[0x500] - linux     20be5a08

    // Dvar_CreateDvar
    // 00437e30  8d34b5d0c9c500     lea     esi, [esi*4+0xc5c9d0]
    // 080b3cc3  b820be5a08         mov     eax, dvarPool
    patch_int32(ADDR(0x00437e30 + 3, 0x080b3cc3 + 1), (int32_t)&dvarPool);

    // 004392ab  b8d0c9c500         mov     eax, dvarPool
    // 080b5180  c745fc20be5a08     mov     dword [ebp-0x4 {next}], dvarPool
    patch_int32(ADDR(0x004392ab + 1, 0x080b5180 + 3), (int32_t)&dvarPool);

    if (COD2X_WIN32) {
        // 0045898b  b8d0c9c500         mov     eax, dvarPool
        patch_int32(0x0045898b + 1, (int32_t)&dvarPool);

        // 00458e22  b8d0c9c500         mov     eax, dvarPool
        patch_int32(0x00458e22 + 1, (int32_t)&dvarPool);
    }

    
    WL(
        patch_int32(0x00437df3 + 1, MAX_DVARS);,    // 00437df3  3d00050000            cmp     eax, 0x500
        patch_int32(0x080b3c74 + 6, MAX_DVARS - 1); // 080b3c74  813d08be5a08ff040000  cmp     dword [dvar_count], 0x4ff
    )
    WL(
        patch_int32(0x00437e09 + 1, MAX_DVARS);, // 00437e09  6800050000         push    0x500
        patch_int32(0x080b3c8c + 4, MAX_DVARS);  // 080b3c8c  c744240c00050000   mov     dword [esp+0xc {var_30}], 0x500
    )

    // Improve error message when too many dvars are registered
    patch_string_ptr(ADDR(0x00437e0f + 1, 0x080b3c9b + 4), "Error while registering cvar '%s'.\nUnable to create more than %i dvars.\n\n"
        "There is too many cvars in your config!\nClean your config from unused dvars and try again.\n\n"
        "Normal config should contains no more than 400 lines of dvars. Compare your config with a default one to find the differences.");

}
