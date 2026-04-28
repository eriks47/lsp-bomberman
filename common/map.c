#include "map.h"

#include <stdio.h>
#include <string.h>

size_t map_cell_count(const game_map_t* map) {
    return (size_t)map->rows * (size_t)map->cols;
}

static uint8_t normalize_cell(char cell) {
    return (uint8_t)cell;
}

int map_load_from_file(const char* path, game_map_t* map) {
    FILE* file = fopen(path, "r");

    if (file == NULL) {
        perror("fopen map");
        return -1;
    }

    memset(map, 0, sizeof(*map));

    unsigned int rows;
    unsigned int cols;
    unsigned int player_speed;
    unsigned int explosion_danger_ticks;
    unsigned int bomb_radius;
    unsigned int bomb_timer_ticks;

    int header_items = fscanf(file,
                              "%u %u %u %u %u %u",
                              &rows,
                              &cols,
                              &player_speed,
                              &explosion_danger_ticks,
                              &bomb_radius,
                              &bomb_timer_ticks);

    if (header_items != 6) {
        fprintf(stderr, "Invalid map header in %s\n", path);
        fclose(file);
        return -1;
    }

    if (rows == 0 || rows > MAX_MAP_ROWS || cols == 0 || cols > MAX_MAP_COLS) {
        fprintf(stderr, "Invalid map size %u x %u\n", rows, cols);
        fclose(file);
        return -1;
    }

    map->rows = (uint8_t)rows;
    map->cols = (uint8_t)cols;
    map->player_speed = (uint16_t)player_speed;
    map->explosion_danger_ticks = (uint16_t)explosion_danger_ticks;
    map->bomb_radius = (uint8_t)bomb_radius;
    map->bomb_timer_ticks = (uint16_t)bomb_timer_ticks;

    size_t cells = map_cell_count(map);

    for (size_t i = 0; i < cells; ++i) {
        char token[16];

        if (fscanf(file, "%15s", token) != 1) {
            fprintf(stderr, "Map has fewer cells than expected\n");
            fclose(file);
            return -1;
        }

        map->cells[i] = normalize_cell(token[0]);
    }

    fclose(file);
    return 0;
}

void map_print(const game_map_t* map) {
    printf("Map %u x %u\n", map->rows, map->cols);

    for (uint8_t row = 0; row < map->rows; ++row) {
        for (uint8_t col = 0; col < map->cols; ++col) {
            size_t index = (size_t)row * map->cols + col;
            printf("%c ", (char)map->cells[index]);
        }

        printf("\n");
    }
}