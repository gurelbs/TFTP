#ifndef CLIENT_OPERATIONS_H
#define CLIENT_OPERATIONS_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

void upload_file(int client_socket, struct sockaddr_in *server_addr, socklen_t addr_len, const char *filename);
void download_file(int client_socket, struct sockaddr_in *server_addr, socklen_t addr_len, const char *filename);
void delete_file(int client_socket, struct sockaddr_in *server_addr, socklen_t addr_len, const char *filename);

#endif /* CLIENT_OPERATIONS_H */
