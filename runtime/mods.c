// Mods: LayeredFS-style replacement files and the game's language (runtime/mods.h). The RomFS reader
// (runtime/services/fs.c) asks mod_romfs_override for every file of the image and serves the mod's copy instead, and
// adds the files a mod has that the game hasn't.
#include "mods.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gamesetup.h"
#include "hle.h"
#include "paths.h"
#include "platform.h"
#include "romfs_build.h"
#include "settings.h"

static bool is_dir(const char *p) {
    struct stat sb;
    return !stat(p, &sb) && S_ISDIR(sb.st_mode);
}

// The romfs/ folder inside dir, up to `depth` levels down (a download unpacked with its own folders around it). Packs
// made for both games (atmosphere/titles/<Pikachu>/ and <Eevee>/) are searched under the set-up game's title id first.
#define EEVEE_TITLE "0100187003A36000"
#define PIKACHU_TITLE "010003F003A34000"
static bool find_romfs(const char *dir, int depth, char *out, size_t cap) {
    char p[1024];
    snprintf(p, sizeof p, "%s/romfs", dir);  // (RomFs / romfs: Windows names ignore case)
    if (is_dir(p)) { snprintf(out, cap, "%s", p); return true; }
    if (depth <= 0) return false;
    const char *name = game_name("extracted/exefs");
    bool pikachu = name && !strcmp(name, "Pikachu");
    snprintf(p, sizeof p, "%s/%s", dir, pikachu ? PIKACHU_TITLE : EEVEE_TITLE);
    if (is_dir(p) && find_romfs(p, depth - 1, out, cap)) return true;
    snprintf(p, sizeof p, "%s/%s", dir, pikachu ? EEVEE_TITLE : PIKACHU_TITLE);
    if (is_dir(p) && find_romfs(p, depth - 1, out, cap)) return true;
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) found = find_romfs(p, depth - 1, out, cap);
    }
    closedir(d);
    return found;
}

