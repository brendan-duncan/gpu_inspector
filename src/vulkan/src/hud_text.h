// The in-app HUD's font and layout, with no graphics API in it.
//
// The HUD is drawn as axis-aligned colored rectangles and nothing else: the panel behind the
// text is one rectangle, and every lit pixel of every glyph is another. Expanding the text to
// rectangles here rather than sampling a font texture in a shader is what keeps the three
// backends' drawing code small — each one only has to put a list of {rect, color} on the screen,
// which needs no font atlas, no sampler and no descriptors, only a vertex buffer and two shaders
// that do nothing but transform and interpolate.
//
// A line of text at scale 2 is 14 pixels tall, so the whole panel is a few hundred rectangles:
// far too few for the instancing to matter next to the frame the application just drew.
//
// Header-only and API-neutral, like json_writer.h beside it: src/d3d12 and src/metal add
// src/vulkan/src to their include path for exactly this (see their CMakeLists.txt).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gpuhud
{

// One rectangle in pixels from the top-left of the target, with a straight (non-premultiplied)
// color. The backends' vertex buffers are arrays of this.
struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;
    float r = 1, g = 1, b = 1, a = 1;
};

// The font: 5x7 glyphs, one bit per pixel, top row first, bit 4 (0b10000) leftmost. Written out
// as binary literals so the glyph is legible in the source — the shapes below are the font.
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kGlyphAdvance = 6;   // one blank column between glyphs

struct Glyph
{
    char c;
    uint8_t rows[kGlyphH];
};

