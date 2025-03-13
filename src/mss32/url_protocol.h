#ifndef COD2_PROTOCOL_H
#define COD2_PROTOCOL_H

#include <windows.h>

#define PROTOCOL_NAME "cod2"
#define REG_COD2_PATH "SOFTWARE\\WOW6432Node\\Activision\\Call of Duty 2"
#define REG_COD2_KEY "MultiEXEString"

/**
 * @brief Gets the path of the Call of Duty 2 executable from the Windows Registry.
 * @param exe_path Buffer to store the executable path.
 * @param size Size of the buffer.
 * @return 1 if the path was found successfully, 0 otherwise.
 */
int get_cod2_exe_path(char *exe_path, DWORD size);

/**
 * @brief Checks if the `cod2://` protocol is already registered in Windows.
 * @return 1 if the protocol exists, 0 otherwise.
 */
int check_protocol_exists();

/**
 * @brief Registers the `cod2://` protocol in Windows, associating it with the Call of Duty 2 executable.
 * @param exe_path Path to the game's executable.
 * @return 1 if the protocol was registered successfully, 0 otherwise.
 */
int register_protocol(const char *exe_path);

/**
 * @brief Initializes the `cod2://` protocol, registering it if it is not already in the system.
 * @return 0 on success, 1 on error.
 */
int url_protocol_init();

void url_connect();


#endif // COD2_PROTOCOL_H
