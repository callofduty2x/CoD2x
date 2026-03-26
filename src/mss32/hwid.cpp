/*
 * hwid.cpp - Hardware ID (HWID) Management System
 *
 * This module provides functions to generate and manage HWIDs based on system components.
 * It now uses MD5 hashing to generate a unique identifier.
 */
#include "hwid.h"

#include <iostream>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <iphlpapi.h>
#include <netioapi.h>
#include <assert.h>
#include <cpuid.h>

#include <initguid.h>
#include <wbemidl.h>

#include "shared.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_client.h"

#define HWID_TESTING 0 // (SET TO 0 AFTER TEST) enable generating random HWID for testing purposes

dvar_t *cl_hwid;
dvar_t *cl_hwid2;

// Global flags and buffers
bool hwid_changed = false;               // Indicates if HWID changed since last check
char hwid_changed_diff[1024] = {};       // Stores the difference in HWID components
int  hwid_changed_count = 0;             // Counts how many times the HWID has changed
char hwid_old[33] = {};                  // Stores the previous HWID
char hwid_regid[33] = {};                // Stores the registry ID

// Global buffers to cache all WMI properties (shared by both generate functions)
#define MAX_WMI_BUFFER_SIZE 128
char hwid_bios_manufacturer[MAX_WMI_BUFFER_SIZE] = {};
char hwid_bios_serial[MAX_WMI_BUFFER_SIZE] = {};
char hwid_cpu_name[MAX_WMI_BUFFER_SIZE] = {};
char hwid_cpu_id[MAX_WMI_BUFFER_SIZE] = {};
char hwid_board_manufacturer[MAX_WMI_BUFFER_SIZE] = {};
char hwid_board_serial[MAX_WMI_BUFFER_SIZE] = {};
char hwid_uuid[MAX_WMI_BUFFER_SIZE] = {};
char hwid_disk_model[MAX_WMI_BUFFER_SIZE] = {};
char hwid_disk_serial[MAX_WMI_BUFFER_SIZE] = {};
char hwid_gpuName[MAX_WMI_BUFFER_SIZE] = {};

// WMI enumeration safety: avoid infinite blocking waits
static const ULONG WMI_NEXT_TIMEOUT_MS = 3000;      // per-iteration timeout for IEnumWbemClassObject::Next
static const int   WMI_NEXT_MAX_TRIES   = 3;        // total ~15s worst-case before giving up

extern bool registry_version_changed; // Flag to indicate if the version has changed
extern bool registry_version_downgrade; // Flag to indicate if the version has been downgraded
extern char registry_previous_version[64]; // Buffer to store the previous version

struct WMIProperty {
    const wchar_t *wmiClass;
    const wchar_t *property;
    char *buffer;
    size_t bufferSize;
};


/*
 * WMI is a Microsoft technology that provides a unified interface for querying system information,
 * including hardware details (like BIOS serial, CPU ID, motherboard UUID), operating system config,
 * and many other management data sources. It is built on top of COM (Component Object Model),
 * and exposes information through a set of namespaces, classes, and properties.
 *
 * Steps:
 *   - Initializes COM and connects to the default WMI namespace: "ROOT\\CIMV2"
 *   - Constructs and executes a WQL (WMI Query Language) query
 *   - Retrieves the first result (WMI object) returned by the query
 *   - Extracts the value of the specified property from that object
 *   - Converts the value (if it's a BSTR string) into a regular multibyte C string (char*) for use
 *
 * Note:
 *   - This function assumes the property is a VT_BSTR string (which is true for most hardware IDs)
 *   - This function uses COM objects and should be called after CoInitializeEx()
 *   - Caller is responsible for calling CoUninitialize() after all COM usage is complete
 */

/**
 * Helper to read a string property from a WMI query result
 */
static HRESULT hwid_WMI_readFirstString(IWbemServices* pServices, const wchar_t* wmiClass, const wchar_t* prop, char* out, size_t outSize)
{
    IEnumWbemClassObject* pEnumerator = NULL;
    IWbemClassObject* pObj = NULL;
    VARIANT vtProp;
    ULONG uReturned = 0;
    out[0] = '\0';

    wchar_t query[256];
    swprintf(query, sizeof(query)/sizeof(wchar_t), L"SELECT * FROM %ls", wmiClass);
    BSTR bstrWQL = SysAllocString(L"WQL");
    BSTR bstrQuery = SysAllocString(query);
    HRESULT hr = pServices->ExecQuery(bstrWQL,
                                      bstrQuery,
                                      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                      NULL, &pEnumerator);
    SysFreeString(bstrWQL);
    SysFreeString(bstrQuery);
    if (FAILED(hr))
        return hr;
    if (!pEnumerator)
        return E_FAIL;
    // Ensure enumerator proxy uses the same auth/impersonation as the service proxy, avoiding per-call negotiation that can hang.
    hr = CoSetProxyBlanket(pEnumerator, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) {
        pEnumerator->Release();
        return hr;
    }

    // Use bounded waits instead of WBEM_INFINITE to prevent hangs if a provider is unresponsive
    for (int tries = 0; tries < WMI_NEXT_MAX_TRIES; ++tries) {
        hr = pEnumerator->Next(WMI_NEXT_TIMEOUT_MS, 1, &pObj, &uReturned);
        if (hr == WBEM_S_TIMEDOUT) {
            if (tries + 1 >= WMI_NEXT_MAX_TRIES) {
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT); // convert to failing HRESULT
                break;
            }
            continue; // keep waiting until max tries
        }
        break;
    }
    if (FAILED(hr)) {
        pEnumerator->Release();
        return hr;
    }
    if (uReturned == 0 || !pObj) {
        pEnumerator->Release();
        return E_FAIL;
    }
    VariantInit(&vtProp);
    hr = pObj->Get(prop, 0, &vtProp, 0, 0);
    if (SUCCEEDED(hr) && vtProp.vt == VT_BSTR && vtProp.bstrVal != NULL)
        WideCharToMultiByte(CP_ACP, 0, vtProp.bstrVal, -1, out, (int)outSize, NULL, NULL);
    else
        out[0] = '\0';
    VariantClear(&vtProp);
    pObj->Release();
    pEnumerator->Release();
    return hr;
}



