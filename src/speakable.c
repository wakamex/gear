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

// Numbers and the symbols around them, as they are said: Pocket TTS reads digits unreliably (139 as
// "39", 0.34 as "through"), so they reach it as words. Integers up to the trillions, decimals and
// versions digit by digit after "point", leading-zero codes digit by digit, four-digit years in
// pairs, times, ISO dates, percent, money, minus, "x" as "times" and the common units.
static const char *ones[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
static const char *tens[] = { "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };

static void say_below_1000(buf *b, int n) {
    if (n >= 100) {
        puts_(b, ones[n / 100]);
        puts_(b, " hundred");
        if (!(n %= 100)) return;
        puts_(b, " ");
    }
    if (n < 20) { puts_(b, ones[n]); return; }
    puts_(b, tens[n / 10]);
    if (n % 10) { puts_(b, " "); puts_(b, ones[n % 10]); }
}

static void say_integer(buf *b, unsigned long long n) {
    static const char *scales[] = { "trillion", "billion", "million", "thousand" };
    static const unsigned long long sizes[] = { 1000000000000ull, 1000000000ull, 1000000ull, 1000ull };
    bool any = false;
    for (int i = 0; i < 4; i++) {
        if (n < sizes[i]) continue;
        if (any) puts_(b, " ");
        say_below_1000(b, (int)(n / sizes[i] % 1000));
        puts_(b, " ");
        puts_(b, scales[i]);
        n %= sizes[i];
        any = true;
    }
    if (n || !any) {
        if (any) puts_(b, " ");
        say_below_1000(b, (int)n);
    }
}

static void say_digits(buf *b, const char *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i) puts_(b, " ");
        puts_(b, ones[d[i] - '0']);
    }
}

// 1905 as "nineteen oh five", 2021 as "twenty twenty one", 2005 as "two thousand five".
static void say_year(buf *b, int y) {
    if (y >= 2000 && y < 2010) { say_integer(b, (unsigned long long)y); return; }
    say_below_1000(b, y / 100);
    if (y % 100 == 0) { puts_(b, " hundred"); return; }
    puts_(b, " ");
    if (y % 100 < 10) puts_(b, "oh ");
    say_below_1000(b, y % 100);
}

// The last word said, made an ordinal: "twenty one" to "twenty first".
static void ordinal(buf *b) {
    static const struct { const char *word, *nth; } odd[] = { { "one", "first" }, { "two", "second" },
        { "three", "third" }, { "five", "fifth" }, { "eight", "eighth" }, { "nine", "ninth" }, { "twelve", "twelfth" } };
    size_t start = b->len;
    while (start && b->s[start - 1] != ' ') start--;
    for (size_t i = 0; i < SDL_arraysize(odd); i++)
        if (!SDL_strcmp(b->s + start, odd[i].word)) { b->len = start; b->s[start] = 0; puts_(b, odd[i].nth); return; }
    if (b->len && b->s[b->len - 1] == 'y') { b->s[--b->len] = 0; puts_(b, "ieth"); return; }
    puts_(b, "th");
}

static const char *unit_word(const char *u, size_t n) {
    static const struct { const char *unit, *word; } units[] = { { "ms", "milliseconds" }, { "s", "seconds" },
        { "min", "minutes" }, { "KB", "kilobytes" }, { "MB", "megabytes" }, { "GB", "gigabytes" }, { "TB", "terabytes" },
        { "KiB", "kilobytes" }, { "MiB", "megabytes" }, { "GiB", "gigabytes" }, { "TiB", "terabytes" },  // as people say them
        { "kHz", "kilohertz" }, { "Hz", "hertz" }, { "fps", "frames a second" } };
    for (size_t i = 0; i < SDL_arraysize(units); i++)
        if (SDL_strlen(units[i].unit) == n && !SDL_strncmp(u, units[i].unit, n)) return units[i].word;
    return NULL;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }
static bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f'); }

static size_t digits(const char *s) {
    size_t n = 0;
    while (digit(s[n])) n++;
    return n;
}

// A commit hash or other hex code (seven or more, with a digit and a letter): its first six
// characters, spelled, are enough to tell it apart (four were not: 6ad68495 and 6ad68651).
enum { HASH_SAID = 6 };
static size_t hash_length(const char *s) {
    size_t n = 0;
    bool d = false, l = false;
    while (hex(s[n])) { d |= digit(s[n]); l |= !digit(s[n]); n++; }
    return n >= 7 && d && l && !word_char(s[n]) ? n : 0;
}

