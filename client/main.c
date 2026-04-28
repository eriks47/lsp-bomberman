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

typedef struct {
    bool active;
    bool alive;
    bool ready;
    uint16_t cell;
    char name[PROTOCOL_PLAYER_NAME_LEN];
} client_player_t;

typedef struct {
    uint8_t my_id;
    uint8_t game_status;
    bool has_map;
    game_map_t map;
    bool explosion_cells[MAX_MAP_CELLS];
    client_player_t players[MAX_PLAYERS];
} client_state_t;

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

static void init_state(client_state_t* state) {
    memset(state, 0, sizeof(*state));

    state->my_id = 255;
    state->game_status = GAME_LOBBY;
    state->has_map = false;

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        state->players[i].active = false;
        state->players[i].alive = false;
        state->players[i].ready = false;
        state->players[i].cell = 0;
        state->players[i].name[0] = '\0';
    }
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  /ready   mark yourself ready\n");
    printf("  /ping    send PING\n");
    printf("  /quit    leave the game\n");
    printf("  /sync    request full state sync\n");
    printf("  /lobby   return to lobby after game end\n");
    printf("  /bomb    place bomb\n");
    printf("  /players show players\n");
    printf("  /map     show map\n");
    printf("  w        move up\n");
    printf("  s        move down\n");
    printf("  a        move left\n");
    printf("  d        move right\n");
}

static bool is_player_start_cell(char cell) {
    return cell >= '1' && cell <= '8';
}

static bool is_bonus_cell(char cell) {
    return cell == 'A' || cell == 'R' || cell == 'T' || cell == 'N';
}

static bool is_base_map_cell(char cell) {
    return cell == '.' || cell == 'H' || cell == 'S' || cell == 'B' ||
           is_bonus_cell(cell) || is_player_start_cell(cell);
}

static uint16_t map_cell_at(const game_map_t* map, uint16_t row, uint16_t col) {
    return make_cell_index(row, col, map->cols);
}

static char bonus_cell_from_type(uint8_t bonus_type) {
    if (bonus_type == BONUS_SPEED) {
        return 'A';
    }

    if (bonus_type == BONUS_RADIUS) {
        return 'R';
    }

    if (bonus_type == BONUS_TIMER) {
        return 'T';
    }

    if (bonus_type == BONUS_BOMB_COUNT) {
        return 'N';
    }

    return '.';
}

static void clear_explosions(client_state_t* state) {
    memset(state->explosion_cells, 0, sizeof(state->explosion_cells));
}

static bool map_blocks_explosion(char cell) {
    return cell == 'H' || cell == 'S';
}

static void set_explosion_cells(client_state_t* state,
                                uint16_t center_cell,
                                uint8_t radius,
                                bool value) {
    if (!state->has_map) {
        return;
    }

    if (center_cell >= map_cell_count(&state->map)) {
        return;
    }

    uint16_t center_row;
    uint16_t center_col;

    split_cell_index(center_cell, state->map.cols, &center_row, &center_col);

    state->explosion_cells[center_cell] = value;

    const int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    for (size_t direction = 0; direction < 4; ++direction) {
        for (uint8_t distance = 1; distance <= radius; ++distance) {
            int row = (int)center_row + directions[direction][0] * distance;
            int col = (int)center_col + directions[direction][1] * distance;

            if (row < 0 || col < 0 || row >= state->map.rows ||
                col >= state->map.cols) {
                break;
            }

            uint16_t cell =
                map_cell_at(&state->map, (uint16_t)row, (uint16_t)col);

            state->explosion_cells[cell] = value;

            if (map_blocks_explosion((char)state->map.cells[cell])) {
                break;
            }
        }
    }
}

