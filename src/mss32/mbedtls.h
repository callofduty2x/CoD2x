#ifndef REQUEST_H
#define REQUEST_H

#include <windows.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

// Definições e constantes
#define SERVER_NAME "server.hostgamer.com.br"
#define SERVER_PORT "443"
#define POST_REQUEST \
    "POST /index.php HTTP/1.1\r\n" \
    "Host: server.hostgamer.com.br\r\n" \
    "Content-Type: application/x-www-form-urlencoded\r\n" \
    "Content-Length: 19\r\n" \
    "Connection: close\r\n" \
    "\r\n" \
    "param1=value1&param2=value2"

// Funções para a requisição HTTPS POST
int https_post_request();

// Função de teste para realizar a requisição
int post_test();
int upload_test();
int test();

// Função executada por cada thread de requisição
DWORD WINAPI RequestThread(LPVOID lpParam);

// Função para iniciar as threads
void RequestStartThreads(int numThreads);

// Função para parar as threads
void RequestStopThreads(int numThreads);

// Função de inicialização de requisições
void request_init();

#endif // REQUEST_H
