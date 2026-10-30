// Let's Go Recomp start screen: a launcher's window (no system frame, taller than wide) with a sidebar - the project's
// icon, the pages, Play - and the page beside it: Game (the game files - picked once, set up and recompiled here:
// runtime/gamesetup.c - and the game options), Graphics (the window, then the F12 menu's graphics page:
// runtime/ui/settings_ui.mm), Image and sound (the same as the F12 menu's), Controls (keyboard / mouse) and Mods
// (mods/: game files replaced, a translation for one; installed from a downloaded file: runtime/mods.c). The look is
// the F12 menu's. In English, or in Portuguese when the system's language is (LARECOMP_LANG=en|pt
// chooses; the shared pages speak Russian too). The choices go to larecomp.ini (settings.c; bind_key_* from
// runtime/hle/bindings.c).
// Tests: LARECOMP_LAUNCHER_SHOT=<frame> saves build/launcher.png (or LARECOMP_SHOT_PATH) and exits;
// LARECOMP_LAUNCHER_TAB=<n> opens that page; LARECOMP_LAUNCHER_AUTOPLAY=1 plays as soon as the game files are ready; LARECOMP_SETUP_DIR=<folder> sets them up
// from that folder at once; LARECOMP_MOD_INSTALL=<file or folder> installs a mod at once;
// LARECOMP_LAUNCHER_KEYS=1 opens the Controls page, =<slot> also waits for a new key for that input
// (runtime/input_bind.h BIND_*), and <slot>:<key name> presses that key for it.
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"
#include "settings_ui.h"

extern "C" {
#include "gamesetup.h"
#include "input_bind.h"
#include "mods.h"
#include "paths.h"
#include "settings.h"
#include "ui2x.h"
void settings_save(void);
void ui_window_init(void);
SDL_Window *ui_window(void);
void ui_window_game_size(SDL_Window *win, int *w, int *h);
void menu_apply_window_requests(void *sdl_window);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
}

// ------------------------------------------------------------------ language
// (runtime/ui/settings_ui.h picks it; this screen's own strings are English or Portuguese)
static bool g_pt;
#define T(en, pt) (g_pt ? (pt) : (en))
static float g_scale = 1;  // the window's height over 1080 (ui_scale): fixed sizes below are for 1080 px

// ------------------------------------------------------------------ keyboard
// The game's actions with the key bound to each. Click one, then press the new key (Esc or 5 s cancels; a key already
// in use swaps over). Controllers keep their buttons: nothing to set for them.
static const struct { int slot; const char *en, *pt; } k_actions[] = {
    {BIND_LS_UP, "Walk up", "Andar para cima"},             {BIND_A, "A (or Enter): confirm", "A (ou Enter): confirmar"},
    {BIND_LS_DOWN, "Walk down", "Andar para baixo"},         {BIND_B, "B (or Backspace): back", "B (ou Backspace): cancelar"},
    {BIND_LS_LEFT, "Walk left", "Andar para a esquerda"},    {BIND_X, "X: menu", "X: menu"},
    {BIND_LS_RIGHT, "Walk right", "Andar para a direita"},   {BIND_Y, "Y: options / box", "Y: opções / caixa"},
    {BIND_UP, "D-pad up", "Direcional cima"},                {BIND_L, "L: move Pokémon / items", "L: mover Pokémon / itens"},
    {BIND_DOWN, "D-pad down", "Direcional baixo"},           {BIND_R, "R: move Pokémon / items", "R: mover Pokémon / itens"},
    {BIND_LEFT, "D-pad left", "Direcional esquerda"},        {BIND_PLUS, "+: partner", "+: parceiro"},
    {BIND_RIGHT, "D-pad right", "Direcional direita"},       {BIND_MINUS, "-: Pokédex", "-: Pokédex"},
};

static const char *key_name(int sc) {
    switch (sc) {
    case SDL_SCANCODE_SPACE: return T("Space", "Espaço");
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_UP: return T("Up arrow", "Seta cima");
    case SDL_SCANCODE_DOWN: return T("Down arrow", "Seta baixo");
    case SDL_SCANCODE_LEFT: return T("Left arrow", "Seta esquerda");
    case SDL_SCANCODE_RIGHT: return T("Right arrow", "Seta direita");
    case 0: return T("(none)", "(nenhuma)");
    default: return bind_key_label(sc);
    }
}

