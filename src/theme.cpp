#include "theme.h"
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>   // GetModuleFileNameA, to resolve the bundled fonts/ dir

namespace theme {
namespace {

// Hex -> sRGB ImVec4 (raw 0..1 sRGB, no gamma conversion, matching how
// StyleColorsLight/Dark ship).
constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

Palette  gPalette;
Spacing  gSpace{4, 8, 12, 16, 24};
float    gScale = 1.0f;
Mode     gMode  = Mode::Light;
ImFont*  gFontRegular = nullptr;
ImFont*  gFontHeader  = nullptr;

// Material 3 palettes. Light is the default brand scheme (Google Blue);
// dark is the corresponding tonal scheme.
void initPalette(Mode m) {
    if (m == Mode::Light) {
        gPalette.bg                  = rgb(0xF8, 0xFA, 0xFD);
        gPalette.surface             = rgb(0xFF, 0xFF, 0xFF);
        gPalette.surfaceAlt          = rgb(0xEE, 0xF1, 0xF6);
        gPalette.border              = rgb(0xE0, 0xE3, 0xE7);
        gPalette.text                = rgb(0x1F, 0x1F, 0x1F);
        gPalette.textMuted           = rgb(0x5F, 0x63, 0x68);
        gPalette.accent              = rgb(0x0B, 0x57, 0xD0);
        gPalette.accentHover         = rgb(0x1A, 0x6D, 0xD4);
        gPalette.accentActive        = rgb(0x0A, 0x4B, 0xB0);
        gPalette.onAccent            = rgb(0xFF, 0xFF, 0xFF);
        gPalette.secondaryContainer  = rgb(0xC2, 0xE7, 0xFF);
        gPalette.onSecondaryContainer= rgb(0x04, 0x1E, 0x49);
        gPalette.danger              = rgb(0xB3, 0x26, 0x1E);
        gPalette.dangerHover         = rgb(0xC5, 0x37, 0x2F);
        gPalette.success             = rgb(0x14, 0x6C, 0x2E);
    } else {
        gPalette.bg                  = rgb(0x13, 0x13, 0x14);
        gPalette.surface             = rgb(0x1E, 0x1F, 0x20);
        gPalette.surfaceAlt          = rgb(0x28, 0x29, 0x2B);
        gPalette.border              = rgb(0x44, 0x47, 0x46);
        gPalette.text                = rgb(0xE3, 0xE3, 0xE3);
        gPalette.textMuted           = rgb(0x9A, 0xA0, 0xA6);
        gPalette.accent              = rgb(0xA8, 0xC7, 0xFA);
        gPalette.accentHover         = rgb(0xBB, 0xD3, 0xFB);
        gPalette.accentActive        = rgb(0x93, 0xB4, 0xF0);
        gPalette.onAccent            = rgb(0x06, 0x2E, 0x6F);
        gPalette.secondaryContainer  = rgb(0x00, 0x4A, 0x77);
        gPalette.onSecondaryContainer= rgb(0xC2, 0xE7, 0xFF);
        gPalette.danger              = rgb(0xDC, 0x36, 0x2E);
        gPalette.dangerHover         = rgb(0xE4, 0x53, 0x4B);
        gPalette.success             = rgb(0x81, 0xC9, 0x95);
    }
}

// A translucent variant of a colour (for hover/active fills over surfaces).
ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// A pill rounding value big enough that ImGui clamps it to half the widget
// height -> fully rounded "pill" ends on any button/chip. Material 3 buttons
// and chips are fully rounded.
float pill() { return scale(40); }

} // namespace

const Palette& colors() { return gPalette; }
const Spacing& space()  { return gSpace; }
float scale(float logicalPx) { return logicalPx * gScale; }
ImFont* fontRegular() { return gFontRegular; }
ImFont* fontHeader()  { return gFontHeader; }
Mode    mode()        { return gMode; }

namespace {
// Load a TTF only if the file actually exists, so a missing system font can
// never trip ImGui's internal assert. Returns nullptr on any failure.
ImFont* tryLoadTTF(ImGuiIO& io, const char* path, float sizePx) {
    if (!path || !path[0]) return nullptr;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fclose(f);
    ImFontConfig cfg;
    cfg.OversampleH = 3;   // crisper horizontal stems
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = false;
    return io.Fonts->AddFontFromFileTTF(path, sizePx, &cfg);
}

// Directory the running .exe lives in, with a trailing separator. The build
// copies fonts/ next to the executable, so this resolves the bundled Roboto
// regardless of the current working directory.
std::string exeDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    std::string p(buf, n ? n : 0);
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash + 1);
}

// Try the bundled Roboto first (next to the exe, then the cwd), then fall back
// to the requested Windows system font. Returns nullptr only if everything is
// missing.
ImFont* loadUiFont(ImGuiIO& io, const char* robotoFile, const char* sysFont,
                   float sizePx) {
    std::string dir = exeDir();
    if (!dir.empty()) {
        if (ImFont* f = tryLoadTTF(io, (dir + "fonts\\" + robotoFile).c_str(), sizePx))
            return f;
    }
    if (ImFont* f = tryLoadTTF(io, (std::string("fonts/") + robotoFile).c_str(), sizePx))
        return f;
    return tryLoadTTF(io, sysFont, sizePx);
}
} // namespace

