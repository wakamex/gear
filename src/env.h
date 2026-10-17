// Sets an environment variable where the C runtime's getenv sees it, for the engines' settings
// (GGML_DISABLE_VULKAN, POCKET_THREADS). On Windows SDL_setenv_unsafe sets the process environment,
// which the C runtime's copy does not follow; elsewhere it calls the C runtime's setenv.
#pragma once
#include <SDL3/SDL.h>
#include <stdlib.h>

static inline void gear_setenv(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    SDL_setenv_unsafe(name, value, 1);
#endif
}
