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

// OpenSSL libraries for encryption and hashing
#include <openssl/aes.h>
#include <openssl/md5.h>
#include <openssl/rand.h>

#include "udp_file_transfer.h"
#include "common.h"

// Function to set socket timeout
void set_socket_timeout(int socket, int seconds, int microseconds) {
  struct timeval timeout;
  timeout.tv_sec = seconds;
  timeout.tv_usec = microseconds;

  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
      0) {
    perror("setsockopt failed");
  }
}

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

// Function to send a packet with retry logic
int send_with_retry(int socket, void *packet, size_t packet_size,
                    struct sockaddr_in *server_addr, socklen_t addr_len,
                    int expect_ack_for_block, char *response_buffer,
                    size_t buffer_size) {
  int retries = 0;
  int received = 0;

  while (retries < MAX_RETRIES) {
    // Send the packet
    if (sendto(socket, packet, packet_size, 0, (struct sockaddr *)server_addr,
               addr_len) < 0) {
      perror("Error sending packet");
      return -1;
    }

    // Wait for response
    received = recvfrom(socket, response_buffer, buffer_size, 0,
                        (struct sockaddr *)server_addr, &addr_len);

    if (received > 0) {
      // Check if it's an ACK for the right block
      uint16_t opcode = ntohs(*(uint16_t *)response_buffer);
      if (opcode == OP_ACK) {
        ack_packet *ack = (ack_packet *)response_buffer;
        uint16_t received_block = ntohs(ack->block_number);

        if (received_block == expect_ack_for_block) {
          // Successfully received ACK for the expected block
          return received;
        } else {
          printf("Received ACK for wrong block: got #%d, expected #%d\n",
                 received_block, expect_ack_for_block);
        }
      } else if (opcode == OP_ERROR) {
        error_packet *error = (error_packet *)response_buffer;
        printf("Error: %s\n", error->error_msg);
        return -1;
      } else {
        printf("Unexpected packet type: %d\n", opcode);
      }
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Timeout occurred
      printf("Timeout waiting for ACK, retrying (%d/%d)...\n", retries + 1,
             MAX_RETRIES);
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
    // Upload file
    FILE *file = fopen(filename, "rb");
    if (!file) {
      perror("Cannot open file");
      close(client_socket);
      exit(EXIT_FAILURE);
    }

    // Calculate MD5 hash for file integrity
    unsigned char md5_digest[MD5_DIGEST_LENGTH];
    calculate_md5(file, md5_digest);
    
    printf("File MD5: ");
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
      printf("%02x", md5_digest[i]);
    }
    printf("\n");

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
                 (struct sockaddr *)&server_addr, addr_len) < 0) {
        perror("Failed to send WRQ");
        fclose(file);
        close(client_socket);
        exit(EXIT_FAILURE);
      }

      printf("Sent write request for %s (try %d/%d)\n", filename, retries + 1,
             MAX_RETRIES);

      // Wait for acknowledgment with timeout
      received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&server_addr, &addr_len);

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
    uint16_t opcode = ntohs(*(uint16_t *)buffer);
    if (opcode == OP_ERROR) {
      error_packet *error = (error_packet *)buffer;
      printf("Error: %s\n", error->error_msg);
      fclose(file);
      close(client_socket);
      exit(EXIT_FAILURE);
    }

    // Send file data with encryption
    uint16_t block_number = 1;
    int bytes_read;
    unsigned char plaintext[DATA_BLOCK_SIZE];
    // unsigned char ciphertext[DATA_BLOCK_SIZE + AES_BLOCK_SIZE]; // Allow for padding

    while (1) {
      data_packet data;
      data.opcode = htons(OP_DATA);
      data.block_number = htons(block_number);

      bytes_read = fread(plaintext, 1, DATA_BLOCK_SIZE, file);
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

      // Encrypt the data before sending
      int encrypted_size = encrypt_data(plaintext, bytes_read, (unsigned char*)data.data);
      
      // Send data packet with retry logic
      received =
          send_with_retry(client_socket, &data, 4 + encrypted_size, &server_addr,
                          addr_len, block_number, buffer, sizeof(buffer));

      if (received < 0) {
        printf("Failed to send block #%d after multiple retries\n",
               block_number);
        fclose(file);
        close(client_socket);
        exit(EXIT_FAILURE);
      }

      printf("Successfully sent block #%d, size: %d (encrypted: %d)\n", 
             block_number, bytes_read, encrypted_size);
      block_number++;

      // Check if this was the last block
      if (bytes_read < DATA_BLOCK_SIZE) {
        // Send MD5 verification packet
        verify_packet verify;
        verify.opcode = htons(OP_VERIFY);
        memcpy(verify.md5_hash, md5_digest, MD5_DIGEST_LENGTH);

        printf("Sending MD5 verification...\n");
        if (sendto(client_socket, &verify, sizeof(verify), 0,
                  (struct sockaddr *)&server_addr, addr_len) < 0) {
          perror("Failed to send verification");
        } else {
          // Wait for verification ACK
          received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&server_addr, &addr_len);
          
          if (received > 0) {
            uint16_t opcode = ntohs(*(uint16_t *)buffer);
            if (opcode == OP_ACK) {
              printf("File integrity verified by server\n");
            } else if (opcode == OP_ERROR) {
              error_packet *error = (error_packet *)buffer;
              printf("Verification failed: %s\n", error->error_msg);
            }
          }
        }
        
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
                 (struct sockaddr *)&server_addr, addr_len) < 0) {
        perror("Failed to send RRQ");
        fclose(file);
        close(client_socket);
        exit(EXIT_FAILURE);
      }

      printf("Sent read request for %s (try %d/%d)\n", filename, retries + 1,
             MAX_RETRIES);

      // Wait for first data packet with timeout
      received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&server_addr, &addr_len);

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
    uint16_t opcode = ntohs(*(uint16_t *)buffer);
    if (opcode == OP_ERROR) {
      error_packet *error = (error_packet *)buffer;
      printf("Error: %s\n", error->error_msg);
      fclose(file);
      close(client_socket);
      exit(EXIT_FAILURE);
    }

    // Process received data
    uint16_t expected_block = 1;
    unsigned char decrypted_data[DATA_BLOCK_SIZE + AES_BLOCK_SIZE];

    do {
      if (opcode != OP_DATA) {
        if (opcode == OP_VERIFY) {
          // Process verification packet
          verify_packet *verify = (verify_packet *)buffer;
          
          // Get the received MD5 hash
          unsigned char received_md5[MD5_DIGEST_LENGTH];
          memcpy(received_md5, verify->md5_hash, MD5_DIGEST_LENGTH);
          
          // Calculate our own MD5 hash of the downloaded file
          unsigned char calculated_md5[MD5_DIGEST_LENGTH];
          
          // Rewind file to calculate MD5
          fseek(file, 0, SEEK_SET);
          calculate_md5(file, calculated_md5);
          
          // Compare MD5 hashes
          int match = 1;
          for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
            if (received_md5[i] != calculated_md5[i]) {
              match = 0;
              break;
            }
          }
          
          if (match) {
            printf("File integrity verified (MD5 hash matched)\n");
            // Send ACK for the verification
            ack_packet ack;
            ack.opcode = htons(OP_ACK);
            ack.block_number = htons(0); // Special block number for verification
            sendto(client_socket, &ack, sizeof(ack), 0,
                   (struct sockaddr *)&server_addr, addr_len);
          } else {
            printf("WARNING: File integrity verification failed!\n");
            printf("Received MD5: ");
            for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
              printf("%02x", received_md5[i]);
            }
            printf("\nCalculated MD5: ");
            for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
              printf("%02x", calculated_md5[i]);
            }
            printf("\n");
            
            // Send error for the verification
            error_packet error;
            error.opcode = htons(OP_ERROR);
            error.error_code = htons(ERR_VERIFICATION);
            strcpy(error.error_msg, "MD5 verification failed");
            sendto(client_socket, &error, sizeof(error), 0,
                   (struct sockaddr *)&server_addr, addr_len);
          }
          break;
        } else {
          printf("Expected data packet (opcode: %d)\n", opcode);
          break;
        }
      }

      data_packet *data = (data_packet *)buffer;
      uint16_t block = ntohs(data->block_number);

      if (block == expected_block) {
        // Get data size and decrypt the data
        int data_size = received - 4;  // Subtract header size
        int decrypted_size = decrypt_data((unsigned char*)data->data, data_size, decrypted_data);
        
        // Write decrypted data to file
        fwrite(decrypted_data, 1, decrypted_size, file);

        // Send ACK
        ack_packet ack;
        ack.opcode = htons(OP_ACK);
        ack.block_number = htons(block);

        retries = 0;
        while (retries < MAX_RETRIES) {
          if (sendto(client_socket, &ack, sizeof(ack), 0,
                     (struct sockaddr *)&server_addr, addr_len) < 0) {
            perror("ACK send error");
            retries++;
          } else {
            break;
          }
        }

        printf("Received block #%d, size: %d (decrypted: %d)\n", 
               block, data_size, decrypted_size);

        expected_block++;

        // Check if this was the last block
        if (data_size < DATA_BLOCK_SIZE) {
          printf("Download complete\n");
          break;
        }
      } else {
        printf("Received unexpected block #%d, expected #%d\n", block,
               expected_block);

        // Send ACK for the last correctly received block
        ack_packet ack;
        ack.opcode = htons(OP_ACK);
        ack.block_number = htons(expected_block - 1);
        sendto(client_socket, &ack, sizeof(ack), 0,
               (struct sockaddr *)&server_addr, addr_len);
      }

      // Wait for next data packet
      retries = 0;
      received = 0;

      while (retries < MAX_RETRIES) {
        received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                            (struct sockaddr *)&server_addr, &addr_len);

        if (received > 0) {
          break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // Timeout, retry ACK and wait again
          ack_packet ack;
          ack.opcode = htons(OP_ACK);
          ack.block_number = htons(expected_block - 1);

          printf(
              "Timeout waiting for block #%d, resending ACK for #%d (%d/%d)\n",
              expected_block, expected_block - 1, retries + 1, MAX_RETRIES);

          sendto(client_socket, &ack, sizeof(ack), 0,
                 (struct sockaddr *)&server_addr, addr_len);

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

      opcode = ntohs(*(uint16_t *)buffer);

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
                 (struct sockaddr *)&server_addr, addr_len) < 0) {
        perror("Failed to send delete request");
        close(client_socket);
        exit(EXIT_FAILURE);
      }

      printf("Sent delete request for %s (try %d/%d)\n", filename, retries + 1,
             MAX_RETRIES);

      // Wait for response with timeout
      received = recvfrom(client_socket, buffer, sizeof(buffer), 0,
                          (struct sockaddr *)&server_addr, &addr_len);

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
    uint16_t opcode = ntohs(*(uint16_t *)buffer);
    if (opcode == OP_ERROR) {
      error_packet *error = (error_packet *)buffer;
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
