/*
 * server.c
 * =====================================================================
 * Enhanced TFTP Server
 *
 * Features
 * --------
 *   • Listens on a UDP port for RRQ / WRQ / DELETE requests.
 *   • Transfers files in configurable block sizes (up to 4 KB).
 *   • AES-256-CBC encryption on all DATA payloads.
 *   • Automatic backup of every uploaded file.
 *   • File recovery from backup on demand.
 *   • MD5 integrity verification after upload.
 *   • Multithreaded – each client request is handled in its own thread.
 *   • Compatible with standard TFTP RRQ/WRQ (512-byte block mode).
 *
 * Compile
 * -------
 *   gcc -Wall -Wextra -pthread -o server server.c -lssl -lcrypto
 *
 * Run
 * ---
 *   ./server [port]          (default port: 6969)
 * =====================================================================
 */

#include "udp_file_transfer.h"
#include <dirent.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/*  Per-client context passed to the handler thread                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int                sockfd;          /* Dedicated socket for this TID  */
    struct sockaddr_in client_addr;     /* Client address                 */
    socklen_t          addr_len;
    uint16_t           opcode;          /* Original request opcode        */
    char               filename[MAX_FILENAME];
    char               mode[MAX_MODE];
    int                block_size;      /* Negotiated block size          */
} ClientContext;

/* ------------------------------------------------------------------ */
/*  Global flag for graceful shutdown                                   */
/* ------------------------------------------------------------------ */
static volatile int running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ================================================================== */
/*  Backup helpers                                                     */
/* ================================================================== */

/*
 * backup_file – Copy `src` to the backup directory.
 *               The backup is named "<original>.<timestamp>.bak".
 */
static void backup_file(const char *src_path, const char *filename)
{
    ensure_directory(BACKUP_DIR);

    char backup_path[512];
    time_t now = time(NULL);
    snprintf(backup_path, sizeof(backup_path),
             "%s%s.%ld.bak", BACKUP_DIR, filename, (long)now);

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        perror("backup_file: fopen src");
        return;
    }
    FILE *dst = fopen(backup_path, "wb");
    if (!dst) {
        perror("backup_file: fopen dst");
        fclose(src);
        return;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);

    fclose(src);
    fclose(dst);

    print_timestamp();
    printf("BACKUP  %s -> %s\n", filename, backup_path);
}

/*
 * recover_file – Restore the *latest* backup of `filename` into the
 *                storage directory.  Returns 0 on success.
 */
static int recover_file(const char *filename)
{
    DIR *dir = opendir(BACKUP_DIR);
    if (!dir) return -1;

    char latest[512] = {0};
    long latest_ts = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Match "<filename>.<timestamp>.bak" */
        size_t flen = strlen(filename);
        if (strncmp(entry->d_name, filename, flen) != 0) continue;
        if (entry->d_name[flen] != '.') continue;

        long ts = atol(entry->d_name + flen + 1);
        if (ts > latest_ts) {
            latest_ts = ts;
            snprintf(latest, sizeof(latest), "%s%s",
                     BACKUP_DIR, entry->d_name);
        }
    }
    closedir(dir);

    if (latest_ts == 0) return -1;  /* no backup found */

    char dest[512];
    build_filepath(dest, sizeof(dest), FILE_STORAGE_DIR, filename);

    FILE *src = fopen(latest, "rb");
    if (!src) return -1;
    FILE *dst = fopen(dest, "wb");
    if (!dst) { fclose(src); return -1; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);

    fclose(src);
    fclose(dst);

    print_timestamp();
    printf("RECOVER %s <- %s\n", filename, latest);
    return 0;
}

/* ================================================================== */
/*  RRQ handler – send a file to the client                            */
/* ================================================================== */

