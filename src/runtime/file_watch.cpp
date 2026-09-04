#include "lwweb/runtime/file_watch.h"

#include "lwweb/ipc/ipc_message.h"

#include <chrono>
#include <condition_variable>
#include <system_error>
#include <utility>

namespace lwweb {
namespace {

constexpr std::size_t kMaxSnapshotEntries = 10000;
constexpr std::size_t kMaxChangesPerBatch = 512;

struct EntryState {
  bool directory = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified{};
};
using Snapshot = std::map<std::string, EntryState>;

Snapshot TakeSnapshot(const std::filesystem::path& root, bool recursive,
                      bool& overflow) {
  Snapshot result;
  overflow = false;
  std::error_code error;
  const auto add = [&](const auto& item) {
    if (result.size() >= kMaxSnapshotEntries) {
      overflow = true;
      return;
    }
    std::error_code item_error;
    const auto status = item.symlink_status(item_error);
    if (item_error) return;
    const auto relative = item.path().lexically_relative(root).generic_u8string();
    if (relative.empty() || relative == ".") return;
    EntryState state;
    state.directory = std::filesystem::is_directory(status);
    if (std::filesystem::is_regular_file(status)) {
      state.size = item.file_size(item_error);
      if (item_error) return;
    }
    state.modified = item.last_write_time(item_error);
    if (item_error) return;
    result.emplace(relative, state);
  };
  if (recursive) {
    for (std::filesystem::recursive_directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
      add(*it);
      if (overflow) break;
    }
  } else {
    for (std::filesystem::directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
      add(*it);
      if (overflow) break;
    }
  }
  if (error) overflow = true;
  return result;
}

}  // namespace

struct FileWatchService::WatchState {
  std::string id;
  std::filesystem::path directory;
  bool recursive = false;
  std::uint32_t debounce_ms = 150;
  Snapshot initial_snapshot;
  bool initial_overflow = false;
  std::atomic<bool> stop{false};
  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  std::thread worker;
};

FileWatchService::FileWatchService(EventCallback callback)
    : callback_(std::move(callback)) {}

FileWatchService::~FileWatchService() { StopAll(); }

std::string FileWatchService::Watch(const std::filesystem::path& directory,
                                    bool recursive, std::uint32_t debounce_ms) {
  if (!callback_)
    throw IpcException("UNSUPPORTED", "File watch event transport is unavailable");
  if (debounce_ms < 25 || debounce_ms > 5000)
    throw IpcException("INVALID_ARGUMENT", "Watch debounce must be 25..5000 ms");
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error)
    throw IpcException("NOT_FOUND", "Watch path must be an existing directory");
  auto state = std::make_shared<WatchState>();
  state->id = "watch_" + std::to_string(sequence_.fetch_add(1) + 1);
  state->directory = directory;
  state->recursive = recursive;
  state->debounce_ms = debounce_ms;
  // Establish the baseline before returning the watcher id. This prevents a
  // file created immediately after fs.watch from being mistaken for an
  // existing entry during the worker's first poll.
  state->initial_snapshot =
      TakeSnapshot(directory, recursive, state->initial_overflow);
  {
    std::lock_guard lock(mutex_);
    if (watches_.size() >= 32)
      throw IpcException("BUSY", "The file watch limit has been reached");
    watches_.emplace(state->id, state);
  }
  state->worker = std::thread([this, state] { Run(state); });
  return state->id;
}

bool FileWatchService::Unwatch(const std::string& watcher_id) {
  std::shared_ptr<WatchState> state;
  {
    std::lock_guard lock(mutex_);
    const auto found = watches_.find(watcher_id);
    if (found == watches_.end()) return false;
    state = found->second;
    watches_.erase(found);
  }
  state->stop.store(true);
  state->wait_cv.notify_all();
  if (state->worker.joinable()) state->worker.join();
  return true;
}

void FileWatchService::StopAll() {
  std::map<std::string, std::shared_ptr<WatchState>> watches;
  {
    std::lock_guard lock(mutex_);
    watches.swap(watches_);
  }
  for (auto& item : watches) {
    item.second->stop.store(true);
    item.second->wait_cv.notify_all();
    if (item.second->worker.joinable()) item.second->worker.join();
  }
}

void FileWatchService::Run(const std::shared_ptr<WatchState>& state) {
  bool initial_overflow = state->initial_overflow;
  auto previous = std::move(state->initial_snapshot);
  while (!state->stop.load()) {
    std::unique_lock lock(state->wait_mutex);
    if (state->wait_cv.wait_for(lock, std::chrono::milliseconds(state->debounce_ms),
                                [&] { return state->stop.load(); }))
      break;
    lock.unlock();
    if (state->stop.load()) break;
    bool overflow = false;
    auto current = TakeSnapshot(state->directory, state->recursive, overflow);
    nlohmann::json changes = nlohmann::json::array();
    for (const auto& item : current) {
      const auto old = previous.find(item.first);
      const bool changed = old == previous.end() || old->second.directory != item.second.directory ||
                           old->second.size != item.second.size ||
                           old->second.modified != item.second.modified;
      if (!changed) continue;
      if (changes.size() >= kMaxChangesPerBatch) { overflow = true; continue; }
      changes.push_back({{"type", old == previous.end() ? "created" : "modified"},
                         {"relativePath", item.first}, {"isDirectory", item.second.directory}});
    }
    for (const auto& item : previous) {
      if (current.find(item.first) != current.end()) continue;
      if (changes.size() >= kMaxChangesPerBatch) { overflow = true; continue; }
      changes.push_back({{"type", "deleted"}, {"relativePath", item.first},
                         {"isDirectory", item.second.directory}});
    }
    previous = std::move(current);
    if (initial_overflow) overflow = true;
    if (initial_overflow || overflow) {
      initial_overflow = false;
      changes = nlohmann::json::array();
    }
    if (changes.empty() && !overflow) continue;
    try {
      callback_("fs.changed", {{"watcherId", state->id},
                                {"changes", std::move(changes)},
                                {"overflow", overflow}});
    } catch (...) {
      // A failing transport must not terminate the watch worker.
    }
  }
}

}  // namespace lwweb
