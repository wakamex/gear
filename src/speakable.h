// An agent's reply, usually Markdown, turned into what is worth hearing. Code blocks and tables are
// named rather than read ("a code block of 12 lines"), Markdown's marks are dropped, links are read
// as their text, bare addresses as their site, and paths as their file's name. List items and
// headings become sentences. A reply longer than `limit` characters once turned is cut at a sentence
// with a note that the rest is not read. The full reply is still shown; this is only what is said.
#pragma once
#include <stddef.h>

char *gear_speakable(const char *reply, size_t limit);  // SDL_free it
