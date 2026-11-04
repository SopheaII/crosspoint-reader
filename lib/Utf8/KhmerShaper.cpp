#include "KhmerShaper.h"

namespace khmer {
namespace {

constexpr uint32_t COENG = 0x17D2;
constexpr uint32_t RO = 0x179A;
constexpr uint32_t NYO = 0x1789;
constexpr uint32_t PRE_BASE_E = 0x17C1;
constexpr size_t MAX_SUBSCRIPTS = 3;
constexpr size_t MAX_SIGNS = 6;

// Decodes a three-byte sequence, the only length Khmer and the slots use.
uint32_t decode3(const unsigned char* p) {
  return (static_cast<uint32_t>(p[0] & 0x0F) << 12) | (static_cast<uint32_t>(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
}

// Returns the code point at text[pos], or 0 unless it is Khmer or a joiner.
uint32_t clusterCodepoint(const std::string_view text, const size_t pos) {
  if (pos + 3 > text.size()) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text.data() + pos);
  const bool khmer = p[0] == 0xE1 && (p[1] == 0x9E || p[1] == 0x9F) && (p[2] & 0xC0) == 0x80;
  const bool joiner = p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x8C || p[2] == 0x8D);
  return khmer || joiner ? decode3(p) : 0;
}

bool isJoiner(const uint32_t cp) { return cp == 0x200C || cp == 0x200D; }
bool isConsonant(const uint32_t cp) { return cp >= 0x1780 && cp <= 0x17A2; }
bool isBase(const uint32_t cp) { return cp >= 0x1780 && cp <= 0x17B3; }
bool isVowel(const uint32_t cp) { return cp >= 0x17B6 && cp <= 0x17C5; }
bool isSign(const uint32_t cp) { return (cp >= 0x17C6 && cp <= 0x17D1) || cp == 0x17D3 || cp == 0x17DD; }
bool isBelowVowel(const uint32_t cp) { return cp >= 0x17BB && cp <= 0x17BD; }

int aboveIndex(const uint32_t cp) {
  switch (cp) {
    case 0x17B7:
      return 0;
    case 0x17B8:
      return 1;
    case 0x17B9:
      return 2;
    case 0x17BA:
      return 3;
    case 0x17C6:
      return 4;
    case 0x17CB:
      return 5;
    case 0x17CC:
      return 6;
    case 0x17CD:
      return 7;
    default:
      return -1;
  }
}

// Below-vowel contexts 0 and 1 are "under a subscript" and "with coeng ro".
int belowContextForBase(const uint32_t base) {
  switch (base) {
    case NYO:
      return 2;
    case 0x178E:
      return 3;
    case RO:
      return 4;
    case 0x179C:
      return 5;
    case 0x17A1:
      return 6;
    default:
      return -1;
  }
}

int aboveClassForBase(const uint32_t base) {
  switch (base) {
    case 0x178E:
      return 0;
    case RO:
      return 1;
    case 0x179C:
      return 2;
    default:
      return -1;
  }
}

char* emit(char* out, const uint32_t cp) {
  *out++ = static_cast<char>(0xE0 | (cp >> 12));
  *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  *out++ = static_cast<char>(0x80 | (cp & 0x3F));
  return out;
}

struct Cluster {
  uint32_t base = 0;
  uint32_t vowel = 0;
  uint32_t subscripts[MAX_SUBSCRIPTS] = {};
  uint32_t signs[MAX_SIGNS] = {};
  size_t subscriptCount = 0;
  size_t signCount = 0;
  bool coengRo = false;
};

// The form of a dependent mark after this cluster's base and subscripts.
uint32_t markForm(const Cluster& cluster, const uint32_t mark) {
  if (isBelowVowel(mark)) {
    int context = belowContextForBase(cluster.base);
    if (cluster.coengRo) context = 1;
    if (cluster.subscriptCount > 0) context = 0;
    return context < 0 ? mark : SLOT_BELOW + context * 3 + (mark - 0x17BB);
  }
  const int index = aboveIndex(mark);
  const int baseClass = aboveClassForBase(cluster.base);
  return index < 0 || baseClass < 0 ? mark : SLOT_ABOVE + baseClass * 8 + index;
}

char* emitCluster(char* out, const Cluster& cluster) {
  const uint32_t vowel = cluster.vowel;
  const bool preBaseOwn = vowel >= PRE_BASE_E && vowel <= 0x17C3;
  const bool split = vowel == 0x17BE || vowel == 0x17BF || vowel == 0x17C0 || vowel == 0x17C4 || vowel == 0x17C5;
  if (preBaseOwn) out = emit(out, vowel);
  if (split) out = emit(out, PRE_BASE_E);
  if (cluster.coengRo) out = emit(out, SLOT_SUBSCRIPT + (RO - 0x1780));

  out = emit(out, cluster.base == NYO && cluster.subscriptCount > 0 ? SLOT_NYO_WITH_SUBSCRIPT : cluster.base);
  for (size_t i = 0; i < cluster.subscriptCount; i++) {
    out = emit(out, SLOT_SUBSCRIPT + (cluster.subscripts[i] - 0x1780));
  }

  switch (vowel) {
    case 0:
      break;
    case 0x17BE:
      out = emit(out, markForm(cluster, 0x17B8));
      break;
    case 0x17BF:
      out = emit(out, SLOT_RIGHT_17BF);
      break;
    case 0x17C0:
      out = emit(out, SLOT_RIGHT_17C0);
      break;
    case 0x17C4:
      out = emit(out, 0x17B6);
      break;
    case 0x17C5:
      out = emit(out, SLOT_RIGHT_17C5);
      break;
    default:
      if (!preBaseOwn) out = emit(out, markForm(cluster, vowel));
      break;
  }

  for (size_t i = 0; i < cluster.signCount; i++) {
    uint32_t sign = cluster.signs[i];
    // The sign that shifts a consonant's series moves below an above vowel.
    if (sign == 0x17CA && (vowel == 0x17B7 || vowel == 0x17B8 || vowel == 0x17B9 || vowel == 0x17BA)) sign = 0x17BB;
    out = emit(out, markForm(cluster, sign));
  }
  return out;
}

}  // namespace

bool containsKhmer(const std::string_view text) {
  for (size_t i = 0; i + 1 < text.size(); i++) {
    const auto first = static_cast<unsigned char>(text[i]);
    const auto second = static_cast<unsigned char>(text[i + 1]);
    if (first == 0xE1 && (second == 0x9E || second == 0x9F)) return true;
  }
  return false;
}

size_t shape(const std::string_view input, char* const output) {
  char* out = output;
  size_t pos = 0;
  while (pos < input.size()) {
    const uint32_t first = clusterCodepoint(input, pos);
    if (!isBase(first)) {
      *out++ = input[pos++];
      continue;
    }
    Cluster cluster;
    cluster.base = first;
    pos += 3;
    while (true) {
      const uint32_t cp = clusterCodepoint(input, pos);
      if (isJoiner(cp)) {
        pos += 3;
      } else if (cp == COENG && isConsonant(clusterCodepoint(input, pos + 3))) {
        const uint32_t consonant = clusterCodepoint(input, pos + 3);
        if (consonant == RO) {
          if (cluster.coengRo) break;
          cluster.coengRo = true;
        } else {
          if (cluster.subscriptCount == MAX_SUBSCRIPTS) break;
          cluster.subscripts[cluster.subscriptCount++] = consonant;
        }
        pos += 6;
      } else if (isVowel(cp) && cluster.vowel == 0) {
        cluster.vowel = cp;
        pos += 3;
      } else if (isSign(cp) && cluster.signCount < MAX_SIGNS) {
        cluster.signs[cluster.signCount++] = cp;
        pos += 3;
      } else {
        break;
      }
    }
    out = emitCluster(out, cluster);
  }
  *out = '\0';
  return static_cast<size_t>(out - output);
}

bool canBreakAt(const char* const text, const size_t offset) {
  const auto* next = reinterpret_cast<const unsigned char*>(text + offset);
  if (next[0] == 0xE1 && next[1] == 0x9E && (next[2] & 0xC0) == 0x80) {
    if (decode3(next) >= 0x17B6) return false;
  } else if (next[0] == 0xE1 && next[1] == 0x9F && (next[2] & 0xC0) == 0x80) {
    const uint32_t cp = decode3(next);
    if (cp <= 0x17C0 || (cp >= 0x17C4 && cp <= 0x17D3) || cp == 0x17DD) return false;
  } else if (next[0] == 0xEE && (next[1] == 0x9E || next[1] == 0x9F) && (next[2] & 0xC0) == 0x80) {
    const uint32_t cp = decode3(next);
    if (cp >= SLOT_SUBSCRIPT && cp <= SLOT_LAST && cp != SLOT_SUBSCRIPT + (RO - 0x1780) &&
        cp != SLOT_NYO_WITH_SUBSCRIPT) {
      return false;
    }
  }
  if (offset < 3) return true;
  const auto* previous = next - 3;
  if ((previous[0] == 0xE1 && previous[1] == 0x9F) || previous[0] == 0xEE) {
    const uint32_t cp = decode3(previous);
    if ((cp >= PRE_BASE_E && cp <= 0x17C3) || cp == SLOT_SUBSCRIPT + (RO - 0x1780)) return false;
  }
  return true;
}

}  // namespace khmer
