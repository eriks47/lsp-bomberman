#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <raylib.h>

#include "../common/map.h"
#include "../common/protocol.h"
#include "graphics/input_field.h"
#include "net/net.h"
#include "config.h"

/* ── Palette ────────────────────────────────────────────────────────────────
 */
#define COL_BG (Color){18, 18, 24, 255}
#define COL_PANEL (Color){28, 28, 38, 255}
#define COL_BORDER (Color){55, 55, 80, 255}
#define COL_ACCENT (Color){255, 200, 50, 255}
#define COL_ACCENT2 (Color){80, 180, 255, 255}
#define COL_TEXT (Color){220, 220, 230, 255}
#define COL_TEXT_DIM (Color){110, 110, 140, 255}
#define COL_RED (Color){220, 70, 70, 255}
#define COL_GREEN (Color){80, 200, 120, 255}
#define COL_WALL (Color){55, 55, 75, 255}
#define COL_SOFT (Color){140, 100, 60, 255}
#define COL_BOMB (Color){220, 70, 70, 255}
#define COL_EXPLOSION (Color){255, 160, 30, 255}
#define COL_EMPTY (Color){30, 30, 42, 255}
#define COL_INPUT_ACTIVE (Color){38, 38, 58, 255}
#define COL_INPUT_IDLE (Color){28, 28, 40, 255}
#define COL_BTN (Color){255, 200, 50, 255}
#define COL_BTN_TEXT (Color){18, 18, 24, 255}

/* ── Player colours ──────────────────────────────────────────────────────────
 */
static const Color PLAYER_COLORS[8] = {
    {100, 180, 255, 255}, /* 1 – blue   */
    {255, 120, 80, 255},  /* 2 – orange */
    {100, 230, 130, 255}, /* 3 – green  */
    {220, 80, 220, 255},  /* 4 – purple */
    {255, 220, 60, 255},  /* 5 – yellow */
    {80, 230, 220, 255},  /* 6 – cyan   */
    {255, 140, 180, 255}, /* 7 – pink   */
    {180, 140, 255, 255}, /* 8 – lilac  */
};

/* ── Screen sizes ────────────────────────────────────────────────────────────
 */
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 1280
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 720
#endif
#ifndef WINDOW_TITLE
#define WINDOW_TITLE "Bomberman"
#endif
#ifndef CLIENT_ID
#define CLIENT_ID "lsp-client-0.1"
#endif
#ifndef CLIENT_ID_LEN
#define CLIENT_ID_LEN 32
#endif

/* ── Game state machine ──────────────────────────────────────────────────────
 */
typedef enum {
    SCREEN_CONNECT,
    SCREEN_LOBBY,
    SCREEN_GAME,
    SCREEN_ENDGAME,
} screen_t;

/* ── Per-player client state ─────────────────────────────────────────────────
 */
typedef struct {
    bool active;
    bool alive;
    bool ready;
    uint16_t cell;
    char name[PROTOCOL_PLAYER_NAME_LEN];
} gui_player_t;

/* ── Full client state ───────────────────────────────────────────────────────
 */
typedef struct {
    /* networking */
    int sockfd;
    struct pollfd pfd;
    bool connected;

    /* connection form */
    char server_address[256];
    uint16_t port;
    char player_name[PROTOCOL_PLAYER_NAME_LEN];
    char client_id[CLIENT_ID_LEN];
    char server_id[PROTOCOL_SERVER_ID_LEN];

    /* identity */
    uint8_t my_id;

    /* game state */
    screen_t screen;
    uint8_t game_status;
    game_map_t map;
    bool has_map;
    bool explosion_cells[MAX_MAP_CELLS];
    gui_player_t players[MAX_PLAYERS];

    /* end-game */
    uint8_t winner_id; /* 255 = draw */
    msg_round_stat_t round_stats[MAX_PLAYERS];
    uint8_t round_stats_count;

    /* notification bar */
    char notify_msg[256];
    double notify_until; /* GetTime() deadline */

    /* connect-screen input fields */
    input_field_t ip_input;
    input_field_t port_input;
    input_field_t name_input;

    /* connect-screen error */
    char connect_error[128];
} client_t;

/* ── Helpers ─────────────────────────────────────────────────────────────────
 */
static void notify(client_t* c, const char* msg) {
    strncpy(c->notify_msg, msg, sizeof(c->notify_msg) - 1);
    c->notify_until = GetTime() + 3.0;
}

