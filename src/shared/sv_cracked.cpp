#include "sv_cracked.h"

// Include system headers
#include <iostream>
#include <string>

// Include internal project headers, shared first
#include "shared.h"
#include "cod2_dvars.h"
#include "sv_cracked.h"

// Define external variables
dvar_t *sv_cracked;

/**
 * Hook function for modifying the authorization state.
 * 
 * @param arg The index of the argument passed to the command.
 * @return A string that represents the new authorization state.
 *
 * This function checks if the server is cracked (sv_cracked) and if the 
 * passed argument is "deny", it will return "accept". Otherwise, it will 
 * return the original argument.
 */
const char *hook_AuthorizeState(int arg)
{
    const char *s = Cmd_Argv(arg);

    // If the server is cracked and the argument is "deny", return "accept"
    if (sv_cracked->value.boolean && strcmp(s, "deny") == 0)
        return "accept";

    return s;
}

/**
 * Called every frame at the start of the frame.
 * 
 * This function can be used to perform actions on every frame, but it is currently empty.
 */
void sv_cracked_frame()
{
}

/**
 * Called once at the start of the game, after common initialization.
 * Used to initialize variables, cvars, and other necessary game state.
 */
void sv_cracked_init()
{
    // Register the "sv_cracked" cvar with different flags depending on the iteration
    for (int i = 0; i <= 1; i++)
    {
        dvarFlags_e flags = i == 0 ? (dvarFlags_e)(DVAR_LATCH | DVAR_CHANGEABLE_RESET) : // allow the value to be changed via cmd when starting the game
                                (dvarFlags_e)(DVAR_ROM | DVAR_CHANGEABLE_RESET);         // then make it read-only to avoid changes

        sv_cracked = Dvar_RegisterBool("sv_cracked", false, flags);
    }

    // The patch needs to be called here because the dvar needs to be initialized before the hooking.
    sv_cracked_patch();
}

/**
 * Called before the main entry point is executed. Used to patch memory 
 * addresses and set up any necessary hooks.
 */
void sv_cracked_patch()
{
    // Patch the function call at the specified address to hook "hook_AuthorizeState"
    patch_call(ADDR(0x0045375b, 0x0808db12), (unsigned int)hook_AuthorizeState); // SV_IPAuthorize

#if COD2X_WIN32
    // If the server is cracked, perform additional patching
    if (sv_cracked->value.boolean)
    {
        patch_nop(0x0453836, 6);  // Disable certain instructions by replacing them with no-op
        Com_Printf("sv_cracked = 1\n");
    }
    else
    {
        Com_Printf("sv_cracked = 0\n");
    }
#endif
}
