// An agent's reply, usually Markdown, turned into what is worth hearing. Code blocks and tables are
// named rather than read ("a code block of 12 lines"), Markdown's marks are dropped, links are read
// as their text, bare addresses as their site, and paths as their file's name. List items and
// headings become sentences. The full reply is still shown; this is only what is said.
#pragma once
#include <stddef.h>

char *gear_speakable(const char *reply);
// The same, and for each byte of what is said, the place in `reply` it was read from, in *from (one
// more than the text's length, the last being the reply's end; SDL_free it): a word's first byte for
// a word, the start of a code block or table for its count.
char *gear_speakable_map(const char *reply, int **from);  // SDL_free it
