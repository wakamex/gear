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

// Tables at their full size: a click on a table drawn by gp_rich gives its Markdown to
// gp_rich_take_table (once; SDL_free it), and gp_rich_table_view draws that table in `area` with its
// columns as wide as their contents on one line, moved by *x and *y across and down.
char *gp_rich_take_table(void);
void gp_rich_table_view(gs_ui *ui, gs_rect area, const char *md, const gp_rich_style *s, float *x, float *y);

// Selecting and links: a finger held still on a message's text for half a second and dragged selects
// (across messages too), and a tap on a web address opens it. The texts of a frame are drawn between
// gp_rich_begin and gp_rich_end; gp_rich_selection is what is selected in them, in order (NULL for
// nothing), those scrolled away and not drawn now included, and gp_rich_unselect drops it. Before
// drawing each message, gp_rich_order gives its place in the conversation (any number that grows
// down the page), which orders what is selected in it among the rest.
void gp_rich_order(Uint64 place);
void gp_rich_begin(gs_ui *ui);
void gp_rich_end(gs_ui *ui);
const char *gp_rich_selection(void);
bool gp_rich_began(void);  // whether a new selection began in the messages this frame
void gp_rich_unselect(void);
