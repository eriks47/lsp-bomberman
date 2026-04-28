#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

static int connect_to_server(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &address.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <host> <port> <player_name>\n", argv[0]);
        return 1;
    }

    const char* host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char* player_name = argv[3];

    int fd = connect_to_server(host, port);

    if (fd < 0) {
        return 1;
    }

    if (send_hello(fd, "lsp-client-0.1", player_name) != 0) {
        fprintf(stderr, "Failed to send HELLO\n");
        close(fd);
        return 1;
    }

    printf("HELLO sent\n");

    msg_header_t header;

    if (recv_header(fd, &header) != 0) {
        fprintf(stderr, "Failed to read response header\n");
        close(fd);
        return 1;
    }

    if (header.msg_type != MSG_WELCOME) {
        fprintf(stderr, "Expected WELCOME, got message type %u\n", header.msg_type);
        close(fd);
        return 1;
    }

    msg_welcome_t welcome;

    if (recv_welcome_payload(fd, &welcome) != 0) {
        fprintf(stderr, "Failed to read WELCOME payload\n");
        close(fd);
        return 1;
    }

    printf("WELCOME received\n");
    printf("Server id: %s\n", welcome.server_id);
    printf("Game status: %u\n", welcome.game_status);
    printf("My player id: %u\n", header.target_id);

    close(fd);

    return 0;
}