// $0.70 as "seventy cents", $8.50 as "eight dollars and fifty cents"; returns the characters taken
// after the $, or 0 when the amount has no cents.
static size_t say_money(buf *b, const char *s) {
    size_t n = digits(s), used = n;
    if (!n) return 0;
    unsigned long long dollars = 0;
    for (size_t i = 0; i < n; i++) dollars = dollars * 10 + (unsigned)(s[i] - '0');
    while (n <= 3 && s[used] == ',' && digits(s + used + 1) == 3) {
        for (size_t i = used + 1; i < used + 4; i++) dollars = dollars * 10 + (unsigned)(s[i] - '0');
        used += 4;
    }
    if (s[used] != '.' || digits(s + used + 1) != 2) return 0;
    int cents = (s[used + 1] - '0') * 10 + (s[used + 2] - '0');
    if (dollars || !cents) {
        say_integer(b, dollars);
        puts_(b, dollars == 1 ? " dollar" : " dollars");
        if (cents) puts_(b, " and ");
    }
    if (cents) {
        say_integer(b, (unsigned long long)cents);
        puts_(b, cents == 1 ? " cent" : " cents");
    }
    return used + 3;
}

static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
    "September", "October", "November", "December" };

// One number starting at s, said; returns the characters it took.
static size_t say_number(buf *b, const char *s) {
    size_t n = digits(s);
    // 2026-10-28
    if (n == 4 && s[4] == '-' && digits(s + 5) == 2 && s[7] == '-' && digits(s + 8) == 2 && !digit(s[10])) {
        int month = SDL_atoi(s + 5), day = SDL_atoi(s + 8);
        if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
            puts_(b, months[month - 1]);
            puts_(b, " ");
            say_integer(b, (unsigned long long)day);
            ordinal(b);
            puts_(b, ", ");
            say_year(b, SDL_atoi(s));
            return 10;
        }
    }
    // 12:15 and 2:05, and 02:55:08 with its seconds
    if (n <= 2 && s[n] == ':' && digits(s + n + 1) == 2) {
        int h = SDL_atoi(s), m = SDL_atoi(s + n + 1);
        size_t used = n + 3;
        say_integer(b, (unsigned long long)h);
        if (m) {
            puts_(b, " ");
            if (m < 10) puts_(b, "oh ");
            say_integer(b, (unsigned long long)m);
        }
        if (s[used] == ':' && digits(s + used + 1) == 2) {
            puts_(b, " and ");
            say_integer(b, (unsigned long long)SDL_atoi(s + used + 1));
            puts_(b, " seconds");
            used += 3;
        }
        return used;
    }
    size_t used = n;
    unsigned long long value = 0;
    bool grouped = false;
    for (size_t i = 0; i < n; i++) value = value * 10 + (unsigned)(s[i] - '0');
    // 7,445 and 535,587,314
    while (n <= 3 && s[used] == ',' && digits(s + used + 1) == 3) {
        for (size_t i = used + 1; i < used + 4; i++) value = value * 10 + (unsigned)(s[i] - '0');
        used += 4;
        grouped = true;
    }
    if (n > 15) say_digits(b, s, n);
    else if (n > 1 && s[0] == '0') say_digits(b, s, n);  // 020, 0073
    else if (!grouped && n == 4 && value >= 1100 && value < 2100 && s[4] != '.') say_year(b, (int)value);
    else say_integer(b, value);
    // 0.34, 1.40, and versions and addresses: 0.5.0, 192.168.2.1
    while (s[used] == '.' && digit(s[used + 1])) {
        size_t f = digits(s + used + 1);
        puts_(b, " point ");
        bool version = s[used + 1 + f] == '.' && digit(s[used + 2 + f]);
        if (version || (used && SDL_strchr(s, '.') != s + used)) say_integer(b, (unsigned long long)SDL_atoi(s + used + 1));
        else say_digits(b, s + used + 1, f);
        used += 1 + f;
    }
    return used;
}

// The end of a site's name (com in Chess.com), where "dot" is said; a file's extension is not.
static bool site_ending(const char *s) {
    static const char *endings[] = { "com", "org", "net", "io", "dev", "ai", "app", "co", "mx", "fyi", "uk", "ca", "gov", "edu" };
    size_t n = 0;
    while (SDL_isalpha((unsigned char)s[n])) n++;
    if (s[n] && s[n] != '/' && s[n] != '.') return false;
    for (size_t i = 0; i < SDL_arraysize(endings); i++)
        if (SDL_strlen(endings[i]) == n && !SDL_strncmp(s, endings[i], n)) return true;
    return false;
}

