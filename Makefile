CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lssl -lcrypto

all: server client

server: server.c common.c udp_file_transfer.h
	$(CC) $(CFLAGS) -o server server.c common.c $(LDFLAGS)

client: client.c common.c udp_file_transfer.h
	$(CC) $(CFLAGS) -o client client.c common.c $(LDFLAGS)

clean:
	rm -f server client
