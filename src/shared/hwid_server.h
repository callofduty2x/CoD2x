/*
 * hwid.h - HWID Ban and Management System Header
 *
 * This header file provides function declarations for managing HWID bans.
 */

 #ifndef HWID_H
 #define HWID_H
 
 #include <cstdio>
 
 /**
  * Checks if a given HWID is banned.
  *
  * @param hwid The HWID to check.
  * @return 1 if the HWID is banned, 0 otherwise.
  */
 int is_hwid_banned(const char *hwid);
 
 /**
  * Bans a given HWID by adding it to the ban list.
  *
  * @param hwid The HWID to ban.
  */
 void ban_hwid(const char *hwid);
 
 /**
  * Command function to ban a user based on client number.
  */
 void AddCommand_Ban();
 
 /**
  * Command function to kick a user based on client number.
  */
 void AddCommand_Kick();
 
 /**
  * Called every frame at the start of the frame.
  */
 void hwid_server_frame();
 
 /**
  * Called once at game start after common initialization.
  * Used to initialize variables, cvars, etc.
  */
 void hwid_server_init();
 
 /**
  * Called before the entry point is executed.
  * Used to patch memory.
  */
 void hwid_server_patch();
 
 #endif // HWID_H
 