static void keys_section(void) {
    int cap_dev, cap_slot, ms_left;
    bind_capture_active(&cap_dev, &cap_slot, &ms_left);
    const size_t n = sizeof k_actions / sizeof k_actions[0];
    const char *sl[] = {T("Mouse sensitivity", "Sensibilidade do mouse"), T("Controller stick dead zone", "Zona morta do analógico")};
    const char *labels[sizeof k_actions / sizeof k_actions[0] + 2];  // (one values' column for the keys and the sliders)
    for (size_t i = 0; i < n; i++) labels[i] = T(k_actions[i].en, k_actions[i].pt);
    labels[n] = sl[0], labels[n + 1] = sl[1];
    float x = ui_label_x(labels, (int)n + 2);
    ui_heading(T("Keyboard", "Teclado"));
    for (size_t k = 0; k < n; k++) {
        size_t i = k < n / 2 ? k * 2 : (k - n / 2) * 2 + 1;  // (in pairs in the table: the moves, then the buttons)
        int slot = k_actions[i].slot;
        bool waiting = cap_dev == BIND_DEV_KEY && cap_slot == slot;
        char txt[96];
        if (waiting) snprintf(txt, sizeof txt, "%s %d", T("Press a key...", "Aperte a tecla..."), (ms_left + 999) / 1000);
        else snprintf(txt, sizeof txt, "%s", key_name(g_bind_key[slot]));
        // (waiting: green; a key changed from the default: gold)
        ImU32 col = waiting ? ImGui::GetColorU32(UI_GREEN_TEXT) : !bind_is_default_slot(BIND_DEV_KEY, slot) ? ImGui::GetColorU32(UI_GOLD) : 0;
        ui_row(labels[i], x);
        ImGui::PushID(slot);
        if (ui_value_button("##key", txt, col)) {
            if (waiting) bind_capture_cancel();
            else bind_capture_begin(BIND_DEV_KEY, slot);
        }
        ImGui::PopID();
    }
    ImGui::PushTextWrapPos(0);
    if (cap_dev == BIND_DEV_KEY)
        ImGui::TextDisabled("%s", T("Press the new key. Esc cancels; a key already in use swaps over.",
                                    "Aperte a nova tecla. Esc cancela; uma tecla já usada troca de lugar."));
    ImGui::PopTextWrapPos();
    if (!bind_is_default(BIND_DEV_KEY)) {
        ui_group_gap();
        if (ui_button(T("Restore default keys", "Restaurar teclas padrão"))) bind_reset(BIND_DEV_KEY);
    }
    ui_heading(T("Mouse and controller", "Mouse e controle"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Mouse while aiming a Poké Ball: move = aim, left = throw.",
                                "Mouse na mira da Poké Ball: mover = mirar, esquerdo = arremessar."));
    ImGui::TextDisabled("%s", T("Otherwise: left = touch screen (Play with your partner). Right = B.",
                                "Fora dela: esquerdo = tocar na tela (Brincar com o parceiro). Direito = B."));
    ImGui::PopTextWrapPos();
    ui_row(sl[0], x);
    ui_slider("##mouse", &g_settings.mouse_sens, 0.02f, 0.30f, "%.2f", 0.01f);
    ui_row(sl[1], x);
    ui_slider("##dead", &g_settings.stick_deadzone, 0.0f, 0.40f, "%.2f", 0.01f);
}

// ------------------------------------------------------------------ the interface at 2x (runtime/ui2x.c)
// Made once from the player's own game files on request (the upscaler is downloaded then); used when made and on.
static void ui2x_section(bool ready, bool *on) {
    ui_heading(T("Sharper interface", "Interface mais nítida"));
    int n = ui2x_running() ? 0 : ui2x_count();
    int st = ui2x_state();
    if (ui2x_running()) {
        static const char *const en[] = {"", "Downloading the upscaler (waifu2x, 35 MB)...", "Reading the interface images...",
                                         "Upscaling them (a few minutes)...", "Saving them..."};
        static const char *const pt[] = {"", "Baixando o ampliador (waifu2x, 35 MB)...", "Lendo as imagens da interface...",
                                         "Ampliando (alguns minutos)...", "Salvando..."};
        ImGui::TextUnformatted(st >= UI2X_DOWNLOADING && st <= UI2X_PACKING ? (g_pt ? pt : en)[st] : "...");
        ImGui::ProgressBar(ui2x_progress(), ImVec2(-FLT_MIN, 0));
        return;
    }
    if (n) {
        char label[160];
        snprintf(label, sizeof label, T("Interface images at 2x (%d)", "Imagens da interface em 2x (%d)"), n);
        const char *l[] = {label};
        ui_toggle("##ui2x", label, ui_label_x(l, 1), on);
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("The game's menu and interface images, redrawn at twice their resolution.",
                                    "As imagens dos menus e da interface do jogo, redesenhadas no dobro da resolução."));
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!ready);
    if (ui_button(n ? T("Make them again", "Gerar de novo") : T("Make the 2x images", "Gerar as imagens em 2x"))) ui2x_generate_start();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !ready ? T("Set up the game files first.", "Configure os arquivos do jogo antes.")
                                       : T("From your own game files, with waifu2x (downloaded once from GitHub, 35 MB). Takes a few minutes and about 1 GB in cache/.",
                                           "A partir dos seus arquivos do jogo, com o waifu2x (baixado uma vez do GitHub, 35 MB). Leva alguns minutos e ocupa cerca de 1 GB em cache/."));
    if (st >= UI2X_FAIL_DOWNLOAD) {
        static const char *const en[] = {"Couldn't download the upscaler: check the connection.",
                                         "The upscaler failed (details in lasrecomp.log): try again.",
                                         "Couldn't read the game files or write cache/."};
        static const char *const pt[] = {"Não foi possível baixar o ampliador: confira a conexão.",
                                         "O ampliador falhou (detalhes no lasrecomp.log): tente de novo.",
                                         "Não foi possível ler os arquivos do jogo ou gravar em cache/."};
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(UI_RED_TEXT, "%s", (g_pt ? pt : en)[st - UI2X_FAIL_DOWNLOAD]);
        ImGui::PopTextWrapPos();
    }
}

