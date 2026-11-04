#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

// The "read" eye shown beside reading time. Hand-drawn 16x12 with a 1px outline,
// close to the status strip's battery glyph.
namespace reading_eye {

inline constexpr int WIDTH = 16;
inline constexpr int HEIGHT = 12;
inline constexpr int GAP = 8;
inline constexpr int MAX_PADDING_SPACES = 12;
inline constexpr uint16_t ROWS[HEIGHT] = {0x0000, 0x07E0, 0x1818, 0x23C4, 0x47E2, 0x87E1,
                                          0x87E1, 0x47E2, 0x23C4, 0x1818, 0x07E0, 0x0000};

// Leading spaces that leave room for the eye and its gap before a text run.
inline int paddingSpaces(const GfxRenderer& renderer, const int fontId) {
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  if (spaceWidth <= 0) return 0;
  return std::min((WIDTH + GAP + spaceWidth - 1) / spaceWidth, MAX_PADDING_SPACES);
}

inline void draw(const GfxRenderer& renderer, const int x, const int y) {
  for (int row = 0; row < HEIGHT; ++row) {
    for (int col = 0; col < WIDTH; ++col) {
      if (ROWS[row] & (0x8000U >> col)) renderer.drawPixel(x + col, y + row, true);
    }
  }
}

}  // namespace reading_eye
