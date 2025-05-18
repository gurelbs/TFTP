/**
 * Minimal UDP File Transfer System - Server Implementation
 * Enhanced with AES encryption and MD5 integrity checking
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>

// OpenSSL libraries for encryption and hashing
#include <openssl/aes.h>
#include <openssl/md5.h>

#include "udp_file_transfer.h"
#include "common.h"

// Function to send an error packet
void send_error(int socket, struct sockaddr_in *client_addr, socklen_t addr_len, uint16_t error_code, const char *error_message) {
    error_packet error;
    error.opcode = htons(OP_ERROR);
    error.error_code = htons(error_code);
    strncpy(error.error_msg, error_message, sizeof(error.error_msg) - 1);
    error.error_msg[sizeof(error.error_msg) - 1] = '\0'; // Ensure null termination
    sendto(socket, &error, sizeof(error), 0, (struct sockaddr*)client_addr, addr_len);
}

// Function to ensure backup directory exists
void ensure_backup_dir(const char* backup_dir) {
    struct stat st = {0};
    
    // Check if directory exists
    if (stat(backup_dir, &st) == -1) {
        // Create directory with rwxr-xr-x permissions (755)
        if (mkdir(backup_dir, 0755) == -1) {
            perror("Failed to create backup directory");
            exit(EXIT_FAILURE);
        }
        printf("Created backup directory: %s\n", backup_dir);
    }
}

// Function to create complete filepath in the backup directory
void create_backup_path(char* full_path, const char* backup_dir, const char* filename) {
    strcpy(full_path, backup_dir);
    strcat(full_path, "/");
    strcat(full_path, filename);
}

// Function to handle write requests
void handle_write_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename, const char *backup_dir) {
    char full_path[MAX_FILENAME_LEN + 256];
    create_backup_path(full_path, backup_dir, filename);
    printf("Saving to: %s\n", full_path);

    FILE* file = fopen(full_path, "wb");
    if (!file) {
        send_error(server_socket, client_addr, addr_len, ERR_ACCESS_DENIED, "Cannot create file");
        return;
    }

    ack_packet ack;
    ack.opcode = htons(OP_ACK);
    ack.block_number = htons(0);
    sendto(server_socket, &ack, sizeof(ack), 0, (struct sockaddr*)client_addr, addr_len);

    unsigned char decrypted_data[DATA_BLOCK_SIZE + AES_BLOCK_SIZE];
    uint16_t expected_block = 1;
    char buffer[1024];
    int received_bytes;
    uint16_t opcode;

    while (1) {
        received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)client_addr, &addr_len);
        if (received_bytes < 0) {
            perror("recvfrom failed");
            break;
        }

        opcode = ntohs(*(uint16_t*)buffer);
        if (opcode == OP_DATA) {
            data_packet* data = (data_packet*)buffer;
            uint16_t block = ntohs(data->block_number);
            printf("Received block #%d\n", block);

            if (block == expected_block) {
                int encrypted_size = received_bytes - 4;
                int decrypted_size = decrypt_data((unsigned char*)data->data, encrypted_size, decrypted_data);
                size_t bytes_written = fwrite(decrypted_data, 1, decrypted_size, file);
                if (bytes_written != (size_t)decrypted_size) {
                    perror("Error writing to file");
                    send_error(server_socket, client_addr, addr_len, ERR_DISK_FULL, "Failed to write to file");
                    fclose(file);
                    remove(full_path);
                    break;
                }
                fflush(file);

                ack.opcode = htons(OP_ACK);
                ack.block_number = htons(block);
                sendto(server_socket, &ack, sizeof(ack), 0, (struct sockaddr*)client_addr, addr_len);

                printf("Decrypted block #%d, size: %d\n", block, decrypted_size);
                expected_block++;

                if (encrypted_size < DATA_BLOCK_SIZE) {
                    printf("Last data block received\n");
                }
            } else if (block > expected_block) {
                ack.opcode = htons(OP_ACK);
                ack.block_number = htons(expected_block - 1);
                sendto(server_socket, &ack, sizeof(ack), 0, (struct sockaddr*)client_addr, addr_len);
                printf("Duplicate block received, sent ACK for block #%d\n", expected_block - 1);
            } else {
                printf("Out of order block received, expected block #%d\n", expected_block);
            }
        } else if (opcode == OP_VERIFY) {
            verify_packet* verify = (verify_packet*)buffer;
            unsigned char client_md5[MD5_DIGEST_LENGTH];
            memcpy(client_md5, verify->md5_hash, MD5_DIGEST_LENGTH);

            printf("Received MD5 hash: ");
            for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
                printf("%02x", client_md5[i]);
            }
            printf("\n");

            fflush(file);
            unsigned char calculated_md5[MD5_DIGEST_LENGTH];
            fclose(file);
            file = fopen(full_path, "rb");
            if (!file) {
                send_error(server_socket, client_addr, addr_len, ERR_ACCESS_DENIED, "Cannot reopen file for verification");
                remove(full_path);
                printf("Error reopening file for verification\n");
                break;
            }

            calculate_md5(file, calculated_md5);

            printf("Calculated MD5 hash: ");
            for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
                printf("%02x", calculated_md5[i]);
            }
            printf("\n");

            int match = 1;
            for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
                if (client_md5[i] != calculated_md5[i]) {
                    match = 0;
                    break;
                }
            }

            if (match) {
                printf("File integrity verified (MD5 hash matched)\n");
                ack.opcode = htons(OP_ACK);
                ack.block_number = htons(0);
                sendto(server_socket, &ack, sizeof(ack), 0, (struct sockaddr*)client_addr, addr_len);
            } else {
                printf("File integrity verification FAILED!\n");
                send_error(server_socket, client_addr, addr_len, ERR_VERIFICATION, "MD5 hash mismatch - file corrupted");
                fclose(file);
                remove(full_path);
                printf("Corrupted file deleted\n");
                break;
            }

            fclose(file);
            printf("Upload complete and verified\n");
            break;
        } else if (opcode == OP_ERROR) {
            error_packet *error = (error_packet*)buffer;
            printf("Error from client: %s\n", error->error_msg);
            fclose(file);
            break;
        } else {
            printf("Unexpected packet type: %d\n", opcode);
            break;
        }
    }
}

// Function to handle read requests
void handle_read_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename, const char *backup_dir) {
    char full_path[MAX_FILENAME_LEN + 256];
    create_backup_path(full_path, backup_dir, filename);
    printf("Reading from: %s\n", full_path);

    FILE* file = fopen(full_path, "rb");
    if (!file) {
        send_error(server_socket, client_addr, addr_len, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    unsigned char md5_digest[MD5_DIGEST_LENGTH];
    calculate_md5(file, md5_digest);

    printf("File MD5: ");
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        printf("%02x", md5_digest[i]);
    }
    printf("\n");

    unsigned char plaintext[DATA_BLOCK_SIZE];
    uint16_t block_number = 1;
    data_packet data;
    int bytes_read;
    char buffer[1024];
    int received_bytes;
    uint16_t opcode;

    while (1) {
        data.opcode = htons(OP_DATA);
        data.block_number = htons(block_number);

        bytes_read = fread(plaintext, 1, DATA_BLOCK_SIZE, file);
        if (bytes_read < 0) {
            perror("File read error");
            break;
        }

        int encrypted_size = encrypt_data(plaintext, bytes_read, (unsigned char*)data.data);
        sendto(server_socket, &data, 4 + encrypted_size, 0, (struct sockaddr*)client_addr, addr_len);

        printf("Sent block #%d, size: %d (encrypted: %d)\n", block_number, bytes_read, encrypted_size);

        received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)client_addr, &addr_len);
        if (received_bytes < 0) {
            perror("ACK receive error");
            break;
        }

        ack_packet* ack = (ack_packet*)buffer;
        if (ntohs(ack->opcode) != OP_ACK || ntohs(ack->block_number) != block_number) {
            printf("Invalid ACK\n");
            break;
        }

        block_number++;

        if (bytes_read < DATA_BLOCK_SIZE) {
            verify_packet verify;
            verify.opcode = htons(OP_VERIFY);
            memcpy(verify.md5_hash, md5_digest, MD5_DIGEST_LENGTH);

            printf("Sending MD5 verification...\n");
            sendto(server_socket, &verify, sizeof(verify), 0, (struct sockaddr*)client_addr, addr_len);

            received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)client_addr, &addr_len);
            if (received_bytes > 0) {
                opcode = ntohs(*(uint16_t*)buffer);
                if (opcode == OP_ACK) {
                    printf("Client verified file integrity\n");
                } else if (opcode == OP_ERROR) {
                    error_packet* error = (error_packet*)buffer;
                    printf("Client reported verification error: %s\n", error->error_msg);
                }
            }

            printf("Download complete\n");
            break;
        }
    }

    fclose(file);
}

// Function to handle delete requests
void handle_delete_request(int server_socket, struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename, const char *backup_dir) {
    char full_path[MAX_FILENAME_LEN + 256];
    create_backup_path(full_path, backup_dir, filename);

    if (remove(full_path) == 0) {
        printf("File deleted successfully: %s\n", full_path);

        ack_packet ack;
        ack.opcode = htons(OP_ACK);
        ack.block_number = htons(0);

        sendto(server_socket, &ack, sizeof(ack), 0, (struct sockaddr*)client_addr, addr_len);
    } else {
        printf("Error deleting file: %s\n", strerror(errno));
        send_error(server_socket, client_addr, addr_len, ERR_ACCESS_DENIED, "Could not delete file");
    }
}

int main(int argc, char *argv[]) {
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[1024];
    int port = DEFAULT_PORT;
    const char* backup_dir = "backup";

    init_aes_keys();
    ensure_backup_dir(backup_dir);

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    if ((server_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server started on port %d\n", port);
    printf("Using AES-128 encryption for data\n");
    printf("File integrity checking with MD5\n");

    while (1) {
        int received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
        if (received_bytes < 0) {
            perror("recvfrom failed");
            continue;
        }

        uint16_t opcode = ntohs(*(uint16_t*)buffer);
        request_packet* req = (request_packet*)buffer;

        switch (opcode) {
            case OP_WRQ:
                printf("Received write request for file: %s\n", req->filename);
                handle_write_request(server_socket, &client_addr, addr_len, req->filename, backup_dir);
                break;
            case OP_RRQ:
                printf("Received read request for file: %s\n", req->filename);
                handle_read_request(server_socket, &client_addr, addr_len, req->filename, backup_dir);
                break;
            case OP_DELETE:
                printf("Received delete request for file: %s\n", req->filename);
                handle_delete_request(server_socket, &client_addr, addr_len, req->filename, backup_dir);
                break;
            default:
                printf("Unknown opcode: %d\n", opcode);
                send_error(server_socket, &client_addr, addr_len, ERR_NOT_DEFINED, "Unknown opcode");
                break;
        }
    }

    close(server_socket);
    return 0;
}
