#include <gtest/gtest.h>

#include <string>

#include "ReaderSession.h"
#include "ReadingTimeFormat.h"

namespace {

TEST(ReaderSession, UsesOnlyBoundedScalarState) { EXPECT_LE(sizeof(ReaderSession), 48u); }

void anchor(ReaderSession& session, uint32_t ms, int64_t epoch, int progress) {
  session.onRenderComplete(ms, epoch, progress);
}

void turnAndRender(ReaderSession& session, bool forward, bool succeeded, uint32_t ms, int64_t epoch, int progress) {
  session.noteTurn(forward, succeeded);
  session.onRenderComplete(ms, epoch, progress);
}

TEST(ReaderSession, IgnoresDwellsUnderTwoSeconds) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, true, true, 2999, 102, 1100);

  EXPECT_EQ(session.durationSeconds(), 0u);
  EXPECT_FALSE(session.isEmitWorthy());
}

TEST(ReaderSession, AccumulatesSuccessfulForwardDwellsAndCompactsEndTime) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, true, true, 6000, 105, 1100);
  turnAndRender(session, true, true, 13000, 112, 1200);

  EXPECT_EQ(session.startTime(), 100);
  EXPECT_EQ(session.durationSeconds(), 12u);
  EXPECT_EQ(session.endTime(), 112);
  EXPECT_EQ(session.endTime(), session.startTime() + session.durationSeconds());
  EXPECT_EQ(session.startProgressBp(), 1000);
  EXPECT_EQ(session.endProgressBp(), 1200);
  EXPECT_TRUE(session.isEmitWorthy());
}

TEST(ReaderSession, ExcludesBackwardSkipAndFailedTurns) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, false, true, 11000, 110, 900);
  turnAndRender(session, true, false, 21000, 120, 1100);
  session.noteTurn(false, false);  // owner uses this signal for skips
  session.onRenderComplete(31000, 130, 2000);

  EXPECT_EQ(session.durationSeconds(), 0u);
}

TEST(ReaderSession, CapsOneDwellAtThirtyMinutes) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, true, true, 3601000, 3700, 2000);

  EXPECT_EQ(session.durationSeconds(), 1800u);
  EXPECT_EQ(session.endTime(), 1900);
}

TEST(ReaderSession, KeepsFirstEpochAndProgressAcrossContributions) {
  ReaderSession session;
  anchor(session, 1000, 1000, 3000);
  turnAndRender(session, true, true, 5000, 1004, 3100);
  turnAndRender(session, true, true, 11000, 1010, 3300);

  EXPECT_EQ(session.startTime(), 1000);
  EXPECT_EQ(session.startProgressBp(), 3000);
  EXPECT_EQ(session.endProgressBp(), 3300);
  EXPECT_EQ(session.durationSeconds(), 10u);
}

TEST(ReaderSession, ClampsProgressRangeAndMonotonicEnd) {
  ReaderSession session;
  anchor(session, 1000, 100, -50);
  turnAndRender(session, true, true, 7000, 106, 12000);
  turnAndRender(session, true, true, 13000, 112, 5000);

  EXPECT_EQ(session.startProgressBp(), 0);
  EXPECT_EQ(session.endProgressBp(), 10000);
}

TEST(ReaderSession, RequiresTenSecondsAndTrustedStart) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 11000, 10, 1100);
  EXPECT_EQ(session.durationSeconds(), 0u);

  turnAndRender(session, true, true, 20000, 19, 1200);
  EXPECT_EQ(session.durationSeconds(), 9u);
  EXPECT_FALSE(session.isEmitWorthy());

  turnAndRender(session, true, true, 22000, 21, 1300);
  EXPECT_EQ(session.durationSeconds(), 11u);
  EXPECT_TRUE(session.isEmitWorthy());
  EXPECT_GT(session.endTime(), session.startTime());
}

TEST(ReaderSession, ResetClearsSummaryAnchorAndPendingTurn) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  session.noteTurn(true, true);
  session.reset();
  session.onRenderComplete(12000, 111, 2000);

  EXPECT_EQ(session.startTime(), 0);
  EXPECT_EQ(session.durationSeconds(), 0u);
  EXPECT_EQ(session.startProgressBp(), 0);
  EXPECT_EQ(session.endProgressBp(), 0);
  EXPECT_FALSE(session.isEmitWorthy());
}

TEST(ReaderSession, MonotonicElapsedHandlesMillisWrap) {
  ReaderSession session;
  anchor(session, 0xfffffc18u, 100, 1000);
  turnAndRender(session, true, true, 4000u, 105, 1100);

  EXPECT_EQ(session.durationSeconds(), 5u);
}

TEST(ReaderSessionReadingTime, CountsEveryKindOfTurnWithoutAClock) {
  ReaderSession session;
  anchor(session, 1000, 0, 0);
  turnAndRender(session, true, true, 2000, 0, 0);    // short forward page
  turnAndRender(session, false, true, 12000, 0, 0);  // backward turn
  turnAndRender(session, true, false, 22000, 0, 0);  // failed turn, same page redrawn

  EXPECT_EQ(session.takeReadingSeconds(22000, false), 21u);
  EXPECT_EQ(session.durationSeconds(), 0u);  // plugin summary rules are unchanged
}

