#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Rule-based Khmer shaping for fonts with one fixed glyph per code point.
//
// shape() rewrites logical Khmer text into drawing order: pre-base vowels and
// coeng ro move before the consonant, split vowels divide in two, and contextual
// forms become private-use code points. The font converter
// (lib/EpdFont/scripts/fontconvert_sdcard.py) fills those code points with the
// matching glyphs and offsets, so the two lists of slots must stay in step.
namespace khmer {

constexpr uint32_t SLOT_SUBSCRIPT = 0xE780;  // + (consonant - U+1780)
constexpr uint32_t SLOT_NYO_WITH_SUBSCRIPT = 0xE7A3;
constexpr uint32_t SLOT_RIGHT_17BF = 0xE7A4;
constexpr uint32_t SLOT_RIGHT_17C0 = 0xE7A5;
constexpr uint32_t SLOT_RIGHT_17C5 = 0xE7A6;
constexpr uint32_t SLOT_BELOW = 0xE7B0;  // + context * 3 + (vowel - U+17BB)
constexpr uint32_t SLOT_ABOVE = 0xE7D0;  // + base class * 8 + above-mark index
constexpr uint32_t SLOT_LAST = 0xE7E7;

// Whether text has anything shape() would change. Allocates nothing.
bool containsKhmer(std::string_view text);

// Output buffer size that shape() can never exceed.
constexpr size_t maxShapedSize(const size_t inputSize) { return inputSize * 2 + 1; }

// Writes the shaped text to output and returns its size. Bytes outside Khmer
// clusters are copied unchanged, so markup and invalid UTF-8 pass through.
size_t shape(std::string_view input, char* output);

// False when a line must not break before text[offset], which would separate a
// mark or subscript from its consonant.
bool canBreakAt(const char* text, size_t offset);

}  // namespace khmer
