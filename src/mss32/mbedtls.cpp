#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fstream>
#include <cstring>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/debug.h>
#include <winsock2.h>
#include <windows.h>
#include <iostream>

#include "../shared/cod2.h"
#include "sftp.h"

CRITICAL_SECTION criticalSectionRequest;
bool stopThreadsRequest = false;
HANDLE threadHandlesRequest[3];

#define SERVER_NAME "server.hostgamer.com.br"
#define SERVER_PORT "443"
#define BOUNDARY "----COD2XSCREENSHOTUPLOAD"

int read_file(const char *filePath, unsigned char **fileData, size_t *fileSize)
{
    FILE *file = fopen(filePath, "rb"); // Abrir arquivo em modo binário
    if (file == NULL)
    {
        printf("Failed to open file: %s\n", filePath);
        return -1;
    }

    fseek(file, 0, SEEK_END); // Ir para o final do arquivo
    *fileSize = ftell(file);  // Obter o tamanho do arquivo
    fseek(file, 0, SEEK_SET); // Voltar para o início do arquivo

    *fileData = (unsigned char *)malloc(*fileSize); // Alocar memória para o conteúdo do arquivo
    if (*fileData == NULL)
    {
        printf("Failed to allocate memory for file content\n");
        fclose(file);
        return -1;
    }

    fread(*fileData, 1, *fileSize, file); // Ler o conteúdo do arquivo
    fclose(file);                         // Fechar o arquivo

    return 0;
}

// Função para fazer a requisição HTTPS
int https_request(const char *method, const char *path, const char *data, const char *filePath)
{
    int ret = 0;
    mbedtls_net_context server_fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    unsigned char buf[1024];

    size_t fileSize = 0;
    unsigned char *fileData = NULL; // Ponteiro para armazenar os dados do arquivo

    // Inicialização das estruturas
    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    // Usando a entropia para o gerador de números aleatórios
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (ret != 0)
    {
        Com_Printf("mbedtls_ctr_drbg_seed failed: -0x%x\n", -ret);
        return -1;
    }

    // Carregando o certificado da CA (se necessário)
    ret = mbedtls_x509_crt_parse_file(&cacert, "ca-cert.pem");
    if (ret < 0)
    {
        Com_Printf("mbedtls_x509_crt_parse_file failed: -0x%x\n", -ret);
        return -1;
    }

    // Conexão com o servidor
    ret = mbedtls_net_connect(&server_fd, SERVER_NAME, SERVER_PORT, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0)
    {
        Com_Printf("mbedtls_net_connect failed: -0x%x\n", -ret);
        return -1;
    }

    // Inicializando a configuração SSL
    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        Com_Printf("mbedtls_ssl_config_defaults failed: -0x%x\n", -ret);
        return -1;
    }

    // Associando o gerador de números aleatórios (RNG) ao SSL
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    // Desabilitando a verificação de certificados SSL
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0)
    {
        Com_Printf("mbedtls_ssl_setup failed: -0x%x\n", -ret);
        return -1;
    }

    ret = mbedtls_ssl_set_hostname(&ssl, SERVER_NAME);
    if (ret != 0)
    {
        Com_Printf("mbedtls_ssl_set_hostname failed: -0x%x\n", -ret);
        return -1;
    }

    // Associando o socket à conexão SSL
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // Handshake SSL/TLS
    ret = mbedtls_ssl_handshake(&ssl);
    if (ret != 0)
    {
        Com_Printf("mbedtls_ssl_handshake failed: -0x%x\n", -ret);
        return -1;
    }

    // Preparando os dados para envio
    char request[2048] = {0};
    if (strcmp(method, "POST") == 0)
    {
        snprintf(request, sizeof(request),
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n"
                 "%s",
                 path, SERVER_NAME, strlen(data), data);
    }
    else if (strcmp(method, "GET") == 0)
    {
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n\r\n",
                 path, SERVER_NAME);
    }
    else if (strcmp(method, "UPLOAD") == 0 && filePath != NULL)
    {
        size_t contentLength = fileSize + strlen(BOUNDARY) + 500; // Incluindo o tamanho do arquivo e o boundary
        char request[2048 + fileSize];                            // O buffer agora deve ser grande o suficiente para o arquivo

        // Lendo o conteúdo do arquivo
        ret = read_file(filePath, &fileData, &fileSize);
        if (ret != 0)
        {
            return -1;
        }

        Com_Printf("contentLength: %d\n", contentLength);
        Com_Printf("fileSize: %d\n", fileSize);

        snprintf(request, sizeof(request),
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Type: multipart/form-data; boundary=%s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n"
                 "--%s\r\n"
                 "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n",
                 path, SERVER_NAME, BOUNDARY, contentLength, BOUNDARY, filePath);

        // Enviando os cabeçalhos da requisição
        ret = mbedtls_ssl_write(&ssl, (unsigned char *)request, strlen(request));
        if (ret < 0)
        {
            printf("mbedtls_ssl_write failed: -0x%x\n", -ret);
            return -1;
        }

        // Enviando o conteúdo do arquivo
        ret = mbedtls_ssl_write(&ssl, fileData, fileSize);
        if (ret < 0)
        {
            printf("mbedtls_ssl_write failed for file content: -0x%x\n", -ret);
            free(fileData);
            return -1;
        }

        // Enviando o final do boundary
        snprintf(request, sizeof(request), "\r\n--%s--\r\n", BOUNDARY);
        ret = mbedtls_ssl_write(&ssl, (unsigned char *)request, strlen(request));
        if (ret < 0)
        {
            printf("mbedtls_ssl_write failed for closing boundary: -0x%x\n", -ret);
            return -1;
        }

        // Liberando a memória do arquivo
        free(fileData);
    }

    // Enviando a requisição
    ret = mbedtls_ssl_write(&ssl, (unsigned char *)request, strlen(request));
    if (ret < 0)
    {
        Com_Printf("mbedtls_ssl_write failed: -0x%x\n", -ret);
        return -1;
    }

    // Recebendo a resposta
    Com_Printf("Response:\n\n");
    while ((ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf) - 1)) > 0)
    {
        buf[ret] = '\0';       // Garantir que o buffer seja terminado corretamente
        Com_Printf("%s", buf); // Exibe a resposta
    }
    if (ret < 0)
    {
        Com_Printf("mbedtls_ssl_read failed: -0x%x\n", -ret);
        return -1;
    }

    // Fechando a conexão SSL e a conexão de rede
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return 0;
}

