#include "discord.h"
#include <cstring>
#include <cstdio>
#include <cassert>
#include "game.h"
#include "shared.h"
#include "../shared/common.h"
#include <tlhelp32.h>
#include <stdbool.h>
#include <stdio.h>
#include "../shared/cod2.h"

// Discord SDK requires success result to proceed
#define DISCORD_REQUIRE(x) assert(x == DiscordResult_Ok)

// Global variables for client state and server data
#define clientState (*((clientState_e *)0x00609fe0)) // Client state, possibly controlling game behavior

#define svr_players ((int *)0x001518F80) // Pointer to the number of players on the server
#define clc_stringData ((PCHAR)0x0096FD5C) // Pointer to client string data
#define clc_stringOffsets ((PINT)0x0096DD5C) // Pointer to offsets in client string data

#define cs0 (clc_stringData + clc_stringOffsets[0]) 
#define cs1 (clc_stringData + clc_stringOffsets[1]) 

static struct Application discord; // Discord application instance
static bool discordInitialized = false; // Flag to track if Discord is initialized
EDiscordResult discordResult = DiscordResult_NotRunning; // Holds the result of Discord operations

// Structure for holding activity data such as game details, map, and players
struct ActivityData
{
    char details[256]; // Game details to be displayed on Discord
    char state[256]; // Game state to be displayed on Discord
    char largeImage[256]; // Image name for large image on Discord
    char map[128]; // Current map name
    char gametype[128]; // Game type (e.g., TDM, FFA)
    char hostname[128]; // Server hostname
};

// Global instance of the ActivityData structure
ActivityData ActivityData;

/**
 * @brief Callback function to update Discord activity.
 * 
 * This function will be called when the Discord activity update result is received. 
 * It checks if the result is successful and then proceeds.
 * 
 * @param data The data passed to the callback (not used in this function).
 * @param result The result of the Discord activity update operation.
 */
void UpdateActivityCallback(void *data, enum EDiscordResult result)
{
    if (result != DiscordResult_Ok)
    {
        return; // Simply return if the update fails instead of terminating
    }
}

/**
 * @brief Sets the activity data on Discord.
 * 
 * This function updates the current activity data (e.g., game details, state, etc.)
 * on Discord, which will be displayed on the user's profile.
 * 
 * @param discord Pointer to the Discord application instance.
 */
void SetActivity(struct Application *discord)
{
    if (!discord || !discord->activities)
    {
        return; // Ensure Discord and activity manager are initialized
    }

    struct DiscordActivity activity;
    memset(&activity, 0, sizeof(activity)); // Clear memory for the activity structure

    // Setting activity details and status for Discord
    sprintf(activity.name, "Call of Duty 2 X"); // Name of the game
    sprintf(activity.details, ActivityData.details); // Details about the game state
    sprintf(activity.assets.large_image, "cod2_fw"); // Large image (game image)
    sprintf(activity.state, ActivityData.state); // Current game state (e.g., "Players Online")

    // Update the activity on Discord
    if (discordResult == DiscordResult_Ok)
    {
        discord->activities->update_activity(discord->activities, &activity, discord, UpdateActivityCallback);
    }
    else
    {
        return; // If Discord is not initialized or not running, skip activity update
    }
}

/**
 * @brief Initializes the Discord application instance.
 * 
 * This function creates and initializes the Discord application instance and prepares 
 * the activity manager. If the initialization is successful, Discord is set up to 
 * update the activity.
 */
void discord_init()
{
    if (!discordInitialized)
    {
        memset(&discord, 0, sizeof(discord)); // Clear the discord instance structure

        // Create and initialize the Discord instance
        struct DiscordCreateParams params;
        DiscordCreateParamsSetDefault(&params);
        params.client_id = 1345907074638417991; // Replace with your actual Discord client ID
        params.flags = DiscordCreateFlags_NoRequireDiscord;
        discordResult = DiscordCreate(DISCORD_VERSION, &params, &discord.core); // Create the Discord instance

        if (discordResult == DiscordResult_Ok)
        {
            discord.activities = discord.core->get_activity_manager(discord.core); // Retrieve activity manager
            if (!discord.activities)
            {
                return;
            }
            discordInitialized = true; // Mark Discord as initialized
        }
    }
}

/**
 * @brief Loops and updates the Discord activity based on the game state.
 * 
 * This function checks the current client state (e.g., disconnected, connected, etc.) 
 * and updates the details, map, and other activity information accordingly. It then 
 * updates the Discord activity.
 */
void discord_frame()
{
    // Update the large image and default details for Discord
    snprintf(ActivityData.largeImage, sizeof(ActivityData.largeImage), "cod2_fw");

    // Get the current game server information (hostname, map, gametype)
    strcpy(ActivityData.hostname, Com_CleanHostnameColors(Info_ValueForKey(cs0, "sv_hostname")));
    strcpy(ActivityData.map, Com_CleanMapName(Info_ValueForKey(cs0, "mapname")));
    strcpy(ActivityData.gametype, Info_ValueForKey(cs0, "g_gametype"));

    // Update the state based on the client state
    switch (clientState)
    {
    case CLIENT_STATE_DISCONNECTED:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Looking to play");
        update_presence(); // Update the presence on Discord
        break;
    case CLIENT_STATE_CINEMATIC:
        memset(ActivityData.details, 0, sizeof(ActivityData.details)); // Clear details for cinematic state
        update_presence();
        break;
    case CLIENT_STATE_AUTHORIZING:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Authorizing server");
        update_presence();
        break;
    case CLIENT_STATE_CONNECTING:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Connecting to a server");
        update_presence();
        break;
    case CLIENT_STATE_CHALLENGING:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Challenging with a server");
        update_presence();
        break;
    case CLIENT_STATE_CONNECTED:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Connected to a server");
        update_presence();
        break;
    case CLIENT_STATE_LOADING:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Loading to a server");
        update_presence();
        break;
    case CLIENT_STATE_PRIMED:
        memset(ActivityData.details, 0, sizeof(ActivityData.details)); // Clear details for primed state
        break;
    case CLIENT_STATE_ACTIVE:
        snprintf(ActivityData.details, sizeof(ActivityData.details), "Playing %s (%s) on %s", ActivityData.map, ActivityData.gametype, ActivityData.hostname);
        snprintf(ActivityData.state, sizeof(ActivityData.largeImage), "Players Online: %d/%s\n", *svr_players, Info_ValueForKey(cs0, "sv_maxclients"));
        update_presence();
        break;
    default:
        memset(ActivityData.details, 0, sizeof(ActivityData.details)); // Clear details for unknown state
        break;
    }
}

/**
 * @brief Updates the Discord presence based on the current game state.
 * 
 * This function checks the Discord status and activity initialization. If Discord 
 * is initialized and running, it updates the activity. If Discord is not initialized, 
 * it creates a new instance.
 */
void update_presence()
{
    if (discordResult == DiscordResult_Ok && discordInitialized)
    {
        SetActivity(&discord); // Set the activity on Discord
        discord.core->run_callbacks(discord.core); // Run Discord callbacks to update the status
    }
    else
    {
        discord_init(); // Create a new Discord instance if not already initialized
    }
}
