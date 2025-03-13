#ifndef SV_CRACKED_H
#define SV_CRACKED_H

// Include necessary system headers
#include <iostream>
#include <string>

// External project headers
#include "shared.h"
#include "cod2_dvars.h"

// Declare external variables
extern dvar_t *sv_cracked;

/**
 * Hook function for modifying the authorization state. This function
 * is called during the authorization process to potentially modify the result.
 * 
 * @param arg The index of the argument passed to the authorization command.
 * @return A string representing the new authorization state.
 *
 * If the server is cracked (sv_cracked is set to true), and the argument
 * passed is "deny", the function returns "accept". Otherwise, it returns
 * the original argument value.
 */
const char *hook_AuthorizeState(int arg);

/**
 * Function called every frame at the start of the frame.
 * 
 * This function can be used to handle operations that need to be
 * performed each frame. As of now, it doesn't perform any actions.
 */
void sv_cracked_frame();

/**
 * Function that is called once during game initialization. This function
 * is used to register and initialize variables, cvars, and any other
 * necessary game-related state.
 */
void sv_cracked_init();

/**
 * Function that is called before the main entry point. Used for memory
 * patching and applying any necessary hooks.
 */
void sv_cracked_patch();

#endif // SV_CRACKED_H
