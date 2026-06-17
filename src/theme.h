// theme.h - the design-token + component layer for the UI redesign.
//
// A single place that owns the "Clean light / pro-tool" look: the colour
// palette, the 4px spacing scale, rounding, DPI scaling, the (optional) larger
// header font, and a small set of reusable widget helpers (cards, semantic
// buttons, setting rows, a segmented control, a status bar). Nothing in here
// touches Vulkan or app logic - it is pure ImGui styling so it stays reusable.
#pragma once
#include <imgui.h>

struct ImFont;

namespace theme {

// --- Design tokens --------------------------------------------------------
// Colours are sRGB 0..1. Names are semantic, not literal, so screens never
// hardcode a hex value again.
struct Palette {
    ImVec4 bg;            // app background / window
    ImVec4 surface;       // panels, sidebar, cards
    ImVec4 surfaceAlt;    // nested children, input wells
    ImVec4 border;        // 1px borders, separators
    ImVec4 text;          // primary text
    ImVec4 textMuted;     // labels, secondary info
    ImVec4 accent;        // primary action, active state, progress, focus
    ImVec4 accentHover;
    ImVec4 accentActive;
    ImVec4 danger;        // destructive (Cancel)
    ImVec4 dangerHover;
    ImVec4 success;       // completed / saved
};

const Palette& colors();

// 4px spacing scale, already multiplied by the DPI factor passed to apply().
// Use these instead of magic numbers for any manual layout maths.
struct Spacing {
    float xs;   // 4
    float sm;   // 8
    float md;   // 12
    float lg;   // 16
    float xl;   // 24
};
const Spacing& space();

// DPI-aware: multiply a logical pixel size by the active scale factor.
float scale(float logicalPx);

// Fonts (both are the built-in font; the header one is simply registered at a
// larger pixel size - no external asset). Call loadFonts() once before the
// backend uploads the atlas, then apply() to install colours/sizes.
void loadFonts(ImGuiIO& io, float dpiScale);
ImFont* fontRegular();
ImFont* fontHeader();

// Install all tokens (colours + sizes) into the current ImGui style and record
// the DPI scale used by scale()/space().
void apply(float dpiScale);

// --- Component helpers ----------------------------------------------------
// Semantic buttons.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Uppercase, muted section title rendered in the header font.
void SectionHeader(const char* text);

// A padded surface child with an optional uppercase header. Always pair
// BeginCard()/EndCard(). `size` follows ImGui child semantics (0 = stretch).
void BeginCard(const char* id, const char* title = nullptr,
               const ImVec2& size = ImVec2(0, 0));
void EndCard();

// A label-left / control-right settings row. Call RowLabel(), then draw a
// single control with a "##id" label; it is stretched to the row width.
void RowLabel(const char* label);

// Segmented (tab-like) toggle. Returns true if the selection changed.
bool SegmentedControl(const char* id, const char* const labels[], int count,
                      int* current);

// Bottom status bar: left-aligned message, right-aligned detail.
void StatusBar(const char* leftText, const char* rightText);

} // namespace theme
