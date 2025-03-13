/*
 * hwid.cpp - Hardware ID (HWID) Management System
 *
 * This module provides functions to generate and manage HWIDs based on system components such as CPU ID and Hard Drive Serial.
 * It also includes hashing functions to generate unique identifiers.
 */
#include "hwid.h"

// Include system headers
#include <iostream>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <iphlpapi.h>
#include <netioapi.h>
#include <assert.h>
#include <cpuid.h>
#include <wincrypt.h>    // For MD5 encryption
#include <openssl/sha.h> // For SHA-256 hashing

// Include internal project headers
#include "shared.h"

// Define external variables
dvar_t *x_hwid;

/**
 * Generates a SHA-256 hash from the given input string.
 *
 * @param str The input string to hash.
 * @param output The output buffer to store the hash result.
 */
void generate_hash(const char *str, unsigned char *output)
{
    SHA256_CTX sha256_ctx;
    SHA256_Init(&sha256_ctx);
    SHA256_Update(&sha256_ctx, str, strlen(str));
    SHA256_Final(output, &sha256_ctx);
}

/**
 * Generates a unique 12-character key based on the SHA-256 hash of an input string.
 *
 * @param str The input string to hash.
 * @param output_key The output buffer to store the generated key.
 */
void generate_unique_key(const char *str, char *output_key)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    generate_hash(str, hash);

    for (int i = 0; i < 12; i++)
    {
        sprintf(&output_key[i * 2], "%02x", hash[i % SHA256_DIGEST_LENGTH]);
    }
    output_key[12] = '\0';
}

/**
 * Retrieves the serial number of the system's hard drive.
 *
 * @return A pointer to the serial number string or NULL if it fails.
 */
char *getHardDriveSerial()
{
    static char serialNumber[BUFFER_SIZE] = {0};
    DWORD serialNumberDWORD = 0;

    if (GetVolumeInformation("C:\\", NULL, 0, &serialNumberDWORD, NULL, NULL, NULL, 0))
    {
        snprintf(serialNumber, BUFFER_SIZE, "%08lX", serialNumberDWORD);
        return serialNumber;
    }
    else
    {
        return NULL;
    }
}

/**
 * Retrieves the CPU ID of the system.
 *
 * @return A pointer to the CPU ID string.
 */
char *getCPUId()
{
    static char cpuId[BUFFER_SIZE] = {0};
    unsigned int cpuInfo[4] = {0};

    __get_cpuid(0, &cpuInfo[0], &cpuInfo[1], &cpuInfo[2], &cpuInfo[3]);
    snprintf(cpuId, BUFFER_SIZE, "%X%X%X%X", cpuInfo[0], cpuInfo[1], cpuInfo[2], cpuInfo[3]);

    return cpuId;
}

/**
 * Generates a unique Hardware ID (HWID) based on the system's hard drive serial number and CPU ID.
 *
 * @return A pointer to the generated HWID string.
 */
char *generateHWID()
{
    static char hwid[13] = {0};
    char *hardDriveSerial = getHardDriveSerial();
    char *cpuId = getCPUId();

    if (hardDriveSerial && cpuId)
    {
        char combined[BUFFER_SIZE];
        snprintf(combined, sizeof(combined), "%s-%s", hardDriveSerial, cpuId);
        generate_unique_key(combined, hwid);
    }
    else
    {
        snprintf(hwid, sizeof(hwid), "ERROR");
    }

    return hwid;
}

/** Called every frame at the start of the frame. */
void hwid_frame()
{
}

/** Called once at game start after common initialization. Used to initialize variables, cvars, etc. */
void hwid_init()
{
    x_hwid = Dvar_RegisterString("x_hwid", generateHWID(), (dvarFlags_e)(DVAR_USERINFO | DVAR_NOWRITE));
}

/** Called before the entry point is executed. Used to patch memory. */
void hwid_patch()
{
}