// Digits, capitals and the punctuation the HUD uses. Anything else prints as a space; lowercase
// is folded to uppercase (FindGlyph), since there are no lowercase shapes.
constexpr Glyph kFont[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},

    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},

    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},

    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100, 0b01000}},
    {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {'/', {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}},
    {'%', {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
    {'<', {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}},
    {'>', {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}},
};

constexpr int kFontCount = (int)(sizeof(kFont) / sizeof(kFont[0]));

inline const Glyph* FindGlyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (int i = 0; i < kFontCount; ++i)
        if (kFont[i].c == c)
            return &kFont[i];
    return &kFont[0];   // space
}

// Width in pixels of `text` drawn at `scale`, without the trailing advance gap.
inline float TextWidth(const char* text, float scale)
{
    const size_t n = text ? strlen(text) : 0;
    if (!n)
        return 0;
    return (float)((n - 1) * kGlyphAdvance + kGlyphW) * scale;
}

inline float TextHeight(float scale) { return kGlyphH * scale; }

inline void AppendRect(std::vector<Rect>& out, float x, float y, float w, float h,
    float r, float g, float b, float a)
{
    if (w <= 0 || h <= 0 || a <= 0)
        return;
    out.push_back(Rect{x, y, w, h, r, g, b, a});
}

// One rectangle per lit pixel. Runs of lit pixels in a row are merged into a single rectangle,
// which roughly halves the count on the digits and costs one comparison per pixel.
inline void AppendText(std::vector<Rect>& out, float x, float y, float scale, const char* text,
    float r, float g, float b, float a)
{
    if (!text)
        return;
    float penX = x;
    for (const char* p = text; *p; ++p, penX += kGlyphAdvance * scale)
    {
        const Glyph* glyph = FindGlyph(*p);
        for (int row = 0; row < kGlyphH; ++row)
        {
            const uint8_t bits = glyph->rows[row];
            if (!bits)
                continue;
            int col = 0;
            while (col < kGlyphW)
            {
                if (!(bits & (1u << (kGlyphW - 1 - col))))
                {
                    ++col;
                    continue;
                }
                int run = 1;
                while (col + run < kGlyphW && (bits & (1u << (kGlyphW - 1 - (col + run)))))
                    ++run;
                AppendRect(out, penX + col * scale, y + row * scale, run * scale, scale, r, g, b, a);
                col += run;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The HUD's contents
//
// What every backend draws, so the HUD reads the same whichever API the application uses and a
// change to the layout happens once. The backend fills this in from what it already measures for
// the UI's frame-time meter.

struct HudState
{
    double frameMs = 0;        // smoothed frame interval
    double minMs = 0;          // shortest and longest of the recent window
    double maxMs = 0;
    double refreshMs = 0;      // display refresh period, 0 when unknown or vsync is off
    uint64_t frame = 0;        // frame number since the library attached
    bool paused = false;
    bool capturing = false;
    const char* backend = "";  // "VULKAN", "D3D12", "METAL"
    // The key that takes a capture ("F11"), or null when there is none: the line it draws is the
    // only thing that tells the user the hotkey is live (hud_hotkey.h).
    const char* hotkey = nullptr;
};

// The panel, in pixels from the top-left. Three lines, plus a line for the capture hotkey when
// one is armed and a line for what the library is doing to the application:
//
//   GPU INSPECTOR - VULKAN
//   16.67 MS  60.0 FPS
//   MIN 15.9  MAX 18.2  VSYNC 60 HZ
//   F11 CAPTURE
//   CAPTURING FRAME 4564
//
// `scale` is the size of one font pixel; the caller picks it from the target's size so the HUD
// stays readable at 4K without swamping a small window.
inline void BuildHud(std::vector<Rect>& out, const HudState& s, uint32_t targetWidth,
    uint32_t targetHeight, float scale)
{
    (void)targetHeight;
    // One entry per line, in the order they are drawn, each with the color that says what it is.
    struct Line
    {
        const char* text;
        float r, g, b;
    };
    Line lines[5];
    int count = 0;
    char line1[64], line2[64], line3[96], line4[64], line5[64];

    snprintf(line1, sizeof(line1), "GPU INSPECTOR - %s", s.backend ? s.backend : "");
    lines[count++] = {line1, 0.36f, 0.66f, 1.00f};   // the title, in the app's blue

    const double fps = s.frameMs > 0 ? 1000.0 / s.frameMs : 0;
    snprintf(line2, sizeof(line2), "%.2f MS  %.1f FPS", s.frameMs, fps);
    lines[count++] = {line2, 0.90f, 0.92f, 0.95f};

    if (s.refreshMs > 0)
        snprintf(line3, sizeof(line3), "MIN %.2f  MAX %.2f  VSYNC %.0f HZ", s.minMs, s.maxMs, 1000.0 / s.refreshMs);
    else
        snprintf(line3, sizeof(line3), "MIN %.2f  MAX %.2f", s.minMs, s.maxMs);
    lines[count++] = {line3, 0.90f, 0.92f, 0.95f};

    // Dimmer than the figures: it is a reminder of what the keyboard does, not a measurement, and
    // it is on the screen every frame the HUD is.
    if (s.hotkey && s.hotkey[0])
    {
        snprintf(line4, sizeof(line4), "%s CAPTURE", s.hotkey);
        lines[count++] = {line4, 0.58f, 0.62f, 0.70f};
    }

    if (s.paused)
    {
        snprintf(line5, sizeof(line5), "PAUSED AT FRAME %llu", (unsigned long long)s.frame);
        lines[count++] = {line5, 1.00f, 0.78f, 0.25f};   // amber
    }
    else if (s.capturing)
    {
        snprintf(line5, sizeof(line5), "CAPTURING FRAME %llu", (unsigned long long)s.frame);
        lines[count++] = {line5, 0.40f, 0.90f, 0.50f};   // green
    }

    const float pad = 4 * scale;
    const float lineStep = (kGlyphH + 3) * scale;

    float widest = 0;
    for (int i = 0; i < count; ++i)
    {
        const float w = TextWidth(lines[i].text, scale);
        if (w > widest)
            widest = w;
    }

    const float originX = pad;
    const float originY = pad;
    const float panelW = widest + pad * 2;
    const float panelH = count * lineStep - 3 * scale + pad * 2;

    // The panel: dark and mostly opaque, so the text stays legible over a bright frame. Clamped to
    // the target in case a tiny window cannot hold it.
    AppendRect(out, originX, originY,
        panelW < (float)targetWidth ? panelW : (float)targetWidth,
        panelH, 0.04f, 0.05f, 0.07f, 0.78f);

    for (int i = 0; i < count; ++i)
        AppendText(out, originX + pad, originY + pad + i * lineStep, scale, lines[i].text,
            lines[i].r, lines[i].g, lines[i].b, 1.0f);
}

// The font pixel size for a target of this width: 1 up to 720p, 2 to 1440p, 3 beyond, so the HUD
// is about the same physical size on any display.
inline float HudScale(uint32_t targetWidth)
{
    if (targetWidth >= 2560)
        return 3.0f;
    if (targetWidth >= 1280)
        return 2.0f;
    return 1.0f;
}

} // namespace gpuhud
