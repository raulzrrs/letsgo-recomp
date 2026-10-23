// The look and the settings pages shared by the start screen and the F12 menu (runtime/ui/settings_ui.h).
#define IMGUI_DEFINE_MATH_OPERATORS
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_ui.h"
#include "imgui_internal.h"

extern "C" {
#include "../paths.h"
#include "../settings.h"
}

// ------------------------------------------------------------------ language
int g_ui_lang;
void ui_pick_language(void) {
    const char *e = getenv("LARECOMP_LANG"), *lang = e && *e ? e : "";
    SDL_Locale **l = NULL;
    int n = 0;
    if (!*lang && (l = SDL_GetPreferredLocales(&n)) && n > 0 && l[0]->language) lang = l[0]->language;
    g_ui_lang = !strncmp(lang, "pt", 2) ? 1 : !strncmp(lang, "ru", 2) ? 2 : 0;
    SDL_free(l);
}

// ------------------------------------------------------------------ look
// After the game's own menus (its Options screen): a lime background with big pale gears, slow diagonal stripes and
// halftone dots; each setting a white pill with dark text, the one under the cursor yellow with a bobbing arrow at its
// left and < > around its value; a description box with a rainbow frame. All of it drawn here from shapes and
// gradients: nothing of the game's.
const ImVec4 UI_GOLD(0.85f, 0.47f, 0.04f, 1), UI_GREEN_TEXT(0.12f, 0.50f, 0.16f, 1), UI_RED_TEXT(0.80f, 0.16f, 0.12f, 1),
    UI_TEXT(0.23f, 0.23f, 0.22f, 1), UI_HEADING(0.26f, 0.42f, 0.07f, 1);

UiFonts ui_load_fonts(void) {
    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    static const ImWchar ranges[] = {0x20, 0xFF, 0x400, 0x45F, 0};  // (Latin-1: Portuguese accents; Cyrillic)
    UiFonts f = {};
    // (Segoe UI Semibold first: dark text on light pills, as the game's)
    const char *paths[] = {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *fp = fopen(p, "rb")) fclose(fp);  // (a missing file is an assertion in ImGui, not a NULL return)
        else continue;
        f.body = io.Fonts->AddFontFromFileTTF(p, 34, &cfg, ranges);
        f.big = io.Fonts->AddFontFromFileTTF(p, 56, &cfg, ranges);
        break;
    }
    char baloo[1024];
    if (rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo))
        f.title = io.Fonts->AddFontFromFileTTF(baloo, 50, &cfg, ranges);
    if (!f.body) f.body = io.Fonts->AddFontDefault();
    if (!f.big) f.big = f.body;
    if (!f.title || g_ui_lang == 2) f.title = f.body;
    return f;
}

void ui_style(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsLight();
    // (rounding past half the height: pills)
    s.WindowRounding = 30, s.ChildRounding = 12, s.PopupRounding = 18, s.FrameRounding = 40, s.GrabRounding = 40;
    s.TabRounding = 40, s.ScrollbarRounding = 40;
    s.WindowPadding = ImVec2(32, 26), s.FramePadding = ImVec2(18, 10), s.ItemSpacing = ImVec2(14, 12);
    s.ItemInnerSpacing = ImVec2(10, 8), s.ScrollbarSize = 16, s.GrabMinSize = 26;
    s.WindowBorderSize = 0, s.ChildBorderSize = 0, s.FrameBorderSize = 2, s.PopupBorderSize = 3, s.TabBarBorderSize = 0;
    ImVec4 *c = s.Colors;
    ImVec4 white(1, 1, 1, 0.94f), yellow(0.98f, 0.88f, 0.33f, 1), yellow_hi(0.99f, 0.93f, 0.50f, 1), yellow_dark(0.95f, 0.79f, 0.20f, 1);
    c[ImGuiCol_Text] = UI_TEXT;
    c[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.45f, 0.36f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.86f, 0.93f, 0.47f, 0.98f);  // (under ui_backdrop)
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(1, 1, 0.97f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.95f, 0.84f, 0.30f, 1);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(0.93f, 0.96f, 0.84f, 1);
    c[ImGuiCol_FrameBgHovered] = yellow_hi;
    c[ImGuiCol_FrameBgActive] = yellow;
    c[ImGuiCol_Button] = white;
    c[ImGuiCol_ButtonHovered] = yellow_hi;
    c[ImGuiCol_ButtonActive] = yellow_dark;
    c[ImGuiCol_Header] = yellow_hi;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.99f, 0.94f, 0.62f, 1);
    c[ImGuiCol_HeaderActive] = yellow;
    c[ImGuiCol_CheckMark] = ImVec4(0.20f, 0.56f, 0.16f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.62f, 0.22f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.58f, 0.10f, 1);
    c[ImGuiCol_Tab] = white;
    c[ImGuiCol_TabHovered] = yellow_hi;
    c[ImGuiCol_TabSelected] = yellow;
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = ImVec4(1, 1, 1, 0.75f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(1, 1, 1, 0.35f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.33f, 0.16f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.29f, 0.43f, 0.20f, 1);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.17f, 0.27f, 0.12f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.62f, 0.22f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(1, 1, 1, 0.5f);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.2f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.98f, 0.82f, 0.25f, 0.5f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // (the focus: the arrow, ui_frame_end, and the yellow row or button)
}

float ui_scale(UiScaler &s, float window_h) {
    float scale = window_h / 1080.0f;
    if (scale < 0.45f) scale = 0.45f;
    ImGui::GetIO().FontGlobalScale = scale;
    if (s.scale < 0) s.base = ImGui::GetStyle();
    if (fabsf(s.scale - scale) > 0.01f) {
        ImGui::GetStyle() = s.base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        s.scale = scale;
    }
    return scale;
}

// ---- drawing
static ImU32 mix(ImU32 a, ImU32 b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ImU32 r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float x = (float)((a >> sh) & 0xFF), y = (float)((b >> sh) & 0xFF);
        r |= (ImU32)(x + (y - x) * t + 0.5f) << sh;
    }
    return r;
}
// The vertices from v0 on coloured by their position (f), keeping their alpha (the anti-aliased edges) times f's
template <class F> static void shade(ImDrawList *dl, int v0, F f) {
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert &v = dl->VtxBuffer[i];
        ImU32 c = f(v.pos), a = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) * ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255;
        v.col = (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}
