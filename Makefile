CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lssl -lcrypto

all: server client

server: server.o common.o server_operations.o
	$(CC) $(CFLAGS) -o server server.o common.o server_operations.o $(LDFLAGS)

client: client.o common.o client_operations.o
	$(CC) $(CFLAGS) -o client client.o common.o client_operations.o $(LDFLAGS)

server.o: server.c udp_file_transfer.h common.h server_operations.h
	$(CC) $(CFLAGS) -c server.c

client.o: client.c udp_file_transfer.h common.h client_operations.h
	$(CC) $(CFLAGS) -c client.c

common.o: common.c udp_file_transfer.h common.h
	$(CC) $(CFLAGS) -c common.c

server_operations.o: server_operations.c udp_file_transfer.h common.h server_operations.h
	$(CC) $(CFLAGS) -c server_operations.c

client_operations.o: client_operations.c udp_file_transfer.h common.h client_operations.h
	$(CC) $(CFLAGS) -c client_operations.c

clean:
	rm -f server client *.o
