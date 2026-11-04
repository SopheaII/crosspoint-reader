#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Books the reader marked as favorites. Only path hashes stay in RAM (at most
// 800 bytes, on every screen); the Library matches them against the hash its
// index stores. The paths live in the file and are loaded just to change them.
class FavoritesStore : public PersistableStore<FavoritesStore> {
 public:
  static constexpr size_t MAX_FAVORITES = 100;

 private:
  static constexpr uint32_t EDIT_MIN_FREE_HEAP = 48 * 1024;
  static constexpr uint32_t EDIT_MIN_BLOCK = 16 * 1024;

  std::vector<std::string> paths;  // empty except during load() and edits
  std::vector<uint64_t> hashes;

  FavoritesStore() = default;
  ~FavoritesStore() = default;
  friend class PersistableStore<FavoritesStore>;

  // Loads the paths for an edit; false means the file could not be read.
  bool beginEdit();
  void rebuildHashes();
  // Saves when changed and releases the paths; false when the save failed.
  bool finishEdit(bool changed);

 public:
  static const char* getFilePath() { return "/.crosspoint/favorites.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Call once at boot: reads the file and keeps only the hashes.
  void load();
  bool isFavorite(const std::string& path) const;
  bool isFavoriteHash(uint64_t pathHash) const;
  // False when the list is full or could not be read.
  bool add(const std::string& path);
  // Returns true if an entry was removed.
  bool remove(const std::string& path);
  // Repoint a moved or renamed book.
  void updatePath(const std::string& oldPath, const std::string& newPath);
  // Drop entries whose file is gone, such as books renamed on a PC.
  void pruneMissing();
  size_t count() const { return hashes.size(); }
  bool full() const { return hashes.size() >= MAX_FAVORITES; }
};

#define FAVORITES FavoritesStore::getInstance()
