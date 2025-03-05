#ifndef SFTP_H
#define SFTP_H

#include <libssh2.h>
#include <libssh2_sftp.h>

int sftp_upload(const char *host, int port, const char *username, const char *password, const char *local_path, const char *remote_path);

#endif // SFTP_H