static void copy_str(char* dst, size_t n, const char* src) {
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static void set_player_name(client_t* c, uint8_t id, const char* name) {
    if (id >= MAX_PLAYERS)
        return;
    copy_protocol_string(c->players[id].name, sizeof(c->players[id].name),
                         name);
}

static uint16_t cell_at(const game_map_t* m, uint16_t row, uint16_t col) {
    return make_cell_index(row, col, m->cols);
}

static bool is_player_start(char ch) {
    return ch >= '1' && ch <= '8';
}
static bool is_bonus(char ch) {
    return ch == 'A' || ch == 'R' || ch == 'T' || ch == 'N';
}
static bool is_base_cell(char ch) {
    return ch == '.' || ch == 'H' || ch == 'S' || ch == 'B' || is_bonus(ch) ||
           is_player_start(ch);
}
static bool blocks_explosion(char ch) {
    return ch == 'H' || ch == 'S';
}

static char bonus_char(uint8_t type) {
    switch (type) {
        case BONUS_SPEED:
            return 'A';
        case BONUS_RADIUS:
            return 'R';
        case BONUS_TIMER:
            return 'T';
        case BONUS_BOMB_COUNT:
            return 'N';
        default:
            return '.';
    }
}

static void init_positions(client_t* c) {
    if (!c->has_map)
        return;
    size_t n = map_cell_count(&c->map);
    for (size_t i = 0; i < n; ++i) {
        char ch = (char)c->map.cells[i];
        if (is_player_start(ch)) {
            uint8_t pid = (uint8_t)(ch - '1');
            if (pid < MAX_PLAYERS) {
                c->players[pid].cell = (uint16_t)i;
                if (c->players[pid].active)
                    c->players[pid].alive = true;
            }
        }
    }
}

static void clear_start_markers(client_t* c) {
    if (!c->has_map)
        return;
    size_t n = map_cell_count(&c->map);
    for (size_t i = 0; i < n; ++i)
        if (is_player_start((char)c->map.cells[i]))
            c->map.cells[i] = '.';
}

static void set_explosion_cells(client_t* c,
                                uint16_t center,
                                uint8_t radius,
                                bool val) {
    if (!c->has_map || center >= map_cell_count(&c->map))
        return;
    uint16_t crow, ccol;
    split_cell_index(center, c->map.cols, &crow, &ccol);
    c->explosion_cells[center] = val;
    const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int d = 0; d < 4; d++) {
        for (uint8_t dist = 1; dist <= radius; dist++) {
            int r = (int)crow + dirs[d][0] * dist;
            int col = (int)ccol + dirs[d][1] * dist;
            if (r < 0 || col < 0 || r >= c->map.rows || col >= c->map.cols)
                break;
            uint16_t idx = cell_at(&c->map, (uint16_t)r, (uint16_t)col);
            c->explosion_cells[idx] = val;
            if (blocks_explosion((char)c->map.cells[idx]))
                break;
        }
    }
}

static void mark_bomb(client_t* c, uint16_t cell) {
    if (!c->has_map || cell >= map_cell_count(&c->map))
        return;
    if (is_base_cell((char)c->map.cells[cell]))
        c->map.cells[cell] = 'B';
}

static void remove_bomb(client_t* c, uint16_t cell) {
    if (!c->has_map || cell >= map_cell_count(&c->map))
        return;
    if ((char)c->map.cells[cell] == 'B')
        c->map.cells[cell] = '.';
}

/* ── Network: handle one server message ──────────────────────────────────────
 */
