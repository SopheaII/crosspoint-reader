#include "LibraryListActivity.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "FavoritesStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;

// Side clearance for the footer text in the panel's bottom corners.
int bottomCornerClearance() {
  const auto& board = BoardConfig::ACTIVE;
  return board.bottomCornerRadius != 0 ? board.bottomCornerRadius : board.cornerRadius;
}

constexpr unsigned long LONG_PRESS_MS = 1000;

// Decoders hold a 32 KB inflate window or JPEG state plus scratch buffers.
constexpr uint32_t COVER_BUILD_MIN_FREE_HEAP = 56 * 1024;
constexpr uint32_t COVER_BUILD_MIN_BLOCK = 36 * 1024;
constexpr unsigned long COVER_SETTLE_MS = 300;
constexpr unsigned long COVER_RETRY_DELAY_MS = 500;
// A button ladder that never reads idle must not block covers for good.
constexpr unsigned long COVER_STUCK_INPUT_MS = 10000;
constexpr unsigned long COVER_POLL_INTERVAL_MS = 10;

// Input is only sampled between loop passes, so a press during a cover build
// would wait or be lost. This reads the buttons directly and stops the build, so
// a press that is still down when loop() resumes gets normal handling. It covers
// the Xteink button ladder and the power button, not touch or digital keys.
bool coverBuildInterrupted = false;
bool lastResetWasCrash() {
  const esp_reset_reason_t reason = esp_reset_reason();
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

bool pollCoverInterrupt() {
  static unsigned long lastPollMs = 0;
  if (coverBuildInterrupted) return true;
  const unsigned long now = millis();
  if (now - lastPollMs < COVER_POLL_INTERVAL_MS) return false;
  lastPollMs = now;
  coverBuildInterrupted = gpio.rawInputActive();
  return coverBuildInterrupted;
}

constexpr int RECENT_TAB = 0;
constexpr int FAVORITES_TAB = 1;
// One tab for the Title and Author sorts; holding it switches between them.
constexpr int NAME_TAB = 2;
constexpr int TAB_SLOTS = NAME_TAB + 1;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs, const bool nameTabByAuthor) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == NAME_TAB && nameTabByAuthor) {
    return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  }
  if (tab == NAME_TAB || tab == FAVORITES_TAB) {
    return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  }
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab, const bool nameTabByAuthor) {
  if (tab == FAVORITES_TAB) return tr(STR_LIBRARY_TAB_FAVORITES);
  if (tab == NAME_TAB) return nameTabByAuthor ? tr(STR_LIBRARY_TAB_AUTHOR) : tr(STR_LIBRARY_TAB_TITLE);
  return tr(STR_LIBRARY_TAB_RECENT);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  // Three short tab labels: a full-slot pill would stretch across a third of
  // the screen, so cap it at the label plus padding (slots stay put).
  tabPillMaxPad = 16;
  tabLongPressEnabled = true;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  // Books copied onto the card from a PC never mark the index dirty, so the first
  // visit after each boot rescans once. Unchanged books reuse their metadata.
  static bool scannedThisBoot = false;
  if (!scannedThisBoot) FAVORITES.pruneMissing();
  const bool rebuildNeeded = !scannedThisBoot || library::isLibraryIndexDirty() ||
                             !index.open(library::libraryIndexPath()) || index.header().metadataEnabled != readMetadata;
  scannedThisBoot = true;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  // The Library is EPUB-only; other formats opened from Browse Files stay out.
  pinnedTotal = 0;
  for (size_t i = 0; i < books.size() && pinnedTotal < RecentBooksStore::MAX_RECENT_BOOKS; i++) {
    if (FsHelpers::hasEpubExtension(books[i].path)) pinnedStoreIndex[pinnedTotal++] = static_cast<uint8_t>(i);
  }
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[pinnedStoreIndex[i]].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

const RecentBook* LibraryListActivity::pinnedBook(const int entry) const {
  const auto& books = RECENT_BOOKS.getBooks();
  if (entry < 0 || entry >= pinnedTotal || pinnedStoreIndex[entry] >= books.size()) return nullptr;
  return &books[pinnedStoreIndex[entry]];
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const RecentBook* book = pinnedBook(selectedEntry());
    if (!book) return;
    path = book->path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent shelf always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (filterActive() || !groupable()); }

bool LibraryListActivity::filterActive() const { return !query.empty() || activeTabIndex == FAVORITES_TAB; }

bool LibraryListActivity::usesBookOptions() const { return isRecentSort(sortOrder) || activeTabIndex == FAVORITES_TAB; }

void LibraryListActivity::onRowLongPress(const int index) {
  if (usesBookOptions()) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent and Favorites long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const RecentBook* book = pinnedBook(entry);
    if (!book) return;
    path = book->path;
    title = book->title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  enum Choice : uint8_t { OPEN, FAVORITE, REMOVE_RECENT, DELETE_BOOK, REBUILD };
  const bool isFavorite = FAVORITES.isFavorite(path);
  const char* labels[5];
  std::array<uint8_t, 5> choices{};
  size_t optionCount = 0;
  const auto addOption = [&](const Choice choice, const char* label) {
    labels[optionCount] = label;
    choices[optionCount++] = choice;
  };
  addOption(OPEN, tr(STR_OPEN));
  addOption(FAVORITE, isFavorite         ? tr(STR_REMOVE_FROM_FAVORITES)
                      : FAVORITES.full() ? tr(STR_FAVORITES_FULL)
                                         : tr(STR_ADD_TO_FAVORITES));
  if (isStoreRow) addOption(REMOVE_RECENT, tr(STR_REMOVE_FROM_RECENTS));
  addOption(DELETE_BOOK, tr(STR_DELETE));
  addOption(REBUILD, tr(STR_LIBRARY_REBUILD));
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), labels, static_cast<int>(optionCount), 0,
                   [this, path, title, choices](const int choice) {
                     swallowHeldReleases();
                     if (choice < 0 || choice >= static_cast<int>(choices.size())) return;
                     switch (choices[static_cast<size_t>(choice)]) {
                       case OPEN:
                         openBookByPath(path);
                         break;
                       case FAVORITE:
                         toggleFavorite(path);
                         break;
                       case REMOVE_RECENT:
                         promptRemoveRecentBook(path, title);
                         break;
                       case DELETE_BOOK:
                         promptDeleteBookByPath(path, title);
                         break;
                       case REBUILD:
                         promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Adds or removes a favorite. On the Favorites tab the list itself changes, so
// the filter is rebuilt and the selection pulled back inside it.
void LibraryListActivity::toggleFavorite(const std::string& path) {
  if (FAVORITES.isFavorite(path)) {
    FAVORITES.remove(path);
  } else if (!FAVORITES.add(path)) {
    LOG_ERR("LIB", "favorite not added (%u saved)", static_cast<unsigned>(FAVORITES.count()));
  }
  if (activeTabIndex == FAVORITES_TAB) {
    RenderLock lock(*this);
    applyFilter();
    auto& nav = activeNav();
    const int count = listCount();
    nav.selected = count == 0 ? 0 : std::min(nav.selected.load(), count);
    nav.followOnBuild = true;
    gridFollow = true;
  }
  requestUpdate();
}

#if FREEINK_CAP_TOUCH
void LibraryListActivity::swipeDelete(const int entry) {
  activeNav().selected = entry + 1;
  if (isRecentSort(sortOrder) && entry < pinnedCount()) {
    if (const RecentBook* book = pinnedBook(entry)) promptDeleteBookByPath(book->path, book->title);
  } else {
    promptDeleteBook(entry);
  }
}
#endif

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::resetAfterRebuild() {
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  if (index < 0 || index >= TAB_SLOTS) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  // The render task reads the index and the filter, so switch under its lock.
  RenderLock lock(*this);
  sortOrder = orderForTab(index, descendingTabs, nameTabByAuthor);
  // The tab decides whether Favorites filters the list, so set it first. The
  // filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  activeTabIndex = index;
  gridTop = 0;
  applyFilter();
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  gridFollow = true;
  requestUpdate();
}

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index, nameTabByAuthor); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  if (index != activeTab()) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (filterActive()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (filterActive()) {
    if (entry < 0 || entry >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[entry];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

bool LibraryListActivity::groupable() const {
  return !degraded && !isRecentSort(sortOrder) && activeTabIndex != FAVORITES_TAB && bookRowCount() > 0;
}

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  const bool favoritesOnly = activeTabIndex == FAVORITES_TAB;
  if (query.empty() && !favoritesOnly) return;

  const std::string needle = query.empty() ? std::string() : library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0 || (favoritesOnly && FAVORITES.count() == 0)) return;
  // Favorites never match more than the list holds, which also lets the walk stop early.
  const int capacity = favoritesOnly ? std::min(total, static_cast<int>(FAVORITES.count())) : total;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(capacity));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(capacity * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  int favoritesSeen = 0;
  for (int row = 0; row < total && matchCount < capacity && favoritesSeen < capacity; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (favoritesOnly) {
      uint64_t pathHash = 0;
      if (!index.readPathHash(record, pathHash) || !FAVORITES.isFavoriteHash(pathHash)) continue;
      favoritesSeen++;
      if (needle.empty()) {
        matches[matchCount++] = static_cast<uint16_t>(row);
        continue;
      }
    }
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    RenderLock lock(*this);
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (groupsCollapsed) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* fileName) {
  title.clear();
  author.clear();
  if (fileName) fileName->clear();
  if (entry < pinnedCount()) {
    const RecentBook* book = pinnedBook(entry);
    if (!book) return false;
    title = book->title;
    author = book->author;
    if (fileName) *fileName = book->path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (fileName) index.readName(record, *fileName);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (inGridMode() && mappedInput.hasTouch()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      RenderLock lock(*this);  // the render task reads gridTop
      const int step = std::max(GRID_COLUMNS, gridCellsVisible);
      const int last = std::max(0, (listCount() - 1) / GRID_COLUMNS * GRID_COLUMNS);
      gridTop = std::clamp(gridTop + (swipe == MappedInputManager::SwipeDir::Up ? step : -step), 0, last);
      gridFollow = false;
      requestUpdate();
      return true;
    }
  }

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (activeTabIndex == NAME_TAB) {
        switchNameTab();
      } else if (!degraded) {
        toggleGridView();
      }
    } else if (usesBookOptions()) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  // On the grid the buttons follow the screen: left/right step to the neighbouring
  // book and up/down move a whole row. Up from the top row returns to the tabs.
  if (inGridMode() && !tabsFocused() && count > 0) {
    using Button = MappedInputManager::Button;
    navigationStartedOnTabs = false;
    const int entry = selectedEntry();
    const auto moveTo = [this](const int ring) {
      gridFollow = true;
      moveRingTo(ring);
    };
    buttonNavigator.onPressAndContinuous({Button::ScreenRight}, [&] { moveTo(entry + 1 < count ? entry + 2 : 1); });
    buttonNavigator.onPressAndContinuous({Button::ScreenLeft}, [&] { moveTo(entry > 0 ? entry : count); });
    buttonNavigator.onPressAndContinuous({Button::ScreenDown}, [&] {
      if (entry / GRID_COLUMNS < (count - 1) / GRID_COLUMNS) moveTo(std::min(entry + GRID_COLUMNS, count - 1) + 1);
    });
    buttonNavigator.onPressAndContinuous({Button::ScreenUp},
                                         [&] { moveTo(entry < GRID_COLUMNS ? 0 : entry - GRID_COLUMNS + 1); });
    return;
  }
  // A button move pulls the grid page to the selection.
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    gridFollow = true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, pageStep()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, pageStep()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);
  const bool grouped = !isRecentSort(sortOrder) && activeTabIndex != FAVORITES_TAB;

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the two-line
  // rows edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (!groupsCollapsed && winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  int rows = 0;
  int headers = 0;
  uint32_t previousInitial = 0;
  std::string rowFile;
  rowFile.reserve(128);
  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && rows < static_cast<int>(cap); entry++) {
    std::string& title = winTitles[static_cast<size_t>(rows)];
    std::string& author = winAuthors[static_cast<size_t>(rows)];
    fui::ListItem item;
    if (groupsCollapsed) {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    } else {
      if (!rowTextFor(entry, title, author, &rowFile)) continue;
      uint32_t initial = 0;
      bool startsGroup = false;
      if (authorGrouped) {
        startsGroup = rows == 0 || author != winAuthors[static_cast<size_t>(rows - 1)];
      } else if (grouped) {
        initial = titleInitialFor(entry);
        startsGroup = rows == 0 || initial != previousInitial;
        previousInitial = initial;
      }
      if (startsGroup) {
        std::string& heading = winHeaders[static_cast<size_t>(headers++)];
        if (authorGrouped)
          formatAuthorHeading(author, heading);
        else
          formatInitialHeading(initial, heading);
        item.sectionHeading = heading.c_str();
      }
      if (!authorGrouped && !author.empty()) item.subtitle = author.c_str();
    }

    item.label = title.c_str();
    // Group headings stay bare; every book row gets its file-type icon.
    if (!groupsCollapsed && !rowFile.empty()) item.icon = listIconFor(UITheme::getFileIcon(rowFile), 32);
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    rows++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
#if FREEINK_CAP_TOUCH
  configureSwipeDelete(screen, props);
#endif
  screen.list(props);
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const int next = nav.drawnRows;
  const auto body = screen.body();
  LOG_DBG("LIB", "page tab=%d top=%d full=%d loaded=%d body=%d..%d next=%d title=%s", activeTabIndex, windowStart,
          nav.drawnRows, rows, body.y, body.bottom(),
          next < rows ? winItems[static_cast<size_t>(next)].actionValue : -1,
          next < rows ? winItems[static_cast<size_t>(next)].label : "<none>");
  LOG_DBG("LIB", "page first=%d title=%s", rows > 0 ? winItems[0].actionValue : -1,
          rows > 0 ? winItems[0].label : "<none>");
#endif
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  const fui::Rect band = BaseTheme::headerBand(renderer,
                                               fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding),
                                                         frameRect.width, static_cast<int16_t>(metrics.headerHeight)},
                                               header);
  fui::header(screen.frame(), band, header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  constexpr int READOUT_GAP = 4;  // the list's fold sits just above the readout's line box
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + READOUT_GAP);
  const Rect area = UITheme::getContentArea(renderer);
  const auto bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - area.y - area.height);
  buildHeader(screen);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                  static_cast<int16_t>(bottomInset + metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  if (bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = activeTabIndex == FAVORITES_TAB ? tr(STR_NO_ENTRIES) : tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  if (inGridMode()) {
    buildGrid(screen);
  } else {
    buildRows(screen);
  }
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit varies with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll. The
// book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  // Bottom-right of the viewable area, clear of the bezel and rounded corner.
  const Rect area = UITheme::getContentArea(renderer);
  const int right = std::max(renderer.getScreenWidth() - area.x - area.width, bottomCornerClearance());
  const int x = renderer.getScreenWidth() - right - width - SIDE_PADDING;
  const int y = area.y + area.height - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_LIBRARY);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded)
    help = activeTabIndex == NAME_TAB ? tr(STR_LIBRARY_HOLD_NAME_TAB) : tr(STR_LIBRARY_HOLD_VIEW);
  else if (!tabsFocused() && usesBookOptions() && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const Rect area = UITheme::getContentArea(renderer);
  const int left = std::max(area.x, bottomCornerClearance());
  const int y = area.y + area.height - metrics.buttonHintsHeight - lineHeight;
  GUI.drawHelpText(renderer,
                   Rect{left + SIDE_PADDING, y, renderer.getScreenWidth() / 2 - left - SIDE_PADDING, lineHeight}, help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

void LibraryListActivity::drawFooter() {
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && !groupsCollapsed && query.empty();
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const bool onGrid = inGridMode() && !tabsFocused();
  const char* previousLabel = canSearch ? tr(STR_SEARCH) : onGrid ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel, previousLabel,
                                            onGrid ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// --- Recent cover grid ---------------------------------------------------------

bool LibraryListActivity::inGridMode() const {
  return gridView && !degraded && (activeTabIndex == RECENT_TAB || activeTabIndex == FAVORITES_TAB);
}

int LibraryListActivity::pageStep() { return inGridMode() ? std::max(1, gridCellsVisible) : activeNav().pageRows(); }

void LibraryListActivity::toggleGridView() {
  gridView = !gridView;
  gridFollow = true;
  gridCoverPending = false;
  activeNav().followOnBuild = true;
  requestUpdate();
}

// Each switch starts from the default A-Z order, like opening the tab fresh.
void LibraryListActivity::switchNameTab() {
  nameTabByAuthor = !nameTabByAuthor;
  descendingTabs &= static_cast<uint8_t>(~(1u << NAME_TAB));
  selectTab(NAME_TAB, false);
}

void LibraryListActivity::onTabLongPress(const int index) {
  app.clearTapFlash();
  // Holding the active tab changes its view; from another tab it just opens that tab.
  if (index != activeTabIndex) {
    selectTab(index, false);
  } else if (index == NAME_TAB) {
    switchNameTab();
  } else {
    toggleGridView();
  }
}

bool LibraryListActivity::pathForEntry(const int entry, std::string& path) {
  path.clear();
  if (entry < 0) return false;
  if (entry < pinnedCount()) {
    const RecentBook* book = pinnedBook(entry);
    if (!book) return false;
    path = book->path;
    return true;
  }
  if (!index.isOpen()) return false;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  return ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readPath(record, path);
}

bool LibraryListActivity::hasFailed(const uint64_t pathHash) const {
  for (size_t i = 0; i < failedCoverCount; ++i) {
    if (failedCovers[i] == pathHash) return true;
  }
  return false;
}

bool LibraryListActivity::rememberFailed(const uint64_t pathHash) {
  if (hasFailed(pathHash)) return true;
  if (failedCoverCount >= failedCovers.size()) return false;
  failedCovers[failedCoverCount++] = pathHash;
  return true;
}

// Fills the visible window's cells from the entries on screen. Paths are re-read
// every build (cheap index reads); the cache path object is only made when a
// cell's book changed, so selection moves cost no allocations.
void LibraryListActivity::prepareGridCells(const int count) {
  std::string path;
  std::string author;
  for (int i = 0; i < GRID_MAX_CELLS; ++i) {
    GridCell& cell = gridCells[static_cast<size_t>(i)];
    const int entry = gridTop + i;
    if (i >= gridCellsVisible || entry >= count || !pathForEntry(entry, path)) {
      cell = GridCell{};
      continue;
    }
    if (cell.path != path) {
      cell = GridCell{};
      cell.path = path;
      cell.pathHash = library::clixPathHash(path.data(), path.size());
      cell.failed = hasFailed(cell.pathHash);
      // Constructing an Epub only derives the cache path; keep it short-lived.
      auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
      if (epub) cell.thumbTemplate = epub->getThumbBmpPath();
    }
    rowTextFor(entry, cell.title, author);
  }
  gridCoverPending = true;
  gridBuildNotBefore = millis() + COVER_SETTLE_MS;
}

bool LibraryListActivity::paintGridCover(const fui::Rect rect, const uint16_t entry) {
  const int slot = static_cast<int>(entry) - gridTop;
  bool drawn = false;
  if (slot >= 0 && slot < GRID_MAX_CELLS) {
    const GridCell& cell = gridCells[static_cast<size_t>(slot)];
    if (!cell.thumbTemplate.empty() &&
        Storage.openFileForRead("LIB", UITheme::getCoverThumbPath(cell.thumbTemplate, gridThumbHeight), coverFile)) {
      if (coverBitmap.parseHeaders() == BmpReaderError::Ok && coverBitmap.getWidth() > 0 &&
          coverBitmap.getHeight() > 0) {
        drawn = GUI.drawCoverThumbFill(renderer, coverBitmap, Rect{rect.x, rect.y, rect.width, rect.height});
      }
      coverFile.close();
    }
  }
  if (!drawn) GUI.drawCoverPlaceholder(renderer, Rect{rect.x, rect.y, rect.width, rect.height});
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  return true;
}

void LibraryListActivity::buildGrid(UiScreen& screen) {
  const int count = listCount();
  const auto& theme = screen.theme();
  const fui::Rect body = screen.body();

  // Covers fill the width in 2:3 boxes, with the same small gap at both edges and
  // between the columns. A panel too short for two rows shows one.
  constexpr int MIN_GAP = 8;
  constexpr int MAX_ROW_GAP = 24;
  constexpr int LABEL_GAP = 8;  // clears the selection frame
  constexpr int MIN_TWO_ROW_COVER_HEIGHT = 140;
  const fui::FontId labelFont = fui::GfxRendererTarget::FONT_LABEL;
  const int labelHeight = screen.target().lineHeight(labelFont) * 2;
  const int labelSpace = labelHeight + LABEL_GAP + 2;
  int rows = 2;
  int coverHeight = (body.height - MIN_GAP) / rows - labelSpace;
  if (coverHeight < MIN_TWO_ROW_COVER_HEIGHT) {
    rows = 1;
    coverHeight = body.height - labelSpace;
  }
  const int coverWidth = std::min(coverHeight * 2 / 3, (body.width - (GRID_COLUMNS + 1) * MIN_GAP) / GRID_COLUMNS);
  if (coverWidth < 1) return;
  coverHeight = std::min(coverHeight, coverWidth * 3 / 2);
  const int sideGap = (body.width - GRID_COLUMNS * coverWidth) / (GRID_COLUMNS + 1);
  const int rowHeight = coverHeight + labelSpace;
  // Share spare height between the rows instead of leaving it all at the bottom.
  const int rowGap = rows > 1 ? std::min(MAX_ROW_GAP, (body.height - rows * rowHeight) / (rows - 1)) : 0;

  gridProps = fui::CoverGridProps{};
  gridProps.columns = GRID_COLUMNS;
  gridProps.gap = 0;
  gridProps.rowGap = static_cast<int16_t>(rowGap);
  gridProps.coverSize = {static_cast<int16_t>(coverWidth), static_cast<int16_t>(coverHeight)};
  gridProps.rowHeight = static_cast<int16_t>(rowHeight);
  gridProps.titleText = theme.smallText;
  gridProps.titleText.font = labelFont;
  gridProps.titleText.maxLines = 2;
  // Titles use the whole cell, which is wider than the cover by one gap.
  gridProps.labelHeight = static_cast<int16_t>(labelHeight);
  gridProps.labelGap = LABEL_GAP;
  gridProps.labelInset = fui::Insets{0, 2, 0, 2};
  const fui::Rect gridRect{static_cast<int16_t>(body.x + sideGap / 2), body.y,
                           static_cast<int16_t>(body.width - sideGap), body.height};
  gridCellsVisible = std::min<int>(
      GRID_MAX_CELLS, fui::coverGridVisibleCells(gridRect, GRID_COLUMNS, gridProps.rowHeight, gridProps.rowGap));
  if (gridCellsVisible <= 0) return;
  // The converter scales a cover to this height; 2:3 art then fills the box.
  gridThumbHeight = coverHeight + 2;

  const int selected = ringPos() - 1;  // -1 while the tab bar has focus
  if (gridFollow && selected >= 0) {
    gridTop = fui::coverGridTopIndexFor(static_cast<uint16_t>(selected), static_cast<uint16_t>(count), GRID_COLUMNS,
                                        static_cast<uint16_t>(gridCellsVisible));
  }
  gridTop = std::clamp(gridTop, 0, std::max(0, (count - 1) / GRID_COLUMNS * GRID_COLUMNS));
  prepareGridCells(count);

  gridProps.count = static_cast<uint16_t>(count);
  gridProps.topIndex = static_cast<uint16_t>(gridTop);
  gridProps.selectedIndex = static_cast<int16_t>(selected);
  gridProps.action = ACTION_ROW;
  gridProps.inputMask = fui::InputTouch | fui::InputLongPress;
  gridProps.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  // A thick frame with a thin white line inside, so it also stands out on dark covers.
  gridProps.selectedCoverFrameGap = 6;
  gridProps.selectedCoverFrameWidth = 5;
  gridProps.scrollIndicator = false;
  gridProps.itemProviderUserData = this;
  gridProps.itemProvider = [](const uint16_t index, void* user) {
    auto* self = static_cast<LibraryListActivity*>(user);
    const int slot = static_cast<int>(index) - self->gridTop;
    const char* title =
        slot >= 0 && slot < GRID_MAX_CELLS ? self->gridCells[static_cast<size_t>(slot)].title.c_str() : nullptr;
    return fui::coverGridItem(title, index);
  };
  gridProps.coverPainterUserData = this;
  gridProps.coverPainter = [](fui::DrawTarget&, const fui::Rect rect, const fui::CoverGridItem&, const uint16_t index,
                              void* user) {
    return static_cast<LibraryListActivity*>(user)->paintGridCover(rect, index);
  };
  fui::coverGrid(screen.frame(), gridRect, gridProps);
}

// Makes at most one missing thumbnail per call and returns true when the grid
// should repaint. Runs on the main task with the render lock held.
//
// Failure handling:
// - A marker file brackets each build. Finding one means the last attempt never
//   returned. After a power cut the cover is rebuilt; after a crash the book gets
//   an empty thumbnail (its placeholder) instead of crashing on every visit.
// - A failed build is not retried this visit, and low memory skips the build:
//   decoders need contiguous heap, and running out aborts the firmware.
// - A button press stops the build and leaves no thumbnail; the cover is retried
//   after the press is handled.
bool LibraryListActivity::buildOneGridCover() {
  gridCoverPending = false;  // the repaint after a finished cover raises it again
  if (gridThumbHeight <= 0) return false;
  for (GridCell& cell : gridCells) {
    if (cell.path.empty() || cell.failed || cell.thumbTemplate.empty() || cell.readyHeight == gridThumbHeight) continue;

    const std::string thumb = UITheme::getCoverThumbPath(cell.thumbTemplate, gridThumbHeight);
    const std::string marker = thumb + ".building";
    if (Storage.exists(marker.c_str())) {
      Storage.remove(thumb.c_str());
      Storage.remove(marker.c_str());
      if (lastResetWasCrash()) {
        LOG_ERR("LIB", "cover build crashed last time, skipping: %s", cell.path.c_str());
        HalFile empty;
        if (Storage.openFileForWrite("LIB", thumb, empty)) empty.close();
        cell.readyHeight = gridThumbHeight;
        return true;
      }
    }
    if (Storage.exists(thumb.c_str())) {
      cell.readyHeight = gridThumbHeight;
      continue;
    }

    if (ESP.getFreeHeap() < COVER_BUILD_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < COVER_BUILD_MIN_BLOCK) {
      LOG_ERR("LIB", "low memory, no cover for now (%u free, %u block)", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      cell.failed = true;
      continue;
    }
    auto epub = makeUniqueNoThrow<Epub>(cell.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("LIB", "OOM: cover epub");
      cell.failed = true;
      continue;
    }
    epub->setupCacheDir();
    HalFile markerFile;
    if (!Storage.openFileForWrite("LIB", marker, markerFile)) {
      LOG_ERR("LIB", "cannot write to book cache: %s", marker.c_str());
      cell.failed = true;
      continue;
    }
    markerFile.close();

    coverBuildInterrupted = false;
    // Input that stayed active past the wait in loop() is treated as a stuck ladder.
    const bool built =
        epub->generateThumbBmpFromSource(gridThumbHeight, inputActiveSince == 0 ? &pollCoverInterrupt : nullptr);
    Storage.remove(marker.c_str());
    const bool thumbExists = Storage.exists(thumb.c_str());
    if (!built && !thumbExists && coverBuildInterrupted) {
      gridBuildNotBefore = millis() + COVER_RETRY_DELAY_MS;
      gridCoverPending = true;
      return false;
    }
    if (thumbExists) {
      cell.readyHeight = gridThumbHeight;
    } else {
      cell.failed = true;
      rememberFailed(cell.pathHash);
    }
    return true;
  }
  return false;
}

// Runs after input, so a press handled this pass wins over starting a build.
void LibraryListActivity::loop() {
  UiTabListActivity::loop();
  if (!gridCoverPending || !inGridMode() || optionPopup.isActive()) {
    gridCoverPending = false;
    inputActiveSince = 0;
    return;
  }
  // Wait out a held button rather than start a build it would stop at once.
  if (gpio.rawInputActive()) {
    if (inputActiveSince == 0) inputActiveSince = millis();
    if (millis() - inputActiveSince < COVER_STUCK_INPUT_MS) return;
  } else {
    inputActiveSince = 0;
  }
  // Leaving the screen or about to repaint: the cells may already be stale. A
  // repaint raises the flag again, and this activity's exit ends the work.
  if (activityManager.hasPendingTransition() || activityManager.updatePending()) return;
  if (static_cast<long>(millis() - gridBuildNotBefore) < 0) return;
  // Try: never wait out an e-ink refresh here, which would stall input.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  if (buildOneGridCover()) requestUpdate();
}