HRESULT hwid_WMI_readSystemDiskSerialAndModel(IWbemServices* pSvc, char* outSerial, char* outModel, char *errorBuffer, size_t errorBufferSize) {
    HRESULT hr = S_OK;
    IEnumWbemClassObject* pEnumerator = nullptr;
    IWbemClassObject* pDisk = nullptr;
    IWbemClassObject* pPartition = nullptr;
    IWbemClassObject* pDrive = nullptr;
    ULONG returnedCount = 0;
    VARIANT varDeviceID, varPartitionDeviceID, varModel, varSerial;
    BSTR bstrWQL = nullptr;
    BSTR bstrQuery = nullptr;
    WCHAR query[256];

    VariantInit(&varDeviceID);
    VariantInit(&varPartitionDeviceID);
    VariantInit(&varModel);
    VariantInit(&varSerial);

    bstrWQL = SysAllocString(L"WQL");

    // Step 1: Get logical disk (C:)
    bstrQuery = SysAllocString(L"SELECT * FROM Win32_LogicalDisk WHERE DeviceID='C:'");
    hr = pSvc->ExecQuery(bstrWQL, bstrQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "ExecQuery for Win32_LogicalDisk failed (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    SysFreeString(bstrQuery);
    bstrQuery = nullptr;

    // Ensure enumerator proxy uses the same auth/impersonation as the service proxy, avoiding per-call negotiation that can hang.
    hr = CoSetProxyBlanket(pEnumerator, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr))
        goto cleanup;

    // Bounded wait for Next
    for (int tries = 0; tries < WMI_NEXT_MAX_TRIES; ++tries) {
        hr = pEnumerator->Next(WMI_NEXT_TIMEOUT_MS, 1, &pDisk, &returnedCount);
        if (hr == WBEM_S_TIMEDOUT) {
            if (tries + 1 >= WMI_NEXT_MAX_TRIES) {
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                break;
            }
            continue;
        }
        break;
    }
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT))
            snprintf(errorBuffer, errorBufferSize, "Timed out while enumerating Win32_LogicalDisk");
        else
            snprintf(errorBuffer, errorBufferSize, "Next on logical disk enumerator failed (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    if (returnedCount == 0) {
        hr = E_FAIL;
        snprintf(errorBuffer, errorBufferSize, "No logical disk found for C:");
        goto cleanup;
    }
    pEnumerator->Release(); 
    pEnumerator = nullptr;

    hr = pDisk->Get(L"DeviceID", 0, &varDeviceID, 0, 0);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to get DeviceID from logical disk (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }

    // Step 2: Map logical disk to partition
    swprintf(query, 256, L"ASSOCIATORS OF {Win32_LogicalDisk.DeviceID='%ls'} WHERE AssocClass=Win32_LogicalDiskToPartition", varDeviceID.bstrVal);
    bstrQuery = SysAllocString(query);
    hr = pSvc->ExecQuery(bstrWQL, bstrQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "ExecQuery for LogicalDiskToPartition failed (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    SysFreeString(bstrQuery);
    bstrQuery = nullptr;

    // Ensure enumerator proxy uses the same auth/impersonation as the service proxy, avoiding per-call negotiation that can hang.
    hr = CoSetProxyBlanket(pEnumerator, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr))
        goto cleanup;

    for (int tries = 0; tries < WMI_NEXT_MAX_TRIES; ++tries) {
        hr = pEnumerator->Next(WMI_NEXT_TIMEOUT_MS, 1, &pPartition, &returnedCount);
        if (hr == WBEM_S_TIMEDOUT) { 
            if (tries + 1 >= WMI_NEXT_MAX_TRIES) { 
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT); 
                break; 
            } 
            continue; 
        }
        break;
    }
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT))
            snprintf(errorBuffer, errorBufferSize, "Timed out while enumerating Win32_LogicalDiskToPartition associators");
        else
            snprintf(errorBuffer, errorBufferSize, "Next on partition enumerator failed (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    if (returnedCount == 0) {
        hr = E_FAIL;
        snprintf(errorBuffer, errorBufferSize, "No partition found for logical disk");
        goto cleanup;
    }
    pEnumerator->Release(); 
    pEnumerator = nullptr;

    hr = pPartition->Get(L"DeviceID", 0, &varPartitionDeviceID, 0, 0);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to get DeviceID from partition (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    

    // Step 3: Map partition to physical disk
    swprintf(query, 256, L"ASSOCIATORS OF {Win32_DiskPartition.DeviceID='%ls'} WHERE AssocClass=Win32_DiskDriveToDiskPartition", varPartitionDeviceID.bstrVal);
    bstrQuery = SysAllocString(query);
    hr = pSvc->ExecQuery(bstrWQL, bstrQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "ExecQuery for DiskDriveToDiskPartition failed (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    SysFreeString(bstrQuery);
    bstrQuery = nullptr;

    // Ensure enumerator proxy uses the same auth/impersonation as the service proxy, avoiding per-call negotiation that can hang.
    hr = CoSetProxyBlanket(pEnumerator, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr))
        goto cleanup;

    for (int tries = 0; tries < WMI_NEXT_MAX_TRIES; ++tries) {
        hr = pEnumerator->Next(WMI_NEXT_TIMEOUT_MS, 1, &pDrive, &returnedCount);
        if (hr == WBEM_S_TIMEDOUT) { 
            if (tries + 1 >= WMI_NEXT_MAX_TRIES) { 
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT); 
                break; 
            } 
            continue; 
        }
        break;
    }
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT))
            snprintf(errorBuffer, errorBufferSize, "Timed out while enumerating Win32_DiskDriveToDiskPartition associators");
        else
            snprintf(errorBuffer, errorBufferSize, "Next on drive enumerator failed (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    if (returnedCount == 0) {
        hr = E_FAIL;
        snprintf(errorBuffer, errorBufferSize, "No physical drive found for partition");
        goto cleanup;
    }
    pEnumerator->Release();
    pEnumerator = nullptr;
    

    // Get Model
    hr = pDrive->Get(L"Model", 0, &varModel, 0, 0);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to get Model from drive (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    if (varModel.vt == VT_BSTR && outModel)
        WideCharToMultiByte(CP_ACP, 0, varModel.bstrVal, -1, outModel, MAX_WMI_BUFFER_SIZE, NULL, NULL);
    else
        outModel[0] = '\0';


    // Get SerialNumber
    hr = pDrive->Get(L"SerialNumber", 0, &varSerial, 0, 0);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to get SerialNumber from drive (0x%08X)", (unsigned int)hr);
        goto cleanup;
    }
    if (varSerial.vt == VT_BSTR && outSerial)
        WideCharToMultiByte(CP_ACP, 0, varSerial.bstrVal, -1, outSerial, MAX_WMI_BUFFER_SIZE, NULL, NULL);
    else
        outSerial[0] = '\0';

    // Fallback to volume serial number (usefull for Wine where serial number is not available)
    if (outSerial[0] == '\0') {
        DWORD serial;
        if (GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0)) {
            snprintf(outSerial, MAX_WMI_BUFFER_SIZE, "%lu", serial);
        } else {
            snprintf(outSerial, MAX_WMI_BUFFER_SIZE, "UnknownSerial");
        }
    }

    hr = S_OK;