static bool handle_server_message(client_t* c) {
    msg_header_t hdr;
    if (recv_header(c->sockfd, &hdr) != 0) {
        notify(c, "Server disconnected");
        c->connected = false;
        c->screen = SCREEN_CONNECT;
        return false;
    }

    switch (hdr.msg_type) {
        case MSG_HELLO: {
            msg_hello_t hello;
            if (recv_hello_payload(c->sockfd, &hello) != 0)
                return false;
            if (hdr.sender_id < MAX_PLAYERS) {
                c->players[hdr.sender_id].active = true;
                c->players[hdr.sender_id].alive =
                    (c->game_status == GAME_RUNNING);
                set_player_name(c, hdr.sender_id, hello.player_name);
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "Player %u joined: %s", hdr.sender_id,
                     hello.player_name);
            notify(c, buf);
            break;
        }

        case MSG_LEAVE:
            if (hdr.sender_id < MAX_PLAYERS) {
                c->players[hdr.sender_id].active = false;
                c->players[hdr.sender_id].alive = false;
            }
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Player %u left", hdr.sender_id);
                notify(c, buf);
            }
            break;

        case MSG_SET_READY:
            if (hdr.sender_id < MAX_PLAYERS)
                c->players[hdr.sender_id].ready = true;
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Player %u is ready", hdr.sender_id);
                notify(c, buf);
            }
            break;

        case MSG_SYNC_BOARD:
            memset(c->explosion_cells, 0, sizeof(c->explosion_cells));
            for (int i = 0; i < MAX_PLAYERS; i++)
                c->players[i].alive = false;
            break;

        case MSG_SET_STATUS: {
            uint8_t status;
            if (recv_status_payload(c->sockfd, &status) != 0)
                return false;
            c->game_status = status;

            if (status == GAME_LOBBY) {
                c->has_map = false;
                memset(c->explosion_cells, 0, sizeof(c->explosion_cells));
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    c->players[i].ready = false;
                    c->players[i].alive = false;
                    c->players[i].cell = 0;
                }
                c->screen = SCREEN_LOBBY;
                notify(c, "Returned to lobby");
            }
            if (status == GAME_RUNNING) {
                for (int i = 0; i < MAX_PLAYERS; i++)
                    if (c->players[i].active)
                        c->players[i].alive = true;
                c->screen = SCREEN_GAME;
            }
            if (status == GAME_END) {
                c->screen = SCREEN_ENDGAME;
            }
            break;
        }

        case MSG_MAP: {
            game_map_t map;
            if (recv_map_payload(c->sockfd, &map) != 0)
                return false;
            c->map = map;
            c->has_map = true;
            memset(c->explosion_cells, 0, sizeof(c->explosion_cells));
            init_positions(c);
            clear_start_markers(c);
            c->screen = SCREEN_GAME;
            notify(c, "Map received – game starting!");
            break;
        }

        case MSG_MOVED: {
            msg_moved_t moved;
            if (recv_moved_payload(c->sockfd, &moved) != 0)
                return false;
            if (moved.player_id < MAX_PLAYERS) {
                c->players[moved.player_id].active = true;
                c->players[moved.player_id].alive = true;
                c->players[moved.player_id].cell = moved.cell;
            }
            break;
        }

        case MSG_BOMB: {
            msg_bomb_t bomb;
            if (recv_bomb_payload(c->sockfd, &bomb) != 0)
                return false;
            mark_bomb(c, bomb.cell);
            break;
        }

        case MSG_EXPLOSION_START: {
            msg_explosion_t ex;
            if (recv_explosion_payload(c->sockfd, &ex) != 0)
                return false;
            remove_bomb(c, ex.cell);
            set_explosion_cells(c, ex.cell, ex.radius, true);
            break;
        }

        case MSG_EXPLOSION_END: {
            msg_explosion_t ex;
            if (recv_explosion_payload(c->sockfd, &ex) != 0)
                return false;
            set_explosion_cells(c, ex.cell, ex.radius, false);
            break;
        }

        case MSG_DEATH: {
            uint8_t pid;
            if (recv_death_payload(c->sockfd, &pid) != 0)
                return false;
            if (pid < MAX_PLAYERS)
                c->players[pid].alive = false;
            if (pid == c->my_id)
                notify(c, "You died!");
            break;
        }

        case MSG_BONUS_AVAILABLE: {
            uint8_t btype;
            uint16_t bcell;
            if (recv_bonus_available_payload(c->sockfd, &btype, &bcell) != 0)
                return false;
            if (c->has_map && bcell < map_cell_count(&c->map))
                c->map.cells[bcell] = (uint8_t)bonus_char(btype);
            break;
        }

        case MSG_BONUS_RETRIEVED: {
            uint8_t pid;
            uint16_t bcell;
            if (recv_bonus_retrieved_payload(c->sockfd, &pid, &bcell) != 0)
                return false;
            if (c->has_map && bcell < map_cell_count(&c->map))
                c->map.cells[bcell] = '.';
            if (pid == c->my_id)
                notify(c, "Bonus collected!");
            break;
        }

        case MSG_BLOCK_DESTROYED: {
            uint16_t bcell;
            if (recv_block_destroyed_payload(c->sockfd, &bcell) != 0)
                return false;
            if (c->has_map && bcell < map_cell_count(&c->map))
                c->map.cells[bcell] = '.';
            break;
        }

        case MSG_WINNER: {
            uint8_t wid;
            if (recv_winner_payload(c->sockfd, &wid) != 0)
                return false;
            c->winner_id = wid;
            c->screen = SCREEN_ENDGAME;
            if (wid == 255)
                notify(c, "Game over – Draw!");
            else if (wid == c->my_id)
                notify(c, "You WIN!");
            else {
                char buf[64];
                snprintf(buf, sizeof(buf), "Player %u wins!", wid);
                notify(c, buf);
            }
            break;
        }

        case MSG_ROUND_STATS: {
            uint8_t cnt;
            if (recv_round_stats_payload(c->sockfd, c->round_stats, &cnt,
                                         MAX_PLAYERS) != 0)
                return false;
            c->round_stats_count = cnt;
            break;
        }

        case MSG_PING:
            send_header(c->sockfd, MSG_PONG, c->my_id, TARGET_SERVER);
            break;

        case MSG_PONG:
            notify(c, "Pong!");
            break;

        case MSG_ERROR:
            notify(c, "Error received from server");
            break;

        case MSG_DISCONNECT:
            notify(c, "Kicked by server");
            c->connected = false;
            c->screen = SCREEN_CONNECT;
            return false;

        default:
            break;
    }
    return true;
}

