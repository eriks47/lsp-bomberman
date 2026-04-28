#ifndef INPUT_FIELD_H
#define INPUT_FIELD_H

#include <raylib.h>
#include <stdio.h>

typedef struct {
    char buf[256];
    int len;
    bool active;
    Rectangle bounds;
} input_field_t;

void draw_input_field(input_field_t* f, const char* label);

#endif  // INPUT_FIELD_H