void loadFonts(ImGuiIO& io, float dpiScale) {
    // Material "Google Sans"-style type: bundled Roboto (Regular for body,
    // Medium for headers) if present, otherwise Segoe UI - geometrically very
    // close - then the built-in bitmap font as a last resort so the app still
    // runs everywhere.
    const char* kSegoe     = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* kSegoeSemi = "C:\\Windows\\Fonts\\seguisb.ttf";   // Segoe UI Semibold

    gFontRegular = loadUiFont(io, "Roboto-Regular.ttf", kSegoe, 15.0f * dpiScale);
    if (!gFontRegular) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * dpiScale;
        gFontRegular = io.Fonts->AddFontDefault(&cfg);
    }

    gFontHeader = loadUiFont(io, "Roboto-Medium.ttf", kSegoeSemi, 19.0f * dpiScale);
    if (!gFontHeader) gFontHeader = loadUiFont(io, "Roboto-Regular.ttf", kSegoe, 19.0f * dpiScale);
    if (!gFontHeader) {
        ImFontConfig hcfg;
        hcfg.SizePixels = 18.0f * dpiScale;
        gFontHeader = io.Fonts->AddFontDefault(&hcfg);
    }
}

namespace {
// Build the ImGui style (sizes + colours) from the active palette/scale/mode.
// Split out so setMode() can re-skin live without re-loading fonts.
void installStyle() {
    initPalette(gMode);

    ImGuiStyle& s = ImGui::GetStyle();
    if (gMode == Mode::Light) ImGui::StyleColorsLight(&s);
    else                      ImGui::StyleColorsDark(&s);
    // ... then override everything below for the Material look.

    const Palette& c = gPalette;

    // Spacing & sizing (logical px, DPI-scaled). Material UI is airy, so the
    // frame padding is generous to give pill buttons and inputs breathing room.
    s.WindowPadding     = ImVec2(gSpace.lg, gSpace.lg);
    s.FramePadding      = ImVec2(gSpace.lg, scale(7));
    s.CellPadding       = ImVec2(gSpace.sm, scale(6));
    s.ItemSpacing       = ImVec2(gSpace.sm, gSpace.sm);
    s.ItemInnerSpacing  = ImVec2(gSpace.sm, scale(6));
    s.IndentSpacing     = gSpace.lg;
    s.ScrollbarSize     = scale(12);
    s.GrabMinSize       = scale(12);

    // Borders. Material filled components have no outline; separation comes from
    // tonal surfaces. Keep a hairline only on cards/popups for definition (ImGui
    // has no real elevation shadow).
    s.WindowBorderSize  = scale(1);
    s.ChildBorderSize   = scale(1);
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = scale(1);
    s.TabBorderSize     = 0.0f;
    s.SeparatorTextBorderSize = scale(1);

    // Rounding. Material 3: large radius on containers/cards, small on inputs;
    // buttons & chips are pill-shaped (handled per-widget in the helpers).
    s.WindowRounding    = scale(16);
    s.ChildRounding     = scale(16);
    s.FrameRounding     = scale(10);
    s.PopupRounding     = scale(12);
    s.GrabRounding      = scale(10);
    s.ScrollbarRounding = scale(12);
    s.TabRounding       = scale(10);

    // Alignment.
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign   = ImVec2(0.5f, 0.5f);

    // Colours.
    ImVec4* col = s.Colors;
    col[ImGuiCol_Text]                  = c.text;
    col[ImGuiCol_TextDisabled]          = c.textMuted;
    col[ImGuiCol_WindowBg]              = c.bg;
    col[ImGuiCol_ChildBg]              = c.surface;
    col[ImGuiCol_PopupBg]               = c.surface;
    col[ImGuiCol_Border]                = c.border;
    col[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    col[ImGuiCol_FrameBg]               = c.surfaceAlt;
    col[ImGuiCol_FrameBgHovered]        = alpha(c.accent, 0.10f);
    col[ImGuiCol_FrameBgActive]         = alpha(c.accent, 0.18f);

    col[ImGuiCol_TitleBg]               = c.surface;
    col[ImGuiCol_TitleBgActive]         = c.surface;
    col[ImGuiCol_TitleBgCollapsed]      = c.surface;
    col[ImGuiCol_MenuBarBg]             = c.surface;

    col[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ScrollbarGrab]         = c.border;
    col[ImGuiCol_ScrollbarGrabHovered]  = c.textMuted;
    col[ImGuiCol_ScrollbarGrabActive]   = c.textMuted;

    col[ImGuiCol_CheckMark]             = c.accent;
    col[ImGuiCol_SliderGrab]            = c.accent;
    col[ImGuiCol_SliderGrabActive]      = c.accentActive;

    col[ImGuiCol_Button]                = c.surfaceAlt;
    col[ImGuiCol_ButtonHovered]         = alpha(c.accent, 0.12f);
    col[ImGuiCol_ButtonActive]          = alpha(c.accent, 0.20f);

    col[ImGuiCol_Header]                = alpha(c.accent, 0.10f);
    col[ImGuiCol_HeaderHovered]         = alpha(c.accent, 0.16f);
    col[ImGuiCol_HeaderActive]          = alpha(c.accent, 0.22f);

    col[ImGuiCol_Separator]             = c.border;
    col[ImGuiCol_SeparatorHovered]      = c.accent;
    col[ImGuiCol_SeparatorActive]       = c.accentActive;

    col[ImGuiCol_ResizeGrip]            = c.border;
    col[ImGuiCol_ResizeGripHovered]     = c.accent;
    col[ImGuiCol_ResizeGripActive]      = c.accentActive;

    col[ImGuiCol_Tab]                   = c.surfaceAlt;
    col[ImGuiCol_TabHovered]            = alpha(c.accent, 0.16f);
    col[ImGuiCol_TabActive]             = c.surface;
    col[ImGuiCol_TabUnfocused]          = c.surfaceAlt;
    col[ImGuiCol_TabUnfocusedActive]    = c.surface;

    col[ImGuiCol_PlotHistogram]         = c.accent;
    col[ImGuiCol_PlotHistogramHovered]  = c.accentHover;

    col[ImGuiCol_TextSelectedBg]        = alpha(c.accent, 0.25f);
    col[ImGuiCol_NavHighlight]          = c.accent;        // keyboard focus ring
    col[ImGuiCol_DragDropTarget]        = c.accent;
    col[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.40f); // M3 scrim
}
} // namespace

void apply(float dpiScale, Mode m) {
    gScale = dpiScale > 0.0f ? dpiScale : 1.0f;
    gSpace = { scale(4), scale(8), scale(12), scale(16), scale(24) };
    gMode  = m;
    installStyle();
}

void setMode(Mode m) {
    if (m == gMode) return;
    gMode = m;
    installStyle();   // re-skin in place; fonts (already in the atlas) are reused
}

// --- Component helpers ----------------------------------------------------
namespace {
bool coloredButton(const char* label, const ImVec4& base, const ImVec4& hover,
                   const ImVec4& active, const ImVec4& text, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, pill()); // Material pill shape
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}
} // namespace

