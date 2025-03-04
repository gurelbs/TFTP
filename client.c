#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(){
    int client_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_socket == -1) {
        perror("socket() error");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(12345);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    char message[] = "Hello, from client!, I am sending this message to server";
    ssize_t bytes_sent = sendto(client_socket, message, strlen(message), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (bytes_sent == -1) {
        perror("sendto() error");
        close(client_socket);
        exit(EXIT_FAILURE);
    }

    char buffer[1024];
    struct sockaddr_in server_response_addr;
    socklen_t server_response_addr_size = sizeof(server_response_addr);
    ssize_t bytes_received  = recvfrom(client_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&server_response_addr, &server_response_addr_size);

    if (bytes_received == -1) {
        perror("recvfrom() error");
        close(client_socket);
        exit(EXIT_FAILURE);
    }

    printf("Recieved response from server: %.*s\n", (int)bytes_received, buffer);

    close(client_socket);
    return 0;
}