/* ── Drawing helpers ─────────────────────────────────────────────────────────
 */
static void draw_panel(int x, int y, int w, int h) {
    DrawRectangle(x, y, w, h, COL_PANEL);
    DrawRectangleLinesEx((Rectangle){x, y, w, h}, 1.5f, COL_BORDER);
}

static void draw_label(int x, int y, int size, const char* text, Color col) {
    DrawText(text, x, y, size, col);
}

static void draw_centered_text(const char* text, int y, int size, Color col) {
    int tw = MeasureText(text, size);
    DrawText(text, (SCREEN_WIDTH - tw) / 2, y, size, col);
}

static void draw_btn(Rectangle r, const char* label, bool hovered) {
    Color bg = hovered ? COL_ACCENT2 : COL_BTN;
    Color txt = COL_BTN_TEXT;
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 2.0f, hovered ? WHITE : COL_ACCENT);
    int tw = MeasureText(label, 18);
    DrawText(label, (int)(r.x + (r.width - tw) / 2),
             (int)(r.y + (r.height - 18) / 2), 18, txt);
}

static bool btn_pressed(Rectangle r) {
    return IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
           CheckCollisionPointRec(GetMousePosition(), r);
}

/* ── Render: connect screen ──────────────────────────────────────────────────
 */
static void render_connect(client_t* c) {
    /* dark gradient background */
    DrawRectangleGradientV(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG,
                           (Color){10, 10, 20, 255});

    /* title */
    draw_centered_text("BOMBERMAN", 80, 64, COL_ACCENT);
    draw_centered_text("Enter server details to connect", 160, 18,
                       COL_TEXT_DIM);

    /* form panel */
    int pw = 420, ph = 280;
    int px = (SCREEN_WIDTH - pw) / 2, py = 210;
    draw_panel(px, py, pw, ph);

    int lx = px + 20, fy = py + 30, fs = 52, gap = 75;

    draw_label(lx, fy, 14, "SERVER IP", COL_TEXT_DIM);
    draw_input_field(&c->ip_input, NULL);

    draw_label(lx, fy + gap, 14, "PORT", COL_TEXT_DIM);
    draw_input_field(&c->port_input, NULL);

    draw_label(lx, fy + gap * 2, 14, "PLAYER NAME", COL_TEXT_DIM);
    draw_input_field(&c->name_input, NULL);

    /* connect button */
    Rectangle btn = {(float)px + 20, (float)(py + ph - 55), (float)(pw - 40),
                     38};
    bool hov = CheckCollisionPointRec(GetMousePosition(), btn);
    draw_btn(btn, "CONNECT", hov);

    /* error */
    if (c->connect_error[0]) {
        int ew = MeasureText(c->connect_error, 16);
        DrawText(c->connect_error, (SCREEN_WIDTH - ew) / 2, py + ph + 10, 16,
                 COL_RED);
    }
}

/* ── Render: lobby screen ────────────────────────────────────────────────────
 */
static void render_lobby(client_t* c) {
    DrawRectangleGradientV(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG,
                           (Color){10, 10, 20, 255});

    draw_centered_text("LOBBY", 40, 48, COL_ACCENT);

    char sub[64];
    snprintf(sub, sizeof(sub), "Connected as: %s  (ID %u)",
             c->my_id < MAX_PLAYERS ? c->players[c->my_id].name : "?",
             c->my_id);
    draw_centered_text(sub, 100, 18, COL_TEXT_DIM);

    /* player list panel */
    int pw = 500, ph = 360;
    int px = (SCREEN_WIDTH - pw) / 2, py = 140;
    draw_panel(px, py, pw, ph);

    draw_label(px + 20, py + 15, 16, "PLAYERS", COL_TEXT_DIM);

    int row_h = 38, row_y = py + 48;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gui_player_t* p = &c->players[i];
        if (!p->active)
            continue;

        Color pc = PLAYER_COLORS[i];
        DrawRectangle(px + 15, row_y, 6, 28, pc);

        char pname[128];
        snprintf(pname, sizeof(pname), "P%d  %s", i + 1, p->name);
        DrawText(pname, px + 32, row_y + 6, 18,
                 i == c->my_id ? COL_ACCENT : COL_TEXT);

        if (p->ready) {
            const char* rdy = "READY";
            int rw = MeasureText(rdy, 16);
            DrawText(rdy, px + pw - rw - 20, row_y + 7, 16, COL_GREEN);
        } else {
            DrawText("waiting...", px + pw - 120, row_y + 7, 15, COL_TEXT_DIM);
        }

        row_y += row_h;
    }

    /* buttons */
    int bw = 160, bh = 40, by = py + ph + 20;
    Rectangle ready_btn = {(float)(SCREEN_WIDTH / 2 - bw - 10), (float)by,
                           (float)bw, (float)bh};
    Rectangle ping_btn = {(float)(SCREEN_WIDTH / 2 + 10), (float)by, (float)bw,
                          (float)bh};

    draw_btn(ready_btn, "READY",
             CheckCollisionPointRec(GetMousePosition(), ready_btn));
    draw_btn(ping_btn, "PING",
             CheckCollisionPointRec(GetMousePosition(), ping_btn));
}

