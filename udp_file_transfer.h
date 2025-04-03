/**
 * Minimal UDP File Transfer System - Phase 1
 * Header file containing shared definitions and structures
 */

 #ifndef UDP_FILE_TRANSFER_H
 #define UDP_FILE_TRANSFER_H
 
 #include <stdint.h>
 
 /* Constants */
 #define MAX_FILENAME_LEN 64
 #define DATA_BLOCK_SIZE 512
 #define DEFAULT_PORT 6969
 
 /* Operation Codes */
 #define OP_RRQ   1  // Read request
 #define OP_WRQ   2  // Write request
 #define OP_DATA  3  // Data packet
 #define OP_ACK   4  // Acknowledgment
 #define OP_ERROR 5  // Error
 
 /* Error Codes */
 #define ERR_NOT_DEFINED     0
 #define ERR_FILE_NOT_FOUND  1
 #define ERR_ACCESS_DENIED   2
 #define ERR_DISK_FULL       3
 
 /* Packet Structures */
 
 // Read/Write Request Packet
 typedef struct {
     uint16_t opcode;               // OP_RRQ or OP_WRQ
     char filename[MAX_FILENAME_LEN];
     char mode[8];                  // "octet" for binary transfer
 } request_packet;
 
 // Data Packet
 typedef struct {
     uint16_t opcode;           // OP_DATA
     uint16_t block_number;
     char data[DATA_BLOCK_SIZE];
 } data_packet;
 
 // Acknowledgment Packet
 typedef struct {
     uint16_t opcode;        // OP_ACK
     uint16_t block_number;
 } ack_packet;
 
 // Error Packet
 typedef struct {
     uint16_t opcode;     // OP_ERROR
     uint16_t error_code;
     char error_msg[64];
 } error_packet;
 
 #endif /* UDP_FILE_TRANSFER_H */
