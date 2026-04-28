#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

static int create_server_socket(uint16_t port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, MAX_PLAYERS) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    int server_fd = create_server_socket(port);

    if (server_fd < 0) {
        return 1;
    }

    printf("Server listening on port %u\n", port);

    struct sockaddr_in client_address;
    socklen_t client_address_size = sizeof(client_address);

    int client_fd = accept(server_fd,
                           (struct sockaddr*)&client_address,
                           &client_address_size);

    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        return 1;
    }

    printf("Client connected\n");

    msg_header_t header;

    if (recv_header(client_fd, &header) != 0) {
        fprintf(stderr, "Failed to read message header\n");
        close(client_fd);
        close(server_fd);
        return 1;
    }

    if (header.msg_type != MSG_HELLO) {
        fprintf(stderr, "Expected HELLO, got message type %u\n", header.msg_type);
        close(client_fd);
        close(server_fd);
        return 1;
    }

    msg_hello_t hello;

    if (recv_hello_payload(client_fd, &hello) != 0) {
        fprintf(stderr, "Failed to read HELLO payload\n");
        close(client_fd);
        close(server_fd);
        return 1;
    }

    uint8_t player_id = 0;

    printf("HELLO received\n");
    printf("Client id: %s\n", hello.client_id);
    printf("Player name: %s\n", hello.player_name);
    printf("Assigned player id: %u\n", player_id);

    if (send_welcome(client_fd, player_id, GAME_LOBBY) != 0) {
        fprintf(stderr, "Failed to send WELCOME\n");
        close(client_fd);
        close(server_fd);
        return 1;
    }

    printf("WELCOME sent\n");

    close(client_fd);
    close(server_fd);

    return 0;
}