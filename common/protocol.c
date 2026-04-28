#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void copy_protocol_string(char* dst, size_t dst_size, const char* src) {
    memset(dst, 0, dst_size);

    if (src == NULL) {
        return;
    }

    strncpy(dst, src, dst_size - 1);
}

int send_all(int fd, const void* data, size_t size) {
    const uint8_t* ptr = data;
    size_t sent = 0;

    while (sent < size) {
        ssize_t result = send(fd, ptr + sent, size - sent, 0);

        if (result <= 0) {
            return -1;
        }

        sent += (size_t)result;
    }

    return 0;
}

int recv_all(int fd, void* data, size_t size) {
    uint8_t* ptr = data;
    size_t received = 0;

    while (received < size) {
        ssize_t result = recv(fd, ptr + received, size - received, 0);

        if (result <= 0) {
            return -1;
        }

        received += (size_t)result;
    }

    return 0;
}

int send_header(int fd, uint8_t msg_type, uint8_t sender_id, uint8_t target_id) {
    msg_header_t header;

    header.msg_type = msg_type;
    header.sender_id = sender_id;
    header.target_id = target_id;

    return send_all(fd, &header, sizeof(header));
}

int recv_header(int fd, msg_header_t* header) {
    return recv_all(fd, header, sizeof(*header));
}

int send_hello(int fd, const char* client_id, const char* player_name) {
    msg_hello_t hello;

    copy_protocol_string(hello.client_id, sizeof(hello.client_id), client_id);
    copy_protocol_string(hello.player_name, sizeof(hello.player_name), player_name);

    if (send_header(fd, MSG_HELLO, TARGET_SERVER, TARGET_SERVER) != 0) {
        return -1;
    }

    return send_all(fd, &hello, sizeof(hello));
}

int recv_hello_payload(int fd, msg_hello_t* hello) {
    return recv_all(fd, hello, sizeof(*hello));
}

int send_welcome(int fd,
                 uint8_t player_id,
                 uint8_t game_status,
                 const protocol_player_info_t* players,
                 uint8_t player_count) {
    msg_welcome_t welcome;

    copy_protocol_string(welcome.server_id, sizeof(welcome.server_id), "lsp-server-0.1");
    welcome.game_status = game_status;
    welcome.other_players_count = player_count;

    if (send_header(fd, MSG_WELCOME, player_id, player_id) != 0) {
        return -1;
    }

    if (send_all(fd, &welcome, sizeof(welcome)) != 0) {
        return -1;
    }

    if (player_count == 0) {
        return 0;
    }

    return send_all(fd, players, sizeof(players[0]) * player_count);
}

int recv_welcome_payload(int fd,
                         msg_welcome_t* welcome,
                         protocol_player_info_t* players,
                         size_t max_players) {
    if (recv_all(fd, welcome, sizeof(*welcome)) != 0) {
        return -1;
    }

    if (welcome->other_players_count > max_players) {
        return -1;
    }

    if (welcome->other_players_count == 0) {
        return 0;
    }

    return recv_all(fd, players, sizeof(players[0]) * welcome->other_players_count);
}

