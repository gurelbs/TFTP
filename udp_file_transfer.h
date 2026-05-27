/*
 * udp_file_transfer.h
 * =====================================================================
 * Enhanced TFTP – UDP-based file-transfer system
 *
 * Defines:
 *   • Wire-format packet structures (RRQ / WRQ / DATA / ACK / ERROR /
 *     DELETE / DACK)
 *   • AES-256-CBC encryption / decryption helpers
 *   • MD5 checksum helper
 *   • Shared constants (port, timeouts, sizes, opcodes)
 *   • Utility function declarations used by both client and server
 * =====================================================================
 */

#ifndef UDP_FILE_TRANSFER_H
#define UDP_FILE_TRANSFER_H

/* ------------------------------------------------------------------ */
/*  Standard headers                                                   */
/* ------------------------------------------------------------------ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* OpenSSL for AES encryption and MD5 hashing */
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/md5.h>
#include <openssl/rand.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

/* Networking */
#define TFTP_PORT           6969        /* Default server listen port       */
#define MAX_FILENAME        256         /* Max filename length              */
#define MAX_MODE            12          /* Max mode string length           */

/* Packet sizes */
#define BLOCK_SIZE          512         /* Standard TFTP block size         */
#define ENHANCED_BLOCK_SIZE 4096        /* Enhanced (larger) block size     */
#define MAX_PACKET_SIZE     (ENHANCED_BLOCK_SIZE + 4)  /* data + header    */

/* Reliability */
#define MAX_RETRIES         5           /* Retransmit attempts              */
#define TIMEOUT_SEC         3           /* Seconds before retransmit        */
#define TIMEOUT_USEC        0           /* Microsecond component            */

/* Opcodes – first two match standard TFTP, rest are extensions */
#define OP_RRQ              1           /* Read request                     */
#define OP_WRQ              2           /* Write request                    */
#define OP_DATA             3           /* Data packet                      */
#define OP_ACK              4           /* Acknowledgment                   */
#define OP_ERROR            5           /* Error                            */
#define OP_DELETE           6           /* Delete request  (extension)      */
#define OP_DACK             7           /* Delete acknowledgment (ext.)     */

/* Error codes (subset – mirrors standard TFTP) */
#define ERR_UNDEFINED       0
#define ERR_FILE_NOT_FOUND  1
#define ERR_ACCESS_DENIED   2
#define ERR_DISK_FULL       3
#define ERR_ILLEGAL_OP      4
#define ERR_UNKNOWN_TID     5
#define ERR_FILE_EXISTS     6
#define ERR_NO_SUCH_USER    7

/* Directories the server uses */
#define FILE_STORAGE_DIR    "./server_files/"
#define BACKUP_DIR          "./server_files/backup/"

/* AES-256-CBC key & IV – in production, exchange these securely. */
#define AES_KEY_SIZE        32   /* 256 bits */
#define AES_IV_SIZE         16   /* 128 bits */

/* ------------------------------------------------------------------ */
/*  Packet structures                                                  */
/* ------------------------------------------------------------------ */

/* Generic header – first two bytes of every packet are the opcode.
 * __attribute__((packed)) ensures no padding between fields so that
 * structs match the exact wire format.                                */

typedef struct __attribute__((packed)) {
    uint16_t opcode;                    /* OP_RRQ or OP_WRQ               */
    char     filename[MAX_FILENAME];    /* null-terminated                */
    char     mode[MAX_MODE];            /* "octet" / "netascii"           */
} RequestPacket;

typedef struct __attribute__((packed)) {
    uint16_t opcode;                    /* OP_DATA                        */
    uint16_t block_num;                 /* 1-based block number           */
    uint8_t  data[ENHANCED_BLOCK_SIZE]; /* file data                      */
} DataPacket;

typedef struct __attribute__((packed)) {
    uint16_t opcode;                    /* OP_ACK                         */
    uint16_t block_num;                 /* acknowledged block number      */
} AckPacket;

typedef struct __attribute__((packed)) {
    uint16_t opcode;                    /* OP_ERROR                       */
    uint16_t error_code;
    char     error_msg[MAX_FILENAME];   /* human-readable message         */
} ErrorPacket;