int post_test()
{
    const char *post_data = "param1=value1&param2=value2";
    int ret = https_request("POST", "/test.php", post_data, NULL);
    if (ret != 0)
    {
        Com_Printf("HTTPS POST request failed\n");
        return -1;
    }
    return 0;
}

int get_test()
{
    int ret = https_request("GET", "/test.php", NULL, NULL);
    if (ret != 0)
    {
        Com_Printf("HTTPS GET request failed\n");
        return -1;
    }
    return 0;
}

int upload_test()
{
    int ret = https_request("UPLOAD", "/upload.php", NULL, "screenshot.jpg");
    if (ret != 0)
    {
        Com_Printf("HTTPS UPLOAD request failed\n");
        return -1;
    }
    return 0;
}

// Função executada por cada thread
DWORD WINAPI RequestThread(LPVOID lpParam)
{
    while (!stopThreadsRequest)
    {
        EnterCriticalSection(&criticalSectionRequest);

        upload_test();
        // post_test();
        get_test();

        LeaveCriticalSection(&criticalSectionRequest);
        Sleep(5000);
    }
    return 0;
}

void RequestStartThreads(int numThreads)
{
    InitializeCriticalSection(&criticalSectionRequest);
    for (int i = 0; i < numThreads; ++i)
    {
        int *threadID = (int *)malloc(sizeof(int));
        *threadID = i + 1;
        threadHandlesRequest[i] = CreateThread(NULL, 0, RequestThread, threadID, 0, NULL);
        if (!threadHandlesRequest[i])
        {
            free(threadID);
        }
    }
}

void RequestStopThreads(int numThreads)
{
    stopThreadsRequest = true;
    WaitForMultipleObjects(numThreads, threadHandlesRequest, TRUE, INFINITE);
    for (int i = 0; i < numThreads; ++i)
    {
        CloseHandle(threadHandlesRequest[i]);
    }
    DeleteCriticalSection(&criticalSectionRequest);
}

void request_init()
{
    RequestStartThreads(1);
}
