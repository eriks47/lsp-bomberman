#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "map.h"
#include "protocol.h"

#define MAX_BOMBS 128
#define MAX_EXPLOSIONS 128
#define ROUND_TIME_LIMIT_TICKS (3 * 60 * TICKS_PER_SECOND)

typedef struct {
    int fd;
    bool active;
    bool ready;
    uint8_t id;
    char name[PROTOCOL_PLAYER_NAME_LEN];
} client_t;

typedef struct {
    bool active;
    uint8_t owner_id;
    uint16_t row;
    uint16_t col;
    uint8_t radius;
    uint16_t timer_ticks;
} active_bomb_t;

typedef struct {
    bool active;
    uint16_t row;
    uint16_t col;
    uint8_t radius;
    uint16_t timer_ticks;
} active_explosion_t;

typedef struct {
    uint8_t id;
    bool active;
    bool alive;
    uint16_t row;
    uint16_t col;
    uint8_t bomb_count;
    uint8_t bomb_radius;
    uint16_t bomb_timer_ticks;
    uint16_t speed;
    uint16_t kills;
    uint16_t destroyed_blocks;
    uint16_t collected_bonuses;
} server_player_t;

typedef struct {
    game_status_t status;
    game_map_t map;
    uint32_t round_ticks_left;
    server_player_t players[MAX_PLAYERS];
    active_bomb_t bombs[MAX_BOMBS];
    active_explosion_t explosions[MAX_EXPLOSIONS];
} game_state_t;

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

static void init_game_state(game_state_t* game, const game_map_t* loaded_map) {
    memset(game, 0, sizeof(*game));

    game->status = GAME_LOBBY;
    game->map = *loaded_map;
    game->round_ticks_left = 0;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        game->players[i].id = i;
        game->players[i].active = false;
        game->players[i].alive = false;
        game->players[i].bomb_count = 1;
        game->players[i].bomb_radius = loaded_map->bomb_radius;
        game->players[i].bomb_timer_ticks = loaded_map->bomb_timer_ticks;
        game->players[i].speed = loaded_map->player_speed;
        game->players[i].kills = 0;
        game->players[i].destroyed_blocks = 0;
        game->players[i].collected_bonuses = 0;
    }
}

static uint16_t cell_index(const game_map_t* map, uint16_t row, uint16_t col) {
    return make_cell_index(row, col, map->cols);
}

static char map_get_cell(const game_map_t* map, uint16_t row, uint16_t col) {
    return (char)map->cells[cell_index(map, row, col)];
}

static void map_set_cell(game_map_t* map, uint16_t row, uint16_t col, char value) {
    map->cells[cell_index(map, row, col)] = (uint8_t)value;
}

static bool in_bounds(const game_map_t* map, int row, int col) {
    return row >= 0 && col >= 0 && row < map->rows && col < map->cols;
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

static void send_header_to_all(const client_t clients[MAX_PLAYERS],
                               uint8_t msg_type,
                               uint8_t sender_id) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
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

static void broadcast_status(const client_t clients[MAX_PLAYERS], uint8_t game_status) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_status(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, game_status);
        }
    }
}

static void broadcast_map(const client_t clients[MAX_PLAYERS], const game_map_t* map) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_map(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, map);
        }
    }
}

static void broadcast_moved(const client_t clients[MAX_PLAYERS],
                            uint8_t player_id,
                            uint16_t cell) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_moved(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, player_id, cell);
        }
    }
}

static void broadcast_bomb(const client_t clients[MAX_PLAYERS],
                           uint8_t owner_id,
                           uint16_t cell) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_bomb(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, owner_id, cell);
        }
    }
}

static void broadcast_explosion(const client_t clients[MAX_PLAYERS],
                                uint8_t msg_type,
                                uint8_t radius,
                                uint16_t cell) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_explosion(clients[i].fd,
                           msg_type,
                           TARGET_SERVER,
                           TARGET_BROADCAST,
                           radius,
                           cell);
        }
    }
}

static void broadcast_death(const client_t clients[MAX_PLAYERS], uint8_t player_id) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_death(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, player_id);
        }
    }
}

static void broadcast_winner(const client_t clients[MAX_PLAYERS], uint8_t winner_id) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_winner(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, winner_id);
        }
    }
}

