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
            request_packet* req = (request_packet*)buffer;
            printf("Received write request for file: %s\n", req->filename);
            
            // Create complete filepath in backup directory
            create_backup_path(full_path, backup_dir, req->filename);
            printf("Saving to: %s\n", full_path);
            
            // Open file for writing in backup directory
            FILE* file = fopen(full_path, "wb");
            if (!file) {
                // Send error: cannot create file
                send_error(server_socket, &client_addr, addr_len,
                           ERR_ACCESS_DENIED, "Cannot create file");
                continue;
            }
            
            // Send initial ACK
            ack_packet ack;
            ack.opcode = htons(OP_ACK);
            ack.block_number = htons(0);
            sendto(server_socket, &ack, sizeof(ack), 0,
                   (struct sockaddr*)&client_addr, addr_len);
            
            // Buffers for decryption
            unsigned char decrypted_data[DATA_BLOCK_SIZE + AES_BLOCK_SIZE];
            
            // Receive data blocks
            uint16_t expected_block = 1;
            while (1) {
                received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&client_addr, &addr_len);
                
                if (received_bytes < 0) {
                    perror("recvfrom failed");
                    break;
                }
                
                // Check opcode
                opcode = ntohs(*(uint16_t*)buffer);
                
                if (opcode == OP_DATA) {
                    data_packet* data = (data_packet*)buffer;
                    uint16_t block = ntohs(data->block_number);
                    printf("Received block #%d\n", block);
                    
                    if (block == expected_block) {
                        // Get encrypted data size and decrypt
                        int encrypted_size = received_bytes - 4;  // Subtract header size
                        int decrypted_size = decrypt_data((unsigned char*)data->data, encrypted_size, decrypted_data);

                        // Write decrypted data to file
                        size_t bytes_written = fwrite(decrypted_data, 1, decrypted_size, file);
                        if (bytes_written != (size_t)decrypted_size) {
                            perror("Error writing to file");
                            send_error(server_socket, &client_addr, addr_len,
                                      ERR_DISK_FULL, "Failed to write to file");
                            fclose(file);
                            remove(full_path);
                            break;
                        }
                        
                        // Make sure to flush the data to disk
                        fflush(file);
                        
                        // Send ACK
                        ack.opcode = htons(OP_ACK);
                        ack.block_number = htons(block);
                        sendto(server_socket, &ack, sizeof(ack), 0,
                               (struct sockaddr*)&client_addr, addr_len);
                        
                        printf("Decrypted block #%d, size: %d\n", block, decrypted_size);
                        expected_block++;
                        
                        // Check if this was the last block (encrypted size might be padded)
                        if (encrypted_size < DATA_BLOCK_SIZE) {
                            printf("Last data block received\n");
                            // We'll wait for the verification packet
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
                } else if (opcode == OP_VERIFY) {
                    // Process verification packet
                    verify_packet* verify = (verify_packet*)buffer;
                    
                    // Get the client's MD5 hash
                    unsigned char client_md5[MD5_DIGEST_LENGTH];
                    memcpy(client_md5, verify->md5_hash, MD5_DIGEST_LENGTH);
                    
                    printf("Received MD5 hash: ");
                    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
                        printf("%02x", client_md5[i]);
                    }
                    printf("\n");
                    
                    // Make sure file is flushed to disk before calculating MD5
                    fflush(file);
                    
                    // Calculate our own MD5 hash of the downloaded file
                    unsigned char calculated_md5[MD5_DIGEST_LENGTH];
                    
                    // Reopen the file to ensure all data is properly read
                    // This forces any cached writes to be committed to disk
                    fclose(file);
                    file = fopen(full_path, "rb");
                    if (!file) {
                        send_error(server_socket, &client_addr, addr_len,
                                  ERR_ACCESS_DENIED, "Cannot reopen file for verification");
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
                    
                    // Compare MD5 hashes
                    int match = 1;
                    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
                        if (client_md5[i] != calculated_md5[i]) {
                            match = 0;
                            break;
                        }
                    }
                    
                    if (match) {
                        printf("File integrity verified (MD5 hash matched)\n");
                        // Send ACK for verification
                        ack.opcode = htons(OP_ACK);
                        ack.block_number = htons(0); // Special block for verification
                        sendto(server_socket, &ack, sizeof(ack), 0,
                               (struct sockaddr*)&client_addr, addr_len);
                    } else {
                        printf("File integrity verification FAILED!\n");
                        // Send error
                        send_error(server_socket, &client_addr, addr_len,
                                  ERR_VERIFICATION, "MD5 hash mismatch - file corrupted");
                        
                        // Delete the corrupted file
                        fclose(file);
                        remove(full_path);
                        printf("Corrupted file deleted\n");
                        break;
                    }
                    
                    // Close file - upload complete with verification
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
                send_error(server_socket, &client_addr, addr_len,
                          ERR_FILE_NOT_FOUND, "File not found");
                continue;
            }
            
            // Calculate MD5 hash for verification
            unsigned char md5_digest[MD5_DIGEST_LENGTH];
            calculate_md5(file, md5_digest);
            
            printf("File MD5: ");
            for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
                printf("%02x", md5_digest[i]);
            }
            printf("\n");
            
            // Buffers for encryption
            unsigned char plaintext[DATA_BLOCK_SIZE];
            // unsigned char ciphertext[DATA_BLOCK_SIZE + AES_BLOCK_SIZE]; // Allow for padding
            
            // Send file in blocks
            uint16_t block_number = 1;
            data_packet data;
            int bytes_read;
            
            while (1) {
                data.opcode = htons(OP_DATA);
                data.block_number = htons(block_number);
                
                // Read original data
                bytes_read = fread(plaintext, 1, DATA_BLOCK_SIZE, file);
                if (bytes_read < 0) {
                    perror("File read error");
                    break;
                }
                
                // Encrypt the data
                int encrypted_size = encrypt_data(plaintext, bytes_read, (unsigned char*)data.data);
                
                // Send encrypted data packet
                sendto(server_socket, &data, 4 + encrypted_size, 0,  // 4 bytes header + encrypted data
                       (struct sockaddr*)&client_addr, addr_len);
                       
                printf("Sent block #%d, size: %d (encrypted: %d)\n", 
                       block_number, bytes_read, encrypted_size);
                
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
                    // Send verification packet with MD5 hash
                    verify_packet verify;
                    verify.opcode = htons(OP_VERIFY);
                    memcpy(verify.md5_hash, md5_digest, MD5_DIGEST_LENGTH);
                    
                    printf("Sending MD5 verification...\n");
                    sendto(server_socket, &verify, sizeof(verify), 0,
                           (struct sockaddr*)&client_addr, addr_len);
                           
                    // Wait for verification acknowledgment
                    received_bytes = recvfrom(server_socket, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&client_addr, &addr_len);
                                    
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
        } else if (opcode == OP_DELETE) {
            request_packet* req = (request_packet*)buffer;
            printf("Received delete request for file: %s\n", req->filename);
            
            // Create complete filepath in backup directory
            create_backup_path(full_path, backup_dir, req->filename);
            
            // Delete the file
            if (remove(full_path) == 0) {
                printf("File deleted successfully: %s\n", full_path);
                
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
                          ERR_ACCESS_DENIED, "Could not delete file");
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
