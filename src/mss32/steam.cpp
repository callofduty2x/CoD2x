#include "steam.h"
#include <cstring>
#include <cstdio>
#include <cassert>
#include "game.h"
#include "shared.h"
#include "../shared/common.h"
#include <tlhelp32.h>
#include <stdbool.h>
#include <stdio.h>

dvar_t *g_steamid64 = NULL;

// Inicializa o Discord e define a atividade
void steam_init()
{
    if (SteamAPI_Init())
    {
        // MessageBoxA(NULL, "SteamAPI_Init() error", "Error", MB_ICONERROR | MB_OK);
        // MessageBoxA(NULL, "SteamAPI_Init()", "Error", MB_ICONERROR | MB_OK);
        // const char *steamName = SteamFriends()->GetPersonaName();
        // MessageBoxA(NULL, steamName, "Steam username", MB_ICONERROR | MB_OK);

        char steamID64Str[64];
        snprintf(steamID64Str, sizeof(steamID64Str), "%llu", SteamAPI_ISteamUser_GetSteamID(SteamUser()));

        g_steamid64 = Dvar_RegisterString("steamid64", steamID64Str, (dvarFlags_e)(DVAR_NOWRITE));

        SteamAPI_Shutdown();
    }
}