static float ui_px(void) { return ImGui::GetIO().FontGlobalScale; }  // 1 at 1080 px

static ImVec2 polar(ImVec2 c, float r, float a) { return ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r); }
static void gear(ImDrawList *dl, ImVec2 c, float r, int teeth, float angle, ImU32 col) {
    dl->AddCircle(c, r * 0.635f, col, 0, r * 0.33f);  // the rim (0.47 - 0.80 r)
    for (int i = 0; i < teeth; i++) {
        float a = angle + i * 2 * IM_PI / teeth, hw = IM_PI / teeth * 0.55f;
        ImVec2 p[4] = {polar(c, r * 0.80f, a - hw), polar(c, r, a - hw * 0.72f), polar(c, r, a + hw * 0.72f), polar(c, r * 0.80f, a + hw)};
        dl->AddConvexPolyFilled(p, 4, col);
    }
    dl->AddCircleFilled(c, r * 0.22f, col);
}

void ui_backdrop(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding) {
    float s = ui_px(), t = (float)ImGui::GetTime(), w = b.x - a.x, h = b.y - a.y;
    float r = fminf(rounding, fminf(w, h) * 0.5f);
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, r);
    shade(dl, v0, [&](ImVec2 p) {
        return mix(IM_COL32(238, 246, 158, 255), IM_COL32(194, 222, 58, 255), (p.x - a.x) / w * 0.35f + (p.y - a.y) / h * 0.65f);
    });
    // the decoration, clipped to the rounded shape: three rects, the corners left plain
    const ImVec4 clips[3] = {ImVec4(a.x + r, a.y, b.x - r, b.y), ImVec4(a.x, a.y + r, a.x + r, b.y - r), ImVec4(b.x - r, a.y + r, b.x, b.y - r)};
    float m = fminf(w, h), spin = t * 0.08f;
    for (const ImVec4 &cl : clips) {
        if (cl.z <= cl.x || cl.w <= cl.y) continue;
        dl->PushClipRect(ImVec2(cl.x, cl.y), ImVec2(cl.z, cl.w), true);
        // wide stripes slanting up to the right, sliding slowly
        float sw = 70 * s, period = sw * 2.6f, off = fmodf(t * 16 * s, period), slant = h * 0.55f;
        for (float x = a.x - slant - period + off; x < b.x; x += period)
            dl->AddQuadFilled(ImVec2(x + slant, a.y), ImVec2(x + slant + sw, a.y), ImVec2(x + sw, b.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 34));
        gear(dl, ImVec2(a.x + w * 0.05f, a.y + h * 0.10f), m * 0.22f, 10, spin, IM_COL32(255, 255, 255, 80));
        gear(dl, ImVec2(b.x - w * 0.03f, a.y + h * 0.62f), m * 0.32f, 12, -spin * 0.8f, IM_COL32(255, 255, 255, 62));
        gear(dl, ImVec2(a.x + w * 0.13f, b.y - h * 0.04f), m * 0.25f, 10, spin * 0.9f + 0.3f, IM_COL32(255, 255, 255, 70));
        gear(dl, ImVec2(a.x + w * 0.78f, a.y + h * 0.02f), m * 0.15f, 9, -spin * 1.3f, IM_COL32(250, 252, 205, 90));
        // halftone dots in the bottom left corner, smaller away from it
        float step = 22 * s, dw = w * 0.30f, dh = h * 0.45f;
        for (float y = b.y - step * 0.5f; y > b.y - dh; y -= step)
            for (float x = a.x + step * 0.5f; x < a.x + dw; x += step) {
                float dx = (x - a.x) / dw, dy = (b.y - y) / dh, d = sqrtf(dx * dx + dy * dy);
                if (d < 1) dl->AddCircleFilled(ImVec2(x, y), step * 0.30f * (1 - d), IM_COL32(255, 255, 255, 120), 10);
            }
        dl->PopClipRect();
    }
}

void ui_window_backdrop(void) {
    ImGuiWindow *w = ImGui::GetCurrentWindow();
    w->DrawList->PushClipRect(w->Pos, w->Pos + w->Size, false);
    ui_backdrop(w->DrawList, w->Pos, w->Pos + w->Size, ImGui::GetStyle().WindowRounding);
    w->DrawList->PopClipRect();
}

// green, cyan, blue, purple (pastel: towards white)
static ImU32 rainbow(float u, float pastel) {
    static const ImU32 st[4] = {IM_COL32(110, 214, 104, 255), IM_COL32(92, 210, 222, 255), IM_COL32(140, 166, 244, 255), IM_COL32(176, 126, 238, 255)};
    u = (u < 0 ? 0 : u > 1 ? 1 : u) * 3;
    int i = u >= 2 ? 2 : (int)u;
    return mix(mix(st[i], st[i + 1], u - i), IM_COL32_WHITE, pastel);
}

void ui_info_box(ImDrawList *dl, ImVec2 a, ImVec2 b) {
    float s = ui_px(), r = 24 * s, bw = 6 * s;
    dl->AddRectFilled(ImVec2(a.x, a.y + 5 * s), ImVec2(b.x, b.y + 5 * s), IM_COL32(80, 110, 20, 55), r);  // (shadow)
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, r);
    shade(dl, v0, [&](ImVec2 p) { return rainbow((p.x - a.x) / (b.x - a.x) * 0.8f + (p.y - a.y) / (b.y - a.y) * 0.2f, 0); });
    ImVec2 ia(a.x + bw, a.y + bw), ib(b.x - bw, b.y - bw);
    v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(ia, ib, IM_COL32_WHITE, r - bw * 0.6f);
    shade(dl, v0, [&](ImVec2 p) {
        float u = (p.x - ia.x) / (ib.x - ia.x), v = (p.y - ia.y) / (ib.y - ia.y);
        return rainbow(u * 0.8f + v * 0.2f, 0.62f + 0.30f * (1 - fabsf(u * 2 - 1)) * (1 - v * 0.5f));
    });
    dl->AddRect(ImVec2(ia.x + 2 * s, ia.y + 2 * s), ImVec2(ib.x - 2 * s, ib.y - 2 * s), IM_COL32(255, 255, 255, 210), r - bw, 0, 2.5f * s);
}

