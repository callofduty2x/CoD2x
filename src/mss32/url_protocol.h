#ifndef COD2_PROTOCOL_H
#define COD2_PROTOCOL_H

#include <windows.h>

#define PROTOCOL_NAME "cod2"
#define REG_COD2_PATH "SOFTWARE\\WOW6432Node\\Activision\\Call of Duty 2"
#define REG_COD2_KEY "MultiEXEString"

/**
 * @brief Obtém o caminho do executável do Call of Duty 2 a partir do Registro do Windows.
 * @param exe_path Buffer para armazenar o caminho do executável.
 * @param size Tamanho do buffer.
 * @return 1 se o caminho foi encontrado com sucesso, 0 caso contrário.
 */
int get_cod2_exe_path(char *exe_path, DWORD size);

/**
 * @brief Verifica se o protocolo `cod2://` já está registrado no Windows.
 * @return 1 se o protocolo existe, 0 caso contrário.
 */
int check_protocol_exists();

/**
 * @brief Registra o protocolo `cod2://` no Windows, associando-o ao executável do Call of Duty 2.
 * @param exe_path Caminho do executável do jogo.
 * @return 1 se o protocolo foi registrado com sucesso, 0 caso contrário.
 */
int register_protocol(const char *exe_path);

/**
 * @brief Inicializa o protocolo `cod2://`, registrando-o caso ainda não esteja no sistema.
 * @return 0 em caso de sucesso, 1 em caso de erro.
 */
int url_protocol_init();

#endif // COD2_PROTOCOL_H
