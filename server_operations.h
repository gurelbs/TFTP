#ifndef SERVER_OPERATIONS_H
#define SERVER_OPERATIONS_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

void handle_write_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, char *buffer, int received_bytes, const char *backup_dir);
void handle_read_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, char *buffer, int received_bytes, const char *backup_dir);
void handle_delete_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, char *buffer, int received_bytes, const char *backup_dir);
void send_error(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, uint16_t error_code, const char *error_msg);
void backup_file(const char *filename, const char *backup_dir);

#endif /* SERVER_OPERATIONS_H */
