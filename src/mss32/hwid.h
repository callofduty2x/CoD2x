/*
 * hwid.h - Header file for Hardware ID (HWID) Management System
 *
 * This module provides function declarations for generating and managing HWIDs based on system components
 * such as CPU ID and Hard Drive Serial. It also includes hashing functions to generate unique identifiers.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>

#define BUFFER_SIZE 256 // Buffer size for strings and hash storage

/**
 * Generates a SHA-256 hash from the given input string.
 *
 * @param str The input string to hash.
 * @param output The output buffer to store the hash result.
 */
void generate_hash(const char *str, unsigned char *output);

/**
 * Generates a unique 12-character key based on the SHA-256 hash of an input string.
 *
 * @param str The input string to hash.
 * @param output_key The output buffer to store the generated key.
 */
void generate_unique_key(const char *str, char *output_key);

/**
 * Retrieves the serial number of the system's hard drive.
 *
 * @return A pointer to the serial number string or NULL if it fails.
 */
char *getHardDriveSerial();

/**
 * Retrieves the CPU ID of the system.
 *
 * @return A pointer to the CPU ID string.
 */
char *getCPUId();

/**
 * Generates a unique Hardware ID (HWID) based on the system's hard drive serial number and CPU ID.
 *
 * @return A pointer to the generated HWID string.
 */
char *generateHWID();

/** Called every frame at the start of the frame. */
void hwid_frame();

/** Called once at game start after common initialization. Used to initialize variables, cvars, etc. */
void hwid_init();

/** Called before the entry point is executed. Used to patch memory. */
void hwid_patch();
