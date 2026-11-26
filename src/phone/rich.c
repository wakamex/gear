#include "rich.h"
#include <SDL3/SDL.h>

enum { ROWS = 64, COLS = 12, PIECES = 32, KEPT = 24 };
static const float PAD = 4, VPAD = 2;  // inside each cell, across and down

typedef struct {
    int rows, cols;
    char *buf;                   // the cells' text, each zero-terminated
    const char *cell[ROWS][COLS];
    int align[COLS];             // -1 left, 0 centre, 1 right: as the rule line's colons say, or else to
                                 // the right when every cell under the header is a number
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
    int ruled[COLS];  // the rule line's alignment of each column, 2 for none given
    for (int c = 0; c < COLS; c++) ruled[c] = 2;
    t->buf = SDL_malloc((size_t)(e - s) + 1);
    char *out = t->buf;
    for (const char *line = s; line < e && t->rows < ROWS;) {
        const char *end = line_end(line);
        if (end > e) end = e;
        if (t->rows == 1 && table_rule(line, end)) {
            // (its colons: ":--" left, ":-:" centre, "--:" right, one column between each pair of bars)
            const char *c = line;
            while (c < end && blank(*c)) c++;
            if (c < end && *c == '|') c++;
            for (int col = 0; c < end && col < COLS; col++) {
                const char *bar = c;
                while (bar < end && *bar != '|') bar++;
                const char *a = c, *b = bar;
                while (a < b && blank(*a)) a++;
                while (b > a && blank(b[-1])) b--;
                if (b > a) ruled[col] = b[-1] == ':' ? (*a == ':' ? 0 : 1) : (*a == ':' ? -1 : 2);
                c = bar + 1;
            }
        } else {
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
        bool number = t->rows > 1;
        for (int r = 1; r < t->rows; r++)
            if (*t->cell[r][c] && !numeric(t->cell[r][c])) number = false;
        t->align[c] = ruled[c] != 2 ? ruled[c] : number ? 1 : -1;
    }
    return t;
}

// The lines `cell` wraps to at `width`, a word at a time (inside a word wider than the line): each
// one's start and length, for drawing it aligned; returns how many (at most `max`).
static int wrap(gs_ui *ui, const char *cell, float font, float width, const char **start, int *len, int max) {
    int n = 0;
    const char *s = cell;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        const char *fit = NULL, *p = s;
        char buf[512];
        while (*p) {
            const char *q = p;
            while (*q == ' ') q++;
            while (*q && *q != ' ') q++;
            size_t l = SDL_min((size_t)(q - s), sizeof buf - 1);
            SDL_memcpy(buf, s, l), buf[l] = 0;
            if (gs_ui_text_width(ui, buf, font) > width) break;
            fit = q, p = q;
        }
        if (!fit) {  // (a word wider than the line: as many of its characters as fit)
            fit = s;
            for (const char *q = s + 1; *q && *q != ' '; q++) {
                if ((*q & 0xC0) == 0x80) continue;
                size_t l = SDL_min((size_t)(q - s), sizeof buf - 1);
                SDL_memcpy(buf, s, l), buf[l] = 0;
                if (gs_ui_text_width(ui, buf, font) > width) break;
                fit = q;
            }
            if (fit == s) { fit = s + 1; while ((*fit & 0xC0) == 0x80) fit++; }
        }
        start[n] = s, len[n] = (int)(fit - s), n++;
        s = fit;
    }
    return n;
}

