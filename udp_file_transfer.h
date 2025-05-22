#ifndef UDP_FILE_TRANSFER_H
#define UDP_FILE_TRANSFER_H

#include <stdint.h>

#define MAX_PACKET_SIZE 1024

// Operation codes
#define OP_READ_REQUEST   1
#define OP_WRITE_REQUEST  2
#define OP_DATA           3
#define ACK               4
#define OP_ERROR          5
#define OP_DELETE_REQUEST 6
#define OP_EOF            7

// Packet structure
struct packet {
    uint16_t opcode;
    uint16_t packet_number;
    char data[MAX_PACKET_SIZE];
};

#endif // UDP_FILE_TRANSFER_H