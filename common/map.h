#ifndef MAP_H
#define MAP_H

#include <stddef.h>
#include <stdint.h>

#define MAX_MAP_ROWS 255
#define MAX_MAP_COLS 255
#define MAX_MAP_CELLS (MAX_MAP_ROWS * MAX_MAP_COLS)

typedef struct {
    uint8_t rows;
    uint8_t cols;
    uint16_t player_speed;
    uint16_t explosion_danger_ticks;
    uint8_t bomb_radius;
    uint16_t bomb_timer_ticks;
    uint8_t cells[MAX_MAP_CELLS];
} game_map_t;

size_t map_cell_count(const game_map_t* map);
int map_load_from_file(const char* path, game_map_t* map);
void map_print(const game_map_t* map);

#endif