int mods_list(char names[][128], int max) {
    DIR *d = opendir("mods");
    if (!d) return 0;
    int n = 0;
    char p[1024], r[1024];
    for (struct dirent *e; n < max && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "mods/%s", e->d_name);
        if (is_dir(p) && find_romfs(p, 4, r, sizeof r)) snprintf(names[n++], 128, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

const char *mods_dir_abs(void) {
    static char p[1100];
    mkdir("mods", 0755);
    snprintf(p, sizeof p, "%s/mods", rt_data_root());
    return p;
}

static const char *romfs_of(const char *name) {
    static char cached_name[128], dir[1024];
    static bool ok;
    if (!name || !*name) return NULL;
    if (strcmp(name, cached_name)) {
        snprintf(cached_name, sizeof cached_name, "%s", name);
        char p[1024];
        snprintf(p, sizeof p, "mods/%s", name);
        ok = find_romfs(p, 4, dir, sizeof dir);  // (the active ones are logged by active_dirs)
    }
    return ok ? dir : NULL;
}

// The active mods' romfs/ folders, made again when g_settings.mod changes
static char g_dirs[MODS_MAX][1024], g_dirs_of[sizeof g_settings.mod];
static int g_ndirs = -1;
static void active_dirs(void) {
    if (g_ndirs >= 0 && !strcmp(g_dirs_of, g_settings.mod)) return;
    snprintf(g_dirs_of, sizeof g_dirs_of, "%s", g_settings.mod);
    g_ndirs = 0;
    char list[sizeof g_settings.mod];
    snprintf(list, sizeof list, "%s", g_settings.mod);
    for (char *save = NULL, *name = strtok_r(list, "|", &save); name && g_ndirs < MODS_MAX; name = strtok_r(NULL, "|", &save)) {
        while (*name == ' ') name++;
        char p[1024];
        snprintf(p, sizeof p, "mods/%s", name);
        if (*name && find_romfs(p, 4, g_dirs[g_ndirs], sizeof g_dirs[0])) LOG("[mods] %d: %s: %s\n", g_ndirs, name, g_dirs[g_ndirs]), g_ndirs++;
        else if (*name) LOG("[mods] %s: no romfs/ folder found under mods/%s\n", name, name);
    }
}

int mod_romfs_dirs(const char **out, int max) {
    active_dirs();
    int n = 0;
    for (; n < g_ndirs && n < max; n++) out[n] = g_dirs[n];
    return n;
}

bool mod_romfs_override(const char *path, char *out, size_t cap) {
    active_dirs();
    struct stat sb;
    for (int i = 0; i < g_ndirs; i++) {
        snprintf(out, cap, "%s/%s", g_dirs[i], path);
        if (!stat(out, &sb) && S_ISREG(sb.st_mode)) return true;
    }
    return false;
}

// bin/message/<folder> -> language code
static const struct { const char *folder, *code; } k_msg_langs[] = {
    {"English", "en-US"}, {"Spanish", "es"},  {"French", "fr"},          {"German", "de"},         {"Italian", "it"},
    {"JPN", "ja"},        {"Korean", "ko"},   {"Simp_Chinese", "zh-CN"}, {"Trad_Chinese", "zh-TW"},
};

const char *mod_language(const char *name) {
    const char *root = romfs_of(name);
    if (!root) return NULL;
    const char *found = NULL;
    char p[1100];
    for (size_t i = 0; i < sizeof k_msg_langs / sizeof k_msg_langs[0]; i++) {
        snprintf(p, sizeof p, "%s/bin/message/%s", root, k_msg_langs[i].folder);
        if (!is_dir(p)) continue;
        if (found) return NULL;  // several languages: the player's choice stands
        found = k_msg_langs[i].code;
    }
    return found;
}

const char *game_language(void) {
    char list[sizeof g_settings.mod];
    snprintf(list, sizeof list, "%s", g_settings.mod);
    for (char *save = NULL, *name = strtok_r(list, "|", &save); name; name = strtok_r(NULL, "|", &save)) {
        static char lang[16];  // (mod_language's answer lives in a table: copied out of the loop's buffer anyway)
        const char *m = mod_language(name);
        if (m) { snprintf(lang, sizeof lang, "%s", m); return lang; }
    }
    return g_settings.game_lang[0] ? g_settings.game_lang : "en-US";
}

// ------------------------------------------------------------------ installing a mod (the start screen's button)
static void rm_tree(const char *p) {  // (only ever called on folders under mods/)
    DIR *d = opendir(p);
    if (!d) { remove(p); return; }
    char c[1100];
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (is_dir(c)) rm_tree(c);
        else remove(c);
    }
    closedir(d);
    rmdir(p);
}

static _Atomic int g_install_state;
static char g_install_src[1024], g_install_name[128];
int mod_install_state(void) { return g_install_state; }
const char *mod_install_name(void) { return g_install_name; }

static void *install_thread(void *arg) {
    (void)arg;
    const char *src = g_install_src, *base = src;
    for (const char *c = src; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    // the name: the file / folder name without its extension, letters, digits, spaces, '-' and '.' only
    char name[128];
    int n = 0;
    for (const char *c = base; *c && n < 60; c++) {
        unsigned char ch = (unsigned char)*c;
        name[n++] = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == ' ' || ch == '.' ? (char)ch : '_';
    }
    name[n] = 0;
    char *dot = strrchr(name, '.');
    if (dot && !is_dir(src)) *dot = 0;
    while (n && name[strlen(name) - 1] == ' ') name[strlen(name) - 1] = 0;
    if (!name[0]) snprintf(name, sizeof name, "mod");
    mkdir("mods", 0755);
    const char *root = src;
    if (!is_dir(src)) {  // an archive: Windows' own tar (libarchive: .zip, .rar, .7z) into mods/.install
        rm_tree("mods/.install");
        mkdir("mods/.install", 0755);
        char tar[600];
        const char *sys = getenv("SystemRoot");
        snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", sys ? sys : "C:\\Windows");
        const char *argv[] = {tar, "-xf", src, "-C", "mods/.install", NULL};
        PlatProc p = plat_spawn(argv, "mods/.install.log", false);
        int r = p ? plat_proc_wait(p, true) : -1;
        remove("mods/.install.log");
        if (r != 0) { LOG("[mods] couldn't unpack %s (tar %d)\n", src, r); g_install_state = MOD_INSTALL_FAIL_UNPACK; goto done; }
        root = "mods/.install";
    }
    {
        char romfs[1024];
        if (!find_romfs(root, 6, romfs, sizeof romfs)) { g_install_state = MOD_INSTALL_FAIL_NOT_MOD; goto done; }
        char dst[300];
        snprintf(dst, sizeof dst, "mods/%s", name);
        if (is_dir(dst)) rm_tree(dst);
        mkdir(dst, 0755);
        strlcat(dst, "/romfs", sizeof dst);
        if (!romfs_copy_tree(romfs, dst, NULL)) { g_install_state = MOD_INSTALL_FAIL_UNPACK; goto done; }
        snprintf(g_install_name, sizeof g_install_name, "%s", name);
        LOG("[mods] installed %s from %s\n", name, src);
        g_install_state = MOD_INSTALL_DONE;
    }
done:
    rm_tree("mods/.install");
    return NULL;
}

void mod_install_start(const char *src) {
    if (g_install_state == MOD_INSTALL_RUNNING) return;
    snprintf(g_install_src, sizeof g_install_src, "%s", src);
    g_install_name[0] = 0;
    g_install_state = MOD_INSTALL_RUNNING;
    pthread_t t;
    pthread_create(&t, NULL, install_thread, NULL);
    pthread_detach(t);
}
