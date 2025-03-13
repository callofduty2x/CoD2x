#include "steam.h"
#include "shared.h"
#include "../shared/common.h"

dvar_t *x_steamid64;

void steam_init()
{
    char steamID64Str[64];
    snprintf(steamID64Str, sizeof(steamID64Str), "%s", "");
    x_steamid64 = Dvar_RegisterString("x_steamid64", steamID64Str, (dvarFlags_e)(DVAR_USERINFO | DVAR_NOWRITE)); 

    if (SteamAPI_Init())
    {
        snprintf(steamID64Str, sizeof(steamID64Str), "%llu", SteamAPI_ISteamUser_GetSteamID(SteamUser()));
        Dvar_SetString(x_steamid64, steamID64Str);
        
        //SteamAPI_Shutdown();
    }
}