TEST(ReaderSessionReadingTime, CurrentPageCountsOnlyWhenRequested) {
  ReaderSession counted;
  anchor(counted, 1000, 0, 0);
  EXPECT_EQ(counted.takeReadingSeconds(61000, true), 60u);

  ReaderSession dropped;
  anchor(dropped, 1000, 0, 0);
  turnAndRender(dropped, true, true, 11000, 0, 0);
  EXPECT_EQ(dropped.takeReadingSeconds(611000, false), 10u);
}

TEST(ReaderSessionReadingTime, CapsEachPageAtThirtyMinutes) {
  ReaderSession session;
  anchor(session, 0, 0, 0);
  EXPECT_EQ(session.takeReadingSeconds(2u * 3600u * 1000u, true), 1800u);
}

TEST(ReaderSessionReadingTime, NothingCountsBeforeFirstPageOrAfterReset) {
  ReaderSession session;
  EXPECT_EQ(session.takeReadingSeconds(50000, true), 0u);

  anchor(session, 1000, 0, 0);
  session.reset();
  EXPECT_EQ(session.takeReadingSeconds(50000, true), 0u);
}

TEST(ReaderSessionReadingTime, PendingMatchesWhatATakeWouldReturnAndChangesNothing) {
  ReaderSession session;
  EXPECT_EQ(session.pendingReadingSeconds(9000), 0u);  // no page yet

  anchor(session, 1000, 0, 0);
  turnAndRender(session, true, true, 11000, 0, 0);  // 10 s banked, new page anchored at 11000
  EXPECT_EQ(session.pendingReadingSeconds(41000), 40u);
  EXPECT_EQ(session.pendingReadingSeconds(41000), 40u);  // asking twice changes nothing
  EXPECT_EQ(session.takeReadingSeconds(41000, true), 40u);
  session.reset();  // the reader resets right after a take
  EXPECT_EQ(session.pendingReadingSeconds(41000), 0u);
}

TEST(ReaderSessionReadingTime, PendingCapsTheCurrentPage) {
  ReaderSession session;
  anchor(session, 0, 0, 0);
  EXPECT_EQ(session.pendingReadingSeconds(5u * 3600u * 1000u), 1800u);
}

TEST(ReaderSessionReadingTime, ASecondFlushDoesNotBankTheSameInterval) {
  // Manual sleep: sleep entry counts the page on screen, activity exit flushes again.
  ReaderSession manual;
  anchor(manual, 1000, 0, 0);
  turnAndRender(manual, true, true, 11000, 0, 0);  // 10 s completed, page anchored at 11000
  EXPECT_EQ(manual.takeReadingSeconds(61000, true), 60u);
  EXPECT_EQ(manual.takeReadingSeconds(91000, true), 0u);
  EXPECT_EQ(manual.pendingReadingSeconds(120000), 0u);

  // Auto-sleep: the page on screen is dropped, and the second flush stays empty.
  ReaderSession timedOut;
  anchor(timedOut, 1000, 0, 0);
  turnAndRender(timedOut, true, true, 11000, 0, 0);
  EXPECT_EQ(timedOut.takeReadingSeconds(61000, false), 10u);
  EXPECT_EQ(timedOut.takeReadingSeconds(91000, true), 0u);
}

TEST(ReaderSessionReadingTime, AFlushStartsANewIntervalAtTheNextRender) {
  ReaderSession session;
  anchor(session, 1000, 0, 0);
  turnAndRender(session, true, true, 11000, 0, 0);
  EXPECT_EQ(session.takeReadingSeconds(61000, false), 10u);

  // A reader that keeps running after a flush (KOReader sync) anchors afresh.
  session.onRenderComplete(62000, 62, 0);
  EXPECT_EQ(session.pendingReadingSeconds(72000), 10u);
  EXPECT_EQ(session.takeReadingSeconds(72000, true), 10u);
}

TEST(ReadingTimeFormat, AddingSecondsSaturatesInsteadOfWrapping) {
  EXPECT_EQ(addReadingSeconds(10, 5), 15u);
  EXPECT_EQ(addReadingSeconds(UINT32_MAX - 2, 5), UINT32_MAX);
  EXPECT_EQ(addReadingSeconds(UINT32_MAX, 1), UINT32_MAX);
}

TEST(ReaderSessionReadingTime, HandlesMillisWrap) {
  ReaderSession session;
  anchor(session, 0xfffffc18u, 0, 0);
  EXPECT_EQ(session.takeReadingSeconds(4000u, true), 5u);
}

TEST(ReadingTimeFormat, SplitsTotalIntoDaysAndLeftoverHours) {
  char text[32];
  const auto format = [&text](uint32_t seconds) {
    formatReadingTime(seconds, "%lud %luh %lum", text, sizeof(text));
    return std::string(text);
  };
  EXPECT_EQ(format(0), "0d 0h 0m");
  EXPECT_EQ(format(59), "0d 0h 0m");
  EXPECT_EQ(format(60), "0d 0h 1m");
  EXPECT_EQ(format(3599), "0d 0h 59m");
  EXPECT_EQ(format(9 * 3600 + 40 * 60), "0d 9h 40m");
  EXPECT_EQ(format(24 * 3600), "1d 0h 0m");
  EXPECT_EQ(format(27 * 3600 + 5 * 60), "1d 3h 5m");
  EXPECT_EQ(format(UINT32_MAX), "49710d 6h 28m");
}

TEST(ReadingTimeFormat, TruncatesSafelyInASmallBuffer) {
  char text[6];
  formatReadingTime(27 * 3600, "%lud %luh %lum", text, sizeof(text));
  EXPECT_STREQ(text, "1d 3h");
}

}  // namespace
