#include "pronounce.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "gs_json.h"
#include "speakable.h"

typedef struct { char word[64], say[128]; } taught;
enum { MOST = 1000 };
static taught list[MOST];
static int count;
static SDL_Time loaded_at = -1;  // the file's time when it was read
static SDL_Mutex *lock;

static void path_of(char *out, size_t size) {
    char *pref = SDL_GetPrefPath("wakamex", "gear");
    SDL_snprintf(out, size, "%spronunciations.txt", pref ? pref : "");
    SDL_free(pref);
}

static void lower(char *s) {
    for (; *s; s++) *s = (char)SDL_tolower((unsigned char)*s);
}

// The list from the file, when it has changed since it was read (under the lock).
static void refresh(void) {
    char path[1100];
    path_of(path, sizeof path);
    SDL_PathInfo info;
    SDL_Time at = SDL_GetPathInfo(path, &info) ? info.modify_time : 0;
    if (at == loaded_at) return;
    loaded_at = at, count = 0;
    char *text = at ? SDL_LoadFile(path, NULL) : NULL, *save = NULL;
    for (char *line = text ? SDL_strtok_r(text, "\n", &save) : NULL; line && count < MOST; line = SDL_strtok_r(NULL, "\n", &save)) {
        char *tab = SDL_strchr(line, '\t');
        if (!tab || tab == line || !tab[1]) continue;
        *tab = 0;
        size_t n = SDL_strlen(tab + 1);
        if (n && tab[n] == '\r') tab[n] = 0;
        SDL_strlcpy(list[count].word, line, sizeof list[count].word), lower(list[count].word);
        SDL_strlcpy(list[count].say, tab + 1, sizeof list[count].say);
        count++;
    }
    SDL_free(text);
}

static void begin(void) {
    static SDL_InitState once;
    if (SDL_ShouldInit(&once)) lock = SDL_CreateMutex(), SDL_SetInitialized(&once, true);
    SDL_LockMutex(lock);
    refresh();
}

static void end(void) { SDL_UnlockMutex(lock); }

static const taught *find(const char *word, size_t n) {
    for (int i = 0; i < count; i++)
        if (SDL_strlen(list[i].word) == n && !SDL_strncasecmp(list[i].word, word, n)) return &list[i];
    return NULL;
}

static bool letter(char c) { return SDL_isalpha((unsigned char)c); }  // (so "proton's" is the word and its 's)

char *gear_pronounce(const char *text) { return gear_pronounce_map(text, NULL, NULL); }

// `out_from[k]` for each byte k written: `from` of the text's byte it stands for (a taught word's
// first), when there is a `from`.
char *gear_pronounce_map(const char *text, const int *from, int **out_from) {
    begin();
    size_t cap = SDL_strlen(text) * 2 + 64, len = 0;
    char *out = SDL_malloc(cap);
    int *map = from ? SDL_malloc(cap * sizeof *map) : NULL;
#define MAPPED(k, i) \
    do { \
        if (map) map[k] = from[i]; \
    } while (0)
    for (const char *s = text; *s;) {
        if (!letter(*s)) { MAPPED(len, s - text); out[len++] = *s++; goto room; }
        const char *e = s;
        while (letter(*e)) e++;
        size_t n = (size_t)(e - s);
        // The word, or its plural: "protons" as the taught "proton" and its "s".
        const taught *t = find(s, n);
        size_t stem = n;
        if (!t && n > 2 && (e[-1] == 's' || e[-1] == 'S')) t = find(s, stem = n - 1);
        if (!t && n > 3 && (e[-1] == 's' || e[-1] == 'S') && (e[-2] == 'e' || e[-2] == 'E')) t = find(s, stem = n - 2);
        if (t) {
            size_t at = len;
            size_t sn = SDL_strlen(t->say);
            if (len + sn + n + 8 >= cap) cap = (len + sn + n + 8) * 2, out = SDL_realloc(out, cap), map = map ? SDL_realloc(map, cap * sizeof *map) : NULL;
            for (size_t k = 0; k < sn; k++) MAPPED(len + k, s - text);
            SDL_memcpy(out + len, t->say, sn), len += sn;
            // (a capital where the word had one, at the start of a sentence)
            if (SDL_isupper((unsigned char)*s)) out[at] = (char)SDL_toupper((unsigned char)out[at]);
            for (size_t k = stem; k < n; k++) MAPPED(len + k - stem, s + k - text);
            SDL_memcpy(out + len, s + stem, n - stem), len += n - stem;
        } else {
            if (len + n + 8 >= cap) cap = (len + n + 8) * 2, out = SDL_realloc(out, cap), map = map ? SDL_realloc(map, cap * sizeof *map) : NULL;
            for (size_t k = 0; k < n; k++) MAPPED(len + k, s + k - text);
            SDL_memcpy(out + len, s, n), len += n;
        }
        s = e;
    room:
        if (len + 8 >= cap) cap *= 2, out = SDL_realloc(out, cap), map = map ? SDL_realloc(map, cap * sizeof *map) : NULL;
    }
#undef MAPPED
    out[len] = 0;
    if (map) map[len] = from[SDL_strlen(text)];
    if (out_from) *out_from = map;
    end();
    return out;
}