static void broadcast_round_stats(const client_t clients[MAX_PLAYERS],
                                  const game_state_t* game) {
    msg_round_stat_t stats[MAX_PLAYERS];
    uint8_t count = 0;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active || game->players[i].active) {
            stats[count].player_id = i;
            stats[count].kills = game->players[i].kills;
            stats[count].destroyed_blocks = game->players[i].destroyed_blocks;
            stats[count].collected_bonuses = game->players[i].collected_bonuses;
            ++count;
        }
    }

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_round_stats(clients[i].fd,
                             TARGET_SERVER,
                             TARGET_BROADCAST,
                             stats,
                             count);
        }
    }
}

static void broadcast_bonus_retrieved(const client_t clients[MAX_PLAYERS],
                                      uint8_t player_id,
                                      uint16_t cell) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_bonus_retrieved(clients[i].fd,
                                 TARGET_SERVER,
                                 TARGET_BROADCAST,
                                 player_id,
                                 cell);
        }
    }
}

static void broadcast_block_destroyed(const client_t clients[MAX_PLAYERS], uint16_t cell) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_block_destroyed(clients[i].fd, TARGET_SERVER, TARGET_BROADCAST, cell);
        }
    }
}

static uint8_t bonus_type_from_cell(char cell) {
    if (cell == 'A') {
        return BONUS_SPEED;
    }

    if (cell == 'R') {
        return BONUS_RADIUS;
    }

    if (cell == 'T') {
        return BONUS_TIMER;
    }

    if (cell == 'N') {
        return BONUS_BOMB_COUNT;
    }

    return BONUS_NONE;
}

static void send_available_bonuses_to_client(int fd, const game_map_t* map) {
    size_t cells = map_cell_count(map);

    for (size_t i = 0; i < cells; ++i) {
        uint8_t bonus_type = bonus_type_from_cell((char)map->cells[i]);

        if (bonus_type != BONUS_NONE) {
            send_bonus_available(fd,
                                 TARGET_SERVER,
                                 TARGET_BROADCAST,
                                 bonus_type,
                                 (uint16_t)i);
        }
    }
}

static void broadcast_available_bonuses(const client_t clients[MAX_PLAYERS],
                                        const game_map_t* map) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            send_available_bonuses_to_client(clients[i].fd, map);
        }
    }
}

static void send_sync_to_client(int fd,
                                const client_t clients[MAX_PLAYERS],
                                const game_state_t* game) {
    send_header(fd, MSG_SYNC_BOARD, TARGET_SERVER, TARGET_BROADCAST);
    send_status(fd, TARGET_SERVER, TARGET_BROADCAST, game->status);
    send_map(fd, TARGET_SERVER, TARGET_BROADCAST, &game->map);

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (!clients[i].active) {
            continue;
        }

        if (game->players[i].active && game->players[i].alive) {
            uint16_t cell = cell_index(&game->map,
                                       game->players[i].row,
                                       game->players[i].col);

            send_moved(fd, TARGET_SERVER, TARGET_BROADCAST, i, cell);
        } else {
            send_death(fd, TARGET_SERVER, TARGET_BROADCAST, i);
        }
    }

    for (size_t i = 0; i < MAX_BOMBS; ++i) {
        if (game->bombs[i].active) {
            uint16_t cell = cell_index(&game->map,
                                       game->bombs[i].row,
                                       game->bombs[i].col);

            send_bomb(fd, TARGET_SERVER, TARGET_BROADCAST, game->bombs[i].owner_id, cell);
        }
    }

    send_available_bonuses_to_client(fd, &game->map);
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

    send_header_to_all(clients, MSG_LEAVE, id);
}

static void accept_new_client(int server_fd,
                              client_t clients[MAX_PLAYERS],
                              const game_state_t* game) {
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
                     game->status,
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

    if (game->status != GAME_LOBBY) {
        send_sync_to_client(client_fd, clients, game);
    }
}

static bool bomb_exists_at(const game_state_t* game, uint16_t row, uint16_t col) {
    for (size_t i = 0; i < MAX_BOMBS; ++i) {
        if (game->bombs[i].active &&
            game->bombs[i].row == row &&
            game->bombs[i].col == col) {
            return true;
        }
    }

    return false;
}