// A white pill, or the cursor's: yellow with a light rim (with the alpha of a disabled row)
void ui_pill(ImDrawList *dl, ImVec2 a, ImVec2 b, bool hot) {
    float r = (b.y - a.y) * 0.5f, s = ui_px();
    if (!hot) { dl->AddRectFilled(a, b, ImGui::GetColorU32(IM_COL32(255, 255, 255, 240)), r); return; }
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(IM_COL32_WHITE), r);
    shade(dl, v0, [&](ImVec2 p) { return mix(IM_COL32(253, 238, 120, 255), IM_COL32(244, 206, 52, 255), (p.y - a.y) / (b.y - a.y)); });
    dl->AddRect(ImVec2(a.x + 1.5f * s, a.y + 1.5f * s), ImVec2(b.x - 1.5f * s, b.y - 1.5f * s), ImGui::GetColorU32(IM_COL32(255, 250, 200, 255)), r, 0, 3 * s);
}

// The cursor: a white, shaded arrow with a dark outline pointing at tip, bobbing left and right
static void cursor_arrow(ImDrawList *dl, ImVec2 tip, float h) {
    tip.x -= (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 7)) * h * 0.20f;
    float w = h * 0.95f;
    ImVec2 p[4] = {tip, ImVec2(tip.x - w, tip.y - h * 0.5f), ImVec2(tip.x - w * 0.70f, tip.y), ImVec2(tip.x - w, tip.y + h * 0.5f)};
    int v0 = dl->VtxBuffer.Size;
    dl->AddConcavePolyFilled(p, 4, IM_COL32_WHITE);
    shade(dl, v0, [&](ImVec2 q) { return mix(IM_COL32(255, 255, 255, 255), IM_COL32(196, 198, 204, 255), (q.y - p[1].y) / h); });
    dl->AddPolyline(p, 4, IM_COL32(78, 78, 78, 255), ImDrawFlags_Closed, h * 0.09f);
}

// ---- the cursor: the row under it (keyboard / controller focus, or the mouse) and its arrow
static bool g_hot_set, g_row_hot;  // a row has the cursor this frame / the row being drawn has it
static ImRect g_hot_row;
static bool nav_rect(ImRect *out) {
    ImGuiContext &g = *GImGui;
    if (!g.NavCursorVisible || !g.NavId || !g.NavWindow) return false;
    *out = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
    return true;
}
static bool rect_hot(const ImRect &r) {
    ImGuiContext &g = *GImGui;
    ImRect n;
    if (nav_rect(&n) && g.NavWindow->RootWindowForNav == ImGui::GetCurrentWindow()->RootWindowForNav && r.Contains(n.GetCenter()))
        return true;
    return !g.NavHighlightItemUnderNav && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r.Min, r.Max);
}

// ---- descriptions
static const char *g_tip_now, *g_tip_last;
bool g_ui_tips_in_box;
void ui_tip(const char *text) {
    bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (hov || ImGui::IsItemFocused()) g_tip_now = text;
    if (hov && !g_ui_tips_in_box) ImGui::SetTooltip("%s", text);
}
const char *ui_tip_last(void) { return g_tip_last; }

void ui_frame_end(void) {
    ImGuiContext &g = *GImGui;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float h = ImGui::GetFrameHeight() * 0.62f;
    ImRect n;
    // (its tip on the pill's left end, before the text column)
    if (g_hot_set) cursor_arrow(dl, ImVec2(g_hot_row.Min.x + ui_inset() * 0.6f, g_hot_row.GetCenter().y), h);
    else if (nav_rect(&n) && g.NavIdIsAlive && g.NavWindow->Active) cursor_arrow(dl, ImVec2(n.Min.x - 4 * ui_px(), n.GetCenter().y), h);
    g_hot_set = false;
    g_tip_last = g_tip_now, g_tip_now = NULL;
}

void ui_title(ImFont *title, const char *text) {
    ImGui::PushFont(title);
    ImGui::TextColored(UI_TEXT, "%s", text);
    ImGui::PopFont();
}

void ui_heading(const char *text) {
    if (ImGui::GetCursorPosY() > ImGui::GetCursorStartPos().y + 1) ImGui::Spacing();  // (a group's gap above it, not at the top)
    ImGui::TextColored(UI_HEADING, "%s", text);
}

// ---- the grid (settings_ui.h)
float ui_inset(void) { return roundf(ImGui::GetFrameHeight() * 0.45f); }

void ui_page_begin(const char *id, ImVec2 size) {
    // (no side padding: the pills reach the page's edges; a little above and below for the cursor row's rim)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
    ImGui::BeginChild(id, size, ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::Indent(ui_inset());
}
void ui_page_end(void) {
    ImGui::Unindent(ui_inset());
    ImGui::EndChild();
}

void ui_group_gap(void) { ImGui::Spacing(); }

// The pill of the row on the current line: from the page's left edge (the text column less the inset) to its right one
static ImRect row_rect(void) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    return ImRect(ImVec2(p.x - ui_inset(), p.y), ImVec2(ImGui::GetCurrentWindow()->WorkRect.Max.x, p.y + ImGui::GetFrameHeight()));
}

bool ui_hot(ImVec2 a, ImVec2 b) {
    ImRect r(a, b);
    bool hot = rect_hot(r);
    if (hot) g_hot_set = true, g_hot_row = r;
    return hot;
}

bool ui_pill_row(void) {
    ImRect r = row_rect();
    g_row_hot = ui_hot(r.Min, r.Max);
    ui_pill(ImGui::GetWindowDrawList(), r.Min, r.Max, g_row_hot);
    return g_row_hot;
}

// The values' column: the pill's right half, so it stays put from one page to the next; after the longest label when
// that doesn't fit (long translations)
float ui_label_x(const char *const *labels, int n) {
    float w = 0;
    for (int i = 0; i < n; i++) w = fmaxf(w, ImGui::CalcTextSize(labels[i]).x);
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float in = ui_inset(), x0 = ImGui::GetCursorScreenPos().x - in;
    float half = x0 + (win->WorkRect.Max.x - x0) * 0.5f, fit = x0 + in + w + ImGui::GetStyle().ItemSpacing.x * 2;
    return fmaxf(half, fit) - win->Pos.x;
}

// The row's pill, yellow when it has the cursor; its label on the text column
void ui_row(const char *label, float x) {
    ui_pill_row();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(x);
    ImGui::SetNextItemWidth(-ui_inset() * 0.6f);
}

