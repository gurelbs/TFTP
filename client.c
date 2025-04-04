/**
 * Minimal UDP File Transfer System - Client Implementation
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <arpa/inet.h>
 #include <sys/socket.h>
 #include "udp_file_transfer.h"
 
 void display_usage() {
     printf("Usage:\n");
     printf("  ./client [server_ip] [port] upload [filename] - Upload a file\n");
     printf("  ./client [server_ip] [port] download [filename] - Download a file\n");
    printf("  ./client [server_ip] [port] delete [filename] - Delete a file\n");
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
         
         sendto(client_socket, &req, sizeof(req), 0,
                (struct sockaddr*)&server_addr, addr_len);
                 
         printf("Sent write request for %s\n", filename);
         
         // Wait for acknowledgment
         int received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                     (struct sockaddr*)&server_addr, &addr_len);
                     
         if (received < 0) {
             perror("Failed to receive response");
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
             
             // Send data packet
             sendto(client_socket, &data, 4 + bytes_read, 0,  // 4 bytes header + data
                    (struct sockaddr*)&server_addr, addr_len);
                    
             printf("Sent block #%d, size: %d\n", block_number, bytes_read);
             
             // Wait for ACK
             received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                      (struct sockaddr*)&server_addr, &addr_len);
                      
             if (received < 0) {
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
                 printf("Upload complete\n");
                 fclose(file);
                 close(client_socket);
                 exit(EXIT_SUCCESS);             
                 break;
             }
         }

            // fclose(file);   
         
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
         
         sendto(client_socket, &req, sizeof(req), 0,
                (struct sockaddr*)&server_addr, addr_len);
                 
         printf("Sent read request for %s\n", filename);
         
         // Receive file data
         uint16_t expected_block = 1;
         
         while (1) {
             int received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)&server_addr, &addr_len);
                         
             if (received < 0) {
                 perror("Receive error");
                 break;
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
             
             if (opcode != OP_DATA) {
                 printf("Expected data packet\n");
                 break;
             }
             
             data_packet *data = (data_packet*)buffer;
             uint16_t block = ntohs(data->block_number);
             
             if (block == expected_block) {
                 // Write data to file
                 int data_size = received - 4;  // Subtract header size
                 fwrite(data->data, 1, data_size, file);
                 
                 // Send ACK
                 ack_packet ack;
                 ack.opcode = htons(OP_ACK);
                 ack.block_number = htons(block);
                 sendto(client_socket, &ack, sizeof(ack), 0,
                        (struct sockaddr*)&server_addr, addr_len);
                 
                 printf("Received block #%d, size: %d\n", block, data_size);
                 
                 expected_block++;
                 
                 // Check if this was the last block
                 if (data_size < DATA_BLOCK_SIZE) {
                     printf("Download complete\n");
                     break;
                 }
             }
         }
         
         fclose(file);
         
     } else if (strcmp(command, "delete") == 0 && filename) {
         // Delete file
         request_packet req;
         req.opcode = htons(OP_DELETE);  // Use delete opcode instead of WRQ
         strncpy(req.filename, filename, MAX_FILENAME_LEN);
         strcpy(req.mode, "octet");
         
         sendto(client_socket, &req, sizeof(req), 0,
                (struct sockaddr*)&server_addr, addr_len);
                
         printf("Sent delete request for %s\n", filename);
         
         // Wait for acknowledgment
         int received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                     (struct sockaddr*)&server_addr, &addr_len);
                     
         if (received < 0) {
             perror("Failed to receive response");
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
         }
         
         printf("Delete request acknowledged\n");
     } else {
         display_usage();
     }
     
     close(client_socket);
     return 0;
 }
