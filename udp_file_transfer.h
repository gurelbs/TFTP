#ifndef UDP_FILE_TRANSFER_H
#define UDP_FILE_TRANSFER_H

#include <stdint.h>

// Operation Codes (similar to TFTP with added delete)
#define OP_RRQ     1   // Read Request
#define OP_WRQ     2   // Write Request
#define OP_DATA    3   // Data Packet
#define OP_ACK     4   // Acknowledgment
#define OP_ERROR   5   // Error
#define OP_DELETE  6   // Delete File (custom operation)

// Maximum packet size
#define MAX_PACKET_SIZE 1024
#define MAX_FILENAME_LENGTH 255
#define BLOCK_SIZE 512

// Error Codes
#define ERROR_FILE_NOT_FOUND 1
#define ERROR_ACCESS_VIOLATION 2
#define ERROR_DISK_FULL 3
#define ERROR_ILLEGAL_OPERATION 4
#define ERROR_UNKNOWN_TRANSFER_ID 5
#define ERROR_FILE_ALREADY_EXISTS 6
#define ERROR_NO_SUCH_USER 7

// Packet Structures
// Request Packet (for RRQ, WRQ, DELETE)
typedef struct {
    uint16_t opcode;
    char filename[MAX_FILENAME_LENGTH];
    char mode[10];  // "netascii" or "octet"
} request_packet;

// Data Packet
typedef struct {
    uint16_t opcode;
    uint16_t block_number;
    uint8_t data[BLOCK_SIZE];
} data_packet;

// Acknowledgment Packet
typedef struct {
    uint16_t opcode;
    uint16_t block_number;
} ack_packet;

// Error Packet
typedef struct {
    uint16_t opcode;
    uint16_t error_code;
    char error_msg[MAX_PACKET_SIZE - 4];
} error_packet;

// Function prototypes for utility functions
int create_udp_socket();
void set_socket_timeout(int socket, int seconds);
void handle_error(const char* message);

#endif // UDP_FILE_TRANSFER_H