bool ui_button(const char *label, float width) {
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float in = ui_inset(), h = ImGui::GetFrameHeight();
    const char *end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end), p = ImGui::GetCursorScreenPos();
    float x0 = win->DC.IsSameLine ? p.x : p.x - in;  // (beside another item: where it ends, plus the spacing)
    float w = width < 0 ? win->WorkRect.Max.x - x0 : width > 0 ? width : ts.x + in * 2;
    ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
    bool hit = ImGui::InvisibleButton(label, ImVec2(w, h));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    bool hot = ui_hot(a, b) || ImGui::IsItemActive();
    ui_pill(win->DrawList, a, b, hot);
    if (!hot) win->DrawList->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), h * 0.5f, 0, 2 * ui_px());  // (a button, not a row)
    win->DrawList->AddText(ImVec2(a.x + in, a.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label, end);
    return hit;
}

// (left / right on a focused item, not being dragged: a step of its value instead of a move to another item)
static int lr_step(void) {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive() || (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled)) return 0;
    bool l = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    bool r = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    if (l || r) ImGui::NavMoveRequestCancel();
    return l == r ? 0 : l ? -1 : 1;
}

bool ui_slider(const char *id, float *v, float lo, float hi, const char *fmt, float step) {
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float old = *v, h = ImGui::GetFrameHeight(), in = ui_inset();
    ImVec2 p = ImGui::GetCursorScreenPos();
    char txt[32], widest[32];
    snprintf(txt, sizeof txt, fmt, *v);
    // (room for the longest value, at least a "000%": the tracks of a page as long as each other)
    snprintf(widest, sizeof widest, fmt, fabsf(hi) > fabsf(lo) ? hi : lo);
    float vw = fmaxf(fmaxf(ImGui::CalcTextSize(widest).x, ImGui::CalcTextSize(txt).x), ImGui::CalcTextSize("000%").x);
    float end = win->WorkRect.Max.x - in;
    // ImGui's slider with nothing of its own drawn: it takes the clicks and drags, the track is drawn below
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, 0);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::SetNextItemWidth(fmaxf(end - vw - in - p.x, h * 2));
    ImGui::SliderFloat(id, v, lo, hi, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    if (int s = lr_step()) *v = fminf(hi, fmaxf(lo, *v + s * step));
    // the track (ImGui's own geometry: the knob where a click puts it), its filled part and the knob; the value after it
    ImDrawList *dl = win->DrawList;
    ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    float grab = ImGui::GetStyle().GrabMinSize, x0 = bb.Min.x + 2 + grab * 0.5f, x1 = bb.Max.x - 2 - grab * 0.5f;
    float t = hi > lo ? (*v - lo) / (hi - lo) : 0, kx = x0 + (x1 - x0) * t, cy = bb.GetCenter().y, th = h * 0.18f, kr = h * 0.27f;
    dl->AddRectFilled(ImVec2(x0 - th * 0.5f, cy - th * 0.5f), ImVec2(x1 + th * 0.5f, cy + th * 0.5f), ImGui::GetColorU32(IM_COL32(222, 232, 190, 255)), th);
    dl->AddRectFilled(ImVec2(x0 - th * 0.5f, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), ImGui::GetColorU32(ImGuiCol_PlotHistogram), th);
    dl->AddCircleFilled(ImVec2(kx, cy), kr, ImGui::GetColorU32(IM_COL32_WHITE));
    dl->AddCircle(ImVec2(kx, cy), kr, ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SliderGrabActive : ImGuiCol_PlotHistogram), 0, h * 0.07f);
    snprintf(txt, sizeof txt, fmt, *v);
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(end - ts.x, cy - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), txt);
    return *v != old;
}

bool ui_value_button(const char *id, const char *text, ImU32 col) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    bool hit = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                                        col ? col : ImGui::GetColorU32(ImGuiCol_Text), text);
    return hit;
}

// A choice as in the game: its value centred, < > around it when the row has the cursor; left / right (d-pad, stick,
// arrow keys) or a click on < > step through the choices, A / Enter / a click on the value opens the list
bool ui_combo(const char *id, int *v, const char *const *items, int n) {
    ImGuiContext &g = *GImGui;
    int old = *v;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight(), cw = h * 0.9f;
    ImRect fr(p, ImVec2(p.x + w, p.y + h)), left(p, ImVec2(p.x + cw, p.y + h)), right(ImVec2(fr.Max.x - cw, p.y), fr.Max);
    bool disabled = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0, hot = g_row_hot;
    int step = 0;
    if (!disabled && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0))
        step = ImGui::IsMouseHoveringRect(left.Min, left.Max) ? -1 : ImGui::IsMouseHoveringRect(right.Min, right.Max) ? 1 : 0;
    ImGui::SetCursorScreenPos(ImVec2(p.x + cw, p.y));
    ImGui::SetNextItemWidth(w - cw * 2);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (open) {
        for (int i = 0; i < n; i++) {
            bool sel = i == *v;
            if (ImGui::Selectable(items[i], sel)) *v = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    } else if (!disabled && ImGui::IsItemFocused()) {
        bool l = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        bool r = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        if (l != r) step = l ? -1 : 1;
        if (l || r) ImGui::NavMoveRequestCancel();  // (a change of the value, not a move to another item)
    }
    if (step && n > 0) *v = (*v + step + n) % n;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const char *txt = *v >= 0 && *v < n ? items[*v] : "";
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(fr.GetCenter().x - ts.x * 0.5f, fr.GetCenter().y - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), txt);
    if (hot && !disabled) {  // < >: chunky, white with a dark outline, as the game's
        float ch = h * 0.32f;
        for (int k = 0; k < 2; k++) {
            float x = k ? right.GetCenter().x : left.GetCenter().x, y = fr.GetCenter().y, d = ch * (k ? 0.8f : -0.8f), t = ch * (k ? 0.55f : -0.55f);
            ImVec2 pts[6] = {ImVec2(x - d * 0.5f, y - ch), ImVec2(x - d * 0.5f + t, y - ch), ImVec2(x + d * 0.5f + t, y),
                             ImVec2(x - d * 0.5f + t, y + ch), ImVec2(x - d * 0.5f, y + ch), ImVec2(x + d * 0.5f, y)};
            dl->AddConcavePolyFilled(pts, 6, IM_COL32(255, 255, 255, 255));
            dl->AddPolyline(pts, 6, IM_COL32(90, 90, 90, 255), ImDrawFlags_Closed, h * 0.05f);
        }
    }
    return *v != old;
}

