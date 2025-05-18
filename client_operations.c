#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include "udp_file_transfer.h"
#include "common.h"
#include "client_operations.h"

void upload_file(int client_socket, struct sockaddr_in *server_addr, socklen_t addr_len, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file for reading");
        return;
    }

    request_packet req_pkt;
    req_pkt.opcode = htons(OP_WRQ);
    strncpy(req_pkt.filename, filename, MAX_FILENAME_LEN);
    strncpy(req_pkt.mode, "octet", sizeof(req_pkt.mode));

    if (sendto(client_socket, &req_pkt, sizeof(req_pkt), 0, (struct sockaddr *)server_addr, addr_len) < 0) {
        perror("sendto failed");
        fclose(file);
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
            fclose(file);
            return;
        }

        if (sendto(client_socket, &data_pkt, bytes_read + 4, 0, (struct sockaddr *)server_addr, addr_len) < 0) {
            perror("sendto failed");
            fclose(file);
            return;
        }

        ack_packet ack_pkt;
        int received_bytes = recvfrom(client_socket, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)server_addr, &addr_len);
        if (received_bytes < 0) {
            perror("recvfrom failed");
            fclose(file);
            return;
        }

        if (ntohs(ack_pkt.opcode) != OP_ACK || ntohs(ack_pkt.block_number) != block_number + 1) {
            printf("Invalid ACK packet\n");
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

void download_file(int client_socket, struct sockaddr_in *server_addr, socklen_t addr_len, const char *filename) {
    request_packet req_pkt;
    req_pkt.opcode = htons(OP_RRQ);
    strncpy(req_pkt.filename, filename, MAX_FILENAME_LEN);
    strncpy(req_pkt.mode, "octet", sizeof(req_pkt.mode));

    if (sendto(client_socket, &req_pkt, sizeof(req_pkt), 0, (struct sockaddr *)server_addr, addr_len) < 0) {
        perror("sendto failed");
        return;
    }

    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to open file for writing");
        return;
    }

    uint16_t block_number = 0;
    while (1) {
        data_packet data_pkt;
        int received_bytes = recvfrom(client_socket, &data_pkt, sizeof(data_pkt), 0, (struct sockaddr *)server_addr, &addr_len);
        if (received_bytes < 0) {
            perror("recvfrom failed");
            fclose(file);
            return;
        }

        if (ntohs(data_pkt.opcode) != OP_DATA || ntohs(data_pkt.block_number) != block_number + 1) {
            printf("Invalid data packet\n");
            fclose(file);
            return;
        }

        fwrite(data_pkt.data, 1, received_bytes - 4, file);
        block_number++;

        ack_packet ack_pkt;
        ack_pkt.opcode = htons(OP_ACK);
        ack_pkt.block_number = htons(block_number);
        if (sendto(client_socket, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)server_addr, addr_len) < 0) {
            perror("sendto failed");
            fclose(file);
            return;
        }

        if (received_bytes < sizeof(data_pkt)) {
            break;
        }
    }

    fclose(file);
}

void delete_file(int client_socket, struct sockaddr_in *server_addr, socklen_t addr_len, const char *filename) {
    request_packet req_pkt;
    req_pkt.opcode = htons(OP_DELETE);
    strncpy(req_pkt.filename, filename, MAX_FILENAME_LEN);
    strncpy(req_pkt.mode, "octet", sizeof(req_pkt.mode));

    if (sendto(client_socket, &req_pkt, sizeof(req_pkt), 0, (struct sockaddr *)server_addr, addr_len) < 0) {
        perror("sendto failed");
        return;
    }

    ack_packet ack_pkt;
    int received_bytes = recvfrom(client_socket, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)server_addr, &addr_len);
    if (received_bytes < 0) {
        perror("recvfrom failed");
        return;
    }

    if (ntohs(ack_pkt.opcode) != OP_ACK || ntohs(ack_pkt.block_number) != 0) {
        printf("Invalid ACK packet\n");
        return;
    }

    printf("File deleted successfully\n");
}
