#pragma once

#include <cstdint>

// Fixed-scalar active-reading summary. The owner reports turn outcomes before
// render and completed pages after render; no allocation or I/O occurs here.
class ReaderSession {
 public:
  void noteTurn(bool forward, bool succeeded);
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBp);
  void reset();
  // Whole seconds on any book page, whichever way it was reached. Call once, before
  // reset(). Pass false to drop the page still on screen (the reader may have left).
  // Ends the interval here, so a second flush (sleep entry, then activity exit)
  // cannot bank the same minutes again.
  uint32_t takeReadingSeconds(uint32_t monotonicMs, bool includeCurrentPage);
  // Seconds a later takeReadingSeconds(monotonicMs, true) would return; changes nothing.
  uint32_t pendingReadingSeconds(uint32_t monotonicMs) const;

  bool isEmitWorthy() const { return startEpoch > 0 && duration >= 10 && endTime() > startEpoch; }
  int64_t startTime() const { return startEpoch; }
  int64_t endTime() const { return startEpoch > 0 ? startEpoch + duration : 0; }
  uint32_t durationSeconds() const { return duration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }

 private:
  static uint16_t clampProgress(int progressBp);
  void addReadingSince(uint32_t monotonicMs);

  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint32_t readingMs = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  bool hasAnchor = false;
  bool pendingForward = false;
};