/* ── Render: game screen ─────────────────────────────────────────────────────
 */
#define CELL_SIZE 36
#define MAP_PAD_X 20
#define MAP_PAD_Y 20
#define SIDEBAR_W 280

static void render_game(client_t* c) {
    ClearBackground(COL_BG);

    /* ── map ── */
    if (c->has_map) {
        int map_px_w = c->map.cols * CELL_SIZE;
        int map_px_h = c->map.rows * CELL_SIZE;

        /* centre map in the left area */
        int avail_w = SCREEN_WIDTH - SIDEBAR_W - MAP_PAD_X * 2;
        int ox = MAP_PAD_X + (avail_w - map_px_w) / 2;
        int oy = (SCREEN_HEIGHT - map_px_h) / 2;

        for (uint8_t row = 0; row < c->map.rows; row++) {
            for (uint8_t col = 0; col < c->map.cols; col++) {
                uint16_t idx = cell_at(&c->map, row, col);
                char ch = (char)c->map.cells[idx];
                int cx = ox + col * CELL_SIZE;
                int cy = oy + row * CELL_SIZE;

                Color bg = COL_EMPTY;
                Color fg = COL_TEXT;

                if (ch == 'H')
                    bg = COL_WALL;
                else if (ch == 'S')
                    bg = COL_SOFT;
                else if (ch == 'B')
                    bg = COL_BOMB;
                else if (is_bonus(ch))
                    bg = (Color){40, 80, 60, 255};

                if (c->explosion_cells[idx])
                    bg = COL_EXPLOSION;

                DrawRectangle(cx, cy, CELL_SIZE - 1, CELL_SIZE - 1, bg);

                /* cell symbol */
                if (ch == 'H') {
                    /* wall – no text, just shade */
                    DrawRectangle(cx + 1, cy + 1, CELL_SIZE - 3, CELL_SIZE - 3,
                                  (Color){45, 45, 65, 255});
                } else if (ch == 'S') {
                    DrawText("#", cx + 10, cy + 8, 18,
                             (Color){180, 130, 80, 255});
                } else if (ch == 'B') {
                    DrawText("o", cx + 10, cy + 7, 20,
                             (Color){255, 80, 60, 255});
                } else if (is_bonus(ch)) {
                    char sym[2] = {ch, '\0'};
                    DrawText(sym, cx + 10, cy + 8, 18, COL_ACCENT);
                } else if (c->explosion_cells[idx]) {
                    DrawText("*", cx + 9, cy + 7, 20, WHITE);
                }
            }
        }

        /* draw players on top */
        for (int pid = 0; pid < MAX_PLAYERS; pid++) {
            gui_player_t* p = &c->players[pid];
            if (!p->active || !p->alive)
                continue;
            uint16_t crow, ccol;
            split_cell_index(p->cell, c->map.cols, &crow, &ccol);
            int cx = ox + (int)ccol * CELL_SIZE;
            int cy = oy + (int)crow * CELL_SIZE;
            Color pc = PLAYER_COLORS[pid];
            DrawCircle(cx + CELL_SIZE / 2, cy + CELL_SIZE / 2,
                       CELL_SIZE / 2 - 3, pc);
            /* player number */
            char num[2] = {'1' + pid, '\0'};
            DrawText(num, cx + CELL_SIZE / 2 - 5, cy + CELL_SIZE / 2 - 9, 18,
                     pid == c->my_id ? COL_BTN_TEXT : WHITE);
            if (pid == c->my_id)
                DrawRectangleLinesEx(
                    (Rectangle){cx, cy, CELL_SIZE - 1, CELL_SIZE - 1}, 2.0f,
                    COL_ACCENT);
        }
    } else {
        draw_centered_text("Waiting for map...", SCREEN_HEIGHT / 2 - 12, 24,
                           COL_TEXT_DIM);
    }

    /* ── sidebar ── */
    int sx = SCREEN_WIDTH - SIDEBAR_W;
    DrawRectangle(sx, 0, SIDEBAR_W, SCREEN_HEIGHT, COL_PANEL);
    DrawLine(sx, 0, sx, SCREEN_HEIGHT, COL_BORDER);

    draw_label(sx + 16, 20, 20, "PLAYERS", COL_TEXT_DIM);

    int ry = 60;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gui_player_t* p = &c->players[i];
        if (!p->active)
            continue;
        Color pc = p->alive ? PLAYER_COLORS[i] : COL_TEXT_DIM;
        DrawRectangle(sx + 12, ry, 5, 32, pc);
        char label[64];
        snprintf(label, sizeof(label), "P%d %s", i + 1, p->name);
        DrawText(label, sx + 26, ry + 4, 16,
                 p->alive ? COL_TEXT : COL_TEXT_DIM);
        if (!p->alive)
            DrawText("DEAD", sx + SIDEBAR_W - 60, ry + 7, 14, COL_RED);
        ry += 44;
    }

    /* controls hint */
    draw_label(sx + 16, SCREEN_HEIGHT - 130, 13, "WASD  move", COL_TEXT_DIM);
    draw_label(sx + 16, SCREEN_HEIGHT - 110, 13, "SPACE place bomb",
               COL_TEXT_DIM);
    draw_label(sx + 16, SCREEN_HEIGHT - 90, 13, "ESC   lobby", COL_TEXT_DIM);
}

