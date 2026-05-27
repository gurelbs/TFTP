/*
 * client.c
 * =====================================================================
 * Enhanced TFTP Client
 *
 * Supports
 * --------
 *   • Upload   (WRQ)  – send a local file to the server.
 *   • Download (RRQ)  – fetch a file from the server.
 *   • Delete          – ask the server to remove a file.
 *   • AES-256-CBC encryption on all DATA payloads.
 *   • MD5 integrity check printed after each transfer.
 *   • Configurable block size and retransmission.
 *
 * Compile
 * -------
 *   gcc -Wall -Wextra -o client client.c -lssl -lcrypto
 *
 * Usage
 * -----
 *   ./client <server_ip> [port]
 *
 *   Interactive menu:
 *     1) Upload a file
 *     2) Download a file
 *     3) Delete a file
 *     4) Quit
 * =====================================================================
 */

#include "udp_file_transfer.h"

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */
static struct sockaddr_in server_addr;
static socklen_t          addr_len = sizeof(struct sockaddr_in);
static int                g_block_size = ENHANCED_BLOCK_SIZE;

/* ================================================================== */
/*  Upload (WRQ)                                                       */
/* ================================================================== */

static int upload_file(int sockfd, const char *filename)
{
    /* Open the local file */
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("upload: fopen");
        return -1;
    }

    /* ---- Send WRQ packet ---------------------------------------- */
    uint8_t req_buf[MAX_PACKET_SIZE];
    memset(req_buf, 0, sizeof(req_buf));

    uint16_t op = htons(OP_WRQ);
    memcpy(req_buf, &op, 2);

    /* Extract base filename */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    size_t off = 2;
    memcpy(req_buf + off, base, strlen(base) + 1);
    off += strlen(base) + 1;
    memcpy(req_buf + off, "enhanced", 9);  /* mode = "enhanced" */
    off += 9;

    sendto(sockfd, req_buf, off, 0,
           (struct sockaddr *)&server_addr, addr_len);

    /* ---- Wait for ACK block 0 ----------------------------------- */
    set_socket_timeout(sockfd, TIMEOUT_SEC, TIMEOUT_USEC);

    AckPacket ack;
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t r = recvfrom(sockfd, &ack, sizeof(ack), 0,
                         (struct sockaddr *)&from, &flen);
    if (r < (ssize_t)sizeof(ack) ||
        ntohs(ack.opcode) != OP_ACK ||
        ntohs(ack.block_num) != 0) {
        fprintf(stderr, "upload: did not receive ACK 0 from server\n");
        fclose(fp);
        return -1;
    }

    /* From now on, talk to the server's child TID (ephemeral port) */
    struct sockaddr_in tid_addr = from;

    printf("  Server ready.  Uploading \"%s\" …\n", base);

    /* ---- Send DATA packets -------------------------------------- */
    uint8_t  raw[ENHANCED_BLOCK_SIZE];
    uint8_t  enc[ENHANCED_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint8_t  pkt[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint16_t block = 1;

    /* MD5 running context */
    EVP_MD_CTX *md5_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md5_ctx, EVP_md5(), NULL);

    while (1) {
        int bytes_read = (int)fread(raw, 1, g_block_size, fp);
        if (bytes_read < 0) break;

        EVP_DigestUpdate(md5_ctx, raw, bytes_read);

        /* Encrypt */
        int enc_len = aes_encrypt(raw, bytes_read, enc);
        if (enc_len < 0) {
            fprintf(stderr, "upload: encryption error\n");
            break;
        }

        /* Build DATA packet */
        uint16_t net_op  = htons(OP_DATA);
        uint16_t net_blk = htons(block);
        memcpy(pkt, &net_op, 2);
        memcpy(pkt + 2, &net_blk, 2);
        memcpy(pkt + 4, enc, enc_len);
        int pkt_len = 4 + enc_len;

        /* Send + wait ACK with retransmit */
        int retries = 0;
        int acked   = 0;
        while (retries < MAX_RETRIES) {
            sendto(sockfd, pkt, pkt_len, 0,
                   (struct sockaddr *)&tid_addr, addr_len);

            AckPacket a;
            struct sockaddr_in afrom;
            socklen_t al = sizeof(afrom);
            ssize_t ar = recvfrom(sockfd, &a, sizeof(a), 0,
                                  (struct sockaddr *)&afrom, &al);
            if (ar >= (ssize_t)sizeof(a) &&
                ntohs(a.opcode) == OP_ACK &&
                ntohs(a.block_num) == block) {
                acked = 1;
                break;
            }

            retries++;
            printf("  block %u – retransmit %d/%d\n",
                   block, retries, MAX_RETRIES);
        }

        if (!acked) {
            fprintf(stderr, "upload: transfer timed out at block %u\n",
                    block);
            break;
        }

        /* Last block? */
        if (bytes_read < g_block_size)
            break;

        block++;
    }

    /* Final MD5 */
    unsigned char digest[MD5_DIGEST_LENGTH];
    EVP_DigestFinal_ex(md5_ctx, digest, NULL);
    EVP_MD_CTX_free(md5_ctx);

    char hex[33];
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    hex[32] = '\0';

    printf("  Upload complete – %u blocks sent.\n", block);
    printf("  MD5: %s\n", hex);

    fclose(fp);
    return 0;
}