// An on / off setting as a row with its value, as the game's (Sí / No)
bool ui_toggle(const char *id, const char *label, float x, bool *v) {
    const char *oo[] = {UI_T("Off", "Desligado", "Выкл."), UI_T("On", "Ligado", "Вкл.")};
    int i = *v;
    ui_row(label, x);
    bool changed = ui_combo(id, &i, oo, 2);
    *v = i != 0;
    return changed;
}

// ------------------------------------------------------------------ tabs
void ui_tabs_begin(int *current, int count) {
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0;
    if (step && count > 0) *current = (*current + step + count) % count;
}

// ------------------------------------------------------------------ the graphics page
static const char *const *graphics_labels(int *n) {
    static const char *l[8];
    l[0] = UI_T("Frame rate limit", "Limite de quadros", "Лимит кадров");
    l[1] = UI_T("Ambient occlusion (SSAO)", "Oclusão de ambiente (SSAO)", "Затенение (SSAO)");
    l[2] = UI_T("Indirect light (GI)", "Luz indireta (GI)", "Непрямой свет (GI)");
    l[3] = UI_T("Shadow filter", "Filtro das sombras", "Фильтр теней");
    l[4] = UI_T("Anti-aliasing", "Antisserrilhado", "Сглаживание");
    l[5] = UI_T("FSR 3.1 mode", "Modo do FSR 3.1", "Режим FSR 3.1");
    l[6] = UI_T("Frame generation", "Geração de quadros", "Генерация кадров");
    l[7] = UI_T("Sharpening (AMD RCAS)", "Nitidez (AMD RCAS)", "Резкость (AMD RCAS)");
    *n = 8;
    return l;
}

float ui_graphics_x(const char *const *extra, int n_extra) {
    int n;
    const char *const *l = graphics_labels(&n);
    float x = ui_label_x(l, n);
    return extra && n_extra ? fmaxf(x, ui_label_x(extra, n_extra)) : x;
}

