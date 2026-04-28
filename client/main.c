#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

static void print_help(void) {
    printf("Commands:\n");
    printf("  /ready  mark yourself ready\n");
    printf("  /ping   send PING\n");
    printf("  /quit   leave the lobby\n");
}

static bool handle_server_message(int fd, uint8_t my_id) {
    msg_header_t header;

    if (recv_header(fd, &header) != 0) {
        printf("Server disconnected\n");
        return false;
    }

    switch (header.msg_type) {
        case MSG_HELLO: {
            msg_hello_t hello;

            if (recv_hello_payload(fd, &hello) != 0) {
                printf("Failed to read HELLO from server\n");
                return false;
            }

            printf("Player %u joined: %s\n", header.sender_id, hello.player_name);
            break;
        }

        case MSG_LEAVE:
            printf("Player %u left\n", header.sender_id);
            break;

        case MSG_SET_READY:
            printf("Player %u is ready\n", header.sender_id);
            break;

        case MSG_PING:
            send_header(fd, MSG_PONG, my_id, TARGET_SERVER);
            break;

        case MSG_PONG:
            printf("PONG received\n");
            break;

        case MSG_ERROR:
            printf("ERROR received from server\n");
            break;

        case MSG_DISCONNECT:
            printf("Server disconnected us\n");
            return false;

        default:
            printf("Unknown message type %u from server\n", header.msg_type);
            break;
    }

    return true;
}

static bool handle_user_input(int fd, uint8_t my_id) {
    char line[128];

    if (fgets(line, sizeof(line), stdin) == NULL) {
        return false;
    }

    line[strcspn(line, "\n")] = '\0';

    if (strcmp(line, "/ready") == 0) {
        if (send_header(fd, MSG_SET_READY, my_id, TARGET_SERVER) != 0) {
            return false;
        }

        printf("READY sent\n");
    } else if (strcmp(line, "/ping") == 0) {
        if (send_header(fd, MSG_PING, my_id, TARGET_SERVER) != 0) {
            return false;
        }

        printf("PING sent\n");
    } else if (strcmp(line, "/quit") == 0) {
        send_header(fd, MSG_LEAVE, my_id, TARGET_SERVER);
        return false;
    } else {
        print_help();
    }

    return true;
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

    protocol_player_info_t players[MAX_PLAYERS];
    msg_welcome_t welcome;

    if (recv_welcome_payload(fd, &welcome, players, MAX_PLAYERS) != 0) {
        fprintf(stderr, "Failed to read WELCOME payload\n");
        close(fd);
        return 1;
    }

    uint8_t my_id = header.target_id;

    printf("Connected to server: %s\n", welcome.server_id);
    printf("Game status: %u\n", welcome.game_status);
    printf("My player id: %u\n", my_id);

    if (welcome.other_players_count > 0) {
        printf("Players already in lobby:\n");

        for (uint8_t i = 0; i < welcome.other_players_count; ++i) {
            printf("  %u: %s ready=%u\n",
                   players[i].id,
                   players[i].player_name,
                   players[i].ready);
        }
    }

    print_help();

    bool running = true;

    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        int max_fd = fd > STDIN_FILENO ? fd : STDIN_FILENO;

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(fd, &read_fds)) {
            running = handle_server_message(fd, my_id);
        }

        if (running && FD_ISSET(STDIN_FILENO, &read_fds)) {
            running = handle_user_input(fd, my_id);
        }
    }

    close(fd);

    return 0;
}