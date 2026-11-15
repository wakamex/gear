#include "rich.h"
#include <SDL3/SDL.h>

enum { ROWS = 64, COLS = 12, PIECES = 32, KEPT = 24 };
static const float PAD = 4, VPAD = 2;  // inside each cell, across and down

typedef struct {
    int rows, cols;
    char *buf;                   // the cells' text, each zero-terminated
    const char *cell[ROWS][COLS];
    bool number[COLS];           // every cell under the header is a number: set to the right
    float font, colw[COLS], rowh[ROWS], line;
} table;

typedef struct { char *text; table *t; float height; int from, to; } piece;  // a paragraph or a table, and its bytes in the text

// A text's layout at one width, kept for the frames after (KEPT of them, the oldest replaced).
typedef struct { Uint64 hash; float width, px; int n; piece p[PIECES]; float height; } layout;
static layout kept[KEPT];
static int next_kept;

static Uint64 hash_of(const char *s) {
    Uint64 h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (Uint8)*s) * 1099511628211ull;
    return h;
}

static bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// The line starting at `s`: its end (at the newline or the text's end).
static const char *line_end(const char *s) {
    const char *e = SDL_strchr(s, '\n');
    return e ? e : s + SDL_strlen(s);
}

static bool table_row(const char *s, const char *e) {
    while (s < e && blank(*s)) s++;
    return s < e && *s == '|';
}

// "|---|:--:|" and the like: the line under a table's header.
static bool table_rule(const char *s, const char *e) {
    bool dash = false;
    for (; s < e; s++) {
        if (*s == '-') dash = true;
        else if (*s != '|' && *s != ':' && !blank(*s)) return false;
    }
    return dash;
}

static bool numeric(const char *s) {
    bool digit = false;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') digit = true;
        else if (!SDL_strchr(".,%+-$~x ", *s)) return false;
    }
    return digit;
}

// The table in lines [s, e): its cells, with backticks and asterisks taken out; the rule line skipped.
static table *parse_table(const char *s, const char *e) {
    table *t = SDL_calloc(1, sizeof *t);
    t->buf = SDL_malloc((size_t)(e - s) + 1);
    char *out = t->buf;
    for (const char *line = s; line < e && t->rows < ROWS;) {
        const char *end = line_end(line);
        if (end > e) end = e;
        if (!(t->rows == 1 && table_rule(line, end))) {
            const char *c = line;
            while (c < end && blank(*c)) c++;
            if (c < end && *c == '|') c++;
            const char *stop = end;
            while (stop > c && blank(stop[-1])) stop--;
            if (stop > c && stop[-1] == '|') stop--;
            int col = 0;
            while (c <= stop && col < COLS) {
                const char *bar = c;
                while (bar < stop && *bar != '|') bar++;
                while (c < bar && blank(*c)) c++;
                const char *last = bar;
                while (last > c && blank(last[-1])) last--;
                t->cell[t->rows][col++] = out;
                for (; c < last; c++)
                    if (*c != '`' && *c != '*') *out++ = *c;
                *out++ = 0;
                c = bar + 1;
            }
            for (; col < COLS; col++) t->cell[t->rows][col] = "";
            if (col > t->cols) t->cols = SDL_min(col, COLS);
            t->rows++;
        }
        line = *end ? end + 1 : end;
    }
    // (a row's columns counted as it went; the table has as many as its widest row)
    t->cols = 0;
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < COLS; c++)
            if (*t->cell[r][c] && c + 1 > t->cols) t->cols = c + 1;
    for (int c = 0; c < t->cols; c++) {
        t->number[c] = t->rows > 1;
        for (int r = 1; r < t->rows; r++)
            if (*t->cell[r][c] && !numeric(t->cell[r][c])) t->number[c] = false;
    }
    return t;
}

// The widest word of a cell at `font`, and its whole width on one line.
static void measure(gs_ui *ui, const char *cell, float font, float *word, float *whole) {
    char w[256];
    *word = 0, *whole = gs_ui_text_width(ui, cell, font);
    for (const char *s = cell; *s;) {
        while (*s == ' ') s++;
        size_t n = 0;
        while (s[n] && s[n] != ' ' && n < sizeof w - 1) n++;
        if (!n) break;
        SDL_memcpy(w, s, n), w[n] = 0;
        float x = gs_ui_text_width(ui, w, font);
        if (x > *word) *word = x;
        s += n;
    }
}