/* ── Render: end-game screen ─────────────────────────────────────────────────
 */
static void render_endgame(client_t* c) {
    DrawRectangleGradientV(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG,
                           (Color){10, 10, 20, 255});

    if (c->winner_id == 255) {
        draw_centered_text("DRAW!", 80, 72, COL_ACCENT2);
    } else if (c->winner_id == c->my_id) {
        draw_centered_text("YOU WIN!", 80, 72, COL_ACCENT);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "PLAYER %u WINS!", c->winner_id + 1);
        draw_centered_text(buf, 80, 72, PLAYER_COLORS[c->winner_id % 8]);
    }

    /* round stats */
    int pw = 520, ph = 60 + c->round_stats_count * 44 + 20;
    int px = (SCREEN_WIDTH - pw) / 2, py = 180;
    draw_panel(px, py, pw, ph);
    draw_label(px + 20, py + 14, 15, "ROUND STATISTICS", COL_TEXT_DIM);

    int ry = py + 42;
    for (int i = 0; i < c->round_stats_count; i++) {
        msg_round_stat_t* s = &c->round_stats[i];
        const char* name =
            s->player_id < MAX_PLAYERS ? c->players[s->player_id].name : "?";
        Color pc = s->player_id < 8 ? PLAYER_COLORS[s->player_id] : COL_TEXT;
        DrawRectangle(px + 12, ry, 5, 32, pc);
        char row[128];
        snprintf(row, sizeof(row),
                 "P%d %-14s  kills %-3u  blocks %-3u  bonuses %u",
                 s->player_id + 1, name, s->kills, s->destroyed_blocks,
                 s->collected_bonuses);
        DrawText(row, px + 26, ry + 7, 15, COL_TEXT);
        ry += 44;
    }

    /* buttons */
    int by = py + ph + 30;
    Rectangle lobby_btn = {(float)(SCREEN_WIDTH / 2 - 170), (float)by, 160, 42};
    Rectangle quit_btn = {(float)(SCREEN_WIDTH / 2 + 10), (float)by, 160, 42};

    draw_btn(lobby_btn, "BACK TO LOBBY",
             CheckCollisionPointRec(GetMousePosition(), lobby_btn));
    draw_btn(quit_btn, "QUIT",
             CheckCollisionPointRec(GetMousePosition(), quit_btn));
}

/* ── Render notification bar ─────────────────────────────────────────────────
 */
static void render_notify(client_t* c) {
    if (GetTime() > c->notify_until)
        return;
    float alpha = (float)(c->notify_until - GetTime()) / 0.5f;
    if (alpha > 1.0f)
        alpha = 1.0f;
    Color bg = (Color){30, 30, 50, (uint8_t)(200 * alpha)};
    Color txt = (Color){220, 220, 230, (uint8_t)(255 * alpha)};
    int tw = MeasureText(c->notify_msg, 17);
    int bw = tw + 40, bh = 36;
    int bx = (SCREEN_WIDTH - bw) / 2, by = SCREEN_HEIGHT - 60;
    DrawRectangleRounded((Rectangle){bx, by, bw, bh}, 0.4f, 8, bg);
    DrawText(c->notify_msg, bx + 20, by + 9, 17, txt);
}

/* ── Update: connect screen ──────────────────────────────────────────────────
 */
static void update_input_active(input_field_t* f) {
    if (!f->active)
        return;
    if (IsKeyPressed(KEY_BACKSPACE) && f->len > 0)
        f->buf[--f->len] = '\0';
    int ch;
    while ((ch = GetCharPressed()) != 0)
        if (f->len < (int)sizeof(f->buf) - 1) {
            f->buf[f->len++] = (char)ch;
            f->buf[f->len] = '\0';
        }
}