int send_status(int fd, uint8_t sender_id, uint8_t target_id, uint8_t game_status) {
    if (send_header(fd, MSG_SET_STATUS, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, &game_status, sizeof(game_status));
}

int recv_status_payload(int fd, uint8_t* game_status) {
    return recv_all(fd, game_status, sizeof(*game_status));
}

int send_map(int fd, uint8_t sender_id, uint8_t target_id, const game_map_t* map) {
    uint8_t size_payload[2];

    size_payload[0] = map->rows;
    size_payload[1] = map->cols;

    if (send_header(fd, MSG_MAP, sender_id, target_id) != 0) {
        return -1;
    }

    if (send_all(fd, size_payload, sizeof(size_payload)) != 0) {
        return -1;
    }

    return send_all(fd, map->cells, map_cell_count(map));
}

int recv_map_payload(int fd, game_map_t* map) {
    uint8_t size_payload[2];

    memset(map, 0, sizeof(*map));

    if (recv_all(fd, size_payload, sizeof(size_payload)) != 0) {
        return -1;
    }

    map->rows = size_payload[0];
    map->cols = size_payload[1];

    if (map->rows == 0 || map->cols == 0) {
        return -1;
    }

    return recv_all(fd, map->cells, map_cell_count(map));
}

int send_move_attempt(int fd, uint8_t sender_id, uint8_t direction_ascii) {
    if (send_header(fd, MSG_MOVE_ATTEMPT, sender_id, TARGET_SERVER) != 0) {
        return -1;
    }

    return send_all(fd, &direction_ascii, sizeof(direction_ascii));
}

int recv_move_attempt_payload(int fd, uint8_t* direction_ascii) {
    return recv_all(fd, direction_ascii, sizeof(*direction_ascii));
}

int send_moved(int fd,
               uint8_t sender_id,
               uint8_t target_id,
               uint8_t player_id,
               uint16_t cell) {
    uint8_t payload[3];
    uint16_t net_cell = htons(cell);

    payload[0] = player_id;
    memcpy(payload + 1, &net_cell, sizeof(net_cell));

    if (send_header(fd, MSG_MOVED, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, payload, sizeof(payload));
}

int recv_moved_payload(int fd, msg_moved_t* moved) {
    uint8_t payload[3];
    uint16_t net_cell;

    if (recv_all(fd, payload, sizeof(payload)) != 0) {
        return -1;
    }

    moved->player_id = payload[0];
    memcpy(&net_cell, payload + 1, sizeof(net_cell));
    moved->cell = ntohs(net_cell);

    return 0;
}

int send_bomb_attempt(int fd, uint8_t sender_id, uint16_t cell) {
    uint16_t net_cell = htons(cell);

    if (send_header(fd, MSG_BOMB_ATTEMPT, sender_id, TARGET_SERVER) != 0) {
        return -1;
    }

    return send_all(fd, &net_cell, sizeof(net_cell));
}

int recv_bomb_attempt_payload(int fd, uint16_t* cell) {
    uint16_t net_cell;

    if (recv_all(fd, &net_cell, sizeof(net_cell)) != 0) {
        return -1;
    }

    *cell = ntohs(net_cell);
    return 0;
}

int send_bomb(int fd,
              uint8_t sender_id,
              uint8_t target_id,
              uint8_t owner_id,
              uint16_t cell) {
    uint8_t payload[3];
    uint16_t net_cell = htons(cell);

    payload[0] = owner_id;
    memcpy(payload + 1, &net_cell, sizeof(net_cell));

    if (send_header(fd, MSG_BOMB, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, payload, sizeof(payload));
}

int recv_bomb_payload(int fd, msg_bomb_t* bomb) {
    uint8_t payload[3];
    uint16_t net_cell;

    if (recv_all(fd, payload, sizeof(payload)) != 0) {
        return -1;
    }

    bomb->owner_id = payload[0];
    memcpy(&net_cell, payload + 1, sizeof(net_cell));
    bomb->cell = ntohs(net_cell);

    return 0;
}

int send_explosion(int fd,
                   uint8_t msg_type,
                   uint8_t sender_id,
                   uint8_t target_id,
                   uint8_t radius,
                   uint16_t cell) {
    uint8_t payload[3];
    uint16_t net_cell = htons(cell);

    payload[0] = radius;
    memcpy(payload + 1, &net_cell, sizeof(net_cell));

    if (send_header(fd, msg_type, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, payload, sizeof(payload));
}

int recv_explosion_payload(int fd, msg_explosion_t* explosion) {
    uint8_t payload[3];
    uint16_t net_cell;

    if (recv_all(fd, payload, sizeof(payload)) != 0) {
        return -1;
    }

    explosion->radius = payload[0];
    memcpy(&net_cell, payload + 1, sizeof(net_cell));
    explosion->cell = ntohs(net_cell);

    return 0;
}

int send_death(int fd, uint8_t sender_id, uint8_t target_id, uint8_t player_id) {
    if (send_header(fd, MSG_DEATH, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, &player_id, sizeof(player_id));
}

int recv_death_payload(int fd, uint8_t* player_id) {
    return recv_all(fd, player_id, sizeof(*player_id));
}

int send_winner(int fd, uint8_t sender_id, uint8_t target_id, uint8_t winner_id) {
    if (send_header(fd, MSG_WINNER, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, &winner_id, sizeof(winner_id));
}

int recv_winner_payload(int fd, uint8_t* winner_id) {
    return recv_all(fd, winner_id, sizeof(*winner_id));
}

int send_bonus_retrieved(int fd,
                         uint8_t sender_id,
                         uint8_t target_id,
                         uint8_t player_id,
                         uint16_t cell) {
    uint8_t payload[3];
    uint16_t net_cell = htons(cell);

    payload[0] = player_id;
    memcpy(payload + 1, &net_cell, sizeof(net_cell));

    if (send_header(fd, MSG_BONUS_RETRIEVED, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, payload, sizeof(payload));
}

int recv_bonus_retrieved_payload(int fd, uint8_t* player_id, uint16_t* cell) {
    uint8_t payload[3];
    uint16_t net_cell;

    if (recv_all(fd, payload, sizeof(payload)) != 0) {
        return -1;
    }

    *player_id = payload[0];
    memcpy(&net_cell, payload + 1, sizeof(net_cell));
    *cell = ntohs(net_cell);

    return 0;
}

int send_block_destroyed(int fd, uint8_t sender_id, uint8_t target_id, uint16_t cell) {
    uint16_t net_cell = htons(cell);

    if (send_header(fd, MSG_BLOCK_DESTROYED, sender_id, target_id) != 0) {
        return -1;
    }

    return send_all(fd, &net_cell, sizeof(net_cell));
}

int recv_block_destroyed_payload(int fd, uint16_t* cell) {
    uint16_t net_cell;

    if (recv_all(fd, &net_cell, sizeof(net_cell)) != 0) {
        return -1;
    }

    *cell = ntohs(net_cell);
    return 0;
}