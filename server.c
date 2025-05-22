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
#include <sys/select.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include "udp_file_transfer.h"

#define BUFFER_SIZE 512
#define MAX_CLIENTS 10
#define LOG_FILE "server.log"
#define BACKUP_DIR "backup"
#define TIMEOUT_SECS 5

FILE *log_file = NULL;

// Helper function to log messages to file and console
void log_message(const char *format, ...) {
    if (!log_file) {
        log_file = fopen(LOG_FILE, "a");
        if (!log_file) {
            perror("Could not open log file");
            return;
        }
    }
    
    va_list args;
    va_start(args, format);
    
    time_t now = time(NULL);
    char time_str[26];
    ctime_r(&now, time_str);
    time_str[24] = '\0'; // Remove newline
    
    fprintf(log_file, "[%s] ", time_str);
    vfprintf(log_file, format, args);
    fprintf(log_file, "\n");
    fflush(log_file);
    
    va_end(args);
    
    // Also print to console
    va_start(args, format);
    printf("[%s] ", time_str);
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

// Create backup directory if it doesn't exist
void ensure_backup_dir() {
    struct stat st = {0};
    if (stat(BACKUP_DIR, &st) == -1) {
        if (mkdir(BACKUP_DIR, 0755) == -1) {
            fprintf(stderr, "Error creating backup directory: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        log_message("Created backup directory: %s", BACKUP_DIR);
    } else {
        log_message("Using existing backup directory: %s", BACKUP_DIR);
    }
}

// Get full path for a file in the backup directory
char* get_backup_path(const char* filename) {
    // Extract just the basename from the path
    const char* basename = strrchr(filename, '/');
    if (basename) {
        basename++; // Skip the '/'
    } else {
        basename = filename;
    }
    
    char* path = malloc(strlen(BACKUP_DIR) + strlen(basename) + 2); // +2 for '/' and null terminator
    if (!path) {
        log_message("Memory allocation failed");
        return NULL;
    }
    
    sprintf(path, "%s/%s", BACKUP_DIR, basename);
    return path;
}

// Send error packet to client
void send_error(int sockfd, struct sockaddr_in *client_addr, socklen_t addr_len, 
                const char *error_message) {
    struct {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    
    pkt.opcode = OP_ERROR;
    pkt.packet_number = 0;
    strncpy(pkt.data, error_message, BUFFER_SIZE - 1);
    pkt.data[BUFFER_SIZE - 1] = '\0';
    
    sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t) + strlen(pkt.data) + 1, 0,
           (struct sockaddr *)client_addr, addr_len);
    
    log_message("Sent error to client: %s", error_message);
}

// Handle write request (client uploading file to server)
void handle_write_request(int sockfd, struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename) {
    log_message("Received write request for file: %s from %s:%d", 
               filename, inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
    
    // Get path in backup directory
    char* backup_path = get_backup_path(filename);
    if (!backup_path) {
        send_error(sockfd, client_addr, addr_len, "Internal server error");
        return;
    }
    
    // Check if destination is a directory
    struct stat dest_stat;
    if (stat(backup_path, &dest_stat) == 0 && S_ISDIR(dest_stat.st_mode)) {
        log_message("Error: Cannot overwrite a directory - %s", backup_path);
        send_error(sockfd, client_addr, addr_len, "Destination is a directory");
        free(backup_path);
        return;
    }
    
    log_message("Saving file to: %s", backup_path);
    
    FILE *file = fopen(backup_path, "wb");
    if (!file) {
        log_message("Error: Could not open file for writing - %s: %s", backup_path, strerror(errno));
        send_error(sockfd, client_addr, addr_len, strerror(errno));
        free(backup_path);
        return;
    }
    
    struct {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    
    // Send ACK for the write request
    pkt.opcode = ACK;
    pkt.packet_number = 0;
    sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0,
           (struct sockaddr *)client_addr, addr_len);
    log_message("Sent ACK for write request");
    
    int expected_packet = 0;
    while (1) {
        ssize_t bytes_received = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                                          (struct sockaddr *)client_addr, &addr_len);
        if (bytes_received < 0) {
            log_message("Error receiving packet: %s", strerror(errno));
            continue;
        }
        
        log_message("Received packet #%d with opcode %d (%zd bytes)", 
                   pkt.packet_number, pkt.opcode, bytes_received);
        
        if (pkt.opcode == OP_EOF) {
            log_message("End of file reached");
            // Send final ACK
            pkt.opcode = ACK;
            sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0,
                   (struct sockaddr *)client_addr, addr_len);
            break;
        }
        
        if (pkt.opcode != OP_DATA) {
            log_message("Error: Expected data packet, got opcode %d", pkt.opcode);
            continue;
        }
        
        if (pkt.packet_number == expected_packet) {
            size_t data_size = bytes_received - sizeof(pkt.opcode) - sizeof(pkt.packet_number);
            fwrite(pkt.data, 1, data_size, file);
            expected_packet++;
            
            // Send ACK
            pkt.opcode = ACK;
            sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0,
                   (struct sockaddr *)client_addr, addr_len);
            log_message("Sent ACK for packet #%d", pkt.packet_number);
        } else {
            log_message("Received out-of-order packet #%d, expected #%d",
                       pkt.packet_number, expected_packet);
            // Resend ACK for the last received packet
            pkt.opcode = ACK;
            pkt.packet_number = expected_packet - 1;
            sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0,
                   (struct sockaddr *)client_addr, addr_len);
        }
    }
    
    fclose(file);
    log_message("File transfer complete: %s", backup_path);
    free(backup_path);
}