static void update_connect(client_t* c) {
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        Vector2 m = GetMousePosition();
        c->ip_input.active = CheckCollisionPointRec(m, c->ip_input.bounds);
        c->port_input.active = CheckCollisionPointRec(m, c->port_input.bounds);
        c->name_input.active = CheckCollisionPointRec(m, c->name_input.bounds);

        /* connect button */
        int pw = 420, ph = 280;
        int px = (SCREEN_WIDTH - pw) / 2, py = 210;
        Rectangle btn = {(float)px + 20, (float)(py + ph - 55),
                         (float)(pw - 40), 38};
        if (CheckCollisionPointRec(m, btn)) {
            if (c->ip_input.len > 0 && c->port_input.len > 0 &&
                c->name_input.len > 0) {
                copy_str(c->server_address, sizeof(c->server_address),
                         c->ip_input.buf);
                c->port = (uint16_t)atoi(c->port_input.buf);
                copy_str(c->player_name, sizeof(c->player_name),
                         c->name_input.buf);
                copy_str(c->client_id, sizeof(c->client_id), CLIENT_ID);

                int fd = create_tcp_client(c->server_address, c->port);
                if (fd < 0) {
                    snprintf(c->connect_error, sizeof(c->connect_error),
                             "Failed to connect to %s:%u", c->server_address,
                             c->port);
                    return;
                }
                c->connect_error[0] = '\0';
                c->sockfd = fd;
                c->pfd.fd = fd;
                c->pfd.events = POLLIN;
                c->connected = true;

                /* handshake */
                if (send_hello(fd, c->client_id, c->player_name) != 0) {
                    snprintf(c->connect_error, sizeof(c->connect_error),
                             "Failed to send HELLO");
                    close(fd);
                    c->connected = false;
                    return;
                }

                msg_header_t hdr;
                if (recv_header(fd, &hdr) != 0 || hdr.msg_type != MSG_WELCOME) {
                    snprintf(c->connect_error, sizeof(c->connect_error),
                             "No WELCOME from server");
                    close(fd);
                    c->connected = false;
                    return;
                }

                protocol_player_info_t existing[MAX_PLAYERS];
                msg_welcome_t welcome;
                if (recv_welcome_payload(fd, &welcome, existing, MAX_PLAYERS) !=
                    0) {
                    snprintf(c->connect_error, sizeof(c->connect_error),
                             "Failed to read WELCOME");
                    close(fd);
                    c->connected = false;
                    return;
                }

                c->my_id = hdr.target_id;
                c->game_status = welcome.game_status;
                copy_str(c->server_id, sizeof(c->server_id), welcome.server_id);

                if (c->my_id < MAX_PLAYERS) {
                    c->players[c->my_id].active = true;
                    c->players[c->my_id].alive =
                        (welcome.game_status == GAME_RUNNING);
                    set_player_name(c, c->my_id, c->player_name);
                }

                for (uint8_t i = 0; i < welcome.other_players_count; i++) {
                    uint8_t pid = existing[i].id;
                    if (pid < MAX_PLAYERS) {
                        c->players[pid].active = true;
                        c->players[pid].ready = existing[i].ready != 0;
                        set_player_name(c, pid, existing[i].player_name);
                    }
                }

                c->screen = SCREEN_LOBBY;
                char msg[64];
                snprintf(msg, sizeof(msg), "Connected! Server: %s",
                         c->server_id);
                notify(c, msg);
            } else {
                copy_str(c->connect_error, sizeof(c->connect_error),
                         "Please fill in all fields");
            }
        }
    }

    /* tab to cycle fields */
    if (IsKeyPressed(KEY_TAB)) {
        if (c->ip_input.active) {
            c->ip_input.active = false;
            c->port_input.active = true;
        } else if (c->port_input.active) {
            c->port_input.active = false;
            c->name_input.active = true;
        } else {
            c->name_input.active = false;
            c->ip_input.active = true;
        }
    }

    update_input_active(&c->ip_input);
    update_input_active(&c->port_input);
    update_input_active(&c->name_input);
}

/* ── Update: lobby screen ────────────────────────────────────────────────────
 */
static void update_lobby(client_t* c) {
    int bw = 160, bh = 40;
    int pw = 500, ph = 360;
    int px = (SCREEN_WIDTH - pw) / 2, py = 140;
    int by = py + ph + 20;

    Rectangle ready_btn = {(float)(SCREEN_WIDTH / 2 - bw - 10), (float)by,
                           (float)bw, (float)bh};
    Rectangle ping_btn = {(float)(SCREEN_WIDTH / 2 + 10), (float)by, (float)bw,
                          (float)bh};

    if (btn_pressed(ready_btn))
        send_header(c->sockfd, MSG_SET_READY, c->my_id, TARGET_SERVER);

    if (btn_pressed(ping_btn))
        send_header(c->sockfd, MSG_PING, c->my_id, TARGET_SERVER);

    if (IsKeyPressed(KEY_ESCAPE)) {
        send_header(c->sockfd, MSG_LEAVE, c->my_id, TARGET_SERVER);
        close(c->sockfd);
        c->connected = false;
        c->screen = SCREEN_CONNECT;
        memset(c->players, 0, sizeof(c->players));
    }
}

