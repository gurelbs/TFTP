#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "udp_file_transfer.h"

#define BUFFER_SIZE 512
#define TIMEOUT 2
#define LOG_FILE "client.log"

FILE *log_file = NULL;

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

void send_file(int sockfd, struct sockaddr_in *server_addr, const char *filename) {
    // Check if file is a directory
    struct stat file_stat;
    if (stat(filename, &file_stat) != 0) {
        log_message("Error: File not found - %s", filename);
        return;
    }
    
    if (S_ISDIR(file_stat.st_mode)) {
        log_message("Error: Cannot upload directories - %s", filename);
        return;
    }
    
    FILE *file = fopen(filename, "rb");
    if (!file) {
        log_message("Error: Could not open file - %s: %s", filename, strerror(errno));
        return;
    }
    
    log_message("Sending file: %s", filename);
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    int packet_number = 0;
    struct udp_packet_t {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    socklen_t addr_len = sizeof(*server_addr);
    
    // Send initial file transfer request
    pkt.opcode = OP_WRITE_REQUEST;
    pkt.packet_number = 0;
    strncpy(pkt.data, filename, BUFFER_SIZE - 1);
    sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)server_addr, addr_len);
    log_message("Sent write request for file: %s", filename);
    
    // Wait for acknowledgment of the request
    if (recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)server_addr, &addr_len) < 0) {
        log_message("Error: Initial write request not acknowledged");
        fclose(file);
        return;
    }
    
    if (pkt.opcode != ACK) {
        log_message("Error: Expected ACK for write request, got opcode %d", pkt.opcode);
        fclose(file);
        return;
    }
    
    log_message("Write request acknowledged, starting file transfer");
    
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        pkt.opcode = OP_DATA;
        pkt.packet_number = packet_number++;
        memcpy(pkt.data, buffer, bytes_read);
        
        log_message("Sending packet #%d (%zu bytes)", pkt.packet_number, bytes_read);
        sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t) + bytes_read, 0, 
               (struct sockaddr *)server_addr, addr_len);
        
        // Wait for ACK
        if (recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)server_addr, &addr_len) < 0) {
            log_message("ACK not received for packet #%d, resending", packet_number - 1);
            fseek(file, -bytes_read, SEEK_CUR); // Resend the same packet
            packet_number--; // Keep the same packet number
        } else if (pkt.opcode == ACK) {
            log_message("Received ACK for packet #%d", pkt.packet_number);
        } else {
            log_message("Error: Expected ACK, got opcode %d", pkt.opcode);
        }
    }
    
    // Send end-of-file packet
    pkt.opcode = OP_EOF;
    pkt.packet_number = packet_number;
    sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0, 
           (struct sockaddr *)server_addr, addr_len);
    log_message("File transfer complete, sent %d packets", packet_number);
    
    fclose(file);
}