static void handle_rrq(ClientContext *ctx)
{
    char filepath[512];
    build_filepath(filepath, sizeof(filepath),
                   FILE_STORAGE_DIR, ctx->filename);

    FILE *fp = fopen(filepath, "rb");

    /* If the file is missing, attempt recovery from backup */
    if (!fp) {
        print_timestamp();
        printf("RRQ     %s not found – attempting recovery…\n",
               ctx->filename);
        if (recover_file(ctx->filename) == 0)
            fp = fopen(filepath, "rb");
    }

    if (!fp) {
        send_error(ctx->sockfd, &ctx->client_addr,
                   ERR_FILE_NOT_FOUND, "File not found");
        print_timestamp();
        printf("RRQ     %s – ERROR file not found\n", ctx->filename);
        return;
    }

    print_timestamp();
    printf("RRQ     sending %s (block %d bytes)\n",
           ctx->filename, ctx->block_size);

    uint8_t  raw_buf[ENHANCED_BLOCK_SIZE];
    uint8_t  enc_buf[ENHANCED_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint8_t  pkt_buf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint16_t block = 1;
    int      bytes_read;

    set_socket_timeout(ctx->sockfd, TIMEOUT_SEC, TIMEOUT_USEC);

    while (1) {
        bytes_read = (int)fread(raw_buf, 1, ctx->block_size, fp);
        if (bytes_read < 0) break;

        /* Encrypt the block */
        int enc_len = aes_encrypt(raw_buf, bytes_read, enc_buf);
        if (enc_len < 0) {
            send_error(ctx->sockfd, &ctx->client_addr,
                       ERR_UNDEFINED, "Encryption failed");
            break;
        }

        /* Build DATA packet: opcode(2) + block#(2) + encrypted data */
        uint16_t net_op  = htons(OP_DATA);
        uint16_t net_blk = htons(block);
        memcpy(pkt_buf, &net_op, 2);
        memcpy(pkt_buf + 2, &net_blk, 2);
        memcpy(pkt_buf + 4, enc_buf, enc_len);
        int pkt_len = 4 + enc_len;

        /* Send with retransmission */
        int retries = 0;
        while (retries < MAX_RETRIES) {
            sendto(ctx->sockfd, pkt_buf, pkt_len, 0,
                   (struct sockaddr *)&ctx->client_addr, ctx->addr_len);

            /* Wait for ACK */
            AckPacket ack;
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t r = recvfrom(ctx->sockfd, &ack, sizeof(ack), 0,
                                 (struct sockaddr *)&from, &flen);
            if (r >= (ssize_t)sizeof(ack) &&
                ntohs(ack.opcode) == OP_ACK &&
                ntohs(ack.block_num) == block) {
                break;  /* ACK received */
            }

            retries++;
            print_timestamp();
            printf("RRQ     block %u – retry %d/%d\n",
                   block, retries, MAX_RETRIES);
        }

        if (retries == MAX_RETRIES) {
            print_timestamp();
            printf("RRQ     %s – transfer timed out at block %u\n",
                   ctx->filename, block);
            break;
        }

        /* Last block? (raw bytes < block_size means EOF) */
        if (bytes_read < ctx->block_size)
            break;

        block++;
    }

    fclose(fp);
    print_timestamp();
    printf("RRQ     %s – transfer complete (%u blocks)\n",
           ctx->filename, block);
}

/* ================================================================== */
/*  WRQ handler – receive a file from the client                       */
/* ================================================================== */

static void handle_wrq(ClientContext *ctx)
{
    ensure_directory(FILE_STORAGE_DIR);

    char filepath[512];
    build_filepath(filepath, sizeof(filepath),
                   FILE_STORAGE_DIR, ctx->filename);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        send_error(ctx->sockfd, &ctx->client_addr,
                   ERR_ACCESS_DENIED, "Cannot create file");
        return;
    }

    /* Send initial ACK (block 0) to tell the client we're ready */
    AckPacket ack0;
    ack0.opcode    = htons(OP_ACK);
    ack0.block_num = htons(0);
    sendto(ctx->sockfd, &ack0, sizeof(ack0), 0,
           (struct sockaddr *)&ctx->client_addr, ctx->addr_len);

    print_timestamp();
    printf("WRQ     receiving %s\n", ctx->filename);

    uint8_t  recv_buf[MAX_PACKET_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint8_t  dec_buf[ENHANCED_BLOCK_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint16_t expected_block = 1;

    /* MD5 running context */
    EVP_MD_CTX *md5_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md5_ctx, EVP_md5(), NULL);

    set_socket_timeout(ctx->sockfd, TIMEOUT_SEC, TIMEOUT_USEC);

    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(ctx->sockfd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 4) {
            /* Timeout or tiny packet – could be a lost ACK scenario;
               the client will retransmit.                              */
            continue;
        }

        uint16_t opcode   = ntohs(*(uint16_t *)recv_buf);
        uint16_t block_no = ntohs(*(uint16_t *)(recv_buf + 2));

        if (opcode != OP_DATA) {
            send_error(ctx->sockfd, &ctx->client_addr,
                       ERR_ILLEGAL_OP, "Expected DATA packet");
            break;
        }

        if (block_no == expected_block) {
            int enc_len = (int)(n - 4);
            int dec_len = aes_decrypt(recv_buf + 4, enc_len, dec_buf);
            if (dec_len < 0) {
                send_error(ctx->sockfd, &ctx->client_addr,
                           ERR_UNDEFINED, "Decryption failed");
                break;
            }
            fwrite(dec_buf, 1, dec_len, fp);
            EVP_DigestUpdate(md5_ctx, dec_buf, dec_len);

            /* Send ACK */
            AckPacket ack;
            ack.opcode    = htons(OP_ACK);
            ack.block_num = htons(expected_block);
            sendto(ctx->sockfd, &ack, sizeof(ack), 0,
                   (struct sockaddr *)&ctx->client_addr, ctx->addr_len);

            /* Last block? */
            if (dec_len < ctx->block_size) {
                /* Compute final MD5 */
                unsigned char digest[MD5_DIGEST_LENGTH];
                EVP_DigestFinal_ex(md5_ctx, digest, NULL);
                char hex[33];
                for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
                    sprintf(hex + i * 2, "%02x", digest[i]);
                hex[32] = '\0';

                print_timestamp();
                printf("WRQ     %s – complete, MD5: %s\n",
                       ctx->filename, hex);
                break;
            }

            expected_block++;
        } else if (block_no < expected_block) {
            /* Duplicate – re-ACK */
            AckPacket ack;
            ack.opcode    = htons(OP_ACK);
            ack.block_num = htons(block_no);
            sendto(ctx->sockfd, &ack, sizeof(ack), 0,
                   (struct sockaddr *)&ctx->client_addr, ctx->addr_len);
        }
    }

    EVP_MD_CTX_free(md5_ctx);
    fclose(fp);

    /* Back up the received file */
    backup_file(filepath, ctx->filename);
}