// A cell's height at `width`: wrapped by gs_ui, or by `wrap` when it is aligned other than left.
static float cell_height(gs_ui *ui, const table *t, int r, int c, float width) {
    if (t->align[c] < 0) return gs_ui_text_height(ui, t->cell[r][c], t->font, width);
    const char *start[64];
    int len[64];
    return SDL_max(1, wrap(ui, t->cell[r][c], t->font, width, start, len, 64)) * t->line;
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
            t->rowh[r] = SDL_max(t->rowh[r], cell_height(ui, t, r, c, t->colw[c] - 2 * PAD - 1));
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
            // (a column aligned other than left, its header too, a line at a time as `wrap` breaks it)
            if (t->align[c] >= 0) {
                const char *start[64];
                int len[64], n = wrap(ui, t->cell[r][c], t->font, inner, start, len, 64);
                for (int k = 0; k < n; k++) {
                    char line[512];
                    SDL_snprintf(line, sizeof line, "%.*s", len[k], start[k]);
                    gs_ui_text(ui, (gs_rect){ cell.x, cell.y + k * t->line, inner, t->line }, line, t->font, col, t->align[c]);
                }
            } else gs_ui_text_wrapped(ui, cell, t->cell[r][c], t->font, col);
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

// ---- Selecting and links ----

// What is selected in the paragraphs pressed (by id: the text's address and the piece's place), kept
// between frames with the text chosen in each and its place on the page; the group spans every
// message, so a selection can run from one into the next, past those no longer drawn. A paragraph
// never pressed or selected keeps no place here.
typedef struct { char id[40]; int from, to; Uint64 frame, place; char *chosen; } picked;
static picked *picks;
static int pick_count, pick_cap;
static Uint64 frame, message_place;
static char *selection;  // what is selected, in order down the page, or NULL
static bool began;       // a new selection began in the messages this frame

static picked *find_pick(const char *id) {
    for (int i = 0; i < pick_count; i++)
        if (!SDL_strcmp(picks[i].id, id)) return &picks[i];
    return NULL;
}

static picked *new_pick(const char *id) {
    if (pick_count == pick_cap) picks = SDL_realloc(picks, (size_t)(pick_cap = pick_cap ? pick_cap * 2 : 64) * sizeof *picks);
    picked *p = &picks[pick_count++];
    SDL_zerop(p);
    SDL_strlcpy(p->id, id, sizeof p->id);
    return p;
}

static void drop_pick(int i) { SDL_free(picks[i].chosen), picks[i] = picks[--pick_count]; }

void gp_rich_order(Uint64 place) { message_place = place; }

void gp_rich_begin(gs_ui *ui) {
    frame++;
    gs_ui_select_group(ui, "messages");
}

static int by_place(const void *a, const void *b) {
    Uint64 x = ((const picked *)a)->place, y = ((const picked *)b)->place;
    return x < y ? -1 : x > y;
}

void gp_rich_end(gs_ui *ui) {
    gs_ui_select_group(ui, NULL);
    // A new selection began: the texts not drawn this frame give up theirs.
    began = gs_ui_select_began(ui, "messages");
    if (began)
        for (int i = pick_count - 1; i >= 0; i--)
            if (picks[i].frame != frame) drop_pick(i);
    // What is selected, each paragraph's on a line of its own, in order down the page.
    SDL_qsort(picks, (size_t)pick_count, sizeof *picks, by_place);
    size_t n = 0;
    for (int i = 0; i < pick_count; i++) n += picks[i].chosen ? SDL_strlen(picks[i].chosen) + 1 : 0;
    SDL_free(selection), selection = NULL;
    if (!n) return;
    selection = SDL_malloc(n), *selection = 0;
    for (int i = 0; i < pick_count; i++)
        if (picks[i].chosen) {
            if (*selection) SDL_strlcat(selection, "\n", n);
            SDL_strlcat(selection, picks[i].chosen, n);
        }
}

const char *gp_rich_selection(void) { return selection; }

bool gp_rich_began(void) { return began; }

void gp_rich_unselect(void) {
    while (pick_count) drop_pick(pick_count - 1);
    SDL_free(selection), selection = NULL;
}

// The pair a paragraph was given, kept with the text it chooses (none when nothing is).
static void keep_selected(picked *p, const char *text, int from, int to, int piece) {
    p->from = from, p->to = to, p->frame = frame, p->place = message_place << 20 | (Uint64)piece;
    int lo = SDL_min(from, to), hi = SDL_max(from, to);
    SDL_free(p->chosen), p->chosen = lo < hi ? SDL_strndup(text + lo, (size_t)(hi - lo)) : NULL;
}

// A paragraph, selectable, its web addresses links (a tap opens one), and the bytes [a, b) of it
// marked as the sentence heard (from - 1 for none).
static void paragraph(gs_ui *ui, gs_rect r, const char *id, int piece, const char *text, const gp_rich_style *s, int a, int b) {
    enum { MOST = 32 };
    gs_ui_styled links[MOST];
    gs_ui_text_span spans[2 * MOST + 4];
    int link_of[2 * MOST + 4];
    int nl = gs_ui_find_links(text, links, MOST);
    int n = nl ? gs_ui_styled_spans(ui, text, links, nl, spans, 2 * MOST, link_of) : 0;
    // (a link over the mark keeps its colour, the first of two that overlap counting)
    if (a < b && n < (int)SDL_arraysize(spans)) spans[n] = (gs_ui_text_span){ a, b, 0, s->marked, s->mark, false }, link_of[n++] = -1;
    picked *p = find_pick(id);
    int from = p ? p->from : 0, to = p ? p->to : 0, clicked = -1;
    gs_ui_text_spans(ui, r, id, text, s->px, s->text, NULL, 0, spans, n, &from, &to, &clicked);
    // (a place once gs_ui has set the pair, a press, where a drag keeps its start; until a new
    // selection begins elsewhere, or the oldest gives way)
    if (p || from || to) keep_selected(p ? p : new_pick(id), text, from, to, piece);
    if (clicked >= 0 && link_of[clicked] >= 0) {
        const gs_ui_styled *l = &links[link_of[clicked]];
        char url[1024];
        bool www = !SDL_strncmp(text + l->start, "www.", 4);
        SDL_snprintf(url, sizeof url, "%s%.*s", www ? "https://" : "", l->end - l->start, text + l->start);
        SDL_OpenURL(url);
    }
}

// The Markdown of the table last clicked, for gp_rich_take_table.
static char *clicked_table;

char *gp_rich_take_table(void) {
    char *t = clicked_table;
    clicked_table = NULL;
    return t;
}

// The table of Markdown `md` at its full size, its columns as wide as their words on one line and its
// text at the style's size, drawn in `area` moved by *x and *y (kept within the table's edges).
void gp_rich_table_view(gs_ui *ui, gs_rect area, const char *md, const gp_rich_style *s, float *x, float *y) {
    table *t = parse_table(md, md + SDL_strlen(md));
    float h = lay_table(ui, t, 1e6f, s->px, s->px), w = 0;
    // (a column of long text no wider than most of the screen, its cells wrapped, so the rest show beside it)
    float cap = SDL_max(area.w * 0.8f, s->px * 8);
    bool capped = false;
    for (int c = 0; c < t->cols; c++)
        if (t->colw[c] > cap) t->colw[c] = cap, capped = true;
    if (capped) {
        h = 1;
        for (int r = 0; r < t->rows; r++) {
            t->rowh[r] = t->line;
            for (int c = 0; c < t->cols; c++) t->rowh[r] = SDL_max(t->rowh[r], cell_height(ui, t, r, c, t->colw[c] - 2 * PAD - 1));
            h += t->rowh[r] + 2 * VPAD;
        }
    }
    for (int c = 0; c < t->cols; c++) w += t->colw[c];
    *x = SDL_clamp(*x, 0, SDL_max(0, w - area.w)), *y = SDL_clamp(*y, 0, SDL_max(0, h - area.h));
    gs_ui_clip(ui, &area);
    draw_table(ui, t, area.x - *x, area.y - *y, s);
    gs_ui_clip(ui, NULL);
    SDL_free(t->buf), SDL_free(t);
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
            // (a click on it opens it at its full size, gp_rich_take_table)
            float tw = 0;
            for (int c = 0; c < p->t->cols; c++) tw += p->t->colw[c];
            gs_rect whole = { at.x, y, tw, p->height };
            if (gs_ui_hovered(ui, whole)) gs_ui_outline(ui, gs_inset(whole, -2), 4, 1, s->rule);
            if (gs_ui_clicked(ui, whole)) {
                SDL_free(clicked_table);
                clicked_table = SDL_malloc((size_t)(p->to - p->from) + 1);
                SDL_memcpy(clicked_table, text + p->from, (size_t)(p->to - p->from)), clicked_table[p->to - p->from] = 0;
            }
        } else if (a < b) {
            // (bytes of the piece's own copy, which begins at p->from)
            gs_ui_text_span span = { a - p->from, b - p->from, 0, s->marked, s->mark, false };
            char id[40];
            SDL_snprintf(id, sizeof id, "%p.%d", (const void *)text, i);
            paragraph(ui, (gs_rect){ at.x, y, at.w, p->height }, id, i, p->text, s, span.start, span.end);
            if (mark_y && !placed) {
                // The line the mark starts on: below the text before it, less a line when that text
                // runs on into the line.
                char *before = SDL_strndup(p->text, (size_t)span.start);
                float h = *before ? gs_ui_text_height(ui, before, s->px, at.w) : 0, line = gs_ui_text_height(ui, "0", s->px, 1e6f);
                *mark_y = y + SDL_max(0, h - line), placed = true;
                SDL_free(before);
            }
        } else {
            char id[40];
            SDL_snprintf(id, sizeof id, "%p.%d", (const void *)text, i);
            paragraph(ui, (gs_rect){ at.x, y, at.w, p->height }, id, i, p->text, s, 0, 0);
        }
        y += p->height + gap;
    }
    return l->height;
}
