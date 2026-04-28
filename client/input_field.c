#include "input_field.h"

void draw_input_field(input_field_t* f, const char* label) {
    Rectangle bounds = f->bounds;
    DrawText(label, (int)bounds.x, (int)bounds.y - 22, 20, WHITE);
    DrawRectangleRec(bounds, f->active ? LIGHTGRAY : WHITE);
    DrawRectangleLinesEx(bounds, 2, f->active ? DARKBLUE : DARKGRAY);
    DrawText(f->buf, (int)bounds.x + 6, (int)bounds.y + 7, 20, BLACK);

    if (f->active && ((int)(GetTime() * 2) % 2 == 0)) {
        int cx = (int)bounds.x + 6 + MeasureText(f->buf, 20);
        DrawLine(cx, (int)bounds.y + 5, cx, (int)bounds.y + 27, BLACK);
    }
}