void ui_page_graphics(float x, bool known) {
    int n;
    const char *const *l = graphics_labels(&n);
    // the world's frame rate (runtime/hle/fps60.c, switched at the next present): the game's 30, 60, or 120 (more
    // experimental); menus stay at 30 with an in-between image (60), battles and captures at 30
    {
        const char *fr[] = {UI_T("30 fps (original)", "30 fps (original)", "30 fps (оригинал)"), "60 fps",
                            UI_T("120 fps (experimental)", "120 fps (experimental)", "120 fps (экспериментально)")};
        int f = !known ? 0 : g_settings.fps == 120 ? 2 : g_settings.fps == 60 ? 1 : 0;
        ui_row(l[0], x);
        ImGui::BeginDisabled(!known);
        if (ui_combo("##fps", &f, fr, 3)) g_settings.fps = f == 2 ? 120 : f == 1 ? 60 : 30;
        ImGui::EndDisabled();
        ui_tip(!known ? UI_T("Only with version 1.0.0 (the base game) of Eevee or Pikachu for now.",
                             "Por enquanto só na versão 1.0.0 (o jogo base) do Eevee ou do Pikachu.",
                             "Пока только с версией 1.0.0 (базовая игра) Eevee или Pikachu.")
                      : UI_T("The world at 60 or 120 fps; menus and the Pokédex smoothed; battles and captures stay at 30.",
                             "Mundo a 60 ou 120 fps; menus e Pokédex suavizados; batalhas e capturas continuam a 30.",
                             "Мир на 60 или 120 к/с; меню и Покедекс сглажены; бои и поимки остаются на 30."));
    }
    ui_group_gap();
    // SSAO and GI by quality level (their parameters: settings_apply_levels; high = what they had before the levels)
    const char *lv[] = {UI_T("Off", "Desligado", "Выкл."), UI_T("Low", "Baixo", "Низкое"), UI_T("Medium", "Médio", "Среднее"),
                        UI_T("High", "Alto", "Высокое")};
    ui_row(l[1], x);
    {   // (SSAO only: ultra = high plus the contact shadows, settings_apply_levels)
        const char *lv5[] = {lv[0], lv[1], lv[2], lv[3], "Ultra"};
        ui_combo("##ssao_lv", &g_settings.ssao_level, lv5, 5);
        if (g_settings.ssao_level != FX_ULTRA)
            ui_tip(UI_T("Soft shadows in corners and where objects meet, from the scene's depth.",
                        "Sombras suaves nos cantos e onde os objetos se encontram, a partir da profundidade da cena.",
                        "Мягкие тени в углах и там, где предметы соприкасаются, по глубине сцены."));
        else
            ui_tip(UI_T("High, plus contact shadows where objects touch the ground (feet, grass, small props).",
                        "Alto, mais sombras de contato onde os objetos tocam o chão (pés, grama, objetos pequenos).",
                        "Высокое плюс контактные тени там, где предметы касаются земли (ноги, трава, мелкие объекты)."));
    }
    ui_row(l[2], x);
    ui_combo("##gi_lv", &g_settings.gi_level, lv, 4);
    ui_tip(UI_T("Light bouncing off nearby surfaces: their colour on the ground and the walls around them.",
                "A luz que reflete nas superfícies próximas: a cor delas no chão e nas paredes em volta.",
                "Свет, отражённый от ближних поверхностей: их цвет на земле и стенах вокруг."));
    // the shadow map's filter (runtime/nvn/vulkan/textures.c vk_sampler, runtime/nvn/maxwell/translate.c shpcf)
    const char *sh[] = {UI_T("Game's", "Do jogo", "Как в игре"), UI_T("Bilinear", "Bilinear", "Билинейный"), UI_T("Smooth", "Suave", "Мягкий")};
    ui_row(l[3], x);
    ui_combo("##shadow_filter", &g_settings.shadow_filter, sh, 3);
    ui_tip(UI_T("The edges of the shadows: as the game draws them, smoothed, or softer still.",
                "As bordas das sombras: como o jogo as desenha, suavizadas ou ainda mais suaves.",
                "Края теней: как рисует игра, сглаженные или ещё мягче."));
    ui_group_gap();
    // anti-aliasing: SMAA on the final image (the interface too), TAA on the scene (jittered, motion vectors:
    // runtime/nvn/vulkan/exec.c taa_run) or FSR 3.1 (temporal, the interface masked: exec.c, fsr3.cpp), the last one
    // upscaling too, with its mode below. With the others a lower internal resolution is scaled bilinearly
    // (settings_apply_levels).
    {
        const char *aa[] = {UI_T("Off", "Desligado", "Выкл."), "SMAA", "TAA", "FSR 3.1"};
        ui_row(l[4], x);
        if (ui_combo("##aa", &g_settings.antialias, aa, 4)) settings_apply_levels();
        ui_tip(UI_T("Smooths jagged edges. SMAA: light, on the final image. TAA and FSR 3.1: steadier, over several frames; FSR 3.1 also draws the scene at a lower resolution and upscales it.",
                    "Suaviza os serrilhados. SMAA: leve, na imagem final. TAA e FSR 3.1: mais estáveis, ao longo de vários quadros; o FSR 3.1 também desenha a cena numa resolução menor e a amplia.",
                    "Сглаживает зубчатые края. SMAA: лёгкое, на итоговом кадре. TAA и FSR 3.1: стабильнее, по нескольким кадрам; FSR 3.1 ещё и рисует сцену в меньшем разрешении и увеличивает её."));
    }
    // FSR 3.1's presets: the render resolution is the window's divided by the ratio (runtime/nvn/output_scale.c)
    if (g_settings.antialias == AA_FSR3) {
        const char *pr[] = {UI_T("Native AA (1.0x)", "AA nativo (1,0x)", "Нативное AA (1,0x)"),
                            UI_T("Quality (1.5x)", "Qualidade (1,5x)", "Качество (1,5x)"),
                            UI_T("Balanced (1.7x)", "Equilibrado (1,7x)", "Баланс (1,7x)"),
                            UI_T("Performance (2.0x)", "Desempenho (2,0x)", "Производительность (2,0x)"),
                            UI_T("Ultra Performance (3.0x)", "Ultra desempenho (3,0x)", "Макс. производительность (3,0x)")};
        ui_row(l[5], x);
        ui_combo("##fsr3p", &g_settings.fsr3_preset, pr, 5);
        ui_tip(UI_T("How much lower the scene is drawn before FSR 3.1 upscales it: the window's size divided by the ratio. Higher ratios run faster and look softer.",
                    "Quanto menor a cena é desenhada antes de o FSR 3.1 ampliá-la: o tamanho da janela dividido pela razão. Razões maiores rodam mais rápido e ficam mais suaves.",
                    "Во сколько раз меньше рисуется сцена перед увеличением FSR 3.1: размер окна, делённый на коэффициент. Больший коэффициент быстрее, но мягче."));
        // its sharpening: RCAS, AMD's robust contrast-adaptive sharpening (CAS) inside FSR 3.1 (exec.c post_process; the
        // frame generation's in-between images take it too)
        float pct = g_settings.fsr3_sharpness * 100;
        ui_row(l[7], x);
        if (ui_slider("##fsr3s", &pct, 0, 100, "%.0f%%", 5)) g_settings.fsr3_sharpness = pct / 100;
        ui_tip(UI_T("FSR 3.1's own sharpening (RCAS, AMD's contrast-adaptive sharpening). 0% turns it off.",
                    "A nitidez do próprio FSR 3.1 (RCAS, o realce adaptativo de contraste da AMD). 0% desliga.",
                    "Собственная резкость FSR 3.1 (RCAS, адаптивное повышение резкости AMD). 0% — выкл."));
    }
    // Frame generation (exec.c frame interpolation): 2, 3 or 4 images per world frame, the in-between ones drawn by
    // running the frame's draws again with their positions between the last frame's and this one's; Auto: as many as the
    // display's refresh shows evenly and the GPU keeps up with (exec.c ip_images). Any GPU.
    {
        const char *fg[] = {UI_T("Off", "Desligado", "Выкл."), "Auto", "2x", "3x", "4x"};
        int cur = g_settings.frame_gen == FG_AUTO ? 1 : g_settings.frame_gen >= 2 && g_settings.frame_gen <= 4 ? g_settings.frame_gen : 0;
        ui_row(l[6], x);
        if (ui_combo("##fg", &cur, fg, 5)) g_settings.frame_gen = cur == 1 ? FG_AUTO : cur ? cur : 0;
        ui_tip(UI_T("Images per frame of the world (60 fps x 2 = 120). Each extra image costs about half as much "
                    "as drawing the scene again, and the window shows the game one frame later. Auto: as many as "
                    "the display's refresh shows evenly (60 fps at 240 Hz: 4x); when the GPU can't keep up, the extra images "
                    "are drawn at a lower resolution first, then fewer of them.",
                    "Imagens por quadro do mundo (60 fps x 2 = 120). Cada imagem a mais custa cerca de metade do que "
                    "desenhar a cena de novo, e a janela mostra o jogo um quadro depois. Auto: tantas quantas a "
                    "frequência da tela mostra por igual (60 fps a 240 Hz: 4x); quando a GPU não acompanha, as imagens a "
                    "mais são desenhadas primeiro numa resolução menor, depois em menor número.",
                    "Кадров на кадр мира (60 fps x 2 = 120). Каждый дополнительный кадр стоит примерно половину "
                    "повторной отрисовки сцены, а окно показывает игру на кадр позже. Авто: столько, сколько частота "
                    "экрана показывает равномерно (60 fps при 240 Гц: 4x); если GPU не успевает, дополнительные кадры "
                    "сначала рисуются в меньшем разрешении, затем их становится меньше."));
    }
    settings_apply_levels();
}