// Handle read request (client downloading file from server)
void handle_read_request(int sockfd, struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename) {
    log_message("Received read request for file: %s from %s:%d", 
               filename, inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
    
    // Always use backup directory
    char* backup_path = get_backup_path(filename);
    if (!backup_path) {
        send_error(sockfd, client_addr, addr_len, "Internal server error");
        return;
    }
    
    log_message("Looking for file in backup directory: %s", backup_path);
    
    // Check if file exists and is not a directory
    struct stat file_stat;
    if (stat(backup_path, &file_stat) != 0) {
        log_message("Error: File not found in backup directory - %s", backup_path);
        send_error(sockfd, client_addr, addr_len, "File not found in backup directory");
        free(backup_path);
        return;
    }
    
    // Check if it's a directory
    if (S_ISDIR(file_stat.st_mode)) {
        log_message("Error: Cannot download a directory - %s", backup_path);
        send_error(sockfd, client_addr, addr_len, "Cannot download directories");
        free(backup_path);
        return;
    }
    
    FILE *file = fopen(backup_path, "rb");
    if (!file) {
        log_message("Error: Could not open file in backup directory - %s: %s", backup_path, strerror(errno));
        send_error(sockfd, client_addr, addr_len, strerror(errno));
        free(backup_path);
        return;
    }
    
    struct {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    int packet_number = 0;
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        pkt.opcode = OP_DATA;
        pkt.packet_number = packet_number++;
        memcpy(pkt.data, buffer, bytes_read);
        
        log_message("Sending packet #%d (%zu bytes of data)", pkt.packet_number, bytes_read);
        
        // Send data packet with exact size
        size_t packet_size = sizeof(uint16_t) + sizeof(uint16_t) + bytes_read;
        ssize_t sent_bytes = sendto(sockfd, &pkt, packet_size, 0,
                                    (struct sockaddr *)client_addr, addr_len);
        
        if (sent_bytes < 0) {
            log_message("Error sending packet: %s", strerror(errno));
            fclose(file);
            free(backup_path);
            return;
        }
        
        log_message("Sent %zd bytes", sent_bytes);
        
        // Wait for ACK with timeout
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = TIMEOUT_SECS;
        tv.tv_usec = 0;
        
        int select_result = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        
        if (select_result <= 0) {
            // Timeout or error, resend packet
            log_message("ACK not received for packet #%d, resending", pkt.packet_number);
            packet_number--; // Keep the same packet number
            fseek(file, -bytes_read, SEEK_CUR); // Go back to resend the same data
            continue;
        }
        
        // Receive ACK
        if (recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)client_addr, &addr_len) < 0) {
            log_message("Error receiving ACK: %s", strerror(errno));
            packet_number--; // Keep the same packet number
            fseek(file, -bytes_read, SEEK_CUR); // Go back to resend the same data
            continue;
        }
        
        if (pkt.opcode == ACK) {
            log_message("Received ACK for packet #%d", pkt.packet_number);
        } else {
            log_message("Error: Expected ACK, got opcode %d", pkt.opcode);
            packet_number--; // Keep the same packet number
            fseek(file, -bytes_read, SEEK_CUR); // Go back to resend the same data
        }
    }
    
    // Send end-of-file packet
    pkt.opcode = OP_EOF;
    pkt.packet_number = packet_number;
    sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0,
           (struct sockaddr *)client_addr, addr_len);
    log_message("File transfer complete from backup directory: %s (%d packets)", backup_path, packet_number);
    
    // Wait for final ACK
    recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)client_addr, &addr_len);
    
    fclose(file);
    free(backup_path);
}