typedef struct __attribute__((packed)) {
    uint16_t opcode;                    /* OP_DELETE                      */
    char     filename[MAX_FILENAME];    /* file to delete                 */
} DeletePacket;

typedef struct __attribute__((packed)) {
    uint16_t opcode;                    /* OP_DACK                        */
    uint16_t status;                    /* 0 = success, !0 = failure      */
    char     message[MAX_FILENAME];     /* human-readable result          */
} DeleteAckPacket;

/* ------------------------------------------------------------------ */
/*  Shared AES key/IV (hardcoded for the exercise; see note above)     */
/* ------------------------------------------------------------------ */
static const unsigned char SHARED_AES_KEY[AES_KEY_SIZE] = {
    0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
    0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
    0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
    0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4
};

static const unsigned char SHARED_AES_IV[AES_IV_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

/* ------------------------------------------------------------------ */
/*  Utility function declarations                                      */
/* ------------------------------------------------------------------ */

/*
 * aes_encrypt – Encrypt `plaintext_len` bytes of `plaintext` into
 *               `ciphertext`.  Returns the ciphertext length on success,
 *               or -1 on failure.
 */
static inline int aes_encrypt(const unsigned char *plaintext,
                               int plaintext_len,
                               unsigned char *ciphertext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0, ciphertext_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                           SHARED_AES_KEY, SHARED_AES_IV) != 1)
        goto fail;
    if (EVP_EncryptUpdate(ctx, ciphertext, &len,
                          plaintext, plaintext_len) != 1)
        goto fail;
    ciphertext_len = len;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1)
        goto fail;
    ciphertext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext_len;

fail:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/*
 * aes_decrypt – Decrypt `ciphertext_len` bytes of `ciphertext` into
 *               `plaintext`.  Returns the plaintext length on success,
 *               or -1 on failure.
 */
static inline int aes_decrypt(const unsigned char *ciphertext,
                               int ciphertext_len,
                               unsigned char *plaintext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0, plaintext_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                           SHARED_AES_KEY, SHARED_AES_IV) != 1)
        goto fail;
    if (EVP_DecryptUpdate(ctx, plaintext, &len,
                          ciphertext, ciphertext_len) != 1)
        goto fail;
    plaintext_len = len;
    if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) != 1)
        goto fail;
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return plaintext_len;

fail:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/*
 * compute_md5 – Compute the MD5 hash of `data` (length `len`) and
 *               store the 32-char hex digest in `out` (must be >= 33 bytes).
 */
static inline void compute_md5(const unsigned char *data, size_t len,
                                char *out)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, digest, NULL);
    EVP_MD_CTX_free(ctx);

    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(out + i * 2, "%02x", digest[i]);
    out[MD5_DIGEST_LENGTH * 2] = '\0';
}

/*
 * set_socket_timeout – Apply a receive-timeout to `sockfd`.
 */
static inline int set_socket_timeout(int sockfd, int sec, int usec)
{
    struct timeval tv;
    tv.tv_sec  = sec;
    tv.tv_usec = usec;
    return setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/*
 * ensure_directory – Create a directory (and parents) if it doesn't exist.
 */
static inline void ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == -1) {
        mkdir(path, 0755);
    }
}

/*
 * build_filepath – Safely concatenate a directory and a filename.
 */
static inline void build_filepath(char *dest, size_t dest_size,
                                   const char *dir, const char *filename)
{
    snprintf(dest, dest_size, "%s%s", dir, filename);
}

/*
 * send_error – Build and send an ERROR packet on `sockfd` to `dest`.
 */
static inline void send_error(int sockfd,
                               struct sockaddr_in *dest,
                               uint16_t code,
                               const char *msg)
{
    ErrorPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.opcode     = htons(OP_ERROR);
    pkt.error_code = htons(code);
    strncpy(pkt.error_msg, msg, sizeof(pkt.error_msg) - 1);
    sendto(sockfd, &pkt, sizeof(pkt), 0,
           (struct sockaddr *)dest, sizeof(*dest));
}

/*
 * print_timestamp – Print the current date/time for log messages.
 */
static inline void print_timestamp(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    printf("[%s] ", buf);
}

#endif /* UDP_FILE_TRANSFER_H */