static bool player_exists_at(const game_state_t* game,
                             uint16_t row,
                             uint16_t col,
                             uint8_t except_id) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (i == except_id) {
            continue;
        }

        if (game->players[i].active &&
            game->players[i].alive &&
            game->players[i].row == row &&
            game->players[i].col == col) {
            return true;
        }
    }

    return false;
}

static bool is_bonus_cell(char cell) {
    return cell == 'A' || cell == 'R' || cell == 'T' || cell == 'N';
}

static bool is_walkable_cell(char cell) {
    return cell == '.' ||
           cell == '1' ||
           cell == '2' ||
           cell == '3' ||
           cell == '4' ||
           cell == '5' ||
           cell == '6' ||
           cell == '7' ||
           cell == '8' ||
           is_bonus_cell(cell);
}

static void apply_bonus(game_state_t* game,
                        const client_t clients[MAX_PLAYERS],
                        uint8_t player_id,
                        uint16_t row,
                        uint16_t col) {
    char cell = map_get_cell(&game->map, row, col);

    if (!is_bonus_cell(cell)) {
        return;
    }

    server_player_t* player = &game->players[player_id];
    player->collected_bonuses += 1;

    if (cell == 'A') {
        player->speed += 1;
    } else if (cell == 'R') {
        player->bomb_radius += 1;
    } else if (cell == 'T') {
        player->bomb_timer_ticks += TICKS_PER_SECOND;
    } else if (cell == 'N') {
        player->bomb_count += 1;
    }

    map_set_cell(&game->map, row, col, '.');

    uint16_t cell_id = cell_index(&game->map, row, col);
    printf("Player %u picked bonus %c at cell %u\n", player_id, cell, cell_id);

    broadcast_bonus_retrieved(clients, player_id, cell_id);
}

static void find_or_create_start_position(game_state_t* game,
                                          uint8_t player_id,
                                          uint16_t* out_row,
                                          uint16_t* out_col) {
    char wanted = (char)('1' + player_id);

    for (uint16_t row = 0; row < game->map.rows; ++row) {
        for (uint16_t col = 0; col < game->map.cols; ++col) {
            if (map_get_cell(&game->map, row, col) == wanted) {
                *out_row = row;
                *out_col = col;
                return;
            }
        }
    }

    for (uint16_t row = 0; row < game->map.rows; ++row) {
        for (uint16_t col = 0; col < game->map.cols; ++col) {
            if (map_get_cell(&game->map, row, col) == '.' &&
                !player_exists_at(game, row, col, player_id)) {
                *out_row = row;
                *out_col = col;
                return;
            }
        }
    }

    *out_row = 0;
    *out_col = 0;
}

static void start_game(game_state_t* game, client_t clients[MAX_PLAYERS]) {
    game->status = GAME_RUNNING;
    game->round_ticks_left = ROUND_TIME_LIMIT_TICKS;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        game->players[i].active = clients[i].active;
        game->players[i].alive = clients[i].active;
        game->players[i].bomb_count = 1;
        game->players[i].bomb_radius = game->map.bomb_radius;
        game->players[i].bomb_timer_ticks = game->map.bomb_timer_ticks;
        game->players[i].speed = game->map.player_speed;
        game->players[i].kills = 0;
        game->players[i].destroyed_blocks = 0;
        game->players[i].collected_bonuses = 0;

        if (clients[i].active) {
            find_or_create_start_position(game,
                                          i,
                                          &game->players[i].row,
                                          &game->players[i].col);

            printf("Player %u starts at row=%u col=%u\n",
                   i,
                   game->players[i].row,
                   game->players[i].col);
        }
    }

    printf("All players are ready. Starting game.\n");

    broadcast_status(clients, GAME_RUNNING);
    broadcast_map(clients, &game->map);
    broadcast_available_bonuses(clients, &game->map);
}

static size_t active_bombs_by_owner(const game_state_t* game, uint8_t owner_id) {
    size_t count = 0;

    for (size_t i = 0; i < MAX_BOMBS; ++i) {
        if (game->bombs[i].active && game->bombs[i].owner_id == owner_id) {
            ++count;
        }
    }

    return count;
}

static int find_free_bomb_slot(const game_state_t* game) {
    for (size_t i = 0; i < MAX_BOMBS; ++i) {
        if (!game->bombs[i].active) {
            return (int)i;
        }
    }

    return -1;
}