cleanup:
    if (pEnumerator) pEnumerator->Release();
    if (pDisk) pDisk->Release();
    if (pPartition) pPartition->Release();
    if (pDrive) pDrive->Release();
    if (bstrWQL) SysFreeString(bstrWQL);
    if (bstrQuery) SysFreeString(bstrQuery);
    VariantClear(&varDeviceID);
    VariantClear(&varPartitionDeviceID);
    VariantClear(&varModel);
    VariantClear(&varSerial);
    return hr;
}



bool hwid_isIntegratedGPU(const wchar_t *name)
{
    if (!name || !*name)
        return false;

    // Tokens that guarantees a discrete GPU
    static const wchar_t * const dgpu_tokens[] = {
        L"NVIDIA", L"GEFORCE", L"RTX", L"GTX", L"QUADRO", L"FIREPRO",
        L"RADEON PRO", L"RADEON RX", L"RADEON R9", L"RADEON R8",
        L"RADEON R7 ",            /* space ensures we catch “R7 250” etc.      */
        L"INTEL ARC"
    };
    // Check if the name contains any of the discrete GPU tokens
    for (size_t i = 0; i < sizeof(dgpu_tokens)/sizeof(dgpu_tokens[0]); ++i)
    {
        size_t toklen = wcslen(dgpu_tokens[i]);
        const wchar_t *p = name;
        for (; *p; ++p)
            if (_wcsnicmp(p, dgpu_tokens[i], toklen) == 0)
                return false;
    }

    // Tokens that guarantees an integrated GPU
    static const wchar_t * const igpu_prefix[] = {
        L"INTEL(R) HD GRAPHICS",    L"INTEL(R) UHD GRAPHICS",
        L"INTEL(R) IRIS GRAPHICS",  L"INTEL(R) IRIS PLUS GRAPHICS",
        L"INTEL(R) IRIS XE GRAPHICS",
        L"INTEL(R) GRAPHICS",       L"INTEL(R) GMA",

        L"AMD RADEON(TM) GRAPHICS", /* Ryzen 7040 “Radeon Graphics”            */
        L"AMD RADEON(TM) VEGA",     /* Vega iGPU (Ryzen ≤ 5000G)               */
        L"AMD RADEON VEGA",
        L"AMD RADEON RX VEGA",      /* RX Vega 3/6/8/11 ‑ note trailing word    */

        L"AMD RADEON R2 GRAPHICS",  L"AMD RADEON R3 GRAPHICS",
        L"AMD RADEON R4 GRAPHICS",  L"AMD RADEON R5 GRAPHICS",
        L"AMD RADEON R6 GRAPHICS",  L"AMD RADEON R7 GRAPHICS",
        L"AMD RADEON R8 GRAPHICS"
    };
    // Check if the name starts with any of the integrated GPU prefixes
    for (size_t i = 0; i < sizeof(igpu_prefix)/sizeof(igpu_prefix[0]); ++i)
        if (_wcsnicmp(name, igpu_prefix[i], wcslen(igpu_prefix[i])) == 0)
            return true;

    // Treat Microsoft Basic Display Adapter as integrated (fallback driver)
    if (_wcsicmp(name, L"Microsoft Basic Display Adapter") == 0)
        return true;

    return false;
}