static void render_map(const client_state_t* state) {
    if (!state->has_map) {
        return;
    }

    printf("\n");

    for (uint8_t row = 0; row < state->map.rows; ++row) {
        for (uint8_t col = 0; col < state->map.cols; ++col) {
            uint16_t cell = map_cell_at(&state->map, row, col);
            char shown = (char)state->map.cells[cell];

            if (state->explosion_cells[cell]) {
                shown = '*';
            }

            for (uint8_t player_id = 0; player_id < MAX_PLAYERS; ++player_id) {
                const client_player_t* player = &state->players[player_id];

                if (player->active && player->alive && player->cell == cell) {
                    shown = (char)('1' + player_id);
                    break;
                }
            }

            printf("%c ", shown);
        }

        printf("\n");
    }

    printf("\n");
}

static void init_player_positions_from_map(client_state_t* state) {
    if (!state->has_map) {
        return;
    }

    size_t cell_count = map_cell_count(&state->map);

    for (size_t i = 0; i < cell_count; ++i) {
        char cell = (char)state->map.cells[i];

        if (is_player_start_cell(cell)) {
            uint8_t player_id = (uint8_t)(cell - '1');

            if (player_id < MAX_PLAYERS) {
                state->players[player_id].cell = (uint16_t)i;

                if (state->players[player_id].active) {
                    state->players[player_id].alive = true;
                }
            }
        }
    }
}

static void clear_start_markers_from_map(client_state_t* state) {
    if (!state->has_map) {
        return;
    }

    size_t cell_count = map_cell_count(&state->map);

    for (size_t i = 0; i < cell_count; ++i) {
        char cell = (char)state->map.cells[i];

        if (is_player_start_cell(cell)) {
            state->map.cells[i] = '.';
        }
    }
}

static uint16_t get_my_cell(const client_state_t* state) {
    if (state->my_id >= MAX_PLAYERS) {
        return 0;
    }

    return state->players[state->my_id].cell;
}

static void set_player_name(client_state_t* state,
                            uint8_t player_id,
                            const char* name) {
    if (player_id >= MAX_PLAYERS) {
        return;
    }

    copy_protocol_string(state->players[player_id].name,
                         sizeof(state->players[player_id].name), name);
}

static void print_players(const client_state_t* state) {
    printf("Players:\n");

    for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
        const client_player_t* player = &state->players[i];

        if (!player->active) {
            continue;
        }

        printf("  %u: %s ready=%u alive=%u cell=%u", i, player->name,
               player->ready ? 1 : 0, player->alive ? 1 : 0, player->cell);

        if (i == state->my_id) {
            printf(" <- you");
        }

        printf("\n");
    }
}

static void handle_map_received(client_state_t* state, const game_map_t* map) {
    state->map = *map;
    state->has_map = true;
    clear_explosions(state);

    init_player_positions_from_map(state);
    clear_start_markers_from_map(state);

    printf("MAP received\n");
    render_map(state);
}

static void remove_bomb_at_cell(client_state_t* state, uint16_t cell) {
    if (!state->has_map) {
        return;
    }

    if (cell >= map_cell_count(&state->map)) {
        return;
    }

    if ((char)state->map.cells[cell] == 'B') {
        state->map.cells[cell] = '.';
    }
}

static void mark_bomb_at_cell(client_state_t* state, uint16_t cell) {
    if (!state->has_map) {
        return;
    }

    if (cell >= map_cell_count(&state->map)) {
        return;
    }

    if (is_base_map_cell((char)state->map.cells[cell])) {
        state->map.cells[cell] = 'B';
    }
}

