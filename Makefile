CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = 

# Targets
all: server client

server: server.c udp_file_transfer.h
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS)

client: client.c udp_file_transfer.h
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS)

clean:
	rm -f server client

.PHONY: all clean