HRESULT hwid_WMI_readVideoController(IWbemServices* pSvc, char* outControllerName, char* errorBuffer, size_t errorBufferSize)
{
    HRESULT hr = S_OK;
    IEnumWbemClassObject* pEnumerator = nullptr;
    IWbemClassObject* pObj = nullptr;
    ULONG returnedCount = 0;
    outControllerName[0] = '\0';
    char fallbackName[MAX_WMI_BUFFER_SIZE] = {0};

    BSTR bstrWQL = SysAllocString(L"WQL");
    BSTR bstrQuery = SysAllocString(L"SELECT Name, AdapterRAM, PNPDeviceID FROM Win32_VideoController");
    hr = pSvc->ExecQuery(bstrWQL, bstrQuery,
                         WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                         NULL, &pEnumerator);
    SysFreeString(bstrWQL);
    SysFreeString(bstrQuery);
    if (FAILED(hr)) {
        snprintf(errorBuffer, errorBufferSize, "ExecQuery for Win32_VideoController failed (0x%08X)", (unsigned int)hr);
        return hr;
    }
    if (!pEnumerator)
        return E_FAIL;

    bool found = false;
    // Ensure enumerator proxy uses the same auth/impersonation as the service proxy, avoiding per-call negotiation that can hang.
    hr = CoSetProxyBlanket(pEnumerator, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) {
        pEnumerator->Release();
        return hr;
    }

    while (true) {

        // Use bounded waits instead of WBEM_INFINITE to prevent hangs if a provider is unresponsive
        for (int tries = 0; tries < WMI_NEXT_MAX_TRIES; ++tries) {
            hr = pEnumerator->Next(WMI_NEXT_TIMEOUT_MS, 1, &pObj, &returnedCount);
            if (hr == WBEM_S_TIMEDOUT) {
                if (tries + 1 >= WMI_NEXT_MAX_TRIES) {
                    hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT); // convert to failing HRESULT
                    break;
                }
                continue; // keep waiting until max tries
            }
            break;
        }
        if (hr != S_OK || !returnedCount) {
            break;
        }
        VARIANT vtName, vtAdapterRAM, vtPNP;
        VariantInit(&vtName);
        VariantInit(&vtAdapterRAM);
        VariantInit(&vtPNP);

        pObj->Get(L"Name", 0, &vtName, 0, 0);
        pObj->Get(L"AdapterRAM", 0, &vtAdapterRAM, 0, 0);
        pObj->Get(L"PNPDeviceID", 0, &vtPNP, 0, 0);

        if (vtName.vt == VT_BSTR && vtName.bstrVal != NULL) {
            // Store fallback candidate if no fallback stored yet
            if (fallbackName[0] == '\0') {
                WideCharToMultiByte(CP_ACP, 0, vtName.bstrVal, -1, fallbackName, MAX_WMI_BUFFER_SIZE, NULL, NULL);
            }

            bool candidate = false;
            // If AdapterRAM is a valid number 32-bit integer and PNPDeviceID is a string
            if ((vtAdapterRAM.vt == VT_I4 || vtAdapterRAM.vt == VT_UI4) && vtPNP.vt == VT_BSTR) {
                // Check if AdapterRAM is greater than 100MB and PNPDeviceID contains "PCI"
                if (vtAdapterRAM.uintVal > 100 * 1024 * 1024 && wcsstr(vtPNP.bstrVal, L"PCI") != NULL) {
                    candidate = true;
                    // Save as fallback if no other candidate found
                    WideCharToMultiByte(CP_ACP, 0, vtName.bstrVal, -1, fallbackName, MAX_WMI_BUFFER_SIZE, NULL, NULL);
                }
            }

            // Cancel the candidate if the name is one of the integrated graphics names
            if (candidate && hwid_isIntegratedGPU(vtName.bstrVal)) {
                candidate = false; // Integrated GPU, not suitable
            }

            if (candidate) {
                WideCharToMultiByte(CP_ACP, 0, vtName.bstrVal, -1, outControllerName, MAX_WMI_BUFFER_SIZE, NULL, NULL);
                found = true;
            }
        }

        VariantClear(&vtName);
        VariantClear(&vtAdapterRAM);
        VariantClear(&vtPNP);
        pObj->Release();

        // If we found a suitable controller, break the loop
        if (found) break;
    }
    pEnumerator->Release();

    if (hr != S_OK) {
        if (hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT))
            snprintf(errorBuffer, errorBufferSize, "Timed out while enumerating Win32_VideoController");
        else
            snprintf(errorBuffer, errorBufferSize, "Error while enumerating Win32_VideoController (0x%08X)", (unsigned int)hr);
        return hr;
    }

    // If no suitable controller was found, use the fallback name
    if (!found && fallbackName[0] != '\0') {
        strncpy(outControllerName, fallbackName, MAX_WMI_BUFFER_SIZE - 1);
        outControllerName[MAX_WMI_BUFFER_SIZE - 1] = '\0'; // Ensure null-termination
        found = true; // We have a fallback
    }

    if (!found) {
        snprintf(errorBuffer, errorBufferSize, "No suitable external video controller found.");
        return E_FAIL;
    }
    return S_OK;
}