// ------------------------------------------------------------------ image and sound
// Volume and the game image's colour, applied live in game (runtime/services/audio.c apply_gain,
// runtime/nvn/vulkan/exec.c post_process, color.frag), and the TAA sharpening at the present
void ui_page_image_sound(void) {
    const char *l_vol = UI_T("Volume", "Volume", "Громкость"), *l_bri = UI_T("Brightness", "Brilho", "Яркость"),
               *l_con = UI_T("Contrast", "Contraste", "Контраст"), *l_sat = UI_T("Saturation", "Saturação", "Насыщенность"),
               *l_shp = UI_T("Sharpness (TAA)", "Nitidez (TAA)", "Резкость (TAA)");
    const char *labels[] = {l_vol, l_bri, l_con, l_sat, l_shp};
    float x = ui_label_x(labels, 5);
    struct Row { const char *label; float *v, lo, hi; } rows[] = {
        {l_vol, &g_settings.volume, 0.0f, 2.0f}, {l_bri, &g_settings.brightness, 0.5f, 1.5f},
        {l_con, &g_settings.contrast, 0.5f, 1.5f}, {l_sat, &g_settings.saturation, 0.0f, 2.0f}};
    for (int i = 0; i < 4; i++) {
        if (i == 1) ui_group_gap();
        float pct = *rows[i].v * 100;
        ui_row(rows[i].label, x);
        ImGui::PushID(100 + i);
        if (ui_slider("##v", &pct, rows[i].lo * 100, rows[i].hi * 100, "%.0f%%", 5)) *rows[i].v = pct / 100;
        ImGui::PopID();
    }
    ImGui::BeginDisabled(g_settings.antialias != AA_TAA);  // (FSR 3.1's: the graphics page, under its mode)
    {
        float pct = g_settings.taa_sharpness * 100;
        ui_row(l_shp, x);
        if (ui_slider("##taa_s", &pct, 0, 100, "%.0f%%", 5)) g_settings.taa_sharpness = pct / 100;
    }
    ImGui::EndDisabled();
    ui_group_gap();
    if (ui_button(UI_T("Reset", "Restaurar", "Сбросить"))) {
        g_settings.volume = g_settings.brightness = g_settings.contrast = g_settings.saturation = 1.0f;
        g_settings.taa_sharpness = 0.5f;
    }
    settings_clamp_color();
}

// ------------------------------------------------------------------ advanced
// The motion vectors' detail, the effects' parameters by hand and the debug views (shown at the present instead of the
// game image: exec.c post_process).
// The parameters (runtime/settings.c settings_apply_levels): the sliders show the quality levels' values; moving any of
// them turns "Custom parameters" on with the values as they are, so the levels then only turn the SSAO and the GI on
// and off (and the contact shadows have their own switch). Applied live, saved with fx_custom.
static bool adv_slider(const char *label, float x, const char *id, float *v, float lo, float hi, const char *fmt, float step,
                       const char *tip) {
    ui_row(label, x);
    bool ch = ui_slider(id, v, lo, hi, fmt, step);
    ui_tip(tip);
    return ch;
}
static bool adv_islider(const char *label, float x, const char *id, int *v, int lo, int hi, const char *tip) {
    float f = (float)*v;
    if (!adv_slider(label, x, id, &f, (float)lo, (float)hi, "%.0f", 1, tip)) return false;
    int n = (int)lroundf(f);
    if (n == *v) return false;
    *v = n;
    return true;
}
static bool adv_toggle(const char *label, float x, const char *id, bool *v, const char *tip) {
    bool ch = ui_toggle(id, label, x, v);
    ui_tip(tip);
    return ch;
}