/* ================================================================== */
/*  Download (RRQ)                                                     */
/* ================================================================== */

static int download_file(int sockfd, const char *filename)
{
    /* ---- Send RRQ packet ---------------------------------------- */
    uint8_t req_buf[MAX_PACKET_SIZE];
    memset(req_buf, 0, sizeof(req_buf));

    uint16_t op = htons(OP_RRQ);
    memcpy(req_buf, &op, 2);

    size_t off = 2;
    memcpy(req_buf + off, filename, strlen(filename) + 1);
    off += strlen(filename) + 1;
    memcpy(req_buf + off, "enhanced", 9);
    off += 9;

    sendto(sockfd, req_buf, off, 0,
           (struct sockaddr *)&server_addr, addr_len);

    printf("  Downloading \"%s\" …\n", filename);

    /* ---- Open output file --------------------------------------- */
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("download: fopen");
        return -1;
    }

    set_socket_timeout(sockfd, TIMEOUT_SEC, TIMEOUT_USEC);

    uint8_t  recv_buf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint8_t  dec_buf[ENHANCED_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint16_t expected_block = 1;

    /* MD5 running context */
    EVP_MD_CTX *md5_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md5_ctx, EVP_md5(), NULL);

    struct sockaddr_in tid_addr;
    int tid_set = 0;

    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 4) {
            /* Check for ERROR packet or timeout */
            if (n >= 4) {
                uint16_t opc = ntohs(*(uint16_t *)recv_buf);
                if (opc == OP_ERROR) {
                    uint16_t ec = ntohs(*(uint16_t *)(recv_buf + 2));
                    fprintf(stderr, "  Server error %u: %s\n",
                            ec, (char *)(recv_buf + 4));
                    break;
                }
            }

            /* Timeout – request retransmit by re-sending last ACK */
            if (expected_block > 1) {
                AckPacket ack;
                ack.opcode    = htons(OP_ACK);
                ack.block_num = htons(expected_block - 1);
                sendto(sockfd, &ack, sizeof(ack), 0,
                       (struct sockaddr *)&tid_addr, addr_len);
            }
            continue;
        }

        /* Capture the server's ephemeral TID on first DATA packet */
        if (!tid_set) {
            tid_addr = from;
            tid_set  = 1;
        }

        uint16_t opcode   = ntohs(*(uint16_t *)recv_buf);
        if (opcode == OP_ERROR) {
            uint16_t ec = ntohs(*(uint16_t *)(recv_buf + 2));
            fprintf(stderr, "  Server error %u: %s\n",
                    ec, (char *)(recv_buf + 4));
            break;
        }

        uint16_t block_no = ntohs(*(uint16_t *)(recv_buf + 2));

        if (opcode == OP_DATA && block_no == expected_block) {
            int enc_len = (int)(n - 4);
            int dec_len = aes_decrypt(recv_buf + 4, enc_len, dec_buf);
            if (dec_len < 0) {
                fprintf(stderr, "  Decryption error at block %u\n",
                        block_no);
                break;
            }

            fwrite(dec_buf, 1, dec_len, fp);
            EVP_DigestUpdate(md5_ctx, dec_buf, dec_len);

            /* Send ACK */
            AckPacket ack;
            ack.opcode    = htons(OP_ACK);
            ack.block_num = htons(expected_block);
            sendto(sockfd, &ack, sizeof(ack), 0,
                   (struct sockaddr *)&tid_addr, addr_len);

            /* Last block? */
            if (dec_len < g_block_size)
                break;

            expected_block++;
        }
    }

    /* Final MD5 */
    unsigned char digest[MD5_DIGEST_LENGTH];
    EVP_DigestFinal_ex(md5_ctx, digest, NULL);
    EVP_MD_CTX_free(md5_ctx);

    char hex[33];
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    hex[32] = '\0';

    printf("  Download complete – %u blocks received.\n", expected_block);
    printf("  MD5: %s\n", hex);

    fclose(fp);
    return 0;
}