// A word with numbers in it, said: the numbers as words, letters beside them set apart (w3sim as
// "w three sim"), and the symbols that go with numbers. Returns whether it ends in a number, so a
// unit in the next word can be read as one.
static bool say_word(buf *b, const char *w, bool after_number) {
    size_t n = SDL_strlen(w);
    const char *unit = unit_word(w, n);
    if (unit && after_number) { puts_(b, unit); return false; }
    const char *last_dot = SDL_strrchr(w, '.');
    bool site = last_dot && site_ending(last_dot + 1);  // every dot of a site's name is said
    bool has_digit = strpbrk(w, "0123456789") != NULL;
    bool number = false;
    for (const char *p = w; *p;) {
        bool start = p == w || !word_char(p[-1]);
        size_t h = start && p[0] == '0' && p[1] == 'x' ? hash_length(p + 2) + 2 : start ? hash_length(p) : 0;
        if (h > 2) {
            const char *code = p[0] == '0' && p[1] == 'x' ? p + 2 : p;
            for (int i = 0; i < HASH_SAID; i++) {
                if (i) puts_(b, " ");
                if (digit(code[i])) puts_(b, ones[code[i] - '0']);
                else put(b, code + i, 1);
            }
            p += h;
            number = false;
            continue;
        }
        if (digit(*p) && p[digits(p)] == '/' && digit(p[digits(p) + 1])) {  // 1/2 and 7/9 as written
            size_t f = digits(p) + 1;
            f += digits(p + f);
            put(b, p, f);
            p += f;
            continue;
        }
        size_t money = *p == '$' && start ? say_money(b, p + 1) : 0;
        if (money) {
            p += 1 + money;
            number = false;
            continue;
        }
        if ((*p == '-' || *p == '$' || *p == '~') && digit(p[1]) && start) {
            puts_(b, *p == '-' ? "minus " : *p == '~' ? "about " : "");
            bool dollars = *p == '$';
            p++;
            p += say_number(b, p);
            if (dollars) puts_(b, " dollars");
            number = !dollars;
            continue;
        }
        if (digit(*p)) {
            if (b->len && word_char(b->s[b->len - 1])) puts_(b, " ");
            const char *begun = p;
            p += say_number(b, p);
            number = true;
            size_t letters = 0;
            while (SDL_isalpha((unsigned char)p[letters])) letters++;
            if (*p == '%') { puts_(b, " percent"); p++; number = false; }
            else if (letters == 1 && *p == 'x' && !word_char(p[1])) { puts_(b, " times"); p++; number = false; }
            else if (letters == 2 && !word_char(p[2]) && (!SDL_strncmp(p, "st", 2) || !SDL_strncmp(p, "nd", 2) || !SDL_strncmp(p, "rd", 2) || !SDL_strncmp(p, "th", 2))) {
                ordinal(b);
                p += 2;
                number = false;
            } else if (letters && !word_char(p[letters]) && (unit = unit_word(p, letters))) {
                puts_(b, " ");
                puts_(b, unit);
                p += letters;
                number = false;
            } else if (*p == '-' && begun == w && digits(p + 1) == SDL_strlen(p + 1)) {  // 4-9 and 496-501
                puts_(b, " to ");
                p++;
            } else if (letters || *p == '-') {  // a name's parts: claude-opus-5-5
                puts_(b, " ");
                if (*p == '-') p++;
            }
            continue;
        }
        if (p > w && word_char(p[-1]) && word_char(p[1]) && (*p == '=' || *p == '+')) {
            puts_(b, *p == '=' ? " equals " : " plus ");  // v=DMARC1, a+b
            p++;
            continue;
        }
        if (*p == '-' && has_digit && p > w && word_char(p[-1]) && word_char(p[1])) {
            puts_(b, " ");  // m8-fogunits-wait3: a name's parts
            p++;
            continue;
        }
        if (*p == '/' && p > w && SDL_isalpha((unsigned char)p[-1]) && SDL_isalpha((unsigned char)p[1])) {
            puts_(b, " ");  // tmp/live, claude/ACCOUNT
            p++;
            continue;
        }
        if (*p == '.' && p > w && SDL_isalpha((unsigned char)p[-1]) && SDL_isalpha((unsigned char)p[1]) && site) {
            puts_(b, " dot ");  // Chess.com
            p++;
            continue;
        }
        put(b, p, 1);
        number = false;
        p++;
    }
    return number;
}

// The word that ends at s[end], spaces after it aside, lowercased into w: where it starts, or -1 when
// what comes before is the start of the text or punctuation, which ends a clause.
static int word_before(const char *s, int end, char *w, size_t size) {
    while (end > 0 && s[end - 1] == ' ') end--;
    int start = end;
    while (start > 0 && (SDL_isalpha((unsigned char)s[start - 1]) || s[start - 1] == '\'')) start--;
    if (start == end) return -1;
    size_t n = SDL_min((size_t)(end - start), size - 1);
    for (size_t i = 0; i < n; i++) w[i] = (char)SDL_tolower((unsigned char)s[start + i]);
    w[n] = 0;
    return start;
}