void receive_file(int sockfd, struct sockaddr_in *server_addr, const char *filename) {
    // Check if the destination is a directory
    struct stat file_stat;
    if (stat(filename, &file_stat) == 0 && S_ISDIR(file_stat.st_mode)) {
        log_message("Error: Cannot download to a directory - %s", filename);
        return;
    }
    
    FILE *file = fopen(filename, "wb");
    if (!file) {
        log_message("Error: Could not open file for writing - %s: %s", filename, strerror(errno));
        return;
    }
    
    log_message("Requesting file: %s from backup directory", filename);
    
    struct udp_packet_t {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    socklen_t addr_len = sizeof(*server_addr);
    int expected_packet_number = 0;
    
    // Send initial read request
    pkt.opcode = OP_READ_REQUEST;
    pkt.packet_number = 0;
    strncpy(pkt.data, filename, BUFFER_SIZE - 1);
    pkt.data[BUFFER_SIZE - 1] = '\0';
    sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t) + strlen(pkt.data) + 1, 0, 
           (struct sockaddr *)server_addr, addr_len);
    log_message("Sent read request for file: %s", filename);
    
    while (1) {
        ssize_t bytes_received = recvfrom(sockfd, &pkt, sizeof(pkt), 0, 
                                          (struct sockaddr *)server_addr, &addr_len);
        if (bytes_received < 0) {
            log_message("Error receiving packet: %s", strerror(errno));
            break;
        }
        
        log_message("Received packet with opcode %d, packet #%d (%zd bytes)", 
                   pkt.opcode, pkt.packet_number, bytes_received);
        
        if (pkt.opcode == OP_ERROR) {
            log_message("Server error: %s", pkt.data);
            fclose(file);
            unlink(filename);  // Delete the empty file
            return;
        }
        
        if (pkt.opcode == OP_EOF) {
            log_message("End of file reached");
            // Send final ACK
            pkt.opcode = ACK;
            sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0, 
                   (struct sockaddr *)server_addr, addr_len);
            break;
        }
        
        if (pkt.opcode != OP_DATA) {
            log_message("Error: Expected data packet, got opcode %d", pkt.opcode);
            continue;
        }
        
        if (pkt.packet_number == expected_packet_number) {
            // Calculate the actual data size in this packet
            size_t data_size = bytes_received - sizeof(pkt.opcode) - sizeof(pkt.packet_number);
            log_message("Writing %zu bytes to file", data_size);
            
            // Write data to file
            if (fwrite(pkt.data, 1, data_size, file) != data_size) {
                log_message("Error writing to file: %s", strerror(errno));
                break;
            }
            
            expected_packet_number++;
            
            // Send ACK
            pkt.opcode = ACK;
            sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0, 
                   (struct sockaddr *)server_addr, addr_len);
            log_message("Sent ACK for packet #%d", pkt.packet_number);
        } else {
            log_message("Received out-of-order packet #%d, expected #%d", 
                       pkt.packet_number, expected_packet_number);
            // Resend ACK for the last received packet
            pkt.opcode = ACK;
            pkt.packet_number = expected_packet_number - 1;
            sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t), 0, 
                   (struct sockaddr *)server_addr, addr_len);
        }
    }
    
    fclose(file);
    log_message("File received: %s", filename);
}

void delete_file(int sockfd, struct sockaddr_in *server_addr, const char *filename) {
    log_message("Requesting deletion of file: %s from backup directory", filename);
    
    struct udp_packet_t {
        uint16_t opcode;
        uint16_t packet_number;
        char data[BUFFER_SIZE];
    } pkt;
    socklen_t addr_len = sizeof(*server_addr);
    
    // Fix the packet structure
    pkt.opcode = OP_DELETE_REQUEST;
    pkt.packet_number = 0;
    strncpy(pkt.data, filename, BUFFER_SIZE - 1);
    pkt.data[BUFFER_SIZE - 1] = '\0';
    
    // Send the full packet with filename in data field
    sendto(sockfd, &pkt, sizeof(uint16_t) + sizeof(uint16_t) + strlen(pkt.data) + 1, 0, 
           (struct sockaddr *)server_addr, addr_len);
    
    // Wait for acknowledgment
    if (recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)server_addr, &addr_len) < 0) {
        log_message("Error: Delete request not acknowledged");
        return;
    }
    
    if (pkt.opcode == ACK) {
        log_message("File deletion from backup directory acknowledged by server");
    } else if (pkt.opcode == OP_ERROR) {
        log_message("Server error: %s", pkt.data);
    } else {
        log_message("Error: File deletion failed, server returned opcode %d", pkt.opcode);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <command> [<filename>]\n", argv[0]);
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  upload <filename>  - Upload a file to the server's backup directory\n");
        fprintf(stderr, "  download <filename> - Download a file from the server's backup directory\n");
        fprintf(stderr, "  delete <filename>  - Delete a file from the server's backup directory\n");
        exit(EXIT_FAILURE);
    }
    
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    const char *command = argv[3];
    const char *filename = (argc == 5) ? argv[4] : NULL;
    
    // Initialize logging
    log_message("Client started with command: %s", command);
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        log_message("Error creating socket: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = TIMEOUT;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        log_message("Error setting socket timeout: %s", strerror(errno));
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        log_message("Error: Invalid address or address not supported");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    log_message("Connecting to server %s:%d", server_ip, port);
    
    if (strcmp(command, "upload") == 0 && filename) {
        send_file(sockfd, &server_addr, filename);
    } else if (strcmp(command, "download") == 0 && filename) {
        receive_file(sockfd, &server_addr, filename);
    } else if (strcmp(command, "delete") == 0 && filename) {
        delete_file(sockfd, &server_addr, filename);
    } else {
        log_message("Invalid command or missing filename");
    }
    
    close(sockfd);
    if (log_file) fclose(log_file);
    return 0;
}