static int find_free_explosion_slot(const game_state_t* game) {
    for (size_t i = 0; i < MAX_EXPLOSIONS; ++i) {
        if (!game->explosions[i].active) {
            return (int)i;
        }
    }

    return -1;
}

static void place_bomb(game_state_t* game,
                       const client_t clients[MAX_PLAYERS],
                       uint8_t player_id,
                       uint16_t requested_cell) {
    if (game->status != GAME_RUNNING) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    server_player_t* player = &game->players[player_id];

    if (!player->active || !player->alive) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    uint16_t actual_cell = cell_index(&game->map, player->row, player->col);

    if (requested_cell != actual_cell ||
        bomb_exists_at(game, player->row, player->col) ||
        active_bombs_by_owner(game, player_id) >= player->bomb_count) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    int slot = find_free_bomb_slot(game);

    if (slot < 0) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    game->bombs[slot].active = true;
    game->bombs[slot].owner_id = player_id;
    game->bombs[slot].row = player->row;
    game->bombs[slot].col = player->col;
    game->bombs[slot].radius = player->bomb_radius;
    game->bombs[slot].timer_ticks = player->bomb_timer_ticks;

    printf("Player %u placed bomb at cell %u\n", player_id, actual_cell);
    broadcast_bomb(clients, player_id, actual_cell);
}

static void move_player(game_state_t* game,
                        const client_t clients[MAX_PLAYERS],
                        uint8_t player_id,
                        uint8_t direction_ascii) {
    if (game->status != GAME_RUNNING) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    server_player_t* player = &game->players[player_id];

    if (!player->active || !player->alive) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    int new_row = (int)player->row;
    int new_col = (int)player->col;

    if (direction_ascii == 'U') {
        new_row -= 1;
    } else if (direction_ascii == 'D') {
        new_row += 1;
    } else if (direction_ascii == 'L') {
        new_col -= 1;
    } else if (direction_ascii == 'R') {
        new_col += 1;
    } else {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    if (!in_bounds(&game->map, new_row, new_col)) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    char target_cell = map_get_cell(&game->map, (uint16_t)new_row, (uint16_t)new_col);

    if (!is_walkable_cell(target_cell) ||
        bomb_exists_at(game, (uint16_t)new_row, (uint16_t)new_col) ||
        player_exists_at(game, (uint16_t)new_row, (uint16_t)new_col, player_id)) {
        send_header(clients[player_id].fd, MSG_ERROR, TARGET_SERVER, player_id);
        return;
    }

    player->row = (uint16_t)new_row;
    player->col = (uint16_t)new_col;

    uint16_t new_cell = cell_index(&game->map, player->row, player->col);

    apply_bonus(game, clients, player_id, player->row, player->col);

    printf("Player %u moved to cell %u\n", player_id, new_cell);
    broadcast_moved(clients, player_id, new_cell);
}

static void kill_players_at_cell(game_state_t* game,
                                 const client_t clients[MAX_PLAYERS],
                                 uint16_t row,
                                 uint16_t col,
                                 uint8_t killer_id) {
    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        server_player_t* player = &game->players[i];

        if (player->active &&
            player->alive &&
            player->row == row &&
            player->col == col) {
            player->alive = false;

            if (killer_id < MAX_PLAYERS &&
                killer_id != i &&
                game->players[killer_id].active) {
                game->players[killer_id].kills += 1;
            }

            printf("Player %u died\n", i);
            broadcast_death(clients, i);
        }
    }
}

static void trigger_bomb_at(game_state_t* game, uint16_t row, uint16_t col) {
    for (size_t i = 0; i < MAX_BOMBS; ++i) {
        if (game->bombs[i].active &&
            game->bombs[i].row == row &&
            game->bombs[i].col == col) {
            game->bombs[i].timer_ticks = 0;
        }
    }
}

