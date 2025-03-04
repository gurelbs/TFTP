#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(){
    int server_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_socket == -1){
        printf("socket() error\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(12345);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1){
        printf("bind() error\n");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    char buffer[1024];
    struct sockaddr_in client_addr;
    socklen_t client_addr_size = sizeof(client_addr);

    while (1) {
        ssize_t bytes_received = recvfrom(server_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &client_addr_size);
        if (bytes_received == -1){
            perror("recvfrom() error");
            continue;
        }

        printf("Recieved message from %s:%d: %.*s\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), (int)bytes_received, buffer);
        sendto(server_socket, buffer, bytes_received, 0, (struct sockaddr *)&client_addr, client_addr_size);

    }
    
    close(server_socket);
    return 0;

}