bool PrimaryButton(const char* label, const ImVec2& size) {
    const Palette& c = gPalette;
    return coloredButton(label, c.accent, c.accentHover, c.accentActive,
                         c.onAccent, size);
}

bool SecondaryButton(const char* label, const ImVec2& size) {
    // Tonal "filled" secondary button: themed Button colours + a pill shape, to
    // match the primary action's silhouette.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, pill());
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    return pressed;
}

bool DangerButton(const char* label, const ImVec2& size) {
    const Palette& c = gPalette;
    return coloredButton(label, c.danger, c.dangerHover, c.danger,
                         ImVec4(1, 1, 1, 1), size);
}

void SectionHeader(const char* text) {
    ImGui::Dummy(ImVec2(0, gSpace.xs));
    if (gFontHeader) ImGui::PushFont(gFontHeader);
    ImGui::PushStyleColor(ImGuiCol_Text, gPalette.textMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    if (gFontHeader) ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, gSpace.xs));
}

void BeginCard(const char* id, const char* title, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, gPalette.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, scale(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gSpace.lg, gSpace.lg));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    if (title) {
        // Compact uppercase title; reuse the muted look without the big font so
        // cards stay dense.
        ImGui::PushStyleColor(ImGuiCol_Text, gPalette.textMuted);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void RowLabel(const char* label) {
    // Fixed label column ~40% of the row, control fills the rest.
    float colW = ImGui::GetContentRegionAvail().x * 0.42f;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, gPalette.textMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(colW);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool SegmentedControl(const char* id, const char* const labels[], int count,
                      int* current) {
    bool changed = false;
    const Palette& c = gPalette;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(scale(4), 0));
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        bool sel = (*current == i);
        // Material 3 segmented button: the selected segment is a tonal
        // secondary-container chip; the rest are quiet surfaces.
        ImVec4 base   = sel ? c.secondaryContainer       : c.surfaceAlt;
        ImVec4 hover  = sel ? c.secondaryContainer       : alpha(c.accent, 0.12f);
        ImVec4 active = sel ? c.secondaryContainer       : alpha(c.accent, 0.20f);
        ImVec4 text   = sel ? c.onSecondaryContainer     : c.text;
        if (coloredButton(labels[i], base, hover, active, text,
                          ImVec2(0, 0)) && !sel) {
            *current = i;
            changed = true;
        }
    }
    ImGui::PopStyleVar(); // ItemSpacing (pill rounding is handled per-button)
    ImGui::PopID();
    return changed;
}

void StatusBar(const char* leftText, const char* rightText) {
    ImGui::PushStyleColor(ImGuiCol_Text, gPalette.textMuted);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(leftText ? leftText : "");
    if (rightText && rightText[0]) {
        float w = ImGui::CalcTextSize(rightText).x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - w);
        ImGui::TextUnformatted(rightText);
    }
    ImGui::PopStyleColor();
}

} // namespace theme