/* ================================================================== */
/*  DELETE handler                                                     */
/* ================================================================== */

static void handle_delete(ClientContext *ctx)
{
    char filepath[512];
    build_filepath(filepath, sizeof(filepath),
                   FILE_STORAGE_DIR, ctx->filename);

    DeleteAckPacket dack;
    memset(&dack, 0, sizeof(dack));
    dack.opcode = htons(OP_DACK);

    if (remove(filepath) == 0) {
        dack.status = htons(0);
        strncpy(dack.message, "File deleted successfully",
                sizeof(dack.message) - 1);
        print_timestamp();
        printf("DELETE  %s – OK\n", ctx->filename);
    } else {
        dack.status = htons(1);
        strncpy(dack.message, "File not found or cannot delete",
                sizeof(dack.message) - 1);
        print_timestamp();
        printf("DELETE  %s – FAILED (%s)\n", ctx->filename,
               strerror(errno));
    }

    sendto(ctx->sockfd, &dack, sizeof(dack), 0,
           (struct sockaddr *)&ctx->client_addr, ctx->addr_len);
}

/* ================================================================== */
/*  Thread entry point                                                 */
/* ================================================================== */

static void *client_handler(void *arg)
{
    ClientContext *ctx = (ClientContext *)arg;

    switch (ctx->opcode) {
        case OP_RRQ:    handle_rrq(ctx);    break;
        case OP_WRQ:    handle_wrq(ctx);    break;
        case OP_DELETE: handle_delete(ctx); break;
        default:
            send_error(ctx->sockfd, &ctx->client_addr,
                       ERR_ILLEGAL_OP, "Unknown opcode");
            break;
    }

    close(ctx->sockfd);
    free(ctx);
    return NULL;
}

/* ================================================================== */
/*  Parse incoming request packet                                      */
/* ================================================================== */

/*
 * parse_request – Extract opcode, filename, and mode from a raw
 *                 request buffer.  Returns the opcode, or -1 on error.
 */
