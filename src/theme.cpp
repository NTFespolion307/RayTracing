#include "theme.h"
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace theme {
namespace {

// Hex -> linear-ish sRGB ImVec4 (we keep values as ImGui expects them: raw
// 0..1 sRGB, no gamma conversion, matching how StyleColorsLight ships).
constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

Palette  gPalette;
Spacing  gSpace{4, 8, 12, 16, 24};
float    gScale = 1.0f;
ImFont*  gFontRegular = nullptr;
ImFont*  gFontHeader  = nullptr;

void initPalette() {
    gPalette.bg           = rgb(0xF4, 0xF5, 0xF7);
    gPalette.surface      = rgb(0xFF, 0xFF, 0xFF);
    gPalette.surfaceAlt   = rgb(0xF8, 0xF9, 0xFB);
    gPalette.border       = rgb(0xDF, 0xE3, 0xE8);
    gPalette.text         = rgb(0x1B, 0x1F, 0x24);
    gPalette.textMuted    = rgb(0x67, 0x72, 0x81);
    gPalette.accent       = rgb(0x2D, 0x6C, 0xDF);
    gPalette.accentHover  = rgb(0x3B, 0x7B, 0xEE);
    gPalette.accentActive = rgb(0x1F, 0x58, 0xC4);
    gPalette.danger       = rgb(0xD6, 0x45, 0x45);
    gPalette.dangerHover  = rgb(0xE2, 0x5C, 0x5C);
    gPalette.success      = rgb(0x2E, 0x9E, 0x5B);
}

// A translucent variant of a colour (for hover/active fills over surfaces).
ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

} // namespace

const Palette& colors() { return gPalette; }
const Spacing& space()  { return gSpace; }
float scale(float logicalPx) { return logicalPx * gScale; }
ImFont* fontRegular() { return gFontRegular; }
ImFont* fontHeader()  { return gFontHeader; }

namespace {
// Load a TTF only if the file actually exists, so a missing system font can
// never trip ImGui's internal assert. Returns nullptr on any failure.
ImFont* tryLoadTTF(ImGuiIO& io, const char* path, float sizePx) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fclose(f);
    ImFontConfig cfg;
    cfg.OversampleH = 3;   // crisper horizontal stems
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = false;
    return io.Fonts->AddFontFromFileTTF(path, sizePx, &cfg);
}
} // namespace

void loadFonts(ImGuiIO& io, float dpiScale) {
    // Smooth, professional UI type: Segoe UI (ships with Windows) rasterised
    // with anti-aliasing, instead of the blocky built-in bitmap font. Headers
    // use the Semibold weight for hierarchy. If a face is missing we fall back
    // to the built-in font so the app still runs everywhere.
    const char* kSegoe     = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* kSegoeSemi = "C:\\Windows\\Fonts\\seguisb.ttf";   // Segoe UI Semibold
    const char* kSegoeBold  = "C:\\Windows\\Fonts\\segoeuib.ttf"; // Segoe UI Bold

    gFontRegular = tryLoadTTF(io, kSegoe, 14.5f * dpiScale);
    if (!gFontRegular) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * dpiScale;
        gFontRegular = io.Fonts->AddFontDefault(&cfg);
    }

    gFontHeader = tryLoadTTF(io, kSegoeSemi, 18.0f * dpiScale);
    if (!gFontHeader) gFontHeader = tryLoadTTF(io, kSegoeBold, 18.0f * dpiScale);
    if (!gFontHeader) gFontHeader = tryLoadTTF(io, kSegoe, 18.0f * dpiScale);
    if (!gFontHeader) {
        ImFontConfig hcfg;
        hcfg.SizePixels = 18.0f * dpiScale;
        gFontHeader = io.Fonts->AddFontDefault(&hcfg);
    }
}

void apply(float dpiScale) {
    gScale = dpiScale > 0.0f ? dpiScale : 1.0f;
    gSpace = { scale(4), scale(8), scale(12), scale(16), scale(24) };
    initPalette();

    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsLight(&s); // sane baseline, then override everything below

    const Palette& c = gPalette;

    // Spacing & sizing (logical px, DPI-scaled).
    s.WindowPadding     = ImVec2(gSpace.lg, gSpace.lg);
    s.FramePadding      = ImVec2(gSpace.md, scale(6));
    s.CellPadding       = ImVec2(gSpace.sm, scale(6));
    s.ItemSpacing       = ImVec2(gSpace.sm, gSpace.sm);
    s.ItemInnerSpacing  = ImVec2(gSpace.sm, scale(6));
    s.IndentSpacing     = gSpace.lg;
    s.ScrollbarSize     = scale(12);
    s.GrabMinSize       = scale(10);

    // Borders.
    s.WindowBorderSize  = scale(1);
    s.ChildBorderSize   = scale(1);
    s.FrameBorderSize   = scale(1);
    s.PopupBorderSize   = scale(1);
    s.TabBorderSize     = 0.0f;
    s.SeparatorTextBorderSize = scale(1);

    // Rounding.
    s.WindowRounding    = scale(8);
    s.ChildRounding     = scale(8);
    s.FrameRounding     = scale(6);
    s.PopupRounding     = scale(8);
    s.GrabRounding      = scale(4);
    s.ScrollbarRounding = scale(8);
    s.TabRounding       = scale(6);

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
    col[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.10f, 0.12f, 0.15f, 0.45f);
}

// --- Component helpers ----------------------------------------------------
namespace {
bool coloredButton(const char* label, const ImVec4& base, const ImVec4& hover,
                   const ImVec4& active, const ImVec4& text, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}
} // namespace

bool PrimaryButton(const char* label, const ImVec2& size) {
    const Palette& c = gPalette;
    return coloredButton(label, c.accent, c.accentHover, c.accentActive,
                         ImVec4(1, 1, 1, 1), size);
}

bool SecondaryButton(const char* label, const ImVec2& size) {
    return ImGui::Button(label, size); // already themed via ImGuiCol_Button
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
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, scale(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gSpace.md, gSpace.md));
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
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(scale(2), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, scale(6));
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        bool sel = (*current == i);
        ImVec4 base   = sel ? c.accent      : c.surfaceAlt;
        ImVec4 hover  = sel ? c.accentHover : alpha(c.accent, 0.12f);
        ImVec4 active = sel ? c.accentActive: alpha(c.accent, 0.20f);
        ImVec4 text   = sel ? ImVec4(1, 1, 1, 1) : c.text;
        if (coloredButton(labels[i], base, hover, active, text,
                          ImVec2(0, 0)) && !sel) {
            *current = i;
            changed = true;
        }
    }
    ImGui::PopStyleVar(2);
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