// ------------------------------------------------------------------ file dialogs (SDL calls back on any thread)
static pthread_mutex_t g_pick_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pick_game[1024], g_pick_mod[1024];
static void SDLCALL picked_game(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_pick_lock);
    snprintf(g_pick_game, sizeof g_pick_game, "%s", files[0]);
    pthread_mutex_unlock(&g_pick_lock);
}
static void SDLCALL picked_mod(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_pick_lock);
    snprintf(g_pick_mod, sizeof g_pick_mod, "%s", files[0]);
    pthread_mutex_unlock(&g_pick_lock);
}
static bool take(char *slot, char *out, size_t n) {
    pthread_mutex_lock(&g_pick_lock);
    bool got = slot[0] != 0;
    if (got) snprintf(out, n, "%s", slot), slot[0] = 0;
    pthread_mutex_unlock(&g_pick_lock);
    return got;
}

// ------------------------------------------------------------------ the game files section
static const ImVec4 &k_green = UI_GREEN_TEXT, &k_red = UI_RED_TEXT;

static void game_files_section(SDL_Window *win) {
    ui_heading(T("Game files", "Arquivos do jogo"));
    int st = setup_state();
    if (setup_running()) {
        static const char *const en[] = {"", "Looking for the game files...", "Copying the game files...",
                                         "Building the game data image...", "Reading the game code...",
                                         "Recompiling the game code...", "Finishing..."};
        static const char *const pt[] = {"", "Procurando os arquivos do jogo...", "Copiando os arquivos do jogo...",
                                         "Montando a imagem de dados do jogo...", "Lendo o código do jogo...",
                                         "Recompilando o código do jogo...", "Finalizando..."};
        ImGui::TextUnformatted(st >= 1 && st <= 6 ? T(en[st], pt[st]) : "...");
        ImGui::ProgressBar(setup_progress(), ImVec2(-FLT_MIN, 0));
        ImGui::TextDisabled("%s", T("This takes a while (10-30 minutes), only once. The game can be played when it's done.",
                                    "Isso demora (de 10 a 30 minutos), só uma vez. O jogo fica liberado quando terminar."));
        return;
    }
    bool ready = setup_ready();
    if (ready) {
        const char *name = game_name("extracted/exefs");
        ImGui::TextColored(k_green, g_pt ? "Pokémon: Let's Go, %s! - pronto para jogar" : "Pokémon: Let's Go, %s! - ready to play",
                           name ? name : "Eevee");
        if (ui_button(T("Change folder...", "Trocar pasta..."))) SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
        return;
    }
    if (st >= SETUP_FAIL_NOT_FOUND) {
        static const char *const en[] = {"No game files found in that folder.", "Those are packed game files (.xci / .nsp).",
                                         "That's not Pokémon: Let's Go, Eevee! or Pikachu!.", "The files couldn't be copied.",
                                         "The game code didn't build.", "The game code builder is missing from this copy.",
                                         "The game's data image (romfs.bin) can't be read."};
        static const char *const pt[] = {"Nenhum arquivo do jogo nessa pasta.", "Esses são arquivos do jogo empacotados (.xci / .nsp).",
                                         "Isso não é o Pokémon: Let's Go, Eevee! nem o Pikachu!.", "Os arquivos não puderam ser copiados.",
                                         "O código do jogo não compilou.", "O compilador do código do jogo não está nesta cópia.",
                                         "A imagem de dados do jogo (romfs.bin) não pode ser lida."};
        static const char *const en_h[] = {"Choose the folder that holds exefs and romfs.", "Extract them first (exefs and romfs).",
                                           "Pick the folder of your own extracted Let's Go, Eevee! or Pikachu!.", "Check there are about 6 GB free and try again.",
                                           "Check there are about 2 GB free and try again; details in lasrecomp.log.", "Download the release again.",
                                           "Pick the folder with the romfs folder itself (not a romfs.bin from another tool); details in lasrecomp.log."};
        static const char *const pt_h[] = {"Escolha a pasta que tem exefs e romfs.", "Extraia antes (exefs e romfs).",
                                           "Escolha a pasta do seu Let's Go, Eevee! ou Pikachu! extraído.", "Confira se há uns 6 GB livres e tente de novo.",
                                           "Confira se há uns 2 GB livres e tente de novo; detalhes no lasrecomp.log.", "Baixe a release de novo.",
                                           "Escolha a pasta com a própria pasta romfs (não um romfs.bin de outra ferramenta); detalhes no lasrecomp.log."};
        int k = st - SETUP_FAIL_NOT_FOUND;
        ImGui::TextColored(k_red, "%s", T(en[k], pt[k]));
        ImGui::PushTextWrapPos(0);  // (the longer hints wrap instead of running past the page)
        ImGui::TextDisabled("%s", T(en_h[k], pt_h[k]));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(T("Select the folder with your own extracted Pokémon: Let's Go, Eevee! or Pikachu! (exefs and romfs).",
                                 "Selecione a pasta com o seu Pokémon: Let's Go, Eevee! ou Pikachu! extraído (exefs e romfs)."));
        ImGui::PopTextWrapPos();
    }
    if (ui_button(T("Select game folder...", "Selecionar pasta do jogo..."), -1))
        SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
}

