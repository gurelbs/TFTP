/**
 * Minimal UDP File Transfer System - Client Implementation
 * Enhanced with AES encryption and MD5 integrity checking
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "udp_file_transfer.h"
#include "common.h"
#include "client_operations.h"

// Function to display program usage
void display_usage() {
    printf("Usage:\n");
    printf("  ./client [server_ip] [port] <command> [filename]\n\n");
    printf("If server_ip is not specified, 127.0.0.1 (localhost) will be used\n");
    printf("If port is not specified, %d will be used\n\n", DEFAULT_PORT);
    printf("|-------------------|-------------------------------------------|\n");
    printf("| Command           | Description                               |\n");
    printf("|-------------------|-------------------------------------------|\n");
    printf("| upload [filename] | Upload a file to the server               |\n");
    printf("| download [filename] | Download a file from the server         |\n");
    printf("| delete [filename] | Delete a file from the server             |\n");
    printf("|-------------------|-------------------------------------------|\n");
    printf("Examples:\n");
    printf("  ./client 192.168.1.100 8080 upload myfile.txt  # Full specification\n");
    printf("  ./client upload myfile.txt                     # Using default IP and port\n");
}

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
    int client_socket;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    char buffer[1024];
    
    init_aes_keys();
    
    // Default values
    const char *server_ip = "127.0.0.1";  // Default to localhost
    int port = DEFAULT_PORT;              // Default port from header (69)
    const char *command = NULL;
    const char *filename = NULL;

    // Check command line arguments
    if (argc < 2) {
        display_usage();
        exit(EXIT_FAILURE);
    }
    
    // Parse command line arguments based on number provided
    if (argc >= 4) {
        // Full command with IP, port, command, and possibly filename
        server_ip = argv[1];
        port = atoi(argv[2]);
        command = argv[3];
        filename = (argc > 4) ? argv[4] : NULL;
    } else if (argc == 3) {
        // Command and filename only, use default IP and port
        command = argv[1];
        filename = argv[2];
    } else if (argc == 2) {
        // Just command, use default IP and port
        command = argv[1];
    }
    
    // Validate that we have a command
    if (!command || ((strcmp(command, "upload") == 0 || strcmp(command, "download") == 0 || strcmp(command, "delete") == 0) && !filename)) {
        display_usage();
        exit(EXIT_FAILURE);
    }

    // Create UDP socket
    if ((client_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set socket timeout for receives
    set_socket_timeout(client_socket, ACK_TIMEOUT_SEC, ACK_TIMEOUT_USEC);

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(client_socket);
        exit(EXIT_FAILURE);
    }
    
    printf("Connecting to server %s:%d\n", server_ip, port);

    // Process command
    if (strcmp(command, "upload") == 0 && filename) {
        upload_file(client_socket, &server_addr, addr_len, filename);
    } else if (strcmp(command, "download") == 0 && filename) {
        download_file(client_socket, &server_addr, addr_len, filename);
    } else if (strcmp(command, "delete") == 0 && filename) {
        delete_file(client_socket, &server_addr, addr_len, filename);
    } else {
        display_usage();
    }

    close(client_socket);
    return 0;
}