char *gear_pronounce_reply(const char *reply) { return gear_pronounce_reply_map(reply, NULL); }

char *gear_pronounce_reply_map(const char *reply, int **from) {
    int *spoken_from = NULL;
    char *spoken = gear_speakable_map(reply, from ? &spoken_from : NULL), *said = gear_pronounce_map(spoken, spoken_from, from);
    SDL_free(spoken), SDL_free(spoken_from);
    return said;
}

static bool save(void) {
    char path[1100];
    path_of(path, sizeof path);
    size_t cap = (size_t)count * 200 + 1, len = 0;
    char *text = SDL_malloc(cap);
    text[0] = 0;
    for (int i = 0; i < count; i++) len += (size_t)SDL_snprintf(text + len, cap - len, "%s\t%s\n", list[i].word, list[i].say);
    bool ok = SDL_SaveFile(path, text, len);
    SDL_free(text);
    SDL_PathInfo info;
    if (ok && SDL_GetPathInfo(path, &info)) loaded_at = info.modify_time;
    return ok;
}

bool gear_pronounce_set(const char *word, const char *say) {
    char w[64];
    SDL_strlcpy(w, word, sizeof w), lower(w);
    if (!*w || SDL_strchr(w, ' ') || SDL_strchr(w, '\t') || SDL_strchr(say, '\t') || SDL_strchr(say, '\n')) return false;
    begin();
    int at = -1;
    for (int i = 0; i < count; i++)
        if (!SDL_strcmp(list[i].word, w)) at = i;
    if (!*say) {
        if (at >= 0) SDL_memmove(&list[at], &list[at + 1], (size_t)(count - at - 1) * sizeof *list), count--;
    } else {
        if (at < 0 && count < MOST) at = count++;
        if (at >= 0) SDL_strlcpy(list[at].word, w, sizeof list[at].word), SDL_strlcpy(list[at].say, say, sizeof list[at].say);
    }
    bool ok = save();
    end();
    return ok;
}

bool gear_pronounce_get(const char *word, char *out, size_t size) {
    begin();
    const taught *t = find(word, SDL_strlen(word));
    if (t) SDL_strlcpy(out, t->say, size);
    end();
    return t != NULL;
}

char *gear_pronounce_list(void) {
    begin();
    gs_jval *a = gs_jval_array();
    for (int i = 0; i < count; i++) {
        gs_jval *o = gs_jval_object();
        gs_jval_set(o, "word", gs_jval_str(list[i].word));
        gs_jval_set(o, "say", gs_jval_str(list[i].say));
        gs_jval_push(a, o);
    }
    end();
    char *json = gs_jval_write(a, false);
    gs_jval_free(a);
    return json;
}

// ---- CMUdict ----