static bool among(const char *w, const char *words) {  // words: each between two spaces
    char key[48];
    SDL_snprintf(key, sizeof key, " %s ", w);
    return SDL_strstr(words, key) != NULL;
}

// Whether "read", about to be said after what b holds and before `after`, is the past form, which is
// then written "red". Phonon said "reed" in "She read only the first page", "It read the engine code"
// and "is read aloud" 16 times of 18, and "red" for "red" in their place 11 times of 12 (by the
// vowel's first resonance, against the same sentences with "reed"). Only where grammar leaves no
// choice: after a form of be or have ("has never read", but not "is read only"), and after he, she or
// it ("it just read"), unless the verb before that takes the plain form ("let it read", "did she
// read"); a few adverbs may come between. Elsewhere ("I read the file") the model follows the sentence.
static bool read_is_past(const buf *b, const char *after) {
    static const char be_have[] = " am is are was were be been being isn't aren't wasn't weren't have has had having haven't hasn't hadn't"
                                  " i've you've we've they've who've would've could've should've you're we're they're it's he's she's that's ";
    static const char adverbs[] = " not no now already just never ever also only still all both yet always often actually fully first longer best then even really ";
    static const char plain[] = " let lets make makes made help helps helped have had did does do didn't doesn't don't can can't cannot could couldn't"
                                " will won't would wouldn't should shouldn't may might must see saw watch watched ";
    char w[40], next[16];
    size_t k = 0;
    while (*after == ' ') after++;
    for (; SDL_isalpha((unsigned char)*after) && k + 1 < sizeof next; after++) next[k++] = (char)SDL_tolower((unsigned char)*after);
    next[k] = 0;
    if (among(next, " access permission permissions ")) return false;  // "give it read access"
    int at = b->len ? word_before(b->s, (int)b->len, w, sizeof w) : -1;
    while (at >= 0 && among(w, adverbs)) at = word_before(b->s, at, w, sizeof w);
    if (at < 0) return false;
    if (among(w, " he she it ")) return word_before(b->s, at, w, sizeof w) < 0 || !among(w, plain);
    return among(w, be_have) && SDL_strcmp(next, "only");
}

// One whitespace-free token, made speakable: an address read as its site, a path as its file's
// name, snake_case as words, an arrow as "to", numbers as words. Returns whether it ends in a number.
static bool token(buf *b, const char *t, size_t n, bool after_number) {
    // Punctuation that ends a sentence or clause stays outside the token.
    size_t tail = n;
    while (tail && SDL_strchr(".,;:!?)\"'", t[tail - 1])) tail--;
    size_t head = 0;
    while (head < tail && SDL_strchr("(\"'", t[head])) head++;
    put(b, t, head);
    const char *w = t + head;
    size_t m = tail - head;
    char word[1024];
    if (m >= sizeof word) { put(b, t + head, n - head); return false; }
    SDL_memcpy(word, w, m);
    word[m] = 0;
    if (!SDL_strcmp(word, "read") && !head && read_is_past(b, t + n)) {
        puts_(b, "red");
        put(b, t + tail, n - tail);
        return false;
    }
    const char *scheme = SDL_strstr(word, "://");
    if (scheme && (!SDL_strncmp(word, "http", 4))) {
        const char *host = scheme + 3;
        if (!SDL_strncmp(host, "www.", 4)) host += 4;
        size_t h = strcspn(host, "/:?#");
        char site[256];
        SDL_snprintf(site, sizeof site, "%.*s", (int)SDL_min(h, sizeof site - 1), host);
        puts_(b, "a ");
        say_word(b, site, false);
        puts_(b, " link");
    } else if (!SDL_strcmp(word, "->") || !SDL_strcmp(word, "=>")) {
        puts_(b, "to");
    } else if (!SDL_strcmp(word, "+") || !SDL_strcmp(word, "=") || !SDL_strcmp(word, "==") || !SDL_strcmp(word, "&")) {
        puts_(b, word[0] == '+' ? "plus" : word[0] == '&' ? "and" : "equals");
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
        char said[1024];
        size_t k = 0;
        for (const char *p = name; *p; p++) said[k++] = *p == '_' && p > name && word_char(p[-1]) && word_char(p[1]) ? ' ' : *p;
        said[k] = 0;
        bool number = say_word(b, said, after_number);
        put(b, t + tail, n - tail);
        return number && tail == n;
    }
    put(b, t + tail, n - tail);
    return false;
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
    bool number = false;  // the last word ended in a number, so a unit after it is read as one
    for (const char *p = plain.s ? plain.s : ""; *p;) {
        if (*p == ' ' || *p == '\t') { if (out->len && out->s[out->len - 1] != ' ') puts_(out, " "); p++; continue; }
        size_t n = strcspn(p, " \t");
        number = token(out, p, n, number);
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
