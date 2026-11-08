#pragma once

#include <string>

// Per-book read flag shared by the reader and the Library. The flag is a
// one-byte marker in the book's cache directory (read.bin), written beside
// progress.bin; a missing marker means "not read".
namespace BookReadState {

// Marker access for a caller that already resolved the book's cache directory.
bool readMarker(const std::string& cachePath);
bool writeMarker(const std::string& cachePath, bool read);

// Marker plus the Home header total, for a caller that already resolved the
// book's cache directory. The total moves only when the flag actually changes.
bool setReadState(const std::string& cachePath, bool read);

// EPUB book path convenience: resolves the cache directory, creating it when
// the flag is set, and keeps APP_STATE.booksRead in step. Returns false only
// if the marker could not be written.
bool isBookRead(const std::string& bookPath);
bool setBookRead(const std::string& bookPath, bool read);

}  // namespace BookReadState
