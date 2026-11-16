// Shared look for both "Cheap Yellow Display" screens: colors, smooth fonts, cards, buttons,
// avatars, progress bars, a few icons, and a screenshot of the screen as a BMP.
// Include once, from ui.cpp, after `static TFT_eSPI tft;`.
#pragma once
#include <TFT_eSPI.h>
#include "fonts/Noto16.h"
#include "fonts/Noto22.h"
#include "fonts/Noto30.h"
#include "fonts/NotoDigits56.h"

namespace kit {

constexpr uint16_t rgb(uint32_t hex) {
  return ((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F);
}

// Dark slate background with bright accents
constexpr uint16_t BG = rgb(0x0F172A), CARD = rgb(0x1E293B), CARD2 = rgb(0x334155);
constexpr uint16_t TEXT = rgb(0xF8FAFC), MUTED = rgb(0x94A3B8), FAINT = rgb(0x64748B), INK = rgb(0x111827);
constexpr uint16_t GOOD = rgb(0x22C55E), GO = rgb(0x16A34A), WARN = rgb(0xF59E0B), BAD = rgb(0xEF4444);
constexpr uint16_t STOP = rgb(0xDC2626), ACCENT = rgb(0x7C3AED), SKY = rgb(0x38BDF8), SOFT_RED = rgb(0xFCA5A5);

// Every person gets their own color
static const uint16_t PERSON[] = {rgb(0xF97316), rgb(0x8B5CF6), rgb(0xEC4899), rgb(0x10B981),
                                  rgb(0x0EA5E9), rgb(0xEAB308), rgb(0xF43F5E), rgb(0x14B8A6)};
inline uint16_t personColor(int id) { return PERSON[(id > 0 ? id - 1 : 0) % 8]; }

inline uint16_t mix(uint16_t a, uint16_t b, uint8_t amountOfA) { return tft.alphaBlend(amountOfA, a, b); }
inline bool isLight(uint16_t c) {
  int r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
  return r * 299 + g * 587 + b * 114 > 160000;
}
inline uint16_t onColor(uint16_t c) { return isLight(c) ? INK : TEXT; }      // text on a colored fill
inline uint16_t softOn(uint16_t c) { return mix(onColor(c), c, 200); }      // second line on a colored fill

struct Rect {
  int16_t x, y, w, h;
  bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  int16_t cx() const { return x + w / 2; }
  int16_t cy() const { return y + h / 2; }
};

// ---- Text ---------------------------------------------------------------------------

enum Font : uint8_t { SM, MD, LG, XL, NONE };  // 16, 22, 30 px and 56 px digits
static Font loaded = NONE;

inline void useFont(Font f) {
  if (f == loaded) return;
  if (loaded != NONE) tft.unloadFont();
  tft.loadFont(f == SM ? Noto16 : f == MD ? Noto22 : f == LG ? Noto30 : NotoDigits56);
  loaded = f;
}

inline int textW(const String& s, Font f) {
  useFont(f);
  return tft.textWidth(s);
}

inline String fit(String s, int w, Font f) {
  if (textW(s, f) <= w) return s;
  while (s.length() && textW(s + "..", f) > w) s.remove(s.length() - 1);
  return s + "..";
}

// The bigger font if the text fits in w, else the smaller one
inline Font fitFont(const String& s, int w, Font big, Font small) { return textW(s, big) <= w ? big : small; }

// datum: TL_DATUM, ML_DATUM, MC_DATUM, MR_DATUM, TC_DATUM ... ; pad clears that width behind the text
inline void text(const String& s, int x, int y, Font f, uint16_t fg, uint16_t bg, uint8_t datum = ML_DATUM, int pad = 0) {
  useFont(f);
  tft.setTextWrap(false, false);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(datum);
  tft.setTextPadding(pad);
  tft.drawString(s, x, y);
  tft.setTextPadding(0);
}

// Up to two centered lines, split at a space
inline void text2(const String& s, int cx, int y, int w, Font f, uint16_t fg, uint16_t bg) {
  if (textW(s, f) <= w) return text(s, cx, y, f, fg, bg, MC_DATUM);
  int cut = -1;
  for (int i = 0; i < (int)s.length(); i++)
    if (s[i] == ' ' && textW(s.substring(0, i), f) <= w) cut = i;
  if (cut < 0) return text(fit(s, w, f), cx, y, f, fg, bg, MC_DATUM);
  int lh = f == SM ? 20 : 26;
  text(s.substring(0, cut), cx, y - lh / 2, f, fg, bg, MC_DATUM);
  text(fit(s.substring(cut + 1), w, f), cx, y + lh / 2, f, fg, bg, MC_DATUM);
}

// ---- Shapes -------------------------------------------------------------------------

inline void card(const Rect& r, uint16_t c, uint16_t bg = BG, int radius = 12) {
  tft.fillSmoothRoundRect(r.x, r.y, r.w, r.h, radius, c, bg);
}

inline void button(const Rect& r, const String& label, uint16_t c, Font f = MD, uint16_t bg = BG) {
  card(r, c, bg);
  if (f == MD && textW(label, MD) > r.w - 16) f = SM;
  text(fit(label, r.w - 16, f), r.cx(), r.cy(), f, onColor(c), c, MC_DATUM);
}

inline void avatar(int cx, int cy, int r, uint16_t c, const String& name, bool onFill, uint16_t bg) {
  uint16_t fill = onFill ? mix(TEXT, c, 235) : c;
  tft.fillSmoothCircle(cx, cy, r, fill, bg);
  String initial = name.length() ? String((char)toupper(name[0])) : String("?");
  text(initial, cx, cy + 1, r >= 20 ? LG : MD, onFill ? c : onColor(c), fill, MC_DATUM);
}

// fraction in 0..1000
inline void bar(int x, int y, int w, int h, int32_t frac, uint16_t c, uint16_t track, uint16_t bg) {
  tft.fillSmoothRoundRect(x, y, w, h, h / 2, track, bg);
  int fw = (int64_t)w * constrain(frac, 0, 1000) / 1000;
  if (fw >= h) tft.fillSmoothRoundRect(x, y, fw, h, h / 2, c, bg);
  else if (fw > 0) tft.fillSmoothCircle(x + h / 2, y + h / 2, h / 2, c, bg);
}

inline void checkIcon(int cx, int cy, int s, uint16_t c, uint16_t bg) {
  tft.drawWideLine(cx - s, cy, cx - s / 3, cy + s * 2 / 3, s / 2.5f, c, bg);
  tft.drawWideLine(cx - s / 3, cy + s * 2 / 3, cx + s, cy - s * 2 / 3, s / 2.5f, c, bg);
}

inline void bangIcon(int cx, int cy, int s, uint16_t c, uint16_t bg) {
  tft.drawWideLine(cx, cy - s, cx, cy + s / 4, s / 2.5f, c, bg);
  tft.fillSmoothCircle(cx, cy + s * 3 / 4, s / 4, c, bg);
}

inline void chevron(int cx, int cy, int s, bool left, uint16_t c, uint16_t bg) {
  int d = left ? 1 : -1;
  tft.drawWideLine(cx + d * s / 2, cy - s, cx - d * s / 2, cy, s / 3.0f, c, bg);
  tft.drawWideLine(cx - d * s / 2, cy, cx + d * s / 2, cy + s, s / 3.0f, c, bg);
}

inline void arrow(const Rect& r, bool up, uint16_t c, uint16_t bg) {
  int cx = r.cx(), cy = r.cy(), s = 12;
  if (up) tft.fillTriangle(cx - s, cy + s / 2, cx + s, cy + s / 2, cx, cy - s / 2 - 2, c);
  else tft.fillTriangle(cx - s, cy - s / 2, cx + s, cy - s / 2, cx, cy + s / 2 + 2, c);
}

inline void dot(int cx, int cy, int r, uint16_t c, uint16_t bg) { tft.fillSmoothCircle(cx, cy, r, c, bg); }

// ---- Screenshot ---------------------------------------------------------------------

constexpr size_t BMP_SIZE = 54 + 320 * 240 * 3;

// Streams the screen as a 24-bit BMP (top row first) to `out`
template <class Out>
void capture(Out& out) {
  uint8_t h[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { memcpy(h + at, &v, 4); };
  put32(2, BMP_SIZE);
  put32(10, 54);
  put32(14, 40);
  put32(18, 320);
  put32(22, (uint32_t)-240);  // negative height = top-down rows
  h[26] = 1;
  h[28] = 24;
  put32(34, 320 * 240 * 3);
  out.write(h, sizeof h);
  static uint8_t row[320 * 3];
  for (int y = 0; y < 240; y++) {
    tft.readRectRGB(0, y, 320, 1, row);
    for (int i = 0; i < 320 * 3; i += 3) std::swap(row[i], row[i + 2]);  // RGB -> BGR
    out.write(row, sizeof row);
  }
}

}  // namespace kit