// ------------------------------------------------------------------ the Mods tab
// The folders under mods/ that hold a romfs/ (runtime/mods.h): one active at a time, or none (the game as it is)
static const char *lang_name(const char *code) {
    static const struct { const char *code, *en, *pt; } k[] = {
        {"en-US", "English", "inglês"}, {"es", "Spanish", "espanhol"}, {"fr", "French", "francês"}, {"de", "German", "alemão"},
        {"it", "Italian", "italiano"}, {"ja", "Japanese", "japonês"}, {"ko", "Korean", "coreano"},
        {"zh-CN", "Simplified Chinese", "chinês simplificado"}, {"zh-TW", "Traditional Chinese", "chinês tradicional"}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(k[i].code, code)) return T(k[i].en, k[i].pt);
    return code;
}

// The mods in their order (the active ones first, as larecomp.ini lists them: "<name>|<name>..."), and which are on
static char g_mods[MODS_MAX][128];
static bool g_mod_on[MODS_MAX];
static int g_nmods;
// Reads mods/ again: the names in `active` ('|' between them) on and first, in that order; the rest after, off
static void mods_reload(const char *active) {
    char found[MODS_MAX][128];
    int n = mods_list(found, MODS_MAX);
    bool used[MODS_MAX] = {};
    g_nmods = 0;
    char list[sizeof g_settings.mod];
    snprintf(list, sizeof list, "%s", active);
    for (char *save = NULL, *name = strtok_r(list, "|", &save); name; name = strtok_r(NULL, "|", &save))
        for (int i = 0; i < n; i++)
            if (!used[i] && !strcmp(found[i], name)) {
                used[i] = true;
                snprintf(g_mods[g_nmods], sizeof g_mods[0], "%s", found[i]);
                g_mod_on[g_nmods++] = true;
            }
    for (int i = 0; i < n; i++)
        if (!used[i]) snprintf(g_mods[g_nmods], sizeof g_mods[0], "%s", found[i]), g_mod_on[g_nmods++] = false;
}
// The active ones, in order, as larecomp.ini keeps them
static void mods_active(char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < g_nmods; i++)
        if (g_mod_on[i]) {
            if (out[0]) strncat(out, "|", cap - strlen(out) - 1);
            strncat(out, g_mods[i], cap - strlen(out) - 1);
        }
}