static void damage_cell(game_state_t* game,
                        const client_t clients[MAX_PLAYERS],
                        uint16_t row,
                        uint16_t col,
                        uint8_t bomb_owner_id,
                        bool* stop_ray) {
    char cell = map_get_cell(&game->map, row, col);

    if (cell == 'H') {
        *stop_ray = true;
        return;
    }

    kill_players_at_cell(game, clients, row, col, bomb_owner_id);

    if (bomb_exists_at(game, row, col)) {
        trigger_bomb_at(game, row, col);
    }

    if (cell == 'S') {
        map_set_cell(&game->map, row, col, '.');

        if (bomb_owner_id < MAX_PLAYERS && game->players[bomb_owner_id].active) {
            game->players[bomb_owner_id].destroyed_blocks += 1;
        }

        broadcast_block_destroyed(clients, cell_index(&game->map, row, col));
        *stop_ray = true;
        return;
    }

    *stop_ray = false;
}

static void create_explosion(game_state_t* game,
                             const client_t clients[MAX_PLAYERS],
                             const active_bomb_t* bomb) {
    uint16_t center_cell = cell_index(&game->map, bomb->row, bomb->col);

    broadcast_explosion(clients, MSG_EXPLOSION_START, bomb->radius, center_cell);

    int explosion_slot = find_free_explosion_slot(game);

    if (explosion_slot >= 0) {
        game->explosions[explosion_slot].active = true;
        game->explosions[explosion_slot].row = bomb->row;
        game->explosions[explosion_slot].col = bomb->col;
        game->explosions[explosion_slot].radius = bomb->radius;
        game->explosions[explosion_slot].timer_ticks = game->map.explosion_danger_ticks;
    }

    kill_players_at_cell(game, clients, bomb->row, bomb->col, bomb->owner_id);

    const int directions[4][2] = {
        {-1, 0},
        {1, 0},
        {0, -1},
        {0, 1}
    };

    for (size_t direction = 0; direction < 4; ++direction) {
        for (uint8_t distance = 1; distance <= bomb->radius; ++distance) {
            int row = (int)bomb->row + directions[direction][0] * distance;
            int col = (int)bomb->col + directions[direction][1] * distance;

            if (!in_bounds(&game->map, row, col)) {
                break;
            }

            bool stop_ray = false;

            damage_cell(game,
                        clients,
                        (uint16_t)row,
                        (uint16_t)col,
                        bomb->owner_id,
                        &stop_ray);

            if (stop_ray) {
                break;
            }
        }
    }
}

static void finish_explosion(game_state_t* game,
                             const client_t clients[MAX_PLAYERS],
                             active_explosion_t* explosion) {
    uint16_t center_cell = cell_index(&game->map, explosion->row, explosion->col);

    broadcast_explosion(clients, MSG_EXPLOSION_END, explosion->radius, center_cell);

    explosion->active = false;
}

static void finish_game(game_state_t* game,
                        const client_t clients[MAX_PLAYERS],
                        uint8_t winner_id,
                        const char* reason) {
    if (game->status != GAME_RUNNING) {
        return;
    }

    game->status = GAME_END;

    printf("Game ended: %s\n", reason);

    if (winner_id == 255) {
        printf("Result: draw\n");
    } else {
        printf("Winner is player %u\n", winner_id);
    }

    broadcast_status(clients, GAME_END);
    broadcast_winner(clients, winner_id);
    broadcast_round_stats(clients, game);
}

static void check_winner(game_state_t* game, const client_t clients[MAX_PLAYERS]) {
    if (game->status != GAME_RUNNING) {
        return;
    }

    uint8_t alive_count = 0;
    uint8_t winner_id = 255;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (game->players[i].active && game->players[i].alive) {
            ++alive_count;
            winner_id = i;
        }
    }

    if (alive_count == 1) {
        finish_game(game, clients, winner_id, "one player left alive");
    } else if (alive_count == 0) {
        finish_game(game, clients, 255, "all players died");
    }
}

static void update_game(game_state_t* game, const client_t clients[MAX_PLAYERS]) {
    if (game->status != GAME_RUNNING) {
        return;
    }

    if (game->round_ticks_left > 0) {
        game->round_ticks_left -= 1;

        if (game->round_ticks_left == 0) {
            finish_game(game, clients, 255, "round time limit reached");
            return;
        }
    }

    for (size_t i = 0; i < MAX_BOMBS; ++i) {
        if (!game->bombs[i].active) {
            continue;
        }

        if (game->bombs[i].timer_ticks > 0) {
            game->bombs[i].timer_ticks -= 1;
        }

        if (game->bombs[i].timer_ticks == 0) {
            active_bomb_t exploded_bomb = game->bombs[i];
            game->bombs[i].active = false;

            printf("Bomb exploded at row=%u col=%u\n", exploded_bomb.row, exploded_bomb.col);
            create_explosion(game, clients, &exploded_bomb);
        }
    }

    for (size_t i = 0; i < MAX_EXPLOSIONS; ++i) {
        if (!game->explosions[i].active) {
            continue;
        }

        if (game->explosions[i].timer_ticks > 0) {
            game->explosions[i].timer_ticks -= 1;
        }

        if (game->explosions[i].timer_ticks == 0) {
            finish_explosion(game, clients, &game->explosions[i]);
        }
    }

    check_winner(game, clients);
}

