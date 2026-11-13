// A non-Steam shortcut for the game in Steam's own list (runtime/steam.c): the start screen's "Add to Steam".
#pragma once
#include <stddef.h>

enum {
    STEAM_OK_ADDED = 0,     // the entry was written
    STEAM_OK_ALREADY = 1,   // Steam already had one by that name
    STEAM_ERR_NO_STEAM = -1,  // no Steam install (or no account folder) found
    STEAM_ERR_STEAM_RUNNING = -2,
    STEAM_ERR_VDF = -3,     // the file is not what this code understands: it was left alone
    STEAM_ERR_WRITE = -4,
};

// Adds (or reports) a shortcut named `name` that starts `exe` with `opts` as its launch options. `out` gets the message
// for the player, in English. Returns one of the codes above.
int steam_add_shortcut(const char *exe, const char *name, const char *opts, char *out, size_t on);