/**
 * Loads all required WMI properties into shared global variables.
 */
HRESULT hwid_WMI_loadProperties(char* errorBuffer, size_t errorBufferSize)
{
    HRESULT hr;
    IWbemLocator *pLocator = NULL;
    IWbemServices *pServices = NULL;

    // Buffer to store names of missing properties
    char missingProps[512] = {0};
    int emptyCount = 0;

    // Read all required WMI properties
    // Define all required WMI properties in an array
    WMIProperty properties[] = {
        {L"Win32_BIOS", L"Manufacturer", hwid_bios_manufacturer, sizeof(hwid_bios_manufacturer)},
        {L"Win32_BIOS", L"SerialNumber", hwid_bios_serial, sizeof(hwid_bios_serial)},
        {L"Win32_Processor", L"Name", hwid_cpu_name, sizeof(hwid_cpu_name)},
        {L"Win32_Processor", L"ProcessorId", hwid_cpu_id, sizeof(hwid_cpu_id)},
        {L"Win32_BaseBoard", L"Manufacturer", hwid_board_manufacturer, sizeof(hwid_board_manufacturer)},
        {L"Win32_BaseBoard", L"SerialNumber", hwid_board_serial, sizeof(hwid_board_serial)},
        {L"Win32_ComputerSystemProduct", L"UUID", hwid_uuid, sizeof(hwid_uuid)}
    };

    // Initialize COM for multithreaded apartment to avoid STA message-pump deadlocks
    hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return hr;

    // Establish process-wide COM security (safe to call multiple times; ignore RPC_E_TOO_LATE)
    {
        HRESULT secHr = CoInitializeSecurity(
            NULL,                        // default security descriptor
            -1,                          // COM negotiates services
            NULL,                        // authentication services
            NULL,                        // reserved
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL,
            EOAC_NONE,
            NULL);
        if (FAILED(secHr) && secHr != RPC_E_TOO_LATE) {
            // If security init fails for other reasons, bail out
            CoUninitialize();
            return secHr;
        }
    }

    hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (void **)&pLocator);
    if (FAILED(hr))
        goto cleanup;

    {
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        hr = pLocator->ConnectServer(ns, NULL, NULL, 0, 0, 0, NULL, &pServices);
        SysFreeString(ns);
    }
    if (FAILED(hr))
        goto cleanup;

    // Ensure enumerator proxy uses the same auth/impersonation as the service proxy, avoiding per-call negotiation that can hang.
    hr = CoSetProxyBlanket(pServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr))
        goto cleanup;



    // Iterate and read each property, check for errors
    for (size_t i = 0; i < sizeof(properties)/sizeof(properties[0]); ++i) {
        HRESULT propHr = hwid_WMI_readFirstString(pServices, properties[i].wmiClass, properties[i].property, properties[i].buffer, properties[i].bufferSize);
        if (FAILED(propHr)) {
            hr = propHr;
            if (propHr == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
                snprintf(errorBuffer, errorBufferSize, "Timeout while reading WMI property '%ls.%ls'. Please check your system's WMI service.", properties[i].wmiClass, properties[i].property);
            } else {
                snprintf(errorBuffer, errorBufferSize, "Failed to read WMI property '%ls.%ls' (Error: 0x%08X).", properties[i].wmiClass, properties[i].property, (unsigned int)propHr);
            }
            goto cleanup;
        }
        if (properties[i].buffer[0] == '\0') {
            emptyCount++;
            // Append missing property name to missingProps
            char propertyName[128];
            sprintf_s(propertyName, sizeof(propertyName), "%ls.%ls ", properties[i].wmiClass, properties[i].property);
            strncat(missingProps, propertyName, sizeof(missingProps) - strlen(missingProps) - 1);
        }
    }

    // Read disk model and serial
    hr = hwid_WMI_readSystemDiskSerialAndModel(pServices, hwid_disk_serial, hwid_disk_model, errorBuffer, errorBufferSize);
    if (FAILED(hr)) {
        goto cleanup;
    }
    if (hwid_disk_model[0] == '\0') {
        emptyCount++;
        strncat(missingProps, "Disc Model ", sizeof(missingProps) - strlen(missingProps) - 1);
    }
    if (hwid_disk_serial[0] == '\0') {
        emptyCount++;
        strncat(missingProps, "Disc Serial Number ", sizeof(missingProps) - strlen(missingProps) - 1);
    }

    // Read the GPU name
    hr = hwid_WMI_readVideoController(pServices, hwid_gpuName, errorBuffer, errorBufferSize);
    if (FAILED(hr)) {
        emptyCount++;
        strncat(missingProps, "Video Controller ", sizeof(missingProps) - strlen(missingProps) - 1);
    }


    // Verify if we have enough properties
    if (emptyCount >= 3) {
        hr = E_FAIL;
        snprintf(errorBuffer, errorBufferSize, "Too many missing WMI properties (%d/%d). Missing: %s",
                 emptyCount, (int)(sizeof(properties)/sizeof(properties[0])), missingProps);
        goto cleanup;
    }

    hr = S_OK; // Success

