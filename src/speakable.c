#include "speakable.h"
#include <SDL3/SDL.h>
#include <string.h>

typedef struct { char *s; size_t len, cap; } buf;

static void put(buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
        b->s = SDL_realloc(b->s, b->cap);
    }
    SDL_memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
static void puts_(buf *b, const char *s) { put(b, s, SDL_strlen(s)); }

static void trim_end(buf *b) {
    while (b->len && (b->s[b->len - 1] == ' ' || b->s[b->len - 1] == '\n')) b->s[--b->len] = 0;
}

// Ends the text so far as a sentence, so a heading, list item or paragraph is not run into the next.
static void end_sentence(buf *b) {
    trim_end(b);
    if (!b->len) return;
    char last = b->s[b->len - 1];
    if (!SDL_strchr(".!?:;", last)) puts_(b, ".");
    puts_(b, " ");
}

static bool word_char(char c) { return SDL_isalnum((unsigned char)c) || (unsigned char)c >= 0x80; }

// One whitespace-free token, made speakable: an address read as its site, a path as its file's
// name, snake_case as words, an arrow as "to".
static void token(buf *b, const char *t, size_t n) {
    // Punctuation that ends a sentence or clause stays outside the token.
    size_t tail = n;
    while (tail && SDL_strchr(".,;:!?)\"'", t[tail - 1])) tail--;
    size_t head = 0;
    while (head < tail && SDL_strchr("(\"'", t[head])) head++;
    put(b, t, head);
    const char *w = t + head;
    size_t m = tail - head;
    char word[1024];
    if (m >= sizeof word) { put(b, t + head, n - head); return; }
    SDL_memcpy(word, w, m);
    word[m] = 0;
    const char *scheme = SDL_strstr(word, "://");
    if (scheme && (!SDL_strncmp(word, "http", 4))) {
        const char *host = scheme + 3;
        if (!SDL_strncmp(host, "www.", 4)) host += 4;
        size_t h = strcspn(host, "/:?#");
        puts_(b, "a ");
        put(b, host, h);
        puts_(b, " link");
    } else if (!SDL_strcmp(word, "->") || !SDL_strcmp(word, "=>")) {
        puts_(b, "to");
    } else {
        // A path: two or more separators, or one that starts it, read as its last part.
        int slashes = 0;
        for (const char *p = word; *p; p++) slashes += *p == '/' || *p == '\\';
        // One separator is a path when it starts the word or the last part looks like a file
        // (src/listen.c), not when it joins words (and/or, 1/2).
        const char *last = SDL_strrchr(word, '/');
        const char *dot = last ? SDL_strrchr(last, '.') : NULL;
        bool file = dot && dot > last + 1 && SDL_isalpha((unsigned char)dot[1]);
        bool path = slashes >= 2 || (slashes == 1 && (word[0] == '/' || word[0] == '~' || word[0] == '.' || file));
        const char *name = word;
        if (path) {
            const char *a = SDL_strrchr(word, '/'), *c = SDL_strrchr(word, '\\');
            const char *cut = a > c ? a : c;
            if (cut && cut[1]) name = cut + 1;
        }
        // snake_case and kebab-free identifiers: underscores between letters read as spaces.
        for (const char *p = name; *p; p++) {
            char c = *p;
            if (c == '_' && p > name && word_char(p[-1]) && word_char(p[1])) c = ' ';
            put(b, &c, 1);
        }
    }
    put(b, t + tail, n - tail);
}

