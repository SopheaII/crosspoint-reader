#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Adds without wrapping around.
inline uint32_t addReadingSeconds(const uint32_t total, const uint32_t seconds) {
  const uint32_t room = UINT32_MAX - total;
  return total + (seconds < room ? seconds : room);
}

// Fills a three-"%lu" format with days, leftover hours and leftover minutes, as in "0d 9h 40m".
// A day is 24 hours of reading, not a calendar day.
inline void formatReadingTime(const uint32_t totalSeconds, const char* format, char* out, const size_t outSize) {
  const uint32_t totalMinutes = totalSeconds / 60;
  const uint32_t totalHours = totalMinutes / 60;
  snprintf(out, outSize, format, static_cast<unsigned long>(totalHours / 24),
           static_cast<unsigned long>(totalHours % 24), static_cast<unsigned long>(totalMinutes % 60));
}