static void mods_section(SDL_Window *win, int inst) {
    static const SDL_DialogFileFilter filters[] = {{"Mod (.zip, .rar, .7z)", "zip;rar;7z"}};
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(T("Mods that replace the game's files (a romfs folder, as Atmosphere's LayeredFS): translations, textures, models. "
                             "Mods of the game's code (exefs, IPS patches, cheats) don't work in this port.",
                             "Mods que trocam arquivos do jogo (uma pasta romfs, como o LayeredFS do Atmosphere): traduções, texturas, modelos. "
                             "Mods do código do jogo (exefs, patches IPS, cheats) não funcionam neste port."));
    ImGui::PopTextWrapPos();
    ui_heading(T("Mods", "Mods"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", T("Turn on the ones to use (several at once). Where two change the same file, the higher one wins: the arrows move it.",
                                "Ligue os que vão ser usados (vários ao mesmo tempo). Se dois trocam o mesmo arquivo, vale o de cima: as setas mudam a ordem."));
    ImGui::PopTextWrapPos();
    // each mod a row: its name (cut short with "..." when long: the whole of it in the tip), an on / off switch and the
    // arrows that move it up / down at the row's right end (bare on the pill; a circle under the mouse)
    int move = -1, dir = 0;
    ImGui::PushStyleColor(ImGuiCol_Button, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    for (int i = 0; i < g_nmods; i++) {
        ImGui::PushID(i);
        ui_pill_row();
        ImGuiWindow *w = ImGui::GetCurrentWindow();
        ImDrawList *dl = w->DrawList;
        float h = ImGui::GetFrameHeight(), sp = ImGui::GetStyle().ItemSpacing.x, gap = sp * 0.5f, sw = h * 1.45f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float end = w->WorkRect.Max.x - ui_inset() * 0.6f, ax = end - h * 2 - gap, sx = ax - sp - sw, nmax = sx - sp * 1.5f;
        ImVec2 ts = ImGui::CalcTextSize(g_mods[i]);
        float ty = p.y + (h - ts.y) * 0.5f;
        ImGui::RenderTextEllipsis(dl, ImVec2(p.x, ty), ImVec2(nmax, ty + ts.y), nmax, nmax, g_mods[i], NULL, &ts);
        // the switch: green with the knob at the right when on
        ImGui::SetCursorScreenPos(ImVec2(sx, p.y));
        if (ImGui::InvisibleButton("##on", ImVec2(sw, h))) g_mod_on[i] = !g_mod_on[i];
        const char *lang = mod_language(g_mods[i]);
        char tip[320];
        if (lang) snprintf(tip, sizeof tip, g_pt ? "%s: troca os textos em %s." : "%s: replaces the %s texts.", g_mods[i], lang_name(lang));
        else snprintf(tip, sizeof tip, "%s", g_mods[i]);
        ui_tip(tip);
        float th = h * 0.62f, cy = p.y + h * 0.5f, kr = th * 0.5f - 3 * g_scale, kx = g_mod_on[i] ? sx + sw - th * 0.5f : sx + th * 0.5f;
        dl->AddRectFilled(ImVec2(sx, cy - th * 0.5f), ImVec2(sx + sw, cy + th * 0.5f),
                          g_mod_on[i] ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : ImGui::GetColorU32(IM_COL32(200, 208, 176, 255)), th * 0.5f);
        dl->AddCircleFilled(ImVec2(kx, cy), kr, ImGui::GetColorU32(IM_COL32_WHITE));
        ImGui::SetCursorScreenPos(ImVec2(ax, p.y));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move = i, dir = -1;
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(i == g_nmods - 1);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move = i, dir = 1;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (move >= 0) {  // (swapped with the one above / below it)
        int j = move + dir;
        char name[128];
        bool on = g_mod_on[move];
        memcpy(name, g_mods[move], sizeof name), memcpy(g_mods[move], g_mods[j], sizeof name), memcpy(g_mods[j], name, sizeof name);
        g_mod_on[move] = g_mod_on[j], g_mod_on[j] = on;
    }
    if (!g_nmods) ImGui::TextDisabled("%s", T("No mods in the mods folder yet: the game as it is.", "Nenhum mod na pasta mods ainda: o jogo original."));
    ui_group_gap();
    if (inst == MOD_INSTALL_RUNNING) ImGui::TextDisabled("%s", T("Installing the mod...", "Instalando o mod..."));
    else if (ui_button(T("Install a mod...", "Instalar mod..."))) SDL_ShowOpenFileDialog(picked_mod, NULL, win, filters, 1, NULL, false);
    ImGui::SameLine();
    if (ui_button(T("Open the mods folder", "Abrir a pasta de mods"))) {
        char url[1200];
        snprintf(url, sizeof url, "file:///%s", mods_dir_abs());
        for (char *c = url; *c; c++) if (*c == '\\') *c = '/';
        SDL_OpenURL(url);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("A mod copied there by hand (its folder, with a romfs inside) shows up here when you come back.",
                                  "Um mod copiado para lá à mão (a pasta dele, com uma romfs dentro) aparece aqui quando você voltar."));
    if (inst == MOD_INSTALL_FAIL_UNPACK || inst == MOD_INSTALL_FAIL_NOT_MOD)
        ImGui::TextColored(k_red, "%s", inst == MOD_INSTALL_FAIL_UNPACK ? T("Couldn't unpack that file.", "Não foi possível abrir esse arquivo.")
                                                                        : T("No mod for this game in that file (no romfs folder).",
                                                                            "Esse arquivo não tem mod para este jogo (nenhuma pasta romfs)."));
}

// ------------------------------------------------------------------ the launcher's window
// A launcher's own window while the start screen is up: no system frame (dragged by its top strip; its own minimize and
// close buttons), taller than wide, in the middle of the screen. Play gives the game its window back: the frame, the
// size of the resolution chosen (Automatic: the game's 1280 x 720; device.c ui_window_game_size) and full screen when
// that setting is on.
static float g_drag_h, g_buttons_w;  // (logical pixels: the strip that drags the window, the buttons at its right end)
static SDL_HitTestResult SDLCALL launcher_hit(SDL_Window *win, const SDL_Point *pt, void *) {
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    return pt->y < g_drag_h && pt->x < w - g_buttons_w ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}
static void window_as_launcher(SDL_Window *win) {
#ifndef __ANDROID__  // (a phone has no window: always the whole screen)
    SDL_SetWindowFullscreen(win, false);
#endif
    SDL_Rect usable = {0, 0, 1920, 1040};
    SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable);
    int lh = usable.h * 86 / 100 < 900 ? usable.h * 86 / 100 : 900, lw = lh * 86 / 100;
    SDL_SetWindowBordered(win, false);
    SDL_SetWindowResizable(win, false);
    SDL_SetWindowSize(win, lw, lh);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowHitTest(win, launcher_hit, NULL);
}
static void window_as_game(SDL_Window *win) {
    SDL_SetWindowHitTest(win, NULL, NULL);
    SDL_SetWindowBordered(win, true);
    SDL_SetWindowResizable(win, true);
    int gw, gh;
    ui_window_game_size(win, &gw, &gh);
    SDL_SetWindowSize(win, gw, gh);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(win, true);
    // black at the new size until the game draws (else the launcher's last image stayed in a corner while it loads);
    // vk_frame_begin makes the swapchain again for the new size
    SDL_SyncWindow(win);
    SDL_PumpEvents();
    static const float black[4] = {0, 0, 0, 1};
    for (int i = 0; i < 3; i++)
        if (vk_frame_begin(black)) vk_frame_end(NULL);
}

// The project's icon (the Master Ball: icon.png beside the exe, or docs/icon), for the sidebar
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
static ImTextureID icon_texture(void) {
    char p[1100];
    if (!rt_resource("icon.png", "docs/icon/icon_1024.png", p, sizeof p)) return 0;
    SDL_Surface *s = SDL_LoadPNG(p);
    if (!s) return 0;
    for (int size = s->w / 2; size >= 256; size /= 2) {  // (halved in steps: smooth at the size it is drawn)
        SDL_Surface *t = SDL_ScaleSurface(s, size, size, SDL_SCALEMODE_LINEAR);
        if (!t) break;
        SDL_DestroySurface(s);
        s = t;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);  // (R, G, B, A bytes in memory)
    SDL_DestroySurface(s);
    if (!rgba) return 0;
    ImTextureID id = (ImTextureID)vk_imgui_texture_rgba(rgba->pixels, rgba->w, rgba->h);
    SDL_DestroySurface(rgba);
    return id;
}

// A window button in the top right corner (minimize: a line, close: an X), drawn so no font is needed
static bool window_button(const char *id, bool close, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered())
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), close ? IM_COL32(196, 43, 28, 255) : IM_COL32(255, 255, 255, 150), size * 0.2f);
    ImU32 col = close && ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : IM_COL32(60, 62, 56, 255);
    float m = size * 0.34f, t = size * 0.06f > 1 ? size * 0.06f : 1;
    if (close) {
        dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + size - m, p.y + size - m), col, t);
        dl->AddLine(ImVec2(p.x + size - m, p.y + m), ImVec2(p.x + m, p.y + size - m), col, t);
    } else {
        dl->AddLine(ImVec2(p.x + m, p.y + size * 0.5f), ImVec2(p.x + size - m, p.y + size * 0.5f), col, t);
    }
    return hit;
}

