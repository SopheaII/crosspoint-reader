#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_AVOID_REPEAT_LOOKBACK = 5;

  std::string openEpubPath;
  uint32_t sleepLcgCount = 0;
  uint32_t sleepLcgState = 0;
  uint32_t sleepOverlayLcgCount = 0;
  uint32_t sleepOverlayLcgState = 0;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  uint32_t totalReadingSeconds = 0;
  // Books marked read from the reader menu. The per-book flag lives in that
  // book's cache dir; this is the running total shown in the Home header.
  uint32_t booksRead = 0;

  void addReadingSeconds(const uint32_t seconds) {
    const uint32_t room = UINT32_MAX - totalReadingSeconds;
    totalReadingSeconds += seconds < room ? seconds : room;
  }

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void getSleepLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft, uint16_t& indexesTotal) const;
  void getSleepOverlayLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft, uint16_t& indexesTotal) const;

  void setSleepLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal);
  void setSleepOverlayLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal);
};

#define APP_STATE CrossPointState::getInstance()