void ui_page_advanced(void) {
    const char *l_rad = UI_T("Radius", "Raio", "Радиус"), *l_int = UI_T("Intensity", "Intensidade", "Интенсивность"),
               *l_smp = UI_T("Samples", "Amostras", "Выборки"), *l_tmp = UI_T("Temporal", "Temporal", "Временное"),
               *l_cs = UI_T("Contact shadows", "Sombras de contato", "Контактные тени"),
               *l_len = UI_T("Length", "Comprimento", "Длина"), *l_str = UI_T("Strength", "Força", "Сила"),
               *l_stp = UI_T("Steps", "Passos", "Шаги"), *l_thk = UI_T("Thickness", "Espessura", "Толщина"),
               *l_ray = UI_T("Rays", "Raios", "Лучи"), *l_res = UI_T("Resolution", "Resolução", "Разрешение"),
               *l_cus = UI_T("Custom parameters", "Parâmetros próprios", "Свои параметры");
    const char *l[] = {UI_T("Per-object motion vectors", "Vetores de movimento por objeto", "Векторы движения объектов"),
                       UI_T("Occlusion alone", "Só a oclusão", "Только затенение"),
                       UI_T("Indirect light alone", "Só a luz indireta", "Только непрямой свет"),
                       UI_T("Motion vectors", "Vetores de movimento", "Векторы движения"),
                       l_rad, l_int, l_smp, l_tmp, l_cs, l_len, l_str, l_stp, l_thk, l_ray, l_res, l_cus};
    float x = ui_label_x(l, 16);
    ui_toggle("##mv_objects", l[0], x, &g_settings.mv_objects);
    ui_tip(UI_T("The motion of each object, not only the camera's, for TAA, FSR 3.1 and the frame generation.",
                "O movimento de cada objeto, não só o da câmera, para o TAA, o FSR 3.1 e a geração de quadros.",
                "Движение каждого объекта, а не только камеры, для TAA, FSR 3.1 и генерации кадров."));

    // ---- the effects' parameters
    ui_heading(UI_T("Effect tuning", "Ajuste dos efeitos", "Настройка эффектов"));
    Settings &S = g_settings;
    bool ch = false;
    ui_toggle("##fx_custom", l_cus, x, &S.fx_custom);
    ui_tip(UI_T("Off: the quality levels set the values below. On: yours, saved; the levels only turn the effects on and off.",
                "Desligado: os níveis de qualidade definem os valores abaixo. Ligado: os seus, salvos; os níveis só ligam e desligam os efeitos.",
                "Выкл.: значения ниже задают уровни качества. Вкл.: ваши, сохраняются; уровни только включают и выключают эффекты."));
    ui_heading("SSAO");
    ImGui::PushID("ssao");
    ImGui::BeginDisabled(!S.ssao);
    ch |= adv_slider(l_rad, x, "##r", &S.ssao_radius, 10, 400, "%.0f", 10,
                     UI_T("How far the occlusion reaches, in world units (~cm). Level: 240.", "Até onde a oclusão alcança, em unidades do mundo (~cm). Nível: 240.",
                          "Дальность затенения в единицах мира (~см). Уровень: 240."));
    ch |= adv_slider(l_int, x, "##i", &S.ssao_intensity, 0, 3, "%.2f", 0.1f,
                     UI_T("How dark it gets. Level: 1.2.", "O quanto escurece. Nível: 1,2.", "Насколько темнее. Уровень: 1,2."));
    ch |= adv_islider(l_smp, x, "##s", &S.ssao_samples, 4, 32,
                      UI_T("Per pixel: more is smoother and slower. Levels: 4 / 6 / 8.", "Por pixel: mais é mais suave e mais lento. Níveis: 4 / 6 / 8.",
                           "На пиксель: больше — глаже и медленнее. Уровни: 4 / 6 / 8."));
    ch |= adv_toggle(l_tmp, x, "##t", &S.ssao_temporal,
                     UI_T("Accumulated over frames: less noise, a little trailing.", "Acumulada ao longo dos quadros: menos ruído, um pouco de rastro.",
                          "Накопление по кадрам: меньше шума, лёгкий шлейф."));
    ImGui::EndDisabled();
    ImGui::PopID();
    ui_heading(l_cs);
    ImGui::PushID("contact");
    ch |= adv_toggle(l_cs, x, "##on", &S.contact_on,
                     UI_T("Short rays toward the sun: small objects' shadows the shadow map misses. Level: SSAO Ultra.",
                          "Raios curtos rumo ao sol: as sombras de objetos pequenos que o mapa de sombras perde. Nível: SSAO Ultra.",
                          "Короткие лучи к солнцу: тени мелких объектов, которых нет на карте теней. Уровень: SSAO Ultra."));
    ImGui::BeginDisabled(!S.contact_on);
    ch |= adv_slider(l_len, x, "##l", &S.contact_length, 10, 200, "%.0f", 5,
                     UI_T("The rays' length in world units. Level: 100.", "O comprimento dos raios em unidades do mundo. Nível: 100.",
                          "Длина лучей в единицах мира. Уровень: 100."));
    float pct = S.contact_strength * 100;
    if (adv_slider(l_str, x, "##st", &pct, 0, 100, "%.0f%%", 5,
                   UI_T("How dark the shadows are. Level: 35%.", "O quanto as sombras escurecem. Nível: 35%.", "Насколько темны тени. Уровень: 35%.")))
        S.contact_strength = pct / 100, ch = true;
    ch |= adv_islider(l_stp, x, "##sp", &S.contact_steps, 4, 32,
                      UI_T("Depth checks per ray. Level: 6.", "Testes de profundidade por raio. Nível: 6.", "Проверок глубины на луч. Уровень: 6."));
    ch |= adv_slider(l_thk, x, "##th", &S.contact_thickness, 5, 100, "%.0f", 5,
                     UI_T("How far behind a surface still casts. Level: 30.", "Até que profundidade atrás de uma superfície ainda faz sombra. Nível: 30.",
                          "На какой глубине за поверхностью ещё есть тень. Уровень: 30."));
    ImGui::EndDisabled();
    ImGui::PopID();
    ui_heading(UI_T("Indirect light (GI)", "Luz indireta (GI)", "Непрямой свет (GI)"));
    ImGui::PushID("gi");
    ImGui::BeginDisabled(!S.gi);
    ch |= adv_slider(l_int, x, "##i", &S.gi_intensity, 0, 4, "%.2f", 0.1f,
                     UI_T("How much bounced light. Level: 2.", "Quanta luz refletida. Nível: 2.", "Сколько отражённого света. Уровень: 2."));
    ch |= adv_slider(l_rad, x, "##r", &S.gi_radius, 50, 1000, "%.0f", 25,
                     UI_T("How far the rays reach, in world units. Level: 300.", "Até onde os raios alcançam, em unidades do mundo. Nível: 300.",
                          "Дальность лучей в единицах мира. Уровень: 300."));
    ch |= adv_slider(l_thk, x, "##th", &S.gi_thickness, 5, 100, "%.0f", 5,
                     UI_T("How thick surfaces are taken to be for a hit. Level: 25.", "A espessura que se supõe nas superfícies para um acerto. Nível: 25.",
                          "Какой считать толщину поверхности для попадания. Уровень: 25."));
    ch |= adv_islider(l_ray, x, "##ry", &S.gi_rays, 1, 8,
                      UI_T("Per GI pixel. Levels: 2 / 3 / 4.", "Por pixel do GI. Níveis: 2 / 3 / 4.", "На пиксель GI. Уровни: 2 / 3 / 4."));
    ch |= adv_islider(l_stp, x, "##sp", &S.gi_steps, 4, 32,
                      UI_T("Depth checks per ray. Levels: 6 / 6 / 8.", "Testes de profundidade por raio. Níveis: 6 / 6 / 8.",
                           "Проверок глубины на луч. Уровни: 6 / 6 / 8."));
    {
        const char *dv[] = {"1/2", "1/3", "1/4"};
        int d = S.gi_div - 2;
        ui_row(l_res, x);
        if (ui_combo("##div", &d, dv, 3)) S.gi_div = d + 2, ch = true;
        ui_tip(UI_T("Of each axis of the scene. Levels: 1/3 low, 1/2 medium and high.", "De cada eixo da cena. Níveis: 1/3 no baixo, 1/2 no médio e no alto.",
                    "По каждой оси сцены. Уровни: 1/3 низкое, 1/2 среднее и высокое."));
    }
    ch |= adv_toggle(l_tmp, x, "##t", &S.gi_temporal,
                     UI_T("Accumulated over frames: less noise, a little trailing.", "Acumulada ao longo dos quadros: menos ruído, um pouco de rastro.",
                          "Накопление по кадрам: меньше шума, лёгкий шлейф."));
    ImGui::EndDisabled();
    ImGui::PopID();
    if (ch) S.fx_custom = true;
    ui_group_gap();
    if (ui_button(UI_T("Level values", "Valores dos níveis", "Значения уровней"))) S.fx_custom = false;
    ui_tip(UI_T("Back to the quality levels' values.", "Volta aos valores dos níveis de qualidade.", "Вернуть значения уровней качества."));
    settings_apply_levels();

    ui_heading(UI_T("Debug views", "Vistas de depuração", "Отладка"));
    ImGui::BeginDisabled(!g_settings.ssao && !g_settings.contact_on);
    ui_toggle("##ssao_debug", l[1], x, &g_settings.ssao_debug);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!g_settings.gi);
    ui_toggle("##gi_debug", l[2], x, &g_settings.gi_debug);
    ImGui::EndDisabled();
    ui_toggle("##mv_debug", l[3], x, &g_settings.mv_debug);
    ui_group_gap();
    if (ui_button(UI_T("Reset", "Restaurar", "Сбросить"))) {
        g_settings.ssao_debug = g_settings.gi_debug = g_settings.mv_debug = false;
        g_settings.mv_objects = true;
        g_settings.fx_custom = false;
        settings_apply_levels();
    }
}