// ------------------------------------------------------------------ the start screen
extern "C" void launcher_lg_run(const char *root) {
    (void)root;
    ui_pick_language();
    g_pt = g_ui_lang == 1;
    ui_window_init();
    SDL_Window *win = ui_window();
    window_as_launcher(win);
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // (the in-game menu's, when back here after a reboot)
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    UiFonts fonts = ui_load_fonts();
    ui_style();
    UiScaler scaler;
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
    ImTextureID icon = icon_texture();

    static const char *const res_en[] = {"Automatic", "720p", "1080p", "1440p", "4K"};
    static const char *const res_pt[] = {"Automática", "720p", "1080p", "1440p", "4K"};
    static const int res_lines[] = {0, 720, 1080, 1440, 2160};
    int res = 0;
    for (int i = 0; i < 5; i++) if (res_lines[i] == g_settings.res_height) res = i;
    bool ui2x_on = g_settings.ui2x;  // (the interface at 2x: runtime/ui2x.c)
    bool full = g_settings.fullscreen, wildb = g_settings.wild_battle, skipc = g_settings.skip_controller, uwide = g_settings.ultrawide,
         gyro = g_settings.gyro_aim;
    mods_reload(g_settings.mod);  // (the folders under mods/: runtime/mods.h)
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    bool autoplay = getenv("LARECOMP_LAUNCHER_AUTOPLAY") != NULL, play = false;
    const char *keys_test = getenv("LARECOMP_LAUNCHER_KEYS");
    enum { TAB_GAME, TAB_GRAPHICS, TAB_IMAGE, TAB_CONTROLS, TAB_MODS, NTABS };
    int tab = getenv("LARECOMP_LAUNCHER_TAB") ? atoi(getenv("LARECOMP_LAUNCHER_TAB")) : keys_test ? TAB_CONTROLS : TAB_GAME;
    if (keys_test && atoi(keys_test) > 0) bind_capture_begin(BIND_DEV_KEY, atoi(keys_test));
    if (getenv("LARECOMP_SETUP_DIR")) setup_start(getenv("LARECOMP_SETUP_DIR"));
    if (getenv("LARECOMP_MOD_INSTALL")) mod_install_start(getenv("LARECOMP_MOD_INSTALL"));
    static const float bg[4] = {0.86f, 0.93f, 0.47f, 1};  // (the F12 menu's look: ui_backdrop over it)
    const ImVec4 side_bg(1, 1, 1, 0.55f), gold(0.98f, 0.82f, 0.22f, 1), gold_hover(1.0f, 0.88f, 0.38f, 1),
        gold_text(0.23f, 0.18f, 0.03f, 1);

    while (!play) {
        menu_apply_window_requests(win);
        SDL_Event e;
        bool focus_back = false;
        while (SDL_PollEvent(&e)) {
            if (bind_capture_event(&e)) continue;  // (waiting for a new key: the press is the binding's)
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focus_back = true;
        }
        char path[1024];
        if (take(g_pick_game, path, sizeof path)) setup_start(path);
        {   // a new version over set-up game files: its game code is rebuilt by itself (once: a failure shows and waits)
            static bool rebuilt;
            if (!rebuilt && !setup_running() && setup_needs_build()) {
                rebuilt = true;
                setup_start(NULL);
            }
        }
        if (take(g_pick_mod, path, sizeof path)) mod_install_start(path);
        static int last_install;
        int inst = mod_install_state();
        if ((inst != last_install && inst == MOD_INSTALL_DONE) || focus_back) {  // a new mod (installed, or copied by hand)
            char was[sizeof g_settings.mod], want[sizeof g_settings.mod + 130];
            mods_active(was, sizeof was);
            if (inst != last_install && inst == MOD_INSTALL_DONE) snprintf(want, sizeof want, "%s%s%s", mod_install_name(), was[0] ? "|" : "", was);
            else snprintf(want, sizeof want, "%s", was);  // (a new one installed: on, and first)
            mods_reload(want);
        }
        last_install = inst;
        bool ready = !setup_running() && setup_ready();
        {   // (tests, LARECOMP_UI2X_GENERATE=1: the 2x interface images made, then exit - status 0 when done)
            static int gen = -1;
            if (gen < 0) gen = getenv("LARECOMP_UI2X_GENERATE") != NULL;
            if (gen == 1 && ready) { ui2x_generate_start(); gen = 2; }
            else if (gen == 2 && !ui2x_running()) exit(ui2x_state() == UI2X_DONE ? 0 : 1);
        }
        if (autoplay && ready && inst != MOD_INSTALL_RUNNING) play = true;
        if (!vk_frame_begin(bg)) { SDL_Delay(16); continue; }
        if (keys_test && strchr(keys_test, ':') && frame == 5) {  // "<slot>:<key name>": the key pressed for the test
            SDL_Event k = {};
            k.type = SDL_EVENT_KEY_DOWN;
            k.key.scancode = SDL_GetScancodeFromName(strchr(keys_test, ':') + 1);
            k.key.down = true;
            SDL_PushEvent(&k);
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        // (the launcher is narrow: its fonts a little smaller than the F12 menu's at the same height; on a phone's dense
        // screen, twice as large)
#ifdef __ANDROID__
        const float zoom = 2.0f;
#else
        const float zoom = 1.0f;
#endif
        g_scale = ui_scale(scaler, io.DisplaySize.y * 0.86f * zoom);
        ImGui::NewFrame();
        float s = g_scale;
        bool known = !setup_ready() || game_build("extracted/exefs") != GAME_BUILD_OTHER;
        const char *only = T("Only with version 1.0.0 (the base game) of Eevee or Pikachu for now.",
                             "Por enquanto só na versão 1.0.0 (o jogo base) do Eevee ou do Pikachu.");
        const char *names[NTABS] = {T("Game", "Jogo"), T("Graphics", "Gráficos"), T("Image and sound", "Imagem e som"),
                                    T("Controls", "Controles"), "Mods"};
        ui_tabs_begin(&tab, NTABS);  // (L1 / R1 on a controller: the page before / after)

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg[0], bg[1], bg[2], 1));
        ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ui_window_backdrop();
        float pad = 22 * s, side_w = 270 * s;
        g_drag_h = 70 * s, g_buttons_w = 120 * s;

        // ---- the sidebar: the icon and the name, the pages, Play
        ImGui::PushStyleColor(ImGuiCol_ChildBg, side_bg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::BeginChild("##side", ImVec2(side_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar);
        {
            {   // the icon and the name in two lines beside it, both centred on one height (Baloo's lines are tall:
                // the two are drawn closer than its line height)
                ImGui::PushFont(fonts.title);
                float fs = ImGui::GetFontSize(), step = fs * 0.78f, block = fs * 1.75f;
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                if (icon) dl->AddImage(icon, p, ImVec2(p.x + block, p.y + block));
                float tx = p.x + block + 12 * s, ty = p.y + (block - step - fs) * 0.5f;
                dl->AddText(ImGui::GetFont(), fs, ImVec2(tx, ty), ImGui::GetColorU32(UI_GOLD), "Let's Go");
                dl->AddText(ImGui::GetFont(), fs, ImVec2(tx, ty + step), ImGui::GetColorU32(UI_GOLD), "Recomp");
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(side_w - pad * 2, block));
            }
            ImGui::TextDisabled("v" RT_APP_VERSION);
            ImGui::Spacing();
            // the pages: pills as tall as the rows, the current one yellow, a white one under the cursor; the names on
            // the rows' text inset
            float row_h = ImGui::GetFrameHeight(), in = ui_inset();
            for (int t = 0; t < NTABS; t++) {
                ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + ImGui::GetContentRegionAvail().x, a.y + row_h);
                ImGui::PushID(t);
                if (ImGui::InvisibleButton("##nav", ImVec2(b.x - a.x, row_h))) tab = t;
                ImGui::PopID();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                bool hot = ui_hot(a, b);
                if (tab == t || hot) ui_pill(dl, a, b, tab == t);
                ImVec2 ts = ImGui::CalcTextSize(names[t]);
                dl->AddText(ImVec2(a.x + in, a.y + (row_h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), names[t]);
            }
            float play_h = 64 * s;
            ImGui::SetCursorPosY(fmaxf(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - pad - play_h));
            ImGui::PushStyleColor(ImGuiCol_Button, gold);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, gold_hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, gold_hover);
            ImGui::PushStyleColor(ImGuiCol_Text, gold_text);
            ImGui::PushFont(fonts.title);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            ImGui::BeginDisabled(!ready);
            if (frame == 0 && ready) ImGui::SetKeyboardFocusHere();
            if (ImGui::Button(T("Play", "Jogar"), ImVec2(-FLT_MIN, play_h))) play = true;
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::PopFont();
            ImGui::PopStyleColor(4);
            if (!ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", T("Set up the game files first (Game page).", "Configure os arquivos do jogo antes (página Jogo)."));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // ---- the page
        ImGui::SameLine(0, 0);
        float cpad = 24 * s;  // (the content's sides: room for the cursor's arrow at the rows' left end)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cpad, pad));
        ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar);
        {
            ui_title(fonts.title, names[tab]);
#ifndef __ANDROID__  // (a phone has no window to minimise or close: the system's own buttons)
            {   // the window's buttons in its top right corner, side by side on one line (placed, not flowed after the title)
                ImVec2 back = ImGui::GetCursorScreenPos(), wp = ImGui::GetWindowPos();
                float bsz = 40 * s, gap = 6 * s, x = wp.x + ImGui::GetWindowWidth() - cpad - bsz * 2 - gap, y = wp.y + pad * 0.6f;
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                if (window_button("##min", false, bsz)) SDL_MinimizeWindow(win);
                ImGui::SetCursorScreenPos(ImVec2(x + bsz + gap, y));
                if (window_button("##close", true, bsz)) exit(0);
                ImGui::SetCursorScreenPos(back);
            }
#endif
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16 * s - ImGui::GetStyle().ItemSpacing.y);  // (16 px to the page)
            ui_page_begin("##page", ImVec2(0, 0));
            if (tab == TAB_GAME) {
                game_files_section(win);
                ui_heading(T("Options", "Opções"));
                // A / B and X / Y by the layout's positions (runtime/hle/bindings.c bind_set_layout)
                const char *ol[] = {T("Wild battles (experimental)", "Batalhas selvagens (experimental)"),
                                    T("Skip controller selection", "Pular a escolha do controle"), T("Controller layout", "Layout do controle"),
                                    T("Gyro aiming", "Mirar com giroscópio")};
                float ox = ui_label_x(ol, 4);
                ImGui::BeginDisabled(!known);
                ui_toggle("##wild", ol[0], ox, &wildb);
                ui_tip(!known ? only : T("Touching a wild Pokémon starts a classic battle instead of going straight to the capture. "
                                         "Its CATCH button throws one Poké Ball, with better odds the weaker the Pokémon is.",
                                         "Encostar num Pokémon selvagem começa uma batalha clássica em vez de ir direto para a captura. "
                                         "O botão de captura (CATCH) joga uma Poké Bola, com mais chance quanto mais fraco o Pokémon estiver."));
                ui_toggle("##skipc", ol[1], ox, &skipc);
                ui_tip(!known ? only : T("The game starts at the Game Freak logo, with your controller already chosen, instead of asking for a button press first.",
                                         "O jogo começa no logo da Game Freak, com o seu controle já escolhido, em vez de pedir antes que se aperte um botão."));
                ImGui::EndDisabled();
                const char *lay[] = {"Nintendo", "Xbox"};
                int lv = bind_layout();
                ui_row(ol[2], ox);
                if (ui_combo("##padlayout", &lv, lay, 2)) bind_set_layout(lv);
                ui_toggle("##gyro", ol[3], ox, &gyro);
                ui_tip(T("Aiming a Poké Ball turns with the controller's gyroscope (DualSense, DualShock 4, Switch Pro, Joy-Con) or, without one, the phone's or tablet's - as in handheld mode on the Switch. The stick and the mouse still aim.",
                         "Ao mirar uma Poké Ball, a mira gira com o giroscópio do controle (DualSense, DualShock 4, Switch Pro, Joy-Con) ou, sem ele, o do celular ou tablet - como no modo portátil do Switch. O analógico e o mouse continuam mirando."));
                ui_tip(T("Which controller buttons are A / B and X / Y. Nintendo: A right, B bottom. Xbox: A bottom, B right, as the letters on an Xbox controller. The button prompts follow.",
                         "Quais botões do controle são A / B e X / Y. Nintendo: A à direita, B embaixo. Xbox: A embaixo, B à direita, como as letras de um controle de Xbox. Os ícones dos botões acompanham."));
            } else if (tab == TAB_GRAPHICS) {
                const char *extra[] = {T("Resolution", "Resolução"), T("Fullscreen", "Tela cheia"),
                                       T("Ultrawide support (experimental)", "Suporte ultrawide (experimental)")};
                float x = ui_graphics_x(extra, 3);
                ui_row(extra[0], x);
                ui_combo("##res", &res, g_pt ? res_pt : res_en, 5);
                ui_toggle("##full", extra[1], x, &full);
                ui_tip(T("When the game starts.", "Ao começar o jogo."));
                ui_toggle("##ultrawide", extra[2], x, &uwide);
                ui_tip(T("A screen wider than 16:9 is filled with the game's image instead of black bars at its sides; the interface stays in a centred 16:9 box.",
                         "Uma tela mais larga que 16:9 é preenchida com a imagem do jogo em vez de faixas pretas nas laterais; a interface fica numa área 16:9 centralizada."));
                ui_group_gap();
                ui_page_graphics(x, known);
                ui2x_section(ready, &ui2x_on);
            } else if (tab == TAB_IMAGE) {
                ui_page_image_sound();
            } else if (tab == TAB_CONTROLS) {
                keys_section();
            } else {
                mods_section(win, inst);
            }
            ui_page_end();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();

        ui_frame_end();
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
        bool shot = shot_at && ++frame == shot_at;
        if (!shot_at) frame++;
        vk_frame_end(shot ? (getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png") : NULL);
        if (shot) exit(0);
    }
    g_settings.res_height = res_lines[res];
    g_settings.fullscreen = full;
    settings_apply_levels();  // (the upscaler follows the anti-aliasing)
    g_settings.wild_battle = wildb;
    g_settings.skip_controller = skipc;
    g_settings.gyro_aim = gyro;
    g_settings.ultrawide = uwide;
    g_settings.ui2x = ui2x_on;
    mods_active(g_settings.mod, sizeof g_settings.mod);
    settings_save();
    vk_imgui_backend_shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
    window_as_game(win);
}
