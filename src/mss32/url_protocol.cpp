#include "url_protocol.h"
#include <stdio.h>
#include "../shared/cod2.h"

int get_cod2_exe_path(char *exe_path, DWORD size) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, REG_COD2_PATH, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        Com_Printf("Erro: Não foi possível abrir a chave do registro do Call of Duty 2.\n");
        return 0;
    }

    result = RegQueryValueEx(hKey, REG_COD2_KEY, NULL, NULL, (LPBYTE)exe_path, &size);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS) {
        Com_Printf("Erro: Não foi possível ler o caminho do executável.\n");
        return 0;
    }

    return 1;
}

int check_protocol_exists() {
    HKEY hKey;
    char keyPath[256];
    snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Classes\\%s", PROTOCOL_NAME);

    if (RegOpenKeyEx(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return 1;
    }
    return 0;
}

int register_protocol(const char *exe_path) {
    HKEY hKey;
    DWORD disposition;
    char command[512];
    char keyPath[256];

    snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Classes\\%s", PROTOCOL_NAME);
    
    if (RegCreateKeyEx(HKEY_CURRENT_USER, keyPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &disposition) != ERROR_SUCCESS) {
        Com_Printf("Erro ao criar chave do protocolo.\n");
        return 0;
    }

    RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE *)"URL:cod2 Protocol", strlen("URL:cod2 Protocol") + 1);
    RegSetValueEx(hKey, "URL Protocol", 0, REG_SZ, (BYTE *)"", 1);
    RegCloseKey(hKey);

    snprintf(keyPath, sizeof(keyPath), "SOFTWARE\\Classes\\%s\\shell\\open\\command", PROTOCOL_NAME);
    if (RegCreateKeyEx(HKEY_CURRENT_USER, keyPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &disposition) != ERROR_SUCCESS) {
        Com_Printf("Erro ao criar chave de comando.\n");
        return 0;
    }

    snprintf(command, sizeof(command), "\"%s\" +openurl \"\"%s\"\"", exe_path, "%%1" + 7);
    RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE *)command, strlen(command) + 1);
    RegCloseKey(hKey);

    Com_Printf("Protocolo cod2:// registrado com sucesso!\n");
    return 1;
}

int url_protocol_init() {
    if (check_protocol_exists()) {
        Com_Printf("O protocolo cod2:// já está registrado.\n");
    } else {
        Com_Printf("Protocolo cod2:// não encontrado. Registrando...\n");

        char exe_path[MAX_PATH] = {0};
        if (!get_cod2_exe_path(exe_path, sizeof(exe_path))) {
            Com_Printf("Erro ao obter o caminho do executável do Call of Duty 2.\n");
            return 1;
        }

        Com_Printf("Encontrado caminho do executável: %s\n", exe_path);

        if (register_protocol(exe_path)) {
            Com_Printf("Registro concluído com sucesso!\n");
        } else {
            Com_Printf("Falha ao registrar o protocolo.\n");
        }
    }
    return 0;
}
