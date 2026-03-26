#include "game.h"

#include "shared.h"
#include "../shared/cod2_dvars.h"

dvar_t* g_cod2x = NULL;

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void game_init() {

    // Register shared cvar between client and server
    // Server cannot change this value
    // Client can change this value to apply differened fixes according to connected server version
    g_cod2x = Dvar_RegisterInt("g_cod2x", 0, 0, APP_VERSION_PROTOCOL, (dvarFlags_e)(DEBUG_RELEASE(DVAR_NOFLAG, DVAR_NOWRITE) | DVAR_SYSTEMINFO));

    // Force server to use the new protocol version
    if (COD2X_LINUX || dedicated->value.integer > 0) {
        g_cod2x->limits.integer.min = APP_VERSION_PROTOCOL;
        g_cod2x->limits.integer.max = APP_VERSION_PROTOCOL;
        Dvar_SetInt(g_cod2x, APP_VERSION_PROTOCOL);
        g_cod2x->modified = false;
    }
}

/** Called before the entry point is called. Used to patch the memory. */
void game_patch()
{

}