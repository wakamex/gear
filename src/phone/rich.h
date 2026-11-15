// A reply's text as the phone shows it: paragraphs wrapped, and Markdown tables laid out as tables,
// their columns as wide as their contents need and no wider.
#pragma once
#include <stdbool.h>
#include "gs_ui.h"

typedef struct {
    float px, small;             // the text's size, and the smallest a table may shrink to
    SDL_FColor text, head, rule, shade;  // the text, a table's header, its lines, and every other row
} gp_rich_style;

// Draws `text` from the top of `at` across its width (when `draw`) and returns its height.
float gp_rich(gs_ui *ui, gs_rect at, const char *text, const gp_rich_style *s, bool draw);