/* ================================================================== */
/*  Delete                                                             */
/* ================================================================== */

static int delete_file(int sockfd, const char *filename)
{
    DeletePacket dpkt;
    memset(&dpkt, 0, sizeof(dpkt));
    dpkt.opcode = htons(OP_DELETE);
    snprintf(dpkt.filename, MAX_FILENAME, "%s", filename);

    sendto(sockfd, &dpkt, sizeof(dpkt), 0,
           (struct sockaddr *)&server_addr, addr_len);

    printf("  Requesting deletion of \"%s\" …\n", filename);

    set_socket_timeout(sockfd, TIMEOUT_SEC, TIMEOUT_USEC);

    DeleteAckPacket dack;
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t r = recvfrom(sockfd, &dack, sizeof(dack), 0,
                         (struct sockaddr *)&from, &flen);

    if (r >= (ssize_t)sizeof(dack) && ntohs(dack.opcode) == OP_DACK) {
        uint16_t st = ntohs(dack.status);
        printf("  Server response (%s): %s\n",
               st == 0 ? "OK" : "FAIL", dack.message);
        return (st == 0) ? 0 : -1;
    }

    fprintf(stderr, "  No response from server.\n");
    return -1;
}

/* ================================================================== */
/*  Interactive menu                                                   */
/* ================================================================== */

static void print_menu(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║     Enhanced TFTP Client – Menu      ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║  1)  Upload a file                   ║\n");
    printf("║  2)  Download a file                 ║\n");
    printf("║  3)  Delete a file                   ║\n");
    printf("║  4)  Quit                            ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("  Choice: ");
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    uint16_t port = TFTP_PORT;
    if (argc >= 3)
        port = (uint16_t)atoi(argv[2]);

    /* Create socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /* Server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server address: %s\n", server_ip);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("========================================\n");
    printf("  Enhanced TFTP Client\n");
    printf("  Server   : %s:%u\n", server_ip, port);
    printf("  Encryption: AES-256-CBC\n");
    printf("  Block size: %d bytes\n", g_block_size);
    printf("========================================\n");

    char input[MAX_FILENAME];

    while (1) {
        print_menu();
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            break;

        int choice = atoi(input);

        switch (choice) {
        case 1: {
            printf("  Local file path: ");
            fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            upload_file(sockfd, input);
            break;
        }
        case 2: {
            printf("  Remote filename: ");
            fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            download_file(sockfd, input);
            break;
        }
        case 3: {
            printf("  Filename to delete: ");
            fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            delete_file(sockfd, input);
            break;
        }
        case 4:
            printf("  Goodbye!\n");
            close(sockfd);
            return EXIT_SUCCESS;
        default:
            printf("  Invalid choice.\n");
            break;
        }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}
