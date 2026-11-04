#include <gtest/gtest.h>

#include <KhmerShaper.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string shaped(const std::string& input) {
  std::vector<char> output(khmer::maxShapedSize(input.size()));
  const size_t size = khmer::shape(input, output.data());
  EXPECT_LE(size + 1, output.size());
  return std::string(output.data(), size);
}

std::string utf8(const std::vector<uint32_t>& codepoints) {
  std::string out;
  for (const uint32_t cp : codepoints) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

constexpr uint32_t subscriptOf(const uint32_t consonant) { return khmer::SLOT_SUBSCRIPT + (consonant - 0x1780); }

}  // namespace

TEST(KhmerShaperTest, NonKhmerTextIsUntouched) {
  const std::string text = "<b>hello</b> caf\xC3\xA9 \xE4\xB8\xAD";
  EXPECT_FALSE(khmer::containsKhmer(text));
  EXPECT_EQ(shaped(text), text);
}

TEST(KhmerShaperTest, InvalidBytesPassThrough) {
  const std::string text = "a\xE1\x9E";
  EXPECT_EQ(shaped(text), text);
}

TEST(KhmerShaperTest, PreBaseVowelMovesBeforeConsonant) {
  EXPECT_TRUE(khmer::containsKhmer(utf8({0x1780})));
  EXPECT_EQ(shaped(utf8({0x1780, 0x17C1})), utf8({0x17C1, 0x1780}));
}

TEST(KhmerShaperTest, SplitVowelsDivideAroundConsonant) {
  EXPECT_EQ(shaped(utf8({0x1780, 0x17C4})), utf8({0x17C1, 0x1780, 0x17B6}));
  EXPECT_EQ(shaped(utf8({0x1780, 0x17BE})), utf8({0x17C1, 0x1780, 0x17B8}));
  EXPECT_EQ(shaped(utf8({0x1780, 0x17C5})), utf8({0x17C1, 0x1780, khmer::SLOT_RIGHT_17C5}));
}

TEST(KhmerShaperTest, SubscriptBecomesItsSlot) {
  EXPECT_EQ(shaped(utf8({0x1780, 0x17D2, 0x178F})), utf8({0x1780, subscriptOf(0x178F)}));
}

TEST(KhmerShaperTest, CoengRoMovesBeforeConsonantAndAfterPreBaseVowel) {
  EXPECT_EQ(shaped(utf8({0x1780, 0x17D2, 0x179A, 0x17C1})), utf8({0x17C1, subscriptOf(0x179A), 0x1780}));
}

TEST(KhmerShaperTest, NyoUsesItsSubscriptFormOnlyWithASubscript) {
  EXPECT_EQ(shaped(utf8({0x1789, 0x17D2, 0x1785})), utf8({khmer::SLOT_NYO_WITH_SUBSCRIPT, subscriptOf(0x1785)}));
  EXPECT_EQ(shaped(utf8({0x1789, 0x17B6})), utf8({0x1789, 0x17B6}));
}

TEST(KhmerShaperTest, BelowVowelFollowsContext) {
  EXPECT_EQ(shaped(utf8({0x1780, 0x17BB})), utf8({0x1780, 0x17BB}));
  EXPECT_EQ(shaped(utf8({0x1780, 0x17D2, 0x1780, 0x17BB})), utf8({0x1780, subscriptOf(0x1780), khmer::SLOT_BELOW}));
  EXPECT_EQ(shaped(utf8({0x1780, 0x17D2, 0x179A, 0x17BC})), utf8({subscriptOf(0x179A), 0x1780, khmer::SLOT_BELOW + 4}));
  // "I": a common word whose vowel sits lower under nyo.
  EXPECT_EQ(shaped(utf8({0x1789, 0x17BB, 0x17C6})), utf8({0x1789, khmer::SLOT_BELOW + 6, 0x17C6}));
}

TEST(KhmerShaperTest, AboveMarkFollowsBaseClass) {
  EXPECT_EQ(shaped(utf8({0x179A, 0x17B7})), utf8({0x179A, khmer::SLOT_ABOVE + 8}));
  EXPECT_EQ(shaped(utf8({0x178E, 0x17CD})), utf8({0x178E, khmer::SLOT_ABOVE + 7}));
  EXPECT_EQ(shaped(utf8({0x1780, 0x17B7})), utf8({0x1780, 0x17B7}));
}

TEST(KhmerShaperTest, SeriesShifterMovesBelowAnAboveVowel) {
  EXPECT_EQ(shaped(utf8({0x17A2, 0x17CA, 0x17B8})), utf8({0x17A2, 0x17B8, 0x17BB}));
}

TEST(KhmerShaperTest, JoinersInsideAClusterAreDropped) {
  EXPECT_EQ(shaped(utf8({0x1780, 0x200C, 0x17B6})), utf8({0x1780, 0x17B6}));
}

TEST(KhmerShaperTest, MarkWithNoConsonantStaysPut) {
  EXPECT_EQ(shaped(utf8({0x17B7})), utf8({0x17B7}));
  EXPECT_EQ(shaped(" " + utf8({0x17D2, 0x1780})), " " + utf8({0x17D2, 0x1780}));
}

TEST(KhmerShaperTest, ShapingTwiceChangesNothing) {
  const std::string once = shaped(utf8({0x1780, 0x17D2, 0x179A, 0x17C4, 0x17C7, 0x179A, 0x17B7}));
  EXPECT_EQ(shaped(once), once);
}

TEST(KhmerShaperTest, ManyMarksStayWithinTheOutputBound) {
  std::string input = utf8({0x1780});
  for (int i = 0; i < 40; i++) input += utf8({0x17D2, 0x1780, 0x17C4, 0x17C6});
  shaped(input);
}

TEST(KhmerShaperTest, LinesDoNotBreakInsideAShapedCluster) {
  const std::string text = shaped(utf8({0x1780, 0x17D2, 0x179A, 0x17C1, 0x17BB, 0x1781}));
  // Pre-base vowel, coeng ro, consonant, below vowel, next consonant.
  EXPECT_FALSE(khmer::canBreakAt(text.c_str(), 3));
  EXPECT_FALSE(khmer::canBreakAt(text.c_str(), 6));
  EXPECT_TRUE(khmer::canBreakAt(utf8({0x1780, 0x1781}).c_str(), 3));
  EXPECT_FALSE(khmer::canBreakAt(utf8({0x1780, 0x17B7}).c_str(), 3));
  EXPECT_FALSE(khmer::canBreakAt(utf8({0x1780, subscriptOf(0x1780)}).c_str(), 3));
  EXPECT_TRUE(khmer::canBreakAt(utf8({0x1780, subscriptOf(0x179A)}).c_str(), 3));
}
