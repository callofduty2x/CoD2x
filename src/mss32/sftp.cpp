#include "sftp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../shared/cod2.h"

int sftp_upload(const char *host, int port, const char *username, const char *password, const char *local_path, const char *remote_path) {
    int sock;
    struct sockaddr_in sin;
    LIBSSH2_SESSION *session;
    LIBSSH2_SFTP *sftp_session;
    LIBSSH2_SFTP_HANDLE *remote_file;
    FILE *local_file;
    char buffer[1024];
    size_t nread;
    int rc;

    // Inicializar Winsock no Windows
    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
            Com_Printf("Erro ao inicializar o Winsock\n");
            return -1;
        }
    #endif

    if (libssh2_init(0) != 0) {
        Com_Printf("Falha ao inicializar a libssh2\n");
        return -1;
    }

    session = libssh2_session_init();
    if (!session) {
        Com_Printf("Falha ao criar a sessão SSH\n");
        return -1;
    }

    // Criar socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Falha ao criar o socket");
        Com_Printf("Falha ao criar o socket\n");
        return -1;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);

    if (connect(sock, (struct sockaddr *) &sin, sizeof(struct sockaddr_in)) != 0) {
        perror("Falha na conexão");
        Com_Printf("Falha na conexão\n");
        return -1;
    }

    rc = libssh2_session_handshake(session, sock);
    if (rc) {
        Com_Printf("Falha no handshake SSH: %d\n", rc);
        return -1;
    }

    rc = libssh2_userauth_password(session, username, password);
    if (rc) {
        Com_Printf("Falha na autenticação SSH: %d\n", rc);
        return -1;
    }

    sftp_session = libssh2_sftp_init(session);
    if (!sftp_session) {
        Com_Printf("Falha ao inicializar o SFTP\n");
        return -1;
    }

    local_file = fopen(local_path, "rb");
    if (!local_file) {
        Com_Printf("Falha ao abrir o arquivo local: %s\n", local_path);
        return -1;
    }

    remote_file = libssh2_sftp_open(sftp_session, remote_path,
                                     LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                     LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
    if (!remote_file) {
        Com_Printf("Falha ao abrir o arquivo remoto: %s\n", remote_path);
        return -1;
    }

    while ((nread = fread(buffer, 1, sizeof(buffer), local_file)) > 0) {
        size_t nwritten = 0;
        while (nwritten < nread) {
            rc = libssh2_sftp_write(remote_file, buffer + nwritten, nread - nwritten);
            if (rc < 0) {
                Com_Printf("Erro ao escrever no arquivo remoto: %d\n", rc);
                return -1;
            }
            nwritten += rc;
        }
    }

    if (ferror(local_file)) {
        Com_Printf("Erro na leitura do arquivo local\n");
        return -1;
    }

    fclose(local_file);
    libssh2_sftp_close(remote_file);
    libssh2_sftp_shutdown(sftp_session);
    libssh2_session_disconnect(session, "Fechando a sessão SSH");
    libssh2_session_free(session);

    #ifdef _WIN32
        closesocket(sock);
        WSACleanup();
    #else
        close(sock);
    #endif

    libssh2_exit();

    Com_Printf("Screenshot uploaded\n");
    return 0;
}