/* ── Update: game screen ─────────────────────────────────────────────────────
 */
static void update_game(client_t* c) {
    if (!c->connected)
        return;

    if (c->game_status == GAME_RUNNING && c->my_id < MAX_PLAYERS &&
        c->players[c->my_id].alive) {
        if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP))
            send_move_attempt(c->sockfd, c->my_id, 'U');
        if (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN))
            send_move_attempt(c->sockfd, c->my_id, 'D');
        if (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_LEFT))
            send_move_attempt(c->sockfd, c->my_id, 'L');
        if (IsKeyPressed(KEY_D) || IsKeyPressed(KEY_RIGHT))
            send_move_attempt(c->sockfd, c->my_id, 'R');
        if (IsKeyPressed(KEY_SPACE)) {
            uint16_t cell =
                c->my_id < MAX_PLAYERS ? c->players[c->my_id].cell : 0;
            send_bomb_attempt(c->sockfd, c->my_id, cell);
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        /* go back to lobby */
        send_header(c->sockfd, MSG_LEAVE, c->my_id, TARGET_SERVER);
        close(c->sockfd);
        c->connected = false;
        c->screen = SCREEN_CONNECT;
        memset(c->players, 0, sizeof(c->players));
    }
}

/* ── Update: end-game screen ─────────────────────────────────────────────────
 */
static void update_endgame(client_t* c) {
    int pw = 520;
    int px = (SCREEN_WIDTH - pw) / 2, py = 180;
    int ph = 60 + c->round_stats_count * 44 + 20;
    int by = py + ph + 30;

    Rectangle lobby_btn = {(float)(SCREEN_WIDTH / 2 - 170), (float)by, 160, 42};
    Rectangle quit_btn = {(float)(SCREEN_WIDTH / 2 + 10), (float)by, 160, 42};

    if (btn_pressed(lobby_btn)) {
        send_status(c->sockfd, c->my_id, TARGET_SERVER, GAME_LOBBY);
    }
    if (btn_pressed(quit_btn)) {
        send_header(c->sockfd, MSG_LEAVE, c->my_id, TARGET_SERVER);
        close(c->sockfd);
        c->connected = false;
        c->screen = SCREEN_CONNECT;
        memset(c->players, 0, sizeof(c->players));
    }
}

/* ── Main ────────────────────────────────────────────────────────────────────
 */
int main(void) {
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, WINDOW_TITLE);
    SetTargetFPS(60);

    client_t c;
    memset(&c, 0, sizeof(c));
    c.screen = SCREEN_CONNECT;
    c.my_id = 255;
    c.winner_id = 255;
    c.sockfd = -1;

    /* position input fields inside the panel */
    int pw = 420, ph = 280;
    int px = (SCREEN_WIDTH - pw) / 2, py = 210;
    int lx = px + 20, fw = pw - 40, fh = 34, fy = py + 46, gap = 75;

    c.ip_input.bounds = (Rectangle){lx, fy, fw, fh};
    c.port_input.bounds = (Rectangle){lx, fy + gap, fw, fh};
    c.name_input.bounds = (Rectangle){lx, fy + gap * 2, fw, fh};
    c.ip_input.active = true;

    while (!WindowShouldClose()) {
        /* ── poll for server messages (non-blocking) ── */
        if (c.connected && c.sockfd >= 0) {
            int ready = poll(&c.pfd, 1, 0);
            if (ready > 0 && (c.pfd.revents & POLLIN)) {
                handle_server_message(&c);
            }
            if (c.pfd.revents & (POLLHUP | POLLERR)) {
                notify(&c, "Connection lost");
                close(c.sockfd);
                c.sockfd = -1;
                c.connected = false;
                c.screen = SCREEN_CONNECT;
            }
        }

        /* ── update ── */
        switch (c.screen) {
            case SCREEN_CONNECT:
                update_connect(&c);
                break;
            case SCREEN_LOBBY:
                update_lobby(&c);
                break;
            case SCREEN_GAME:
                update_game(&c);
                break;
            case SCREEN_ENDGAME:
                update_endgame(&c);
                break;
        }

        /* ── render ── */
        BeginDrawing();
        ClearBackground(COL_BG);

        switch (c.screen) {
            case SCREEN_CONNECT:
                render_connect(&c);
                break;
            case SCREEN_LOBBY:
                render_lobby(&c);
                break;
            case SCREEN_GAME:
                render_game(&c);
                break;
            case SCREEN_ENDGAME:
                render_endgame(&c);
                break;
        }

        render_notify(&c);
        EndDrawing();
    }

    if (c.connected && c.sockfd >= 0) {
        send_header(c.sockfd, MSG_LEAVE, c.my_id, TARGET_SERVER);
        close(c.sockfd);
    }

    CloseWindow();
    return 0;
}
