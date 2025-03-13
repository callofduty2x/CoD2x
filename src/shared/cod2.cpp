#include "cod2.h"
#include <cstring> // Para manipulação de strings

// Definição da função para limpar as cores do hostname
char *Com_CleanHostnameColors(const char *hostname)
{
    int i = 0, j = 0;
    char *cleanedHostname = (char *)malloc(strlen(hostname) + 1); // Aloca memória para a string processada

    if (cleanedHostname == NULL)
    {
        return NULL; // Se não conseguir alocar memória, retorna NULL
    }

    while (hostname[i] != '\0')
    {
        if ((hostname[i] == '^') && ((hostname[i + 1] >= '0' && hostname[i + 1] <= '9') || (hostname[i + 1] >= 'a' && hostname[i + 1] <= 'z') || (hostname[i + 1] >= 'A' && hostname[i + 1] <= 'Z')))
        {
            i += 2; // Ignora o código de cor
        }
        else
        {
            cleanedHostname[j++] = hostname[i++]; // Copia o caractere para a nova string
        }
    }

    cleanedHostname[j] = '\0'; // Garante que a string final está terminada com '\0'
    return cleanedHostname;
}

char *Com_CleanMapName(const char *mapName)
{
    char *cleanedMapName = (char *)malloc(strlen(mapName) + 1); // Aloca memória para a string processada

    if (cleanedMapName == NULL)
    {
        return NULL; // Se não conseguir alocar memória, retorna NULL
    }

    strcpy(cleanedMapName, mapName); // Copia o conteúdo original para a nova variável

    // Verifica se o nome do mapa começa com 'mp_'
    if (strncmp(cleanedMapName, "mp_", 3) == 0)
    {
        // Remove o 'mp_' (desloca o ponteiro 3 posições)
        memmove(cleanedMapName, cleanedMapName + 3, strlen(cleanedMapName) - 2);
    }

    // Coloca a primeira letra em maiúscula e o restante em minúscula
    if (cleanedMapName[0] != '\0')
    {
        cleanedMapName[0] = toupper(cleanedMapName[0]); // Primeira letra em maiúscula
        for (int i = 1; cleanedMapName[i] != '\0'; i++)
        {
            cleanedMapName[i] = tolower(cleanedMapName[i]); // Restante em minúscula
        }
    }

    return cleanedMapName;
}