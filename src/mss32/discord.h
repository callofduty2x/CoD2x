#ifndef DISCORD_H
#define DISCORD_H

#include "discord_game_sdk.h"

/**
 * @brief Structure to store Discord application state and related components.
 *
 * This structure holds the pointers to the core Discord interface and the activity manager
 * for managing the Discord application's presence and activity.
 */
struct Application
{
    struct IDiscordCore *core;                /**< Pointer to the core Discord interface. */
    struct IDiscordActivityManager *activities; /**< Pointer to the activity manager interface. */
};

/**
 * @brief Callback function to handle the result of updating the activity status.
 *
 * This function is invoked when the activity update request is completed.
 * It can be used to process the success or failure of the activity update.
 *
 * @param data Pointer to any additional data passed with the callback.
 * @param result The result of the activity update (success or failure).
 */
void UpdateActivityCallback(void *data, enum EDiscordResult result);

/**
 * @brief Sets the activity status for the Discord application.
 *
 * This function is used to set the current activity status, including information 
 * like game name, state, and other relevant details, to be shown in the Discord client.
 *
 * @param discord A pointer to the Application structure, which holds Discord-related components.
 */
void SetActivity(struct Application *discord);

/**
 * @brief Initializes the Discord integration.
 *
 * This function initializes the core Discord SDK, sets up the necessary components, 
 * and prepares the application for interacting with Discord features, like activity management.
 */
void discord_init();

/**
 * @brief Main loop for running Discord-related tasks.
 *
 * This function runs the necessary loops or tasks required for continuous Discord activity updates 
 * and presence management, ensuring the integration stays active.
 */
void discord_frame();


/**
 * @brief Starts the specified number of threads for handling Discord-related tasks.
 *
 * This function spawns multiple threads to handle various Discord tasks in parallel, 
 * such as updating activity or processing Discord events.
 *
 * @param numThreads The number of threads to be created for Discord processing.
 */
void StartThreads(int numThreads);

/**
 * @brief Checks if Discord is currently running.
 *
 * This function checks if the Discord client or process is active and running on the system.
 *
 * @return `true` if Discord is running, `false` otherwise.
 */
bool is_discord_running();

/**
 * @brief Updates the Discord presence with the latest information.
 *
 * This function is used to update the application's presence status on Discord, 
 * such as the game being played or the activity being done.
 */
void update_presence();

#endif // DISCORD_H
