#include "ReaderSession.h"

#include <algorithm>

namespace {
constexpr uint32_t MIN_DWELL_SECONDS = 2;
constexpr uint32_t MAX_DWELL_SECONDS = 1800;
constexpr uint32_t MAX_PAGE_READING_MS = MAX_DWELL_SECONDS * 1000;
}  // namespace

uint16_t ReaderSession::clampProgress(const int progressBp) {
  return static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
}

void ReaderSession::noteTurn(const bool forward, const bool succeeded) { pendingForward = forward && succeeded; }

void ReaderSession::onRenderComplete(const uint32_t monotonicMs, const int64_t trustedEpoch, const int progressBp) {
  const uint16_t progress = clampProgress(progressBp);
  addReadingSince(monotonicMs);
  if (pendingForward && hasAnchor && anchorEpoch > 0) {
    const uint32_t elapsedSeconds = (monotonicMs - anchorMs) / 1000;
    if (elapsedSeconds >= MIN_DWELL_SECONDS) {
      const uint32_t contribution = std::min(elapsedSeconds, MAX_DWELL_SECONDS);
      if (duration == 0) {
        startEpoch = anchorEpoch;
        startProgress = anchorProgress;
        endProgress = anchorProgress;
      }
      duration += contribution;
      endProgress = std::max(endProgress, progress);
    }
  }

  anchorMs = monotonicMs;
  anchorEpoch = trustedEpoch;
  anchorProgress = progress;
  hasAnchor = true;
  pendingForward = false;
}

void ReaderSession::addReadingSince(const uint32_t monotonicMs) {
  if (hasAnchor) readingMs += std::min(monotonicMs - anchorMs, MAX_PAGE_READING_MS);
}

uint32_t ReaderSession::pendingReadingSeconds(const uint32_t monotonicMs) const {
  const uint32_t current = hasAnchor ? std::min(monotonicMs - anchorMs, MAX_PAGE_READING_MS) : 0;
  return (readingMs + current) / 1000;
}

uint32_t ReaderSession::takeReadingSeconds(const uint32_t monotonicMs, const bool includeCurrentPage) {
  if (includeCurrentPage) addReadingSince(monotonicMs);
  const uint32_t seconds = readingMs / 1000;
  readingMs = 0;
  // Nothing counted so far can be counted again; a new interval starts at the
  // next completed render.
  hasAnchor = false;
  return seconds;
}

void ReaderSession::reset() {
  anchorMs = 0;
  anchorEpoch = 0;
  startEpoch = 0;
  duration = 0;
  readingMs = 0;
  anchorProgress = 0;
  startProgress = 0;
  endProgress = 0;
  hasAnchor = false;
  pendingForward = false;
}