// Column widths for `width`: as wide as their cells, when they fit; otherwise each at least its widest
// word, the rest shared by how much more each needs, at the largest size from px down to `small` at
// which the words fit.
static float lay_table(gs_ui *ui, table *t, float width, float px, float small) {
    float least[COLS], most[COLS];
    for (float font = px;; font = SDL_max(small, font * 0.9f)) {
        float need = 0, full = 0;
        for (int c = 0; c < t->cols; c++) {
            least[c] = most[c] = 0;
            for (int r = 0; r < t->rows; r++) {
                float word, whole;
                measure(ui, t->cell[r][c], font, &word, &whole);
                least[c] = SDL_max(least[c], word), most[c] = SDL_max(most[c], whole);
            }
            least[c] += 2 * PAD + 1, most[c] += 2 * PAD + 1;  // (and a line between columns)
            need += least[c], full += most[c];
        }
        t->font = font;
        if (full <= width) {
            for (int c = 0; c < t->cols; c++) t->colw[c] = most[c];
            break;
        }
        if (need <= width || font <= small) {
            float spare = SDL_max(0, width - need), wanted = full - need;
            for (int c = 0; c < t->cols; c++) t->colw[c] = least[c] + (wanted > 0 ? spare * (most[c] - least[c]) / wanted : 0);
            break;
        }
    }
    t->line = gs_ui_text_height(ui, "0", t->font, 1e6f);
    float h = 0;
    for (int r = 0; r < t->rows; r++) {
        t->rowh[r] = t->line;
        for (int c = 0; c < t->cols; c++)
            t->rowh[r] = SDL_max(t->rowh[r], gs_ui_text_height(ui, t->cell[r][c], t->font, t->colw[c] - 2 * PAD - 1));
        h += t->rowh[r] + 2 * VPAD;
    }
    return h + 1;  // (and the line under the header)
}

static void draw_table(gs_ui *ui, const table *t, float x, float y, const gp_rich_style *s) {
    float w = 0;
    for (int c = 0; c < t->cols; c++) w += t->colw[c];
    float top = y;
    for (int r = 0; r < t->rows; r++) {
        float h = t->rowh[r] + 2 * VPAD;
        if (r > 0 && r % 2 == 0) gs_ui_fill(ui, (gs_rect){ x, y, w, h }, s->shade);
        float cx = x;
        for (int c = 0; c < t->cols; c++) {
            float inner = t->colw[c] - 2 * PAD - 1;
            gs_rect cell = { cx + PAD, y + VPAD, inner, t->rowh[r] };
            SDL_FColor col = r == 0 ? s->head : s->text;
            if (t->number[c] && gs_ui_text_width(ui, t->cell[r][c], t->font) <= inner)
                gs_ui_text(ui, (gs_rect){ cell.x, cell.y, inner, t->line }, t->cell[r][c], t->font, col, 1);
            else gs_ui_text_wrapped(ui, cell, t->cell[r][c], t->font, col);
            cx += t->colw[c];
        }
        y += h;
        if (r == 0) gs_ui_fill(ui, (gs_rect){ x, y, w, 1 }, s->rule), y += 1;
    }
    float cx = x;
    for (int c = 0; c + 1 < t->cols; c++) cx += t->colw[c], gs_ui_fill(ui, (gs_rect){ cx - 1, top, 1, y - top }, s->rule);
}

static void forget(layout *l) {
    for (int i = 0; i < l->n; i++) {
        SDL_free(l->p[i].text);
        if (l->p[i].t) SDL_free(l->p[i].t->buf), SDL_free(l->p[i].t);
    }
    SDL_zerop(l);
}