// ARPAbet's sounds as plain letters: the vowels as in "ah", "may", "see", the consonants as spelled.
static const char *sound(const char *p) {
    static const char *const table[][2] = {
        { "AA", "ah" }, { "AE", "a" },  { "AH", "uh" }, { "AO", "aw" }, { "AW", "ow" }, { "AY", "eye" }, { "EH", "eh" },
        { "ER", "er" }, { "EY", "ay" }, { "IH", "ih" }, { "IY", "ee" }, { "OW", "oh" }, { "OY", "oy" },  { "UH", "uu" },
        { "UW", "oo" }, { "B", "b" },   { "CH", "ch" }, { "D", "d" },   { "DH", "th" }, { "F", "f" },    { "G", "g" },
        { "HH", "h" },  { "JH", "j" },  { "K", "k" },   { "L", "l" },   { "M", "m" },   { "N", "n" },    { "NG", "ng" },
        { "P", "p" },   { "R", "r" },   { "S", "s" },   { "SH", "sh" }, { "T", "t" },   { "TH", "th" },  { "V", "v" },
        { "W", "w" },   { "Y", "y" },   { "Z", "z" },   { "ZH", "zh" },
    };
    for (size_t i = 0; i < SDL_arraysize(table); i++)
        if (!SDL_strcmp(table[i][0], p)) return table[i][1];
    return "";
}

void gear_pronounce_respell(const char *phones, char *out, size_t size) {
    // The phones without their stress digits, and which are vowels (all of ARPAbet's begin with one).
    char p[64][4];
    bool vowel[64];
    int n = 0;
    for (const char *s = phones; *s && n < 64;) {
        while (*s == ' ') s++;
        int k = 0;
        for (; *s && *s != ' '; s++)
            if (!SDL_isdigit((unsigned char)*s) && k < 3) p[n][k++] = *s;
        if (!k) continue;
        p[n][k] = 0;
        vowel[n] = SDL_strchr("AEIOU", p[n][0]) != NULL;
        n++;
    }
    // Syllables: each vowel with the consonants before it, a run of two or more between vowels split
    // after its first, so "P R OW T AA N" is "proh-tahn".
    out[0] = 0;
    size_t len = 0;
    int last_vowel = -1;
    for (int i = 0; i < n; i++) {
        if (vowel[i]) {
            if (last_vowel >= 0) {
                int between = i - last_vowel - 1, keep = between >= 2 ? 1 : 0;
                for (int j = last_vowel + 1; j <= last_vowel + keep; j++) len += (size_t)SDL_snprintf(out + len, size > len ? size - len : 0, "%s", sound(p[j]));
                len += (size_t)SDL_snprintf(out + len, size > len ? size - len : 0, "-");
                for (int j = last_vowel + 1 + keep; j < i; j++) len += (size_t)SDL_snprintf(out + len, size > len ? size - len : 0, "%s", sound(p[j]));
            } else
                for (int j = 0; j < i; j++) len += (size_t)SDL_snprintf(out + len, size > len ? size - len : 0, "%s", sound(p[j]));
            len += (size_t)SDL_snprintf(out + len, size > len ? size - len : 0, "%s", sound(p[i]));
            last_vowel = i;
        }
    }
    for (int j = last_vowel + 1; j < n; j++) len += (size_t)SDL_snprintf(out + len, size > len ? size - len : 0, "%s", sound(p[j]));
}

bool gear_pronounce_suggest(const char *models, const char *word, char *out, size_t size) {
    char path[1100], w[64];
    SDL_snprintf(path, sizeof path, "%s/cmudict.dict", models);
    SDL_strlcpy(w, word, sizeof w), lower(w);
    size_t n = SDL_strlen(w), len = 0;
    char *all = n ? SDL_LoadFile(path, &len) : NULL;
    bool found = false;
    for (char *line = all; line && line < all + len && !found;) {
        char *e = memchr(line, '\n', (size_t)(all + len - line));
        if (!e) e = all + len;
        if ((size_t)(e - line) > n && !SDL_strncmp(line, w, n) && line[n] == ' ') {
            char phones[256];
            size_t m = SDL_min((size_t)(e - line) - n - 1, sizeof phones - 1);
            SDL_memcpy(phones, line + n + 1, m), phones[m] = 0;
            char *hash = SDL_strchr(phones, '#');  // (a comment after the phones)
            if (hash) *hash = 0;
            gear_pronounce_respell(phones, out, size);
            found = true;
        }
        line = e + 1;
    }
    SDL_free(all);
    return found;
}
