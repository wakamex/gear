// Words said the way the user has taught: "proton" said as "Pro-tawn", since the speech model reads
// only letters and says some words wrongly every time. The list is kept in pronunciations.txt in
// gear's data folder, a "WORD<TAB>SAY" line each, and read again when it changes on disk, so the
// window, the hub and gear read all follow it. A suggestion for a word comes from CMUdict, the
// Carnegie Mellon pronouncing dictionary, written out as a respelling ("proh-tahn").
#pragma once
#include <stdbool.h>
#include <stddef.h>

// `text` with each taught word replaced by how to say it, matched whole and in any case, with a
// plural's "s" or "es" kept after it and a capital first letter kept. SDL_free it.
char *gear_pronounce(const char *text);
// The same carrying `from` (each byte of `text`'s place elsewhere, one more than its length) over to
// what is written, in *out_from (SDL_free it).
char *gear_pronounce_map(const char *text, const int *from, int **out_from);
// What is said of a reply: gear_speakable's reading of it, with the taught words. SDL_free it.
char *gear_pronounce_reply(const char *reply);
// The same with each byte's place in `reply` (gear_speakable_map), in *from; SDL_free it.
char *gear_pronounce_reply_map(const char *reply, int **from);
// Teaches `word` (one word, no spaces) to be said as `say`; an empty `say` forgets it. False when the
// list could not be written.
bool gear_pronounce_set(const char *word, const char *say);
// What `word` is taught to be said as, into `out`; false when it is not taught.
bool gear_pronounce_get(const char *word, char *out, size_t size);
// The list as JSON, [{"word", "say"}] in the order taught. SDL_free it.
char *gear_pronounce_list(void);
// CMUdict's first pronunciation of `word`, respelled in syllables of plain letters ("proh-tahn",
// "may-zahn"), from cmudict.dict in the models folder `models`; false when the word is not in it.
bool gear_pronounce_suggest(const char *models, const char *word, char *out, size_t size);
// The same from ARPAbet phones as CMUdict writes them ("P R OW1 T AA2 N"), for the tests.
void gear_pronounce_respell(const char *phones, char *out, size_t size);
