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

#include "udp_file_transfer.h"
#include "common.h"
#include "server_operations.h"

// Function to handle file integrity verification using MD5
int verify_file_integrity(const char *filename, unsigned char *expected_md5) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file for MD5 verification");
        return 0;
    }

    unsigned char actual_md5[MD5_DIGEST_LENGTH];
    calculate_md5(file, actual_md5);
    fclose(file);

    return memcmp(expected_md5, actual_md5, MD5_DIGEST_LENGTH) == 0;
}

int main(int argc, char *argv[]) {
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[1024];
    int port = DEFAULT_PORT;
    const char* backup_dir = "backup";  // Backup directory name
    char full_path[MAX_FILENAME_LEN + 256]; // For the complete filepath
    
    init_aes_keys();
    
    // Ensure backup directory exists
    ensure_backup_dir(backup_dir);
    
    // Parse command line arguments for port
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    // Create UDP socket
    if ((server_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    
    // Bind socket to address
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    printf("Server started on port %d\n", port);
    printf("Using AES-128 encryption for data\n");
    printf("File integrity checking with MD5\n");
    
    // Main server loop
    while (1) {
        int received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0,
                             (struct sockaddr*)&client_addr, &addr_len);
        
        if (received_bytes < 0) {
            perror("recvfrom failed");
            continue;
        }
        
        // Process packet based on opcode
        uint16_t opcode = ntohs(*(uint16_t*)buffer);
        
        if (opcode == OP_WRQ) {  // Write request
            handle_write_request(server_socket, &client_addr, addr_len, buffer, received_bytes, backup_dir);
        } else if (opcode == OP_RRQ) {  // Read request
            handle_read_request(server_socket, &client_addr, addr_len, buffer, received_bytes, backup_dir);
        } else if (opcode == OP_DELETE) {
            handle_delete_request(server_socket, &client_addr, addr_len, buffer, received_bytes, backup_dir);
        } else {
            printf("Unknown opcode: %d\n", opcode);
            
            // Send error: unknown opcode
            send_error(server_socket, &client_addr, addr_len, 
                      ERR_NOT_DEFINED, "Unknown opcode");
        }
    }
    close(server_socket);
    return 0;
}