static bool handle_server_message(int fd, client_state_t* state) {
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

            if (header.sender_id < MAX_PLAYERS) {
                state->players[header.sender_id].active = true;
                state->players[header.sender_id].alive =
                    state->game_status == GAME_RUNNING;
                set_player_name(state, header.sender_id, hello.player_name);
            }

            printf("Player %u joined: %s\n", header.sender_id,
                   hello.player_name);
            print_players(state);
            break;
        }

        case MSG_LEAVE:
            if (header.sender_id < MAX_PLAYERS) {
                state->players[header.sender_id].active = false;
                state->players[header.sender_id].alive = false;
            }

            printf("Player %u left\n", header.sender_id);
            print_players(state);
            render_map(state);
            break;

        case MSG_SET_READY:
            if (header.sender_id < MAX_PLAYERS) {
                state->players[header.sender_id].ready = true;
            }

            printf("Player %u is ready\n", header.sender_id);
            print_players(state);
            break;

        case MSG_SYNC_BOARD:
            printf("SYNC_BOARD received\n");
            clear_explosions(state);

            for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
                state->players[i].alive = false;
            }

            break;

        case MSG_SET_STATUS: {
            uint8_t game_status;

            if (recv_status_payload(fd, &game_status) != 0) {
                printf("Failed to read SET_STATUS payload\n");
                return false;
            }

            state->game_status = game_status;

            printf("Game status changed: %u\n", game_status);

            if (game_status == GAME_LOBBY) {
                state->has_map = false;
                clear_explosions(state);

                for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
                    state->players[i].ready = false;
                    state->players[i].alive = false;
                    state->players[i].cell = 0;
                }

                printf("Returned to lobby. Type /ready to start again.\n");
            }

            if (game_status == GAME_RUNNING) {
                for (uint8_t i = 0; i < MAX_PLAYERS; ++i) {
                    if (state->players[i].active) {
                        state->players[i].alive = true;
                    }
                }
            }

            break;
        }

        case MSG_MAP: {
            game_map_t map;

            if (recv_map_payload(fd, &map) != 0) {
                printf("Failed to read MAP payload\n");
                return false;
            }

            handle_map_received(state, &map);
            break;
        }

        case MSG_MOVED: {
            msg_moved_t moved;

            if (recv_moved_payload(fd, &moved) != 0) {
                printf("Failed to read MOVED payload\n");
                return false;
            }

            if (moved.player_id < MAX_PLAYERS) {
                state->players[moved.player_id].active = true;
                state->players[moved.player_id].alive = true;
                state->players[moved.player_id].cell = moved.cell;
            }

            printf("Player %u moved to cell %u\n", moved.player_id, moved.cell);

            render_map(state);
            break;
        }

        case MSG_BOMB: {
            msg_bomb_t bomb;

            if (recv_bomb_payload(fd, &bomb) != 0) {
                printf("Failed to read BOMB payload\n");
                return false;
            }

            mark_bomb_at_cell(state, bomb.cell);

            printf("Player %u placed bomb at cell %u\n", bomb.owner_id,
                   bomb.cell);

            render_map(state);
            break;
        }

        case MSG_EXPLOSION_START: {
            msg_explosion_t explosion;

            if (recv_explosion_payload(fd, &explosion) != 0) {
                printf("Failed to read EXPLOSION_START payload\n");
                return false;
            }

            remove_bomb_at_cell(state, explosion.cell);
            set_explosion_cells(state, explosion.cell, explosion.radius, true);

            printf("Explosion started at cell %u radius %u\n", explosion.cell,
                   explosion.radius);

            render_map(state);
            break;
        }

        case MSG_EXPLOSION_END: {
            msg_explosion_t explosion;

            if (recv_explosion_payload(fd, &explosion) != 0) {
                printf("Failed to read EXPLOSION_END payload\n");
                return false;
            }

            set_explosion_cells(state, explosion.cell, explosion.radius, false);

            printf("Explosion ended at cell %u radius %u\n", explosion.cell,
                   explosion.radius);

            render_map(state);
            break;
        }

        case MSG_DEATH: {
            uint8_t player_id;

            if (recv_death_payload(fd, &player_id) != 0) {
                printf("Failed to read DEATH payload\n");
                return false;
            }

            if (player_id < MAX_PLAYERS) {
                state->players[player_id].alive = false;
            }

            printf("Player %u died\n", player_id);
            render_map(state);
            break;
        }

        case MSG_BONUS_AVAILABLE: {
            uint8_t bonus_type;
            uint16_t cell;

            if (recv_bonus_available_payload(fd, &bonus_type, &cell) != 0) {
                printf("Failed to read BONUS_AVAILABLE payload\n");
                return false;
            }

            if (state->has_map && cell < map_cell_count(&state->map)) {
                state->map.cells[cell] =
                    (uint8_t)bonus_cell_from_type(bonus_type);
            }

            printf("Bonus available at cell %u type %u\n", cell, bonus_type);
            render_map(state);
            break;
        }

        case MSG_BONUS_RETRIEVED: {
            uint8_t player_id;
            uint16_t cell;

            if (recv_bonus_retrieved_payload(fd, &player_id, &cell) != 0) {
                printf("Failed to read BONUS_RETRIEVED payload\n");
                return false;
            }

            if (state->has_map && cell < map_cell_count(&state->map)) {
                state->map.cells[cell] = '.';
            }

            printf("Player %u picked bonus at cell %u\n", player_id, cell);
            render_map(state);
            break;
        }

        case MSG_BLOCK_DESTROYED: {
            uint16_t cell;

            if (recv_block_destroyed_payload(fd, &cell) != 0) {
                printf("Failed to read BLOCK_DESTROYED payload\n");
                return false;
            }

            if (state->has_map && cell < map_cell_count(&state->map)) {
                state->map.cells[cell] = '.';
            }

            printf("Block destroyed at cell %u\n", cell);
            render_map(state);
            break;
        }

        case MSG_WINNER: {
            uint8_t winner_id;

            if (recv_winner_payload(fd, &winner_id) != 0) {
                printf("Failed to read WINNER payload\n");
                return false;
            }

            if (winner_id == 255) {
                printf("Game ended with draw\n");
            } else {
                printf("Winner is player %u\n", winner_id);
            }

            print_players(state);
            break;
        }

        case MSG_ROUND_STATS: {
            msg_round_stat_t stats[MAX_PLAYERS];
            uint8_t count;

            if (recv_round_stats_payload(fd, stats, &count, MAX_PLAYERS) != 0) {
                printf("Failed to read ROUND_STATS payload\n");
                return false;
            }

            printf("\nRound statistics:\n");

            for (uint8_t i = 0; i < count; ++i) {
                const char* name = "unknown";

                if (stats[i].player_id < MAX_PLAYERS &&
                    state->players[stats[i].player_id].name[0] != '\0') {
                    name = state->players[stats[i].player_id].name;
                }

                printf(
                    "  Player %u (%s): kills=%u destroyed_blocks=%u "
                    "bonuses=%u\n",
                    stats[i].player_id, name, stats[i].kills,
                    stats[i].destroyed_blocks, stats[i].collected_bonuses);
            }

            printf("\n");
            break;
        }

        case MSG_PING:
            send_header(fd, MSG_PONG, state->my_id, TARGET_SERVER);
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

