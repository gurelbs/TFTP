#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>
#include "udp_file_transfer.h"
#include "common.h"
#include "server_operations.h"

void handle_write_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, char *buffer, int received_bytes, const char *backup_dir) {
    request_packet *req = (request_packet *)buffer;
    char filename[MAX_FILENAME_LEN];
    strncpy(filename, req->filename, MAX_FILENAME_LEN);
    filename[MAX_FILENAME_LEN - 1] = '\0';

    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to open file for writing");
        send_error(server_socket, client_addr, addr_len, ERR_ACCESS_DENIED, "Failed to open file for writing");
        return;
    }

    uint16_t block_number = 0;
    int retries = 0;
    while (1) {
        data_packet data_pkt;
        int received_bytes = recvfrom(server_socket, &data_pkt, sizeof(data_pkt), 0, (struct sockaddr *)client_addr, &addr_len);
        if (received_bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++retries >= MAX_RETRIES) {
                    perror("Max retries reached");
                    send_error(server_socket, client_addr, addr_len, ERR_TRANSMISSION, "Max retries reached");
                    fclose(file);
                    return;
                }
                continue;
            } else {
                perror("recvfrom failed");
                send_error(server_socket, client_addr, addr_len, ERR_TRANSMISSION, "recvfrom failed");
                fclose(file);
                return;
            }
        }

        if (ntohs(data_pkt.opcode) != OP_DATA || ntohs(data_pkt.block_number) != block_number + 1) {
            send_error(server_socket, client_addr, addr_len, ERR_NOT_DEFINED, "Invalid data packet");
            fclose(file);
            return;
        }

        fwrite(data_pkt.data, 1, received_bytes - 4, file);
        block_number++;

        ack_packet ack_pkt;
        ack_pkt.opcode = htons(OP_ACK);
        ack_pkt.block_number = htons(block_number);
        sendto(server_socket, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)client_addr, addr_len);

        if (received_bytes < sizeof(data_pkt)) {
            break;
        }
    }

    fclose(file);
    backup_file(filename, backup_dir);
}

void handle_read_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, char *buffer, int received_bytes, const char *backup_dir) {
    request_packet *req = (request_packet *)buffer;
    char filename[MAX_FILENAME_LEN];
    strncpy(filename, req->filename, MAX_FILENAME_LEN);
    filename[MAX_FILENAME_LEN - 1] = '\0';

    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file for reading");
        send_error(server_socket, client_addr, addr_len, ERR_FILE_NOT_FOUND, "Failed to open file for reading");
        return;
    }

    uint16_t block_number = 0;
    while (1) {
        data_packet data_pkt;
        data_pkt.opcode = htons(OP_DATA);
        data_pkt.block_number = htons(block_number + 1);
        int bytes_read = fread(data_pkt.data, 1, DATA_BLOCK_SIZE, file);

        if (bytes_read < 0) {
            perror("fread failed");
            send_error(server_socket, client_addr, addr_len, ERR_NOT_DEFINED, "fread failed");
            fclose(file);
            return;
        }

        int sent_bytes = sendto(server_socket, &data_pkt, bytes_read + 4, 0, (struct sockaddr *)client_addr, addr_len);
        if (sent_bytes < 0) {
            perror("sendto failed");
            send_error(server_socket, client_addr, addr_len, ERR_TRANSMISSION, "sendto failed");
            fclose(file);
            return;
        }

        ack_packet ack_pkt;
        int received_bytes = recvfrom(server_socket, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)client_addr, &addr_len);
        if (received_bytes < 0) {
            perror("recvfrom failed");
            send_error(server_socket, client_addr, addr_len, ERR_TRANSMISSION, "recvfrom failed");
            fclose(file);
            return;
        }

        if (ntohs(ack_pkt.opcode) != OP_ACK || ntohs(ack_pkt.block_number) != block_number + 1) {
            send_error(server_socket, client_addr, addr_len, ERR_NOT_DEFINED, "Invalid ACK packet");
            fclose(file);
            return;
        }

        block_number++;
        if (bytes_read < DATA_BLOCK_SIZE) {
            break;
        }
    }

    fclose(file);
}

void handle_delete_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, char *buffer, int received_bytes, const char *backup_dir) {
    request_packet *req = (request_packet *)buffer;
    char filename[MAX_FILENAME_LEN];
    strncpy(filename, req->filename, MAX_FILENAME_LEN);
    filename[MAX_FILENAME_LEN - 1] = '\0';

    if (remove(filename) < 0) {
        perror("Failed to delete file");
        send_error(server_socket, client_addr, addr_len, ERR_ACCESS_DENIED, "Failed to delete file");
        return;
    }

    ack_packet ack_pkt;
    ack_pkt.opcode = htons(OP_ACK);
    ack_pkt.block_number = 0;
    sendto(server_socket, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)client_addr, addr_len);
}

void send_error(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, uint16_t error_code, const char *error_msg) {
    error_packet err_pkt;
    err_pkt.opcode = htons(OP_ERROR);
    err_pkt.error_code = htons(error_code);
    strncpy(err_pkt.error_msg, error_msg, sizeof(err_pkt.error_msg) - 1);
    err_pkt.error_msg[sizeof(err_pkt.error_msg) - 1] = '\0';
    sendto(server_socket, &err_pkt, sizeof(err_pkt), 0, (struct sockaddr *)client_addr, addr_len);
}

void backup_file(const char *filename, const char *backup_dir) {
    char backup_filename[MAX_FILENAME_LEN + 256];
    snprintf(backup_filename, sizeof(backup_filename), "%s/%s", backup_dir, filename);

    FILE *src = fopen(filename, "rb");
    if (!src) {
        perror("Failed to open source file for backup");
        return;
    }

    FILE *dest = fopen(backup_filename, "wb");
    if (!dest) {
        perror("Failed to open destination file for backup");
        fclose(src);
        return;
    }

    char buffer[1024];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dest);
    }

    fclose(src);
    fclose(dest);
}
