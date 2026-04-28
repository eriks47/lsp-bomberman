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

typedef struct {
    int fd;
    bool active;
    bool ready;
    uint8_t id;
    char name[PROTOCOL_PLAYER_NAME_LEN];
} client_t;

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

static void init_clients(client_t clients[MAX_PLAYERS]) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        clients[i].fd = -1;
        clients[i].active = false;
        clients[i].ready = false;
        clients[i].id = i;
        clients[i].name[0] = '\0';
    }
}

static int find_free_client_id(const client_t clients[MAX_PLAYERS]) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (!clients[i].active) {
            return i;
        }
    }

    return -1;
}

static uint8_t build_player_list(const client_t clients[MAX_PLAYERS],
                                 protocol_player_info_t players[MAX_PLAYERS]) {
    uint8_t count = 0;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            players[count].id = clients[i].id;
            players[count].ready = clients[i].ready ? 1 : 0;
            copy_protocol_string(players[count].player_name,
                                 sizeof(players[count].player_name),
                                 clients[i].name);
            ++count;
        }
    }

    return count;
}

static void broadcast_header(const client_t clients[MAX_PLAYERS],
                             uint8_t msg_type,
                             uint8_t sender_id,
                             int except_fd) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active && clients[i].fd != except_fd) {
            send_header(clients[i].fd, msg_type, sender_id, TARGET_BROADCAST);
        }
    }
}

static void broadcast_hello(const client_t clients[MAX_PLAYERS],
                            uint8_t sender_id,
                            const msg_hello_t* hello,
                            int except_fd) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active && clients[i].fd != except_fd) {
            if (send_header(clients[i].fd, MSG_HELLO, sender_id, TARGET_BROADCAST) == 0) {
                send_all(clients[i].fd, hello, sizeof(*hello));
            }
        }
    }
}

static void broadcast_status(const client_t clients[MAX_PLAYERS],
                             uint8_t game_status) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_status(clients[i].fd,
                        TARGET_SERVER,
                        TARGET_BROADCAST,
                        game_status);
        }
    }
}

static void broadcast_map(const client_t clients[MAX_PLAYERS],
                          const game_map_t* map) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_map(clients[i].fd,
                     TARGET_SERVER,
                     TARGET_BROADCAST,
                     map);
        }
    }
}

static bool all_ready(const client_t clients[MAX_PLAYERS]) {
    uint8_t active_count = 0;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            ++active_count;

            if (!clients[i].ready) {
                return false;
            }
        }
    }

    return active_count >= 2;
}

static void disconnect_client(client_t clients[MAX_PLAYERS], uint8_t id) {
    printf("Player %u disconnected\n", id);

    close(clients[id].fd);
    clients[id].fd = -1;
    clients[id].active = false;
    clients[id].ready = false;
    clients[id].name[0] = '\0';

    broadcast_header(clients, MSG_LEAVE, id, -1);
}

static void accept_new_client(int server_fd, client_t clients[MAX_PLAYERS]) {
    struct sockaddr_in client_address;
    socklen_t client_address_size = sizeof(client_address);

    int client_fd = accept(server_fd,
                           (struct sockaddr*)&client_address,
                           &client_address_size);

    if (client_fd < 0) {
        perror("accept");
        return;
    }

    int free_id = find_free_client_id(clients);

    if (free_id < 0) {
        fprintf(stderr, "Lobby is full\n");
        send_header(client_fd, MSG_DISCONNECT, TARGET_SERVER, TARGET_BROADCAST);
        close(client_fd);
        return;
    }

    msg_header_t header;

    if (recv_header(client_fd, &header) != 0 || header.msg_type != MSG_HELLO) {
        fprintf(stderr, "New client did not send HELLO\n");
        close(client_fd);
        return;
    }

    msg_hello_t hello;

    if (recv_hello_payload(client_fd, &hello) != 0) {
        fprintf(stderr, "Failed to read HELLO payload\n");
        close(client_fd);
        return;
    }

    protocol_player_info_t existing_players[MAX_PLAYERS];
    uint8_t existing_count = build_player_list(clients, existing_players);

    if (send_welcome(client_fd,
                     (uint8_t)free_id,
                     GAME_LOBBY,
                     existing_players,
                     existing_count) != 0) {
        fprintf(stderr, "Failed to send WELCOME\n");
        close(client_fd);
        return;
    }

    clients[free_id].fd = client_fd;
    clients[free_id].active = true;
    clients[free_id].ready = false;
    clients[free_id].id = (uint8_t)free_id;
    copy_protocol_string(clients[free_id].name,
                         sizeof(clients[free_id].name),
                         hello.player_name);

    printf("Player %d connected: %s\n", free_id, clients[free_id].name);
    broadcast_hello(clients, (uint8_t)free_id, &hello, client_fd);
}

static void handle_client_message(client_t clients[MAX_PLAYERS],
                                  uint8_t id,
                                  game_status_t* game_status,
                                  const game_map_t* map)  {
    msg_header_t header;

    if (recv_header(clients[id].fd, &header) != 0) {
        disconnect_client(clients, id);
        return;
    }

    switch (header.msg_type) {
        case MSG_PING:
            printf("PING from player %u\n", id);
            send_header(clients[id].fd, MSG_PONG, TARGET_SERVER, id);
            break;

        case MSG_PONG:
            printf("PONG from player %u\n", id);
            break;

        case MSG_LEAVE:
            disconnect_client(clients, id);
            break;

        case MSG_SET_READY:
            if (*game_status != GAME_LOBBY) {
                send_header(clients[id].fd, MSG_ERROR, TARGET_SERVER, id);
                break;
            }

            clients[id].ready = true;
            printf("Player %u is ready\n", id);
            broadcast_header(clients, MSG_SET_READY, id, -1);

            if (all_ready(clients)) {
                *game_status = GAME_RUNNING;

                printf("All players are ready. Starting game.\n");

                broadcast_status(clients, GAME_RUNNING);
                broadcast_map(clients, map);
            }
            break;

        default:
            fprintf(stderr,
                    "Unknown message type %u from player %u\n",
                    header.msg_type,
                    id);
            send_header(clients[id].fd, MSG_ERROR, TARGET_SERVER, id);
            break;
    }
}



int main(int argc, char* argv[]) {
   if (argc != 3) {
    fprintf(stderr, "Usage: %s <port> <map_file>\n", argv[0]);
    return 1;
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    game_map_t map;

if (map_load_from_file(argv[2], &map) != 0) {
    return 1;
    }

    printf("Loaded map from %s\n", argv[2]);
    map_print(&map);

    game_status_t game_status = GAME_LOBBY;


    int server_fd = create_server_socket(port);

    if (server_fd < 0) {
        return 1;
    }

    client_t clients[MAX_PLAYERS];
    init_clients(clients);

    printf("Server listening on port %u\n", port);

    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        int max_fd = server_fd;

        for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
            if (clients[i].active) {
                FD_SET(clients[i].fd, &read_fds);

                if (clients[i].fd > max_fd) {
                    max_fd = clients[i].fd;
                }
            }
        }

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            accept_new_client(server_fd, clients);
        }

        for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
            if (clients[i].active && FD_ISSET(clients[i].fd, &read_fds)) {
                handle_client_message(clients, i, &game_status, &map);
            }
        }
    }

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            close(clients[i].fd);
        }
    }

    close(server_fd);

    return 0;
}