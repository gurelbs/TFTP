/**
 * Minimal UDP File Transfer System - Client Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include "udp_file_transfer.h"

// Function to set socket timeout
void set_socket_timeout(int socket, int seconds, int microseconds) {
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = microseconds;
    
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
    }
}

void display_usage() {
    printf("Usage:\n");
    printf("  ./client [server_ip] [port] upload [filename] - Upload a file\n");
    printf("  ./client [server_ip] [port] download [filename] - Download a file\n");
    printf("  ./client [server_ip] [port] delete [filename] - Delete a file\n");
}

// Function to send a packet with retry logic
int send_with_retry(int socket, void *packet, size_t packet_size, 
                   struct sockaddr_in *server_addr, socklen_t addr_len,
                   int expect_ack_for_block, char *response_buffer, size_t buffer_size) {
                   
    int retries = 0;
    int received = 0;
    
    while (retries < MAX_RETRIES) {
        // Send the packet
        if (sendto(socket, packet, packet_size, 0, 
                  (struct sockaddr*)server_addr, addr_len) < 0) {
            perror("Error sending packet");
            return -1;
        }
        
        // Wait for response
        received = recvfrom(socket, response_buffer, buffer_size, 0,
                           (struct sockaddr*)server_addr, &addr_len);
                           
        if (received > 0) {
            // Check if it's an ACK for the right block
            uint16_t opcode = ntohs(*(uint16_t*)response_buffer);
            if (opcode == OP_ACK) {
                ack_packet *ack = (ack_packet*)response_buffer;
                uint16_t received_block = ntohs(ack->block_number);
                
                if (received_block == expect_ack_for_block) {
                    // Successfully received ACK for the expected block
                    return received;
                } else {
                    printf("Received ACK for wrong block: got #%d, expected #%d\n", 
                           received_block, expect_ack_for_block);
                }
            } else if (opcode == OP_ERROR) {
                error_packet *error = (error_packet*)response_buffer;
                printf("Error: %s\n", error->error_msg);
                return -1;
            } else {
                printf("Unexpected packet type: %d\n", opcode);
            }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Timeout occurred
            printf("Timeout waiting for ACK, retrying (%d/%d)...\n", retries+1, MAX_RETRIES);
            retries++;
        } else {
            perror("Error receiving ACK");
            return -1;
        }
    }
    
    printf("Maximum retries exceeded, giving up\n");
    return -1;
}

int main(int argc, char *argv[]) {
    int client_socket;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    char buffer[1024];
    
    // Check command line arguments
    if (argc < 4) {
        display_usage();
        exit(EXIT_FAILURE);
    }
    
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    const char *command = argv[3];
    const char *filename = (argc > 4) ? argv[4] : NULL;
    
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
    
    // Process command
    if (strcmp(command, "upload") == 0 && filename) {
        // Upload file
        FILE *file = fopen(filename, "rb");
        if (!file) {
            perror("Cannot open file");
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Send write request
        request_packet req;
        req.opcode = htons(OP_WRQ);
        strncpy(req.filename, filename, MAX_FILENAME_LEN);
        strcpy(req.mode, "octet");
        
        int retries = 0;
        int received = 0;
        
        // Send WRQ with retry logic
        while (retries < MAX_RETRIES) {
            if (sendto(client_socket, &req, sizeof(req), 0,
                      (struct sockaddr*)&server_addr, addr_len) < 0) {
                perror("Failed to send WRQ");
                fclose(file);
                close(client_socket);
                exit(EXIT_FAILURE);
            }
            
            printf("Sent write request for %s (try %d/%d)\n", filename, retries+1, MAX_RETRIES);
            
            // Wait for acknowledgment with timeout
            received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr*)&server_addr, &addr_len);
                               
            if (received > 0) {
                break;  // Got a response
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout occurred
                retries++;
                printf("Timeout waiting for WRQ ACK, retrying...\n");
            } else {
                perror("Failed to receive response");
                fclose(file);
                close(client_socket);
                exit(EXIT_FAILURE);
            }
        }
        
        if (retries >= MAX_RETRIES) {
            printf("Maximum retries exceeded, giving up\n");
            fclose(file);
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Check if error response
        uint16_t opcode = ntohs(*(uint16_t*)buffer);
        if (opcode == OP_ERROR) {
            error_packet *error = (error_packet*)buffer;
            printf("Error: %s\n", error->error_msg);
            fclose(file);
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Send file data
        uint16_t block_number = 1;
        int bytes_read;
        
        while (1) {
            data_packet data;
            data.opcode = htons(OP_DATA);
            data.block_number = htons(block_number);
            
            bytes_read = fread(data.data, 1, DATA_BLOCK_SIZE, file);
            if (bytes_read == 0) {
                if (ferror(file)) {
                    perror("File read error");
                    fclose(file);
                    close(client_socket);
                    exit(EXIT_FAILURE);
                }
                // EOF reached (likely empty file)
                printf("End of file reached\n");
            }
            
            // Send data packet with retry logic
            received = send_with_retry(client_socket, &data, 4 + bytes_read, 
                                     &server_addr, addr_len, 
                                     block_number, buffer, sizeof(buffer));
                                     
            if (received < 0) {
                printf("Failed to send block #%d after multiple retries\n", block_number);
                fclose(file);
                close(client_socket);
                exit(EXIT_FAILURE);
            }
            
            printf("Successfully sent block #%d, size: %d\n", block_number, bytes_read);
            block_number++;
            
            // Check if this was the last block
            if (bytes_read < DATA_BLOCK_SIZE) {
                printf("Upload complete\n");
                fclose(file);
                close(client_socket);
                exit(EXIT_SUCCESS);
                break;
            }
        }
        
    } else if (strcmp(command, "download") == 0 && filename) {
        // Download file
        FILE *file = fopen(filename, "wb");
        if (!file) {
            perror("Cannot create file");
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Send read request
        request_packet req;
        req.opcode = htons(OP_RRQ);
        strncpy(req.filename, filename, MAX_FILENAME_LEN);
        strcpy(req.mode, "octet");
        
        int retries = 0;
        int received = 0;
        
        // Send RRQ with retry logic
        while (retries < MAX_RETRIES) {
            if (sendto(client_socket, &req, sizeof(req), 0,
                      (struct sockaddr*)&server_addr, addr_len) < 0) {
                perror("Failed to send RRQ");
                fclose(file);
                close(client_socket);
                exit(EXIT_FAILURE);
            }
            
            printf("Sent read request for %s (try %d/%d)\n", filename, retries+1, MAX_RETRIES);
            
            // Wait for first data packet with timeout
            received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr*)&server_addr, &addr_len);
                               
            if (received > 0) {
                break;  // Got a response
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout occurred
                retries++;
                printf("Timeout waiting for data packet, retrying...\n");
            } else {
                perror("Failed to receive response");
                fclose(file);
                close(client_socket);
                exit(EXIT_FAILURE);
            }
        }
        
        if (retries >= MAX_RETRIES) {
            printf("Maximum retries exceeded, giving up\n");
            fclose(file);
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Check if error response
        uint16_t opcode = ntohs(*(uint16_t*)buffer);
        if (opcode == OP_ERROR) {
            error_packet *error = (error_packet*)buffer;
            printf("Error: %s\n", error->error_msg);
            fclose(file);
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Process received data
        uint16_t expected_block = 1;
        
        do {
            if (opcode != OP_DATA) {
                printf("Expected data packet (opcode: %d)\n", opcode);
                break;
            }
            
            data_packet *data = (data_packet*)buffer;
            uint16_t block = ntohs(data->block_number);
            
            if (block == expected_block) {
                // Write data to file
                int data_size = received - 4;  // Subtract header size
                fwrite(data->data, 1, data_size, file);
                
                // Send ACK with retry logic (ACK doesn't need response verification)
                ack_packet ack;
                ack.opcode = htons(OP_ACK);
                ack.block_number = htons(block);
                
                retries = 0;
                while (retries < MAX_RETRIES) {
                    if (sendto(client_socket, &ack, sizeof(ack), 0,
                              (struct sockaddr*)&server_addr, addr_len) < 0) {
                        perror("ACK send error");
                        retries++;
                    } else {
                        break;
                    }
                }
                
                printf("Received block #%d, size: %d\n", block, data_size);
                
                expected_block++;
                
                // Check if this was the last block
                if (data_size < DATA_BLOCK_SIZE) {
                    printf("Download complete\n");
                    break;
                }
            } else {
                printf("Received unexpected block #%d, expected #%d\n", block, expected_block);
                
                // Send ACK for the last correctly received block
                ack_packet ack;
                ack.opcode = htons(OP_ACK);
                ack.block_number = htons(expected_block - 1);
                sendto(client_socket, &ack, sizeof(ack), 0,
                      (struct sockaddr*)&server_addr, addr_len);
            }
            
            // Wait for next data packet
            retries = 0;
            received = 0;
            
            while (retries < MAX_RETRIES) {
                received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                                   (struct sockaddr*)&server_addr, &addr_len);
                                   
                if (received > 0) {
                    break;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Timeout, retry ACK and wait again
                    ack_packet ack;
                    ack.opcode = htons(OP_ACK);
                    ack.block_number = htons(expected_block - 1);
                    
                    printf("Timeout waiting for block #%d, resending ACK for #%d (%d/%d)\n", 
                          expected_block, expected_block - 1, retries+1, MAX_RETRIES);
                          
                    sendto(client_socket, &ack, sizeof(ack), 0,
                          (struct sockaddr*)&server_addr, addr_len);
                          
                    retries++;
                } else {
                    perror("Receive error");
                    fclose(file);
                    close(client_socket);
                    exit(EXIT_FAILURE);
                }
            }
            
            if (retries >= MAX_RETRIES) {
                printf("Maximum retries exceeded, giving up\n");
                break;
            }
            
            opcode = ntohs(*(uint16_t*)buffer);
            
        } while (received > 0);
        
        fclose(file);
        
    } else if (strcmp(command, "delete") == 0 && filename) {
        // Delete file
        request_packet req;
        req.opcode = htons(OP_DELETE);
        strncpy(req.filename, filename, MAX_FILENAME_LEN);
        strcpy(req.mode, "octet");
        
        int retries = 0;
        int received = 0;
        
        // Send delete request with retry logic
        while (retries < MAX_RETRIES) {
            if (sendto(client_socket, &req, sizeof(req), 0,
                      (struct sockaddr*)&server_addr, addr_len) < 0) {
                perror("Failed to send delete request");
                close(client_socket);
                exit(EXIT_FAILURE);
            }
            
            printf("Sent delete request for %s (try %d/%d)\n", filename, retries+1, MAX_RETRIES);
            
            // Wait for response with timeout
            received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr*)&server_addr, &addr_len);
                               
            if (received > 0) {
                break;  // Got a response
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout occurred
                retries++;
                printf("Timeout waiting for delete ACK, retrying...\n");
            } else {
                perror("Failed to receive response");
                close(client_socket);
                exit(EXIT_FAILURE);
            }
        }
        
        if (retries >= MAX_RETRIES) {
            printf("Maximum retries exceeded, giving up\n");
            close(client_socket);
            exit(EXIT_FAILURE);
        }
        
        // Check if error response
        uint16_t opcode = ntohs(*(uint16_t*)buffer);
        if (opcode == OP_ERROR) {
            error_packet *error = (error_packet*)buffer;
            printf("Error: %s\n", error->error_msg);
            close(client_socket);
            exit(EXIT_FAILURE);
        } else if (opcode == OP_ACK) {
            printf("File deleted successfully\n");
        } else {
            printf("Unexpected response\n");
        }
    } else {
        display_usage();
    }
    
    close(client_socket);
    return 0;
}