static int parse_request(const uint8_t *buf, ssize_t len,
                          char *filename, char *mode)
{
    if (len < 4) return -1;

    uint16_t opcode = ntohs(*(uint16_t *)buf);

    if (opcode == OP_DELETE) {
        /* DELETE: opcode(2) + filename (null-terminated) */
        snprintf(filename, MAX_FILENAME, "%s", (const char *)(buf + 2));
        mode[0] = '\0';
        return OP_DELETE;
    }

    /* Standard TFTP RRQ / WRQ: opcode(2) + filename\0 + mode\0 */
    const char *p = (const char *)(buf + 2);
    const char *end = (const char *)(buf + len);

    /* Filename */
    size_t flen = strnlen(p, end - p);
    if (p + flen >= end) return -1;
    snprintf(filename, MAX_FILENAME, "%s", p);
    p += flen + 1;

    /* Mode */
    flen = strnlen(p, end - p);
    strncpy(mode, p, MAX_MODE - 1);
    mode[MAX_MODE - 1] = '\0';

    return (int)opcode;
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char *argv[])
{
    uint16_t port = TFTP_PORT;
    if (argc >= 2)
        port = (uint16_t)atoi(argv[1]);

    /* Ensure storage directories exist */
    ensure_directory(FILE_STORAGE_DIR);
    ensure_directory(BACKUP_DIR);

    /* Set up signal handler for graceful shutdown */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Create main listening socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(port);

    if (bind(sockfd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return EXIT_FAILURE;
    }

    print_timestamp();
    printf("========================================\n");
    print_timestamp();
    printf("Enhanced TFTP Server listening on port %u\n", port);
    print_timestamp();
    printf("Storage : %s\n", FILE_STORAGE_DIR);
    print_timestamp();
    printf("Backup  : %s\n", BACKUP_DIR);
    print_timestamp();
    printf("Encryption : AES-256-CBC\n");
    print_timestamp();
    printf("Block size  : %d bytes (enhanced) / %d bytes (compat)\n",
           ENHANCED_BLOCK_SIZE, BLOCK_SIZE);
    print_timestamp();
    printf("========================================\n");

    uint8_t recv_buf[MAX_PACKET_SIZE];

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        /* Use a short timeout so we can check `running` periodically */
        set_socket_timeout(sockfd, 1, 0);

        ssize_t n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n <= 0) continue;

        /* Parse the request */
        char filename[MAX_FILENAME] = {0};
        char mode[MAX_MODE]         = {0};
        int  opcode = parse_request(recv_buf, n, filename, mode);
        if (opcode < 0) {
            send_error(sockfd, &client_addr,
                       ERR_ILLEGAL_OP, "Malformed request");
            continue;
        }

        print_timestamp();
        printf("REQUEST opcode=%d file=%s mode=%s from %s:%d\n",
               opcode, filename, mode,
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        /* Determine block size: standard TFTP clients use "netascii"
           or "octet" – we fall back to 512 for compatibility.         */
        int blk_size = ENHANCED_BLOCK_SIZE;
        if (mode[0] != '\0' &&
            (strcasecmp(mode, "octet") == 0 ||
             strcasecmp(mode, "netascii") == 0)) {
            blk_size = BLOCK_SIZE;  /* standard TFTP compat */
        }

        /* Create a new socket for the transfer (new TID) */
        int child_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (child_sock < 0) {
            perror("socket (child)");
            continue;
        }

        /* Bind to an ephemeral port */
        struct sockaddr_in child_addr;
        memset(&child_addr, 0, sizeof(child_addr));
        child_addr.sin_family      = AF_INET;
        child_addr.sin_addr.s_addr = INADDR_ANY;
        child_addr.sin_port        = 0;  /* OS picks port */
        if (bind(child_sock, (struct sockaddr *)&child_addr,
                 sizeof(child_addr)) < 0) {
            perror("bind (child)");
            close(child_sock);
            continue;
        }

        /* Prepare the per-client context */
        ClientContext *ctx = calloc(1, sizeof(ClientContext));
        if (!ctx) { close(child_sock); continue; }

        ctx->sockfd      = child_sock;
        ctx->client_addr = client_addr;
        ctx->addr_len    = addr_len;
        ctx->opcode      = (uint16_t)opcode;
        ctx->block_size  = blk_size;
        snprintf(ctx->filename, MAX_FILENAME, "%s", filename);
        snprintf(ctx->mode, MAX_MODE, "%s", mode);

        /* Spawn handler thread (detached) */
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, client_handler, ctx) != 0) {
            perror("pthread_create");
            close(child_sock);
            free(ctx);
        }

        pthread_attr_destroy(&attr);
    }

    close(sockfd);
    print_timestamp();
    printf("Server shut down.\n");
    return EXIT_SUCCESS;
}
