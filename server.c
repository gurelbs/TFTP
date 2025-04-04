/**
 * Minimal UDP File Transfer System - Server Implementation
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
 
int main(int argc, char *argv[]) {
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[1024];
    int port = DEFAULT_PORT;
    const char* backup_dir = "backup";  // Backup directory name
    char full_path[MAX_FILENAME_LEN + 256]; // For the complete filepath
    
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
            request_packet* req = (request_packet*)buffer;
            printf("Received write request for file: %s\n", req->filename);
            
            // Create complete filepath in backup directory
            create_backup_path(full_path, backup_dir, req->filename);
            printf("Saving to: %s\n", full_path);
            
            // Open file for writing in backup directory
            FILE* file = fopen(full_path, "wb");
            if (!file) {
                // Send error: cannot create file
                error_packet error;
                error.opcode = htons(OP_ERROR);
                error.error_code = htons(ERR_ACCESS_DENIED);
                strcpy(error.error_msg, "Cannot create file");
                sendto(server_socket, &error, sizeof(error), 0,
                       (struct sockaddr*)&client_addr, addr_len);
                continue;
            }
            
            // Send initial ACK
            ack_packet ack;
            ack.opcode = htons(OP_ACK);
            ack.block_number = htons(0);
            sendto(server_socket, &ack, sizeof(ack), 0,
                   (struct sockaddr*)&client_addr, addr_len);
            
            // Receive data blocks
            uint16_t expected_block = 1;
            while (1) {
                received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&client_addr, &addr_len);
                
                if (received_bytes < 0) {
                    perror("recvfrom failed");
                    break;
                }
                
                data_packet* data = (data_packet*)buffer;
                if (ntohs(data->opcode) != OP_DATA) {
                    break;  // Not a data packet
                }
                
                uint16_t block = ntohs(data->block_number);
                printf("Received block #%d\n", block);
                
                if (block == expected_block) {
                    // Write data to file
                    int data_size = received_bytes - 4;  // Subtract header size
                    fwrite(data->data, 1, data_size, file);
                    
                    // Send ACK
                    ack.opcode = htons(OP_ACK);
                    ack.block_number = htons(block);
                    sendto(server_socket, &ack, sizeof(ack), 0,
                           (struct sockaddr*)&client_addr, addr_len);
                    
                    expected_block++;
                    
                    // Check if this was the last block
                    if (data_size < DATA_BLOCK_SIZE) {
                           printf("Received last block #%d\n", block);
                           
                           // Close file
                           fclose(file);
                           
                           // Send final ACK
                           ack.opcode = htons(OP_ACK);
                           ack.block_number = htons(block);  // Should be the same as received block
                           sendto(server_socket, &ack, sizeof(ack), 0,
                                   (struct sockaddr*)&client_addr, addr_len);
                           printf("Download complete\n");
                        break;
                    }
                   } else if (block > expected_block) {
                       // Duplicate block, send ACK for expected block
                       ack.opcode = htons(OP_ACK);
                       ack.block_number = htons(expected_block - 1);
                       sendto(server_socket, &ack, sizeof(ack), 0,
                               (struct sockaddr*)&client_addr, addr_len);
                       printf("Duplicate block received, sent ACK for block #%d\n", expected_block - 1);
                   } else {
                       // Out of order block, ignore it
                       printf("Out of order block received, expected block #%d\n", expected_block);
                   }

                   // Check for error response
                   uint16_t opcode = ntohs(*(uint16_t*)buffer);
                   if (opcode == OP_ERROR) {
                       error_packet *error = (error_packet*)buffer;
                       printf("Error: %s\n", error->error_msg);
                       fclose(file);
                       break;
                }
            }
            
        } else if (opcode == OP_RRQ) {  // Read request
            request_packet* req = (request_packet*)buffer;
            printf("Received read request for file: %s\n", req->filename);
            
            // Create complete filepath in backup directory
            create_backup_path(full_path, backup_dir, req->filename);
            printf("Reading from: %s\n", full_path);
            
            // Open file for reading from backup directory
            FILE* file = fopen(full_path, "rb");
            if (!file) {
                // Send error: file not found
                error_packet error;
                error.opcode = htons(OP_ERROR);
                error.error_code = htons(ERR_FILE_NOT_FOUND);
                strcpy(error.error_msg, "File not found");
                sendto(server_socket, &error, sizeof(error), 0,
                       (struct sockaddr*)&client_addr, addr_len);
                continue;
            }
            
            // Send file in blocks
            uint16_t block_number = 1;
            data_packet data;
            int bytes_read;
            
            while (1) {
                data.opcode = htons(OP_DATA);
                data.block_number = htons(block_number);
                
                bytes_read = fread(data.data, 1, DATA_BLOCK_SIZE, file);
                if (bytes_read < 0) {
                    perror("File read error");
                    break;
                }
                
                // Send data packet
                sendto(server_socket, &data, 4 + bytes_read, 0,  // 4 bytes header + data
                       (struct sockaddr*)&client_addr, addr_len);
                       
                printf("Sent block #%d, size: %d\n", block_number, bytes_read);
                
                // Wait for ACK
                received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&client_addr, &addr_len);
                                
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
                
                // Check if this was the last block
                if (bytes_read < DATA_BLOCK_SIZE) {
                    printf("Download complete\n");
                    break;
                }
            }
            
            fclose(file);
        } else if (opcode == OP_DELETE) {
            request_packet* req = (request_packet*)buffer;
            printf("Received delete request for file: %s\n", req->filename);
            
            // Construct the full path
            char filepath[512];
            sprintf(filepath, "backup/%s", req->filename);
            
            // Delete the file
            if (remove(filepath) == 0) {
                printf("File deleted successfully: %s\n", filepath);
                
                // Send acknowledgment
                ack_packet ack;
                ack.opcode = htons(OP_ACK);
                ack.block_number = htons(0);  // Block 0 for delete ack
                
                sendto(server_socket, &ack, sizeof(ack), 0,
                       (struct sockaddr*)&client_addr, addr_len);
            } else {
                printf("Error deleting file: %s\n", strerror(errno));
                
                // Send error response
                send_error(server_socket, &client_addr, addr_len, 
                          2, "Could not delete file");
            }
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
