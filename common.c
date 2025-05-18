#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/aes.h>
#include <openssl/md5.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "udp_file_transfer.h"
#include "common.h"

// Hardcoded AES key for demonstration (in real app, use secure key exchange)
unsigned char aes_key[AES_KEY_SIZE] = "TFTPSecretKey123";
AES_KEY enc_key, dec_key;

// Function to calculate MD5 hash of a file
void calculate_md5(FILE *file, unsigned char *digest) {
    MD5_CTX md5Context;
    unsigned char buffer[1024];
    int bytes;

    // Reset file position to beginning
    fseek(file, 0, SEEK_SET);

    MD5_Init(&md5Context);
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        MD5_Update(&md5Context, buffer, bytes);
    }
    MD5_Final(digest, &md5Context);

    // Reset file position to beginning
    fseek(file, 0, SEEK_SET);
}

// Function to encrypt data
int encrypt_data(unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext) {
    unsigned char iv[AES_BLOCK_SIZE] = {0}; // Initialization vector (all zeros for simplicity)
    int padding = AES_BLOCK_SIZE - (plaintext_len % AES_BLOCK_SIZE);
    int ciphertext_len = plaintext_len + padding;

    // Create a buffer that includes padding
    unsigned char *padded_plaintext = malloc(ciphertext_len);
    memcpy(padded_plaintext, plaintext, plaintext_len);

    // PKCS#7 padding
    memset(padded_plaintext + plaintext_len, padding, padding);

    // Encrypt in CBC mode
    AES_cbc_encrypt(padded_plaintext, ciphertext, ciphertext_len, &enc_key, iv, AES_ENCRYPT);

    free(padded_plaintext);
    return ciphertext_len;
}

// Function to decrypt data
int decrypt_data(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext) {
    unsigned char iv[AES_BLOCK_SIZE] = {0}; // Initialization vector (all zeros for simplicity)

    // Decrypt in CBC mode
    AES_cbc_encrypt(ciphertext, plaintext, ciphertext_len, &dec_key, iv, AES_DECRYPT);

    // Handle PKCS#7 padding
    int padding = plaintext[ciphertext_len - 1];

    // Verify valid padding (must be between 1 and AES_BLOCK_SIZE)
    if (padding > 0 && padding <= AES_BLOCK_SIZE) {
        // Check if all padding bytes are correct
        int valid_padding = 1;
        for (int i = ciphertext_len - padding; i < ciphertext_len; i++) {
            if (plaintext[i] != padding) {
                valid_padding = 0;
                break;
            }
        }
        if (valid_padding) {
            return ciphertext_len - padding;
        }
    }
    // If padding is invalid, return the full length
    return ciphertext_len;
}

// Function to initialize AES keys (call this in main of client/server)
void init_aes_keys() {
    AES_set_encrypt_key(aes_key, 128, &enc_key);
    AES_set_decrypt_key(aes_key, 128, &dec_key);
}

// Function to set socket timeout
void set_socket_timeout(int socket, int sec, int usec) {
    struct timeval timeout;
    timeout.tv_sec = sec;
    timeout.tv_usec = usec;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Set socket timeout failed");
        exit(EXIT_FAILURE);
    }
}

// Function to ensure backup directory exists
void ensure_backup_dir(const char *backup_dir) {
    struct stat st = {0};
    if (stat(backup_dir, &st) == -1) {
        if (mkdir(backup_dir, 0700) < 0) {
            perror("Failed to create backup directory");
            exit(EXIT_FAILURE);
        }
    }
}