static void reset_to_lobby(game_state_t* game,
                           client_t clients[MAX_PLAYERS],
                           const game_map_t* initial_map) {
    init_game_state(game, initial_map);
    game->status = GAME_LOBBY;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            clients[i].ready = false;
        }
    }

    printf("Game returned to lobby\n");
    broadcast_status(clients, GAME_LOBBY);
}

static void handle_client_message(client_t clients[MAX_PLAYERS],
                                  game_state_t* game,
                                  const game_map_t* initial_map,
                                  uint8_t id) {
    msg_header_t header;

    if (recv_header(clients[id].fd, &header) != 0) {
        disconnect_client(clients, id);
        game->players[id].active = false;
        game->players[id].alive = false;
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
            game->players[id].active = false;
            game->players[id].alive = false;
            check_winner(game, clients);
            break;

        case MSG_SET_READY:
            if (game->status != GAME_LOBBY) {
                send_header(clients[id].fd, MSG_ERROR, TARGET_SERVER, id);
                break;
            }

            clients[id].ready = true;
            printf("Player %u is ready\n", id);
            send_header_to_all(clients, MSG_SET_READY, id);

            if (all_ready(clients)) {
                start_game(game, clients);
            }

            break;

        case MSG_MOVE_ATTEMPT: {
            uint8_t direction_ascii;

            if (recv_move_attempt_payload(clients[id].fd, &direction_ascii) != 0) {
                disconnect_client(clients, id);
                break;
            }

            move_player(game, clients, id, direction_ascii);
            break;
        }

        case MSG_BOMB_ATTEMPT: {
            uint16_t requested_cell;

            if (recv_bomb_attempt_payload(clients[id].fd, &requested_cell) != 0) {
                disconnect_client(clients, id);
                break;
            }

            place_bomb(game, clients, id, requested_cell);
            break;
        }

        case MSG_SET_STATUS: {
            uint8_t requested_status;

            if (recv_status_payload(clients[id].fd, &requested_status) != 0) {
                disconnect_client(clients, id);
                game->players[id].active = false;
                game->players[id].alive = false;
                break;
            }

            if (game->status == GAME_END && requested_status == GAME_LOBBY) {
                reset_to_lobby(game, clients, initial_map);
            } else {
                send_header(clients[id].fd, MSG_ERROR, TARGET_SERVER, id);
            }

            break;
        }

        case MSG_SYNC_REQUEST:
            printf("SYNC_REQUEST from player %u\n", id);
            send_sync_to_client(clients[id].fd, clients, game);
            break;

        default:
            fprintf(stderr, "Unknown message type %u from player %u\n", header.msg_type, id);
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

    game_map_t loaded_map;

    if (map_load_from_file(argv[2], &loaded_map) != 0) {
        return 1;
    }

    printf("Loaded map from %s\n", argv[2]);
    map_print(&loaded_map);

    int server_fd = create_server_socket(port);

    if (server_fd < 0) {
        return 1;
    }

    client_t clients[MAX_PLAYERS];
    init_clients(clients);

    game_state_t game;
    init_game_state(&game, &loaded_map);

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

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000000 / TICKS_PER_SECOND;

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (ready < 0) {
            perror("select");
            break;
        }

        if (ready > 0 && FD_ISSET(server_fd, &read_fds)) {
            accept_new_client(server_fd, clients, &game);
        }

        for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
            if (clients[i].active && FD_ISSET(clients[i].fd, &read_fds)) {
                handle_client_message(clients, &game, &loaded_map, i);
            }
        }

        update_game(&game, clients);
    }

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        if (clients[i].active) {
            close(clients[i].fd);
        }
    }

    close(server_fd);

    return 0;
}