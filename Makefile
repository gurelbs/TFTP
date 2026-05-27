# Enhanced TFTP – UDP-Based File Transfer System
# ================================================
#
# Targets:
#   make          – build server + client
#   make server   – build server only
#   make client   – build client only
#   make clean    – remove binaries
#   make test     – quick smoke test (start server, upload, download)

CC       = gcc
CFLAGS   = -Wall -Wextra -g -O2
LDFLAGS  = -lssl -lcrypto -lpthread

.PHONY: all clean test

all: server client

server: server.c udp_file_transfer.h
	$(CC) $(CFLAGS) -o $@ server.c $(LDFLAGS)

client: client.c udp_file_transfer.h
	$(CC) $(CFLAGS) -o $@ client.c $(LDFLAGS)

clean:
	rm -f server client
	rm -rf server_files/

test: all
	@echo "=== Creating test file ==="
	echo "Hello, Enhanced TFTP!" > test_upload.txt
	@echo "=== Starting server in background ==="
	./server 6969 &
	sleep 1
	@echo "=== Upload test ==="
	echo -e "1\ntest_upload.txt\n4" | ./client 127.0.0.1 6969
	@echo "=== Stopping server ==="
	kill %1 2>/dev/null || true
	@echo "=== Done ==="
