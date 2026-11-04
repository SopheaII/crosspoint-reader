#include "FavoritesStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <LibraryFormat.h>
#include <Logging.h>

#include <algorithm>

void FavoritesStore::rebuildHashes() {
  hashes.clear();
  hashes.reserve(paths.size());
  for (const auto& path : paths) hashes.push_back(library::clixPathHash(path.data(), path.size()));
}

void FavoritesStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["paths"].to<JsonArray>();
  for (const auto& path : paths) arr.add(path);
}

bool FavoritesStore::fromJson(JsonVariantConst doc) {
  paths.clear();
  JsonArrayConst arr = doc["paths"].as<JsonArrayConst>();
  paths.reserve(std::min(arr.size(), MAX_FAVORITES));
  for (JsonVariantConst item : arr) {
    if (paths.size() >= MAX_FAVORITES) break;
    const char* path = item.as<const char*>();
    if (path && path[0] != '\0') paths.emplace_back(path);
  }
  return true;
}

void FavoritesStore::load() {
  loadFromFile();
  rebuildHashes();
  std::vector<std::string>().swap(paths);
}

bool FavoritesStore::beginEdit() {
  // Loading the list uses std::string, which aborts the firmware when out of heap.
  if (ESP.getFreeHeap() < EDIT_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < EDIT_MIN_BLOCK) {
    LOG_ERR("FAV", "low memory, favorites not changed");
    return false;
  }
  if (loadFromFile()) return true;
  paths.clear();
  // A file that exists but cannot be read must not be overwritten.
  if (!Storage.exists(getFilePath())) return true;
  LOG_ERR("FAV", "cannot read favorites");
  return false;
}

bool FavoritesStore::finishEdit(const bool changed) {
  const bool saved = !changed || saveToFile();
  // RAM follows the file, so a failed save does not show a favorite that is not kept.
  if (changed && saved) rebuildHashes();
  if (!saved) LOG_ERR("FAV", "cannot save favorites");
  std::vector<std::string>().swap(paths);
  return saved;
}

bool FavoritesStore::isFavorite(const std::string& path) const {
  return isFavoriteHash(library::clixPathHash(path.data(), path.size()));
}

bool FavoritesStore::isFavoriteHash(const uint64_t pathHash) const {
  return std::find(hashes.begin(), hashes.end(), pathHash) != hashes.end();
}

bool FavoritesStore::add(const std::string& path) {
  if (isFavorite(path)) return true;
  if (full() || !beginEdit()) return false;
  paths.push_back(path);
  return finishEdit(true);
}

bool FavoritesStore::remove(const std::string& path) {
  if (!isFavorite(path) || !beginEdit()) return false;
  const auto it = std::find(paths.begin(), paths.end(), path);
  const bool found = it != paths.end();
  if (found) paths.erase(it);
  return finishEdit(found) && found;
}

void FavoritesStore::updatePath(const std::string& oldPath, const std::string& newPath) {
  if (!isFavorite(oldPath) || !beginEdit()) return;
  const auto it = std::find(paths.begin(), paths.end(), oldPath);
  const bool found = it != paths.end();
  if (found) *it = newPath;
  finishEdit(found);
}

void FavoritesStore::pruneMissing() {
  if (hashes.empty() || !beginEdit()) return;
  const auto before = paths.size();
  paths.erase(std::remove_if(paths.begin(), paths.end(),
                             [](const std::string& path) { return !Storage.exists(path.c_str()); }),
              paths.end());
  finishEdit(paths.size() != before);
}