cleanup:
    if (pServices)
        pServices->Release();
    if (pLocator)
        pLocator->Release();
    CoUninitialize();
    return hr;
}




/**
 * Generates an hash from the given input string.
 *
 * @param str The input string to hash.
 * @param output The output buffer to store the hash result
 */
void hwid_hash_FVN1A(const char *str, uint32_t *output)
{
    // FVN-1a hash
    uint32_t hash = 2166136261u;
    while (*str)
    {
        hash ^= (unsigned char)(*str++);
        hash *= 16777619;
    }

    hash &= 0x7FFFFFFF; // clear highest bit (bit 31) to avoid negative numbers
    if (hash == 0)
        hash = 1; // avoid returning zero

    *output = hash;
}

/**
 * Generates a HWID hash (integer) using a subset of the loaded properties.
 * Combines BIOS serial, CPU ID, Board serial, and UUID.
 */
int hwid_generate_short()
{
    char combined[512];
    // For this simpler HWID, use BIOS serial, CPU ID, Board serial, and UUID.
    snprintf(combined, sizeof(combined), "%s %s %s %s", hwid_bios_serial, hwid_cpu_id, hwid_board_serial, hwid_uuid);

    //MessageBoxA(NULL, combined, "HWID", MB_OK);

    uint32_t hash;
    hwid_hash_FVN1A(combined, &hash);

    return hash;
}





// Resets the HWID values in the registry and memory for cases when then the way HWID is generated has changed
void hwid_restartRegistry() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Activision\\Call of Duty 2", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
    {
        // Clear the HWID change related values
        RegDeleteValueA(hKey, "HWID");
        RegDeleteValueA(hKey, "HWID_OLD");
        RegDeleteValueA(hKey, "HWID_SOURCE");
        RegDeleteValueA(hKey, "HWID_DIFF");
        RegDeleteValueA(hKey, "HWID_DIFF_COUNT");

        // Reset also memory
        hwid_changed_diff[0] = '\0';
        hwid_old[0] = '\0';
        hwid_changed = false;
        hwid_changed_count = 0;

        RegCloseKey(hKey);
    }
    else
    {
        SHOW_ERROR("Failed to open registry key for HWID.");
        exit(1);
    }
}

// Resets registry values that can be removed after sending the values to update server
void hwid_clearRegistryFromHWIDChange() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Activision\\Call of Duty 2", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
    {
        // Clear the HWID change related values
        RegDeleteValueA(hKey, "HWID_DIFF");
        RegDeleteValueA(hKey, "HWID_OLD");

        // Reset also memory
        hwid_changed_diff[0] = '\0';
        hwid_old[0] = '\0';

        RegCloseKey(hKey);
    }
    else
    {
        SHOW_ERROR("Failed to open registry key for HWID.");
        exit(1);
    }
}


 /**
 * Generates a unique Hardware ID (HWID) based on the system's hardware components
 *
 * @return A pointer to the generated HWID string.
 */
