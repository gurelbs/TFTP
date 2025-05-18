#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <openssl/aes.h>

extern unsigned char aes_key[AES_KEY_SIZE];
extern AES_KEY enc_key, dec_key;

void calculate_md5(FILE *file, unsigned char *digest);
int encrypt_data(unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext);
int decrypt_data(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext);
void init_aes_keys();

#endif