static bool send_move_command(int fd,
                              client_state_t* state,
                              uint8_t direction_ascii) {
    if (state->game_status != GAME_RUNNING) {
        printf("Game is not running yet\n");
        return true;
    }

    if (state->my_id >= MAX_PLAYERS || !state->players[state->my_id].alive) {
        printf("You are not alive\n");
        return true;
    }

    if (send_move_attempt(fd, state->my_id, direction_ascii) != 0) {
        return false;
    }

    return true;
}

static bool handle_user_input(int fd, client_state_t* state) {
    char line[128];

    if (fgets(line, sizeof(line), stdin) == NULL) {
        return false;
    }

    line[strcspn(line, "\n")] = '\0';

    if (strcmp(line, "/ready") == 0) {
        if (send_header(fd, MSG_SET_READY, state->my_id, TARGET_SERVER) != 0) {
            return false;
        }

        printf("READY sent\n");
    } else if (strcmp(line, "/ping") == 0) {
        if (send_header(fd, MSG_PING, state->my_id, TARGET_SERVER) != 0) {
            return false;
        }

        printf("PING sent\n");
    } else if (strcmp(line, "/quit") == 0) {
        send_header(fd, MSG_LEAVE, state->my_id, TARGET_SERVER);
        return false;
    } else if (strcmp(line, "/sync") == 0) {
        if (send_header(fd, MSG_SYNC_REQUEST, state->my_id, TARGET_SERVER) !=
            0) {
            return false;
        }

        printf("SYNC_REQUEST sent\n");
    } else if (strcmp(line, "/lobby") == 0) {
        if (state->game_status != GAME_END) {
            printf("Game is not ended yet\n");
            return true;
        }

        if (send_status(fd, state->my_id, TARGET_SERVER, GAME_LOBBY) != 0) {
            return false;
        }

        printf("Return to lobby requested\n");
    } else if (strcmp(line, "/bomb") == 0) {
        if (state->game_status != GAME_RUNNING) {
            printf("Game is not running yet\n");
            return true;
        }

        uint16_t cell = get_my_cell(state);

        if (send_bomb_attempt(fd, state->my_id, cell) != 0) {
            return false;
        }

        printf("BOMB_ATTEMPT sent for cell %u\n", cell);
    } else if (strcmp(line, "w") == 0) {
        return send_move_command(fd, state, 'U');
    } else if (strcmp(line, "s") == 0) {
        return send_move_command(fd, state, 'D');
    } else if (strcmp(line, "a") == 0) {
        return send_move_command(fd, state, 'L');
    } else if (strcmp(line, "d") == 0) {
        return send_move_command(fd, state, 'R');
    } else if (strcmp(line, "/players") == 0) {
        print_players(state);
    } else if (strcmp(line, "/map") == 0) {
        render_map(state);
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
        fprintf(stderr, "Expected WELCOME, got message type %u\n",
                header.msg_type);
        close(fd);
        return 1;
    }

    protocol_player_info_t existing_players[MAX_PLAYERS];
    msg_welcome_t welcome;

    if (recv_welcome_payload(fd, &welcome, existing_players, MAX_PLAYERS) !=
        0) {
        fprintf(stderr, "Failed to read WELCOME payload\n");
        close(fd);
        return 1;
    }

    client_state_t state;
    init_state(&state);

    state.my_id = header.target_id;
    state.game_status = welcome.game_status;

    if (state.my_id < MAX_PLAYERS) {
        state.players[state.my_id].active = true;
        state.players[state.my_id].alive = welcome.game_status == GAME_RUNNING;
        set_player_name(&state, state.my_id, player_name);
    }

    for (uint8_t i = 0; i < welcome.other_players_count; ++i) {
        uint8_t player_id = existing_players[i].id;

        if (player_id < MAX_PLAYERS) {
            state.players[player_id].active = true;
            state.players[player_id].alive =
                welcome.game_status == GAME_RUNNING;
            state.players[player_id].ready = existing_players[i].ready != 0;
            set_player_name(&state, player_id, existing_players[i].player_name);
        }
    }

    printf("Connected to server: %s\n", welcome.server_id);
    printf("Game status: %u\n", welcome.game_status);
    printf("My player id: %u\n", state.my_id);

    if (welcome.other_players_count > 0) {
        printf("Players already in lobby:\n");

        for (uint8_t i = 0; i < welcome.other_players_count; ++i) {
            printf("  %u: %s ready=%u\n", existing_players[i].id,
                   existing_players[i].player_name, existing_players[i].ready);
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
            running = handle_server_message(fd, &state);
        }

        if (running && FD_ISSET(STDIN_FILENO, &read_fds)) {
            running = handle_user_input(fd, &state);
        }
    }

    close(fd);

    return 0;
}