char* hwid_generate_long()
{
    char hwid_raw[1024];
    snprintf(hwid_raw, sizeof(hwid_raw),
             "BIOS manufacturer: %s\nBIOS serial: %s\nCPU name: %s\nCPU ID: %s\nMotherboard manufacturer: %s\nMotherboard serial: %s\nDisk Model: %s\nDisk Serial: %s\nGPU Name: %s",
             hwid_bios_manufacturer, hwid_bios_serial, hwid_cpu_name, hwid_cpu_id,
             hwid_board_manufacturer, hwid_board_serial, hwid_disk_model, hwid_disk_serial,
             hwid_gpuName);

    //MessageBoxA(NULL, hwid_raw, "HWID", MB_OK);
    /*
    ---------------------------
    HWID
    ---------------------------
    BIOS manufacturer: American Megatrends Inc.
    BIOS serial: System Serial Number
    CPU name: 13th Gen Intel(R) Core(TM) i7-13700K
    CPU ID: BFEBFBFF000B0671
    Motherboard manufacturer: ASUSTeK COMPUTER INC.
    Motherboard serial: 221213970402628
    UUID: 8F71A255-391A-3529-B885-581122D0358F
    Disk Model: Samsung SSD 990 EVO 2TB
    Total Memory: 68448202752
    GPU Name: NVIDIA GeForce GTX 1080 Ti
    ---------------------------
    */

    char* hwid_MD5 = CL_BuildMD5(hwid_raw, strlen(hwid_raw));

    // Registry update
    HKEY hKey;
    char regHWID[64] = {0};
    DWORD dwType = REG_SZ;
    DWORD dwSize = sizeof(regHWID);
    hwid_changed = false;


    // In version 1.4.4.4 we have restarted the HWID2 data
    // Example: previous version was 1.4.4.3 and user upgraded to 1.4.4.5, meaning 1.4.4.4 was skipped (in which HWID was restarted) -> we need to restart the HWID
    bool isUpgradeFromOldVersion_1_4_4_4_OrOlder = registry_version_changed && (registry_previous_version[0] == '\0' || version_compare(registry_previous_version, "1.4.4.4") < 0);
    if (isUpgradeFromOldVersion_1_4_4_4_OrOlder) {
        hwid_restartRegistry();
    }

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Activision\\Call of Duty 2", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
    {
        LONG regResult = RegQueryValueExA(hKey, "HWID", NULL, &dwType, (LPBYTE)regHWID, &dwSize);
        if (regResult == ERROR_SUCCESS && dwType == REG_SZ)
        {
            // Hash does not equal to previously saved HWID
            if (strcmp(regHWID, hwid_MD5) != 0)
            {
                hwid_changed = true;

                // Save previous HWID to HWID_OLD before updating
                regResult = RegSetValueExA(hKey, "HWID_OLD", 0, REG_SZ, (const BYTE*)regHWID, (DWORD)strlen(regHWID) + 1);
                if (regResult != ERROR_SUCCESS)
                {
                    SHOW_ERROR("Failed to save old HWID to registry.");
                    exit(1);
                }

                // Increase HWID_DIFF_COUNT in registry
                DWORD dwCount = 1;
                DWORD dwCountSize = sizeof(dwCount);
                regResult = RegQueryValueExA(hKey, "HWID_DIFF_COUNT", NULL, &dwType, (LPBYTE)&dwCount, &dwCountSize);
                if (regResult == ERROR_SUCCESS && dwType == REG_DWORD)
                    dwCount++;
                regResult = RegSetValueExA(hKey, "HWID_DIFF_COUNT", 0, REG_DWORD, (const BYTE*)&dwCount, sizeof(dwCount));
                if (regResult != ERROR_SUCCESS)
                {
                    SHOW_ERROR("Failed to update HWID_DIFF_COUNT in registry.");
                    exit(1);
                }

                // Read HWID_SOURCE from registry and compare it with the new combined string
                char regHWID_Source[1024] = {0};
                DWORD dwSizeSource = sizeof(regHWID_Source);
                regResult = RegQueryValueExA(hKey, "HWID_SOURCE", NULL, &dwType, (LPBYTE)regHWID_Source, &dwSizeSource);
                if (regResult != ERROR_SUCCESS || dwType != REG_SZ) {
                    //SHOW_ERROR("Failed to read HWID_SOURCE from registry.");
                    //exit(1);
                    // Ignore error as it might not exists if moved from an older version
                    regHWID_Source[0] = '\0'; // No HWID_SOURCE found
                } else {
                    // Decode
                    char hwid_raw_base64[2048] = {0};
                    int base64status = base64_decode(regHWID_Source, (uint8_t*)hwid_raw_base64, sizeof(hwid_raw_base64));
                    if (base64status < 0) {
                        SHOW_ERROR("Failed to decode HWID source from Base64.");
                        exit(1);
                    }
                    // Find the changed lines
                    if (strcmp(hwid_raw_base64, hwid_raw) != 0) {
                        // Find the changed lines
                        char* srcCopy = _strdup(hwid_raw_base64);
                        char* newCopy = _strdup(hwid_raw);
                        if (srcCopy && newCopy) {
                            char* srcSave;
                            char* newSave;
                            char* srcLine = strtok_s(srcCopy, "\n", &srcSave);
                            char* newLine = strtok_s(newCopy, "\n", &newSave);
                            int lineNum = 1;
                            while (srcLine && newLine) {
                                if (strcmp(srcLine, newLine) != 0) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg), "%s\n%s\n", srcLine, newLine);
                                    strncat(hwid_changed_diff, msg, sizeof(hwid_changed_diff) - strlen(hwid_changed_diff) - 1);
                                }
                                srcLine = strtok_s(NULL, "\n", &srcSave);
                                newLine = strtok_s(NULL, "\n", &newSave);
                                ++lineNum;
                            }
                        }
                        free(srcCopy);
                        free(newCopy);

                        // If there are any changed lines, show them in a single MessageBox
                        if (hwid_changed_diff[0] != '\0') {
                            //MessageBoxA(NULL, hwid_changed_diff, "HWID Change Detected", MB_OK | MB_ICONWARNING);

                            // Save the difference to the registry
                            regResult = RegSetValueExA(hKey, "HWID_DIFF", 0, REG_SZ, (const BYTE*)hwid_changed_diff, (DWORD)strlen(hwid_changed_diff) + 1);
                            if (regResult != ERROR_SUCCESS)
                            {
                                SHOW_ERROR("Failed to save HWID change difference to registry.");
                                exit(1);
                            }
                        }
                    } else {
                        SHOW_ERROR("HWID source has not changed, but HWID hash is different. This should not happen.");
                        exit(1);
                    }
                }
            }
        }

        // Save new HWID to registry
        regResult = RegSetValueExA(hKey, "HWID", 0, REG_SZ, (const BYTE*)hwid_MD5, (DWORD)strlen(hwid_MD5) + 1);
        if (regResult != ERROR_SUCCESS)
        {
            SHOW_ERROR("Failed to save HWID to registry.");
            exit(1);
        }

        // Save the source of HWID into registry, encode it
        char hwid_raw_base64[2048] = {0};
        int base64status = base64_encode((const uint8_t*)hwid_raw, strlen(hwid_raw), hwid_raw_base64, sizeof(hwid_raw_base64));
        if (base64status < 0) {
            SHOW_ERROR("Failed to encode HWID source to Base64.");
            exit(1);
        }
        regResult = RegSetValueExA(hKey, "HWID_SOURCE", 0, REG_SZ, (const BYTE*)hwid_raw_base64, (DWORD)strlen(hwid_raw_base64) + 1);
        if (regResult != ERROR_SUCCESS)
        {
            SHOW_ERROR("Failed to save HWID source to registry.");
            exit(1);
        }
        


        // Read old HWID from registry
        dwSize = sizeof(hwid_old);
        regResult = RegQueryValueExA(hKey, "HWID_OLD", NULL, &dwType, (LPBYTE)hwid_old, &dwSize);
        if (regResult == ERROR_SUCCESS && dwType == REG_SZ)
            hwid_old[sizeof(hwid_old) - 1] = '\0';
        else
            hwid_old[0] = '\0'; // No old HWID found


        // Read diff from registry
        dwSize = sizeof(hwid_changed_diff);
        regResult = RegQueryValueExA(hKey, "HWID_DIFF", NULL, &dwType, (LPBYTE)hwid_changed_diff, &dwSize);
        if (regResult == ERROR_SUCCESS && dwType == REG_SZ)
            hwid_changed_diff[sizeof(hwid_changed_diff) - 1] = '\0'; // Ensure null-termination
        else
            hwid_changed_diff[0] = '\0'; // No diff found

        // Read HWID_DIFF_COUNT from registry
        DWORD dwCount = 0;
        DWORD dwCountSize = sizeof(dwCount);
        regResult = RegQueryValueExA(hKey, "HWID_DIFF_COUNT", NULL, &dwType, (LPBYTE)&dwCount, &dwCountSize);
        if (regResult == ERROR_SUCCESS && dwType == REG_DWORD)
            hwid_changed_count = (int)dwCount;
        else
            hwid_changed_count = 0; // No count found, reset to 0



        RegCloseKey(hKey);
    }
    else
    {
        SHOW_ERROR("Failed to open registry key for HWID.");
        exit(1);
    }

    return hwid_MD5;
}


/**
 * Generates a unique REGID for the game, used for computer identification.
 * This REGID is used to verify if the HWID is unique accross multiple computers.
 */
void hwid_generate_regid() {
    HKEY hKey;
    char regid[64] = {0};
    DWORD dwType = REG_SZ;
    DWORD dwSize = sizeof(regid);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Activision\\Call of Duty 2", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
    {
        LONG regResult = RegQueryValueExA(hKey, "REGID", NULL, &dwType, (LPBYTE)regid, &dwSize);
        if (regResult != ERROR_SUCCESS || dwType != REG_SZ || regid[0] == '\0')
        {
            // Generate new random string and hash it
            char randomStr[64];
            snprintf(randomStr, sizeof(randomStr), "%u_%u", (unsigned int)GetTickCount(), rand());
            char* regidHash = CL_BuildMD5(randomStr, strlen(randomStr));

            // Save new REGID to registry        
            RegSetValueExA(hKey, "REGID", 0, REG_SZ, (const BYTE*)regidHash, (DWORD)strlen(regidHash) + 1);

            strncpy(hwid_regid, regidHash, sizeof(hwid_regid) - 1);
            hwid_regid[sizeof(hwid_regid) - 1] = '\0';
        } else
        {
            // Use existing REGID
            strncpy(hwid_regid, regid, sizeof(hwid_regid) - 1);
            hwid_regid[sizeof(hwid_regid) - 1] = '\0';
        }
        RegCloseKey(hKey);
    }
    else
    {
        SHOW_ERROR("Failed to open registry key for HWID REGID.");
        exit(1);
    }
}


/** Called every frame at the start of the frame. */
void hwid_frame()
{
    // Check if HWID has changed since last check
    if (hwid_changed) {
        hwid_changed = false;
        
        // The error is disabled untill its ready
        /*Com_Error(ERR_DROP, 
            "Your hardware ID has changed.\n\n"
            "If you were banned, this new hardware ID will also be banned. "
            "If not, you may continue playing. "
            "If this happens often, please contact developers of CoD2x.");*/
    }
}

/** Called once at game start after common initialization. Used to initialize variables, cvars, etc. */
void hwid_init()
{
    HRESULT hr;
    char errorBuffer[512];
    while ((hr = hwid_WMI_loadProperties(errorBuffer, sizeof(errorBuffer))) != S_OK) {
        showErrorMessage("Fatal Error", "Error while generating HWID.\n\nUnable to load WMI properties.\nError: 0x%08X %s", hr, errorBuffer);
        int result = MessageBoxA(NULL, 
            "Do you want to retry or exit?\n\nIf the problem persists, please restart your computer and try again.", 
            "Retry or Exit", 
            MB_RETRYCANCEL | MB_ICONQUESTION);
        if (result == IDRETRY)
            Sleep(500);
        else {
            exit(1);
            return;
        }
    }

    dvarFlags_e nowrite = HWID_TESTING ? DVAR_NOFLAG : DVAR_NOWRITE; // allow rewriting while testing

    cl_hwid = Dvar_RegisterInt("cl_hwid", hwid_generate_short(), INT32_MIN, INT32_MAX, (dvarFlags_e)(DVAR_USERINFO | nowrite));
    cl_hwid2 = Dvar_RegisterString("cl_hwid2", hwid_generate_long(), (dvarFlags_e)(DVAR_USERINFO | nowrite));

    hwid_generate_regid();
}

/** Called before the entry point is executed. Used to patch memory. */
void hwid_patch()
{
}