#include "BookReadState.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointState.h"
#include "ProgressFile.h"

namespace {
// Same base directory Epub uses for its cache folders.
constexpr const char* BOOK_CACHE_DIR = "/.crosspoint";
constexpr const char* READ_FILE = "/read.bin";
constexpr const char* READ_TMP_FILE = "/read.bin.tmp";
}  // namespace

namespace BookReadState {

bool readMarker(const std::string& cachePath) {
  // A power cut between the write's remove and rename leaves only the complete .tmp copy.
  for (const char* name : {READ_FILE, READ_TMP_FILE}) {
    HalFile f;
    uint8_t marker = 0;
    if (Storage.openFileForRead("RD", cachePath + name, f) && f.read(&marker, sizeof(marker)) == sizeof(marker)) {
      return marker != 0;
    }
  }
  return false;
}

bool writeMarker(const std::string& cachePath, const bool read) {
  if (!read) {
    // Missing files already mean "not read"; the temp may be a left-over write.
    Storage.remove((cachePath + READ_FILE).c_str());
    Storage.remove((cachePath + READ_TMP_FILE).c_str());
    // A failed remove leaves the marker in place, so report the real state
    // instead of claiming the flag was cleared.
    return !Storage.exists((cachePath + READ_FILE).c_str()) &&
           !Storage.exists((cachePath + READ_TMP_FILE).c_str());
  }
  const uint8_t marker = 1;
  return ProgressFile::writeAtomic(cachePath, &marker, sizeof(marker), "read.bin");
}

bool setReadState(const std::string& cachePath, const bool read) {
  if (readMarker(cachePath) == read) return true;
  if (read && !Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("RD", "Could not create book cache dir: %s", cachePath.c_str());
    return false;
  }
  if (!writeMarker(cachePath, read)) {
    // A failed rename can leave the complete .tmp copy, which readMarker treats
    // as authoritative. Count the flag if it did land, so the total and the
    // next menu open cannot disagree.
    if (readMarker(cachePath) != read) return false;
  }
  if (read) {
    if (APP_STATE.booksRead < UINT32_MAX) ++APP_STATE.booksRead;
  } else if (APP_STATE.booksRead > 0) {
    --APP_STATE.booksRead;
  }
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("RD", "Could not save books read total");
  }
  return true;
}

bool isBookRead(const std::string& bookPath) {
  return readMarker(Epub::cachePathFor(bookPath, BOOK_CACHE_DIR));
}

bool setBookRead(const std::string& bookPath, const bool read) {
  return setReadState(Epub::cachePathFor(bookPath, BOOK_CACHE_DIR), read);
}

}  // namespace BookReadState