// Inline Markdown dropped from one line of text: code spans keep their content, links their text,
// emphasis marks go. Then each word goes through token().
static void inline_text(buf *out, const char *s) {
    buf plain = { 0 };
    for (const char *p = s; *p;) {
        if (*p == '`') {
            size_t ticks = strspn(p, "`");
            const char *close = SDL_strstr(p + ticks, ticks == 1 ? "`" : ticks == 2 ? "``" : "```");
            if (close) { put(&plain, p + ticks, (size_t)(close - p - ticks)); p = close + ticks; continue; }
            p += ticks;
            continue;
        }
        if (*p == '!' && p[1] == '[') { p++; continue; }  // an image reads as its alt text
        if (*p == '[') {
            const char *close = SDL_strstr(p, "](");
            const char *end = close ? SDL_strchr(close, ')') : NULL;
            if (close && end && !memchr(p + 1, '\n', (size_t)(close - p))) {
                char text[512];
                size_t n = SDL_min((size_t)(close - p - 1), sizeof text - 1);
                SDL_memcpy(text, p + 1, n);
                text[n] = 0;
                puts_(&plain, text);
                p = end + 1;
                continue;
            }
        }
        if (*p == '*' || (*p == '_' && (p[1] == '_' || p == s || !word_char(p[-1]) || !word_char(p[1])))) {
            // Emphasis marks; an underscore inside a word is part of an identifier and stays.
            p += (p[1] == *p) ? 2 : 1;
            continue;
        }
        if (*p == '<') {  // HTML tags and autolinks: an autolink keeps its address, a tag goes
            const char *end = SDL_strchr(p, '>');
            if (end && (SDL_strncmp(p + 1, "http", 4) == 0)) { put(&plain, p + 1, (size_t)(end - p - 1)); p = end + 1; continue; }
            if (end && (SDL_isalpha((unsigned char)p[1]) || p[1] == '/')) { p = end + 1; continue; }
        }
        put(&plain, p, 1);
        p++;
    }
    for (const char *p = plain.s ? plain.s : ""; *p;) {
        if (*p == ' ' || *p == '\t') { if (out->len && out->s[out->len - 1] != ' ') puts_(out, " "); p++; continue; }
        size_t n = strcspn(p, " \t");
        token(out, p, n);
        p += n;
    }
    SDL_free(plain.s);
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool table_rule(const char *s) {  // |---|:--:|
    s = skip_spaces(s);
    if (*s != '|' && *s != '-' && *s != ':') return false;
    for (; *s; s++) if (!SDL_strchr("|-: \t", *s)) return false;
    return true;
}

static bool thematic_break(const char *s) {
    s = skip_spaces(s);
    char c = *s;
    if (c != '-' && c != '*' && c != '_') return false;
    int n = 0;
    for (; *s; s++) {
        if (*s == c) n++;
        else if (*s != ' ') return false;
    }
    return n >= 3;
}

static void counted(buf *b, const char *what, int n, const char *unit) {
    char text[128];
    if (n == 1) SDL_snprintf(text, sizeof text, "A one-%s %s", unit, what);
    else SDL_snprintf(text, sizeof text, "A %s of %d %ss", what, n, unit);
    end_sentence(b);
    puts_(b, text);
    end_sentence(b);
}

char *gear_speakable(const char *reply, size_t limit) {
    buf b = { 0 };
    puts_(&b, "");
    char *copy = SDL_strdup(reply ? reply : "");
    // Line by line: fences and tables are counted, headings and list items become sentences, and the
    // lines of a paragraph join with spaces.
    char fence[4] = "";
    int code_lines = 0, table_rows = 0;
    bool table_has_rule = false;
    for (char *l = copy, *next; l; l = next) {
        next = SDL_strchr(l, '\n');
        if (next) *next++ = 0;
        size_t len = SDL_strlen(l);
        if (len && l[len - 1] == '\r') l[--len] = 0;
        const char *t = skip_spaces(l);
        if (fence[0]) {
            if (!SDL_strncmp(t, fence, 3)) { counted(&b, "code block", code_lines, "line"); fence[0] = 0; }
            else code_lines++;
            continue;
        }
        if (!SDL_strncmp(t, "```", 3) || !SDL_strncmp(t, "~~~", 3)) { SDL_memcpy(fence, t, 3); fence[3] = 0; code_lines = 0; continue; }
        if (*t == '|') {
            if (table_rule(t)) table_has_rule = true;
            else table_rows++;
            continue;
        }
        if (table_rows) {
            // The header row is not counted as a row of the table once a rule follows it.
            counted(&b, "table", table_has_rule ? table_rows - 1 : table_rows, "row");
            table_rows = 0;
            table_has_rule = false;
        }
        if (!*t) { end_sentence(&b); continue; }
        if (thematic_break(t)) { end_sentence(&b); continue; }
        while (*t == '>') t = skip_spaces(t + 1);
        if (*t == '#') {
            while (*t == '#') t++;
            end_sentence(&b);
            inline_text(&b, skip_spaces(t));
            end_sentence(&b);
            continue;
        }
        const char *item = NULL;
        if ((*t == '-' || *t == '*' || *t == '+') && t[1] == ' ') item = t + 2;
        else if (SDL_isdigit((unsigned char)*t)) {
            const char *d = t;
            while (SDL_isdigit((unsigned char)*d)) d++;
            if ((*d == '.' || *d == ')') && d[1] == ' ') item = d + 2;
        }
        if (item) {
            if (!SDL_strncmp(item, "[ ] ", 4) || !SDL_strncmp(item, "[x] ", 4)) item += 4;  // task boxes
            end_sentence(&b);
            inline_text(&b, skip_spaces(item));
            end_sentence(&b);
            continue;
        }
        if (b.len && b.s[b.len - 1] != ' ') puts_(&b, " ");
        inline_text(&b, t);
    }
    if (fence[0]) counted(&b, "code block", code_lines, "line");  // a fence never closed
    if (table_rows) counted(&b, "table", table_has_rule ? table_rows - 1 : table_rows, "row");
    end_sentence(&b);
    trim_end(&b);
    SDL_free(copy);
    // "!." and "?." where a quoted or coded sentence ends inside the sentence around it.
    size_t o = 0;
    for (size_t i = 0; i < b.len; i++)
        if (!(b.s[i] == '.' && i && (b.s[i - 1] == '!' || b.s[i - 1] == '?'))) b.s[o++] = b.s[i];
    if (b.s) b.s[b.len = o] = 0;
    // Too long to listen to: cut at the last sentence that fits.
    if (limit && b.len > limit) {
        size_t cut = 0;
        for (size_t i = 0; i + 1 < limit && i + 1 < b.len; i++)
            if (SDL_strchr(".!?", b.s[i]) && b.s[i + 1] == ' ') cut = i + 1;
        if (!cut) cut = limit;
        b.len = cut;
        b.s[cut] = 0;
        trim_end(&b);
        puts_(&b, " The rest is not read aloud.");
    }
    return b.s;
}