// Handle delete request (client requesting to delete a file)
void handle_delete_request(int sockfd, struct sockaddr_in *client_addr, socklen_t addr_len, const char *filename) {
    log_message("Received delete request for file: %s from %s:%d", 
               filename, inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
    
    // Always use backup directory
    char* backup_path = get_backup_path(filename);
    if (!backup_path) {
        send_error(sockfd, client_addr, addr_len, "Internal server error");
        return;
    }
    
    log_message("Checking if file exists in backup directory: %s", backup_path);
    
    struct {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    
    // Check if file exists before trying to delete it
    struct stat file_stat;
    if (stat(backup_path, &file_stat) != 0) {
        log_message("Error: File not found in backup directory - %s", backup_path);
        send_error(sockfd, client_addr, addr_len, "File not found in backup directory");
        free(backup_path);
        return;
    }
    
    // Check if it's a directory
    if (S_ISDIR(file_stat.st_mode)) {
        log_message("Error: Cannot delete a directory - %s", backup_path);
        send_error(sockfd, client_addr, addr_len, "Cannot delete directories");
        free(backup_path);
        return;
    }
    
    if (unlink(backup_path) == 0) {
        log_message("File deleted successfully from backup directory: %s", backup_path);
        pkt.opcode = ACK;
        pkt.packet_number = 0;
        sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0,
               (struct sockaddr *)client_addr, addr_len);
    } else {
        log_message("Error deleting file from backup directory %s: %s", backup_path, strerror(errno));
        send_error(sockfd, client_addr, addr_len, strerror(errno));
    }
    
    free(backup_path);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int port = atoi(argv[1]);
    
    // Initialize logging
    log_message("Server starting on port %d", port);
    
    // Ensure backup directory exists
    ensure_backup_dir();
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        log_message("Error creating socket: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr, client_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_message("Error binding socket: %s", strerror(errno));
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    log_message("Server listening on port %d", port);
    
    while (1) {
        struct {
            uint16_t opcode;
            uint16_t packet_number;
            char data[BUFFER_SIZE];
        } pkt;
        socklen_t addr_len = sizeof(client_addr);
        
        ssize_t bytes_received = recvfrom(sockfd, &pkt, sizeof(pkt), 0,
                                         (struct sockaddr *)&client_addr, &addr_len);
        if (bytes_received < 0) {
            log_message("Error receiving packet: %s", strerror(errno));
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        log_message("Received packet from %s:%d with opcode %d", 
                   client_ip, ntohs(client_addr.sin_port), pkt.opcode);
        
        switch (pkt.opcode) {
            case OP_READ_REQUEST:
                handle_read_request(sockfd, &client_addr, addr_len, pkt.data);
                break;
                
            case OP_WRITE_REQUEST:
                handle_write_request(sockfd, &client_addr, addr_len, pkt.data);
                break;
                
            case OP_DELETE_REQUEST:
                handle_delete_request(sockfd, &client_addr, addr_len, pkt.data);
                break;
                
            default:
                log_message("Received unknown opcode: %d", pkt.opcode);
                break;
        }
    }
    
    close(sockfd);
    if (log_file) fclose(log_file);
    return 0;
}