static void add_text(layout *l, const char *text, const char *s, const char *e) {
    while (s < e && (*s == '\n' || blank(*s))) s++;
    while (e > s && (e[-1] == '\n' || blank(e[-1]))) e--;
    if (s == e || l->n == PIECES) return;
    char *copy = SDL_malloc((size_t)(e - s) + 1);
    SDL_memcpy(copy, s, (size_t)(e - s)), copy[e - s] = 0;
    l->p[l->n++] = (piece){ copy, NULL, 0, (int)(s - text), (int)(e - text) };
}

static layout *lay_out(gs_ui *ui, const char *text, float width, const gp_rich_style *st) {
    Uint64 h = hash_of(text);
    for (int i = 0; i < KEPT; i++)
        if (kept[i].hash == h && kept[i].width == width && kept[i].px == st->px) return &kept[i];
    layout *l = &kept[next_kept];
    next_kept = (next_kept + 1) % KEPT;
    forget(l);
    l->hash = h, l->width = width, l->px = st->px;
    const char *run = text;
    for (const char *s = text; *s;) {
        const char *e = line_end(s);
        const char *next = *e ? e + 1 : e;
        if (table_row(s, e) && *next && table_rule(next, line_end(next)) && l->n < PIECES - 1) {
            add_text(l, text, run, s);
            const char *end = next;
            while (*end && table_row(end, line_end(end))) end = *line_end(end) ? line_end(end) + 1 : line_end(end);
            l->p[l->n++] = (piece){ NULL, parse_table(s, end), 0, (int)(s - text), (int)(end - text) };
            s = run = end;
            continue;
        }
        s = next;
    }
    add_text(l, text, run, text + SDL_strlen(text));
    float gap = st->px * 0.5f;
    for (int i = 0; i < l->n; i++) {
        piece *p = &l->p[i];
        p->height = p->t ? lay_table(ui, p->t, width, st->px, st->small) : gs_ui_text_height(ui, p->text, st->px, width);
        l->height += p->height + (i ? gap : 0);
    }
    return l;
}

float gp_rich(gs_ui *ui, gs_rect at, const char *text, const gp_rich_style *s, bool draw) {
    if (!draw) return lay_out(ui, text, at.w, s)->height;
    return gp_rich_marked(ui, at, text, s, 0, 0, NULL);
}

float gp_rich_marked(gs_ui *ui, gs_rect at, const char *text, const gp_rich_style *s, int start, int end, float *mark_y) {
    layout *l = lay_out(ui, text, at.w, s);
    float y = at.y, gap = s->px * 0.5f;
    bool placed = false;
    for (int i = 0; i < l->n; i++) {
        const piece *p = &l->p[i];
        int a = SDL_max(start, p->from), b = SDL_min(end, p->to);  // what of the mark is in this piece
        if (p->t) {
            if (a < b) {
                float w = 0;
                for (int c = 0; c < p->t->cols; c++) w += p->t->colw[c];
                gs_ui_round(ui, (gs_rect){ at.x - 4, y - 2, w + 8, p->height + 4 }, 4, s->mark);
                if (mark_y && !placed) *mark_y = y, placed = true;
            }
            draw_table(ui, p->t, at.x, y, s);
        } else if (a < b) {
            // (bytes of the piece's own copy, which begins at p->from)
            gs_ui_text_span span = { a - p->from, b - p->from, 0, s->marked, s->mark, false };
            int from = 0, to = 0, clicked;
            gs_ui_text_spans(ui, (gs_rect){ at.x, y, at.w, p->height }, "spoken", p->text, s->px, s->text, NULL, 0, &span, 1, &from, &to, &clicked);
            if (mark_y && !placed) {
                // The line the mark starts on: below the text before it, less a line when that text
                // runs on into the line.
                char *before = SDL_strndup(p->text, (size_t)span.start);
                float h = *before ? gs_ui_text_height(ui, before, s->px, at.w) : 0, line = gs_ui_text_height(ui, "0", s->px, 1e6f);
                *mark_y = y + SDL_max(0, h - line), placed = true;
                SDL_free(before);
            }
        } else gs_ui_text_wrapped(ui, (gs_rect){ at.x, y, at.w, p->height }, p->text, s->px, s->text);
        y += p->height + gap;
    }
    return l->height;
}
