#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

// The small book shown before the count of books marked read in the header
// status strip: a 12px reduction of the Lucide "book" glyph the library list
// uses for EPUB rows, drawn with the same 1px outline weight as the eye and
// the battery glyph.
namespace reading_book {

inline constexpr int WIDTH = 10;
inline constexpr int HEIGHT = 12;
// Gap between the glyph and its count, the same as the eye's.
inline constexpr int GAP = 8;
// The count is a separate statistic, so leave the glyph clear of the reading time.
inline constexpr int LEADING_GAP = 14;
inline constexpr int MAX_PADDING_SPACES = 16;
inline constexpr uint16_t ROWS[HEIGHT] = {0x3F00, 0x7F80, 0x4080, 0x4080, 0x4080, 0x4080,
                                          0x4080, 0x4080, 0xFFC0, 0x4080, 0x4080, 0x3F00};

// Leading spaces that leave room for the glyph and its gaps before a text run.
inline int paddingSpaces(const GfxRenderer& renderer, const int fontId) {
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  if (spaceWidth <= 0) return 0;
  return std::min((LEADING_GAP + WIDTH + GAP + spaceWidth - 1) / spaceWidth, MAX_PADDING_SPACES);
}

inline void draw(const GfxRenderer& renderer, const int x, const int y) {
  for (int row = 0; row < HEIGHT; ++row) {
    for (int col = 0; col < WIDTH; ++col) {
      if (ROWS[row] & (0x8000U >> col)) renderer.drawPixel(x + col, y + row, true);
    }
  }
}

}  // namespace reading_book
