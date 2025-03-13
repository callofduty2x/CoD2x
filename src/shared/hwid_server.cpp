/*
 * hwid.cpp - HWID Ban and Management System
 *
 * This module provides functions to check, add, and manage HWID bans in the game server.
 * It integrates with the server command system to allow banning and kicking players.
 */

#include "hwid_server.h"

// Include system headers
#include <iostream>
#include <string>
#include <cstdio>

// Include internal project headers
#include "shared.h"
#include "hwid_server.h"

// Define maximum string length
#define MAX_STRINGLENGTH 1024

/**
 * Checks if the given HWID is banned.
 *
 * @param hwid The HWID to check.
 * @return 1 if the HWID is banned, 0 otherwise.
 */
int is_hwid_banned(const char *hwid)
{
    FILE *file = fopen("ban.txt", "r");
    if (!file)
    {
        Com_Printf("[HWID] Error opening ban file.\n");
        return 0;
    }

    char line[128];
    while (fgets(line, sizeof(line), file))
    {
        // Remove newline characters
        line[strcspn(line, "\r\n")] = 0;

        if (strcmp(line, hwid) == 0)
        {
            fclose(file);
            return 1; // HWID is banned
        }
    }

    fclose(file);
    return 0; // HWID not found
}

/**
 * Bans a given HWID by adding it to the ban file.
 *
 * @param hwid The HWID to ban.
 */
void ban_hwid(const char *hwid)
{
    if (is_hwid_banned(hwid))
    {
        Com_Printf("[HWID] The HWID %s is already banned.\n", hwid);
        return;
    }

    FILE *file = fopen("ban.txt", "a");
    if (!file)
    {
        Com_Printf("[HWID] Error opening ban file for writing. Creating..\n");
        return;
    }

    fprintf(file, "%s\n", hwid);
    fclose(file);
    Com_Printf("[HWID] HWID %s successfully banned.\n", hwid);
}

/**
 * Command function to ban a user based on client number.
 */
void AddCommand_Ban()
{
    char userinfo[MAX_STRINGLENGTH];
    const char *xhwid;
    const char *clientNumStr = Cmd_Argv(1);

    if (clientNumStr && clientNumStr[0] != '\0')
    {
        Com_Printf("ban user\n");
        SV_GetUserInfo_Hook(atoi(clientNumStr), userinfo, MAX_STRINGLENGTH);

        xhwid = Info_ValueForKey(userinfo, "x_hwid");
        ban_hwid(xhwid);
    }
    else
    {
        Com_Printf("Usage: ban <client number>. Use 'status' command to get client number.\n");
    }
}

/**
 * Called every frame at the start of the frame.
 */
void hwid_server_frame()
{
}

/**
 * Called once at game start after common initialization. Used to initialize variables, cvars, etc.
 */
void hwid_server_init()
{
    Cmd_AddCommand("ban", AddCommand_Ban);
}
