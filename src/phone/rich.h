// A reply's text as the phone shows it: paragraphs wrapped, and Markdown tables laid out as tables,
// their columns as wide as their contents need and no wider.
#pragma once
#include <stdbool.h>
#include "gs_ui.h"

typedef struct {
    float px, small;             // the text's size, and the smallest a table may shrink to
    SDL_FColor text, head, rule, shade;  // the text, a table's header, its lines, and every other row
    SDL_FColor marked, mark;             // marked text, and the tint behind it
} gp_rich_style;

// Draws `text` from the top of `at` across its width (when `draw`) and returns its height.
float gp_rich(gs_ui *ui, gs_rect at, const char *text, const gp_rich_style *s, bool draw);
// The same with the bytes [start, end) of `text` marked, as the sentence being heard is: their
// words brighter on a tint (a table's whole rows tinted). *mark_y gets the top of the line the mark
// starts on, or is left alone when nothing is marked.
float gp_rich_marked(gs_ui *ui, gs_rect at, const char *text, const gp_rich_style *s, int start, int end, float *mark_y);
