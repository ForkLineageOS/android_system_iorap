// Copyright (C) 2017 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "read_ahead.h"

#include "prefetcher/task_id.h"
#include "serialize/arena_ptr.h"
#include "serialize/protobuf_io.h"

#include <android-base/logging.h>
#include <android-base/chrono_utils.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <functional>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unordered_map>

namespace iorap {
namespace prefetcher {

enum class PrefetchStrategy {
  kFadvise = 0,
  kMmapLocked = 1,
  kMlock = 2,
};

std::ostream& operator<<(std::ostream& os, PrefetchStrategy ps) {
  switch (ps) {
    case PrefetchStrategy::kFadvise:
      os << "fadvise";
      break;
    case PrefetchStrategy::kMmapLocked:
      os << "mmap";
      break;
    case PrefetchStrategy::kMlock:
      os << "mlock";
      break;
    default:
      os << "<invalid>";
  }
  return os;
}

static constexpr PrefetchStrategy kPrefetchStrategy = PrefetchStrategy::kFadvise;

static PrefetchStrategy GetPrefetchStrategy() {
  static bool initialized = false;
  static PrefetchStrategy strat = kPrefetchStrategy;

  if (initialized == false) {
    initialized = true;
    const char* prefetch_env = getenv("IORAP_PREFETCH_STRATEGY");
    if (prefetch_env == nullptr) {
      LOG(VERBOSE) << "ReadAhead strategy defaulted. Did you want to set $IORAP_PREFETCH_STRATEGY ?";
      return strat;
    }
    if (strcmp(prefetch_env, "mmap") == 0) {
      strat = PrefetchStrategy::kMmapLocked;
      LOG(VERBOSE) << "ReadAhead strategy: kMmapLocked";
    } else if (strcmp(prefetch_env, "mlock") == 0) {
      strat = PrefetchStrategy::kMlock;
      LOG(VERBOSE) << "ReadAhead strategy: kMlock";
    } else if (strcmp(prefetch_env, "fadvise") == 0) {
      strat = PrefetchStrategy::kFadvise;
      LOG(VERBOSE) << "ReadAhead strategy: kFadvise";
    } else {
      LOG(WARNING) << "Unknown IORAP_PREFETCH_STRATEGY: " << prefetch_env << ", ignoring";
    }
  }

  return strat;
}

using ReadAheadIndex = int64_t;

static size_t g_read_ahead_open_files = 0;

struct ReadAheadFileEntry {
  ReadAheadIndex index_;
  android::base::unique_fd fd_;
  std::string file_path_;

  ReadAheadFileEntry(ReadAheadIndex index, const std::string& file_path)
      : index_(index),
        fd_(TEMP_FAILURE_RETRY(open(file_path.c_str(), O_RDONLY))),
        file_path_(file_path) {
    if (fd_.get() < 0) {
      PLOG(ERROR) << "ReadAhead: Failed to open file for prefetch: "
                  << file_path;
    }
  }

  bool IsValid() const {
    return fd_.get() >= 0;
  }

  friend std::ostream& operator<<(std::ostream& os, const ReadAheadFileEntry& entry) {
    return os << entry.file_path_;
  }
};

struct ReadAheadMemoryMap {
  void* address_;
  size_t length_;
};

}  // namespace prefetcher
}  // namespace iorap

// Hash API implementation for ReadAheadFileEntry.
namespace std {
template <> struct hash<iorap::prefetcher::ReadAheadFileEntry> {
  using argument_type = iorap::prefetcher::ReadAheadFileEntry;
  using result_type = std::size_t;
  result_type operator()(argument_type const& entry) const noexcept {
    return std::hash<iorap::prefetcher::ReadAheadIndex>{}(entry.index_);
  }
};
}

namespace iorap {
namespace prefetcher {

using ReadAheadFileMap = std::unordered_map<ReadAheadIndex,
                                            ReadAheadFileEntry>;

using ReadAheadMemoryMapList = std::vector<ReadAheadMemoryMap>;
// FIXME: should not be a global.
static ReadAheadMemoryMapList g_memory_map_list_;

std::unordered_map<size_t /*index*/, ReadAheadFileMap> g_read_ahead_file_map;


// static std::unordered_map<size_t /*id*/, ReadAheadMemoryMapList> g_memory_map_list_map_;

bool ReadAhead::PerformReadAhead(const ReadAheadFileEntry& entry, size_t length, size_t offset) {
  CHECK(entry.IsValid());

  if (GetPrefetchStrategy() == PrefetchStrategy::kFadvise) {
    if (posix_fadvise(entry.fd_, offset, length, POSIX_FADV_WILLNEED) < 0) {
      PLOG(ERROR) << "ReadAhead: Failed to fadvise entry " << entry
                  << ", offset=" << offset << ", length=" << length;
      return false;
    }
  } else if (GetPrefetchStrategy() == PrefetchStrategy::kMmapLocked
        || GetPrefetchStrategy() == PrefetchStrategy::kMlock) {
    ReadAheadMemoryMap mapping;
    mapping.length_ = length;

    const bool need_mlock = GetPrefetchStrategy() == PrefetchStrategy::kMlock;

    int flags = MAP_SHARED;
    if (!need_mlock) {
      // MAP_LOCKED is a best-effort to lock the page. it could still be
      // paged in later at a fault.
      flags |= MAP_LOCKED;
    }

    mapping.address_ =
      mmap(/*addr*/nullptr, length, PROT_READ, flags, entry.fd_, offset);

    if (mapping.address_ == nullptr) {
      PLOG(ERROR) << "ReadAhead: Failed to mmap entry " << entry
                  << ", offset=" << offset << ", length=" << length;
      return false;
    }

    g_memory_map_list_.push_back(mapping);

    // Strong guarantee that page will be locked if mlock returns successfully.
    if (need_mlock && mlock(mapping.address_, mapping.length_) < 0) {
      PLOG(ERROR) << "ReadAhead: Failed to mlock entry " << entry
                  << ", offset=" << offset << ", length=" << length;
      return false;
    }
  } else {
    LOG(FATAL) << "Unsupported prefetch strategy: " << (int) GetPrefetchStrategy();
  }

  return true;
}

void ReadAhead::FinishTask(const TaskId& id) {
  // Clean up all file descriptors.
  auto it = g_read_ahead_file_map.find(id.id);
  if (it != g_read_ahead_file_map.end()) {
    ReadAheadFileMap file_map = std::move(it->second);
    g_read_ahead_file_map.erase(it);

    LOG(VERBOSE) << "ReadAhead (Finish): Closed " << file_map.size() << " file descriptors.";

    CHECK_GE(g_read_ahead_open_files, file_map.size());
    g_read_ahead_open_files -= file_map.size();

    for (auto& kv_pair : file_map) {
      CHECK(kv_pair.second.IsValid());
    }
  }
  if (g_read_ahead_file_map.size() == 0) {
    CHECK_EQ(g_read_ahead_open_files, 0u);
  }

  // TODO: list of maps should be in the readahead.
  // we can probably create a new readahead for each in-flight task
  // in the looper.

  size_t unmapped_count = 0;
  for (auto& entry : g_memory_map_list_) {
    if (munmap(entry.address_, entry.length_) < 0) {
      PLOG(WARNING) << "ReadAhead (Finish): Failed to munmap address: "
                    << entry.address_ << ", length: " << entry.length_;
      continue;
    }

    unmapped_count++;
  }
  g_memory_map_list_.clear();

  LOG(VERBOSE) << "ReadAhead (Finish): Unmapped " << unmapped_count << " entries";
}

void ReadAhead::BeginTask(const TaskId& id) {
  LOG(VERBOSE) << "BeginTask: " << id;

  // TODO: atrace.
  android::base::Timer timer{};
  android::base::Timer open_timer{};

  // XX: Should we rename all the 'Create' to 'Make', or rename the 'Make' to 'Create' ?
  // Unfortunately make_unique, make_shared, etc is the standard C++ terminology.
  serialize::ArenaPtr<serialize::proto::TraceFile> trace_file_ptr =
      serialize::ProtobufIO::Open(id.path);

  if (trace_file_ptr == nullptr) {
    // TODO: distinguish between missing trace (this is OK, most apps wont have one)
    // and a bad error.
    LOG(ERROR) << "ReadAhead failed, missing trace file? " << id.path;
    return;
  }

  // TODO: The "Task[Id]" should probably be the one owning the trace file.
  // When the task is fully complete, the task can be deleted and the
  // associated arenas can go with them.

  ReadAheadFileMap file_map;

  // TODO: we should probably have the file entries all be relative
  // to the package path?

  // Open every file in the trace index.
  const serialize::proto::TraceFileIndex& index = trace_file_ptr->index();
  size_t opened_valid_files = 0;
  for (const serialize::proto::TraceFileIndexEntry& index_entry : index.entries()) {
    LOG(VERBOSE) << "ReadAhead: found file entry: " << index_entry.file_name();

    ReadAheadFileEntry file_entry{index_entry.id(), index_entry.file_name()};
    if (!file_entry.IsValid()) {
      LOG(WARNING) << "ReadAhead: skip entry '" << file_entry
                   << "'; the file could not be opened for reading";
      continue;
    }

    file_map.insert({ index_entry.id(), std::move(file_entry) });
    g_read_ahead_open_files++;
    opened_valid_files++;
  }
  LOG(VERBOSE) << "ReadAhead: Opened " << opened_valid_files << " file descriptors.";
  std::chrono::milliseconds open_duration_ms = open_timer.duration();

  // Go through every trace entry and readahead every (file,offset,len) tuple.

  size_t total_entries = 0;
  size_t succeeded_entries = 0;
  size_t sum_length = 0;
  size_t sum_length_total = 0;

  const serialize::proto::TraceFileList& file_list = trace_file_ptr->list();
  for (const serialize::proto::TraceFileEntry& file_entry : file_list.entries()) {
    ++total_entries;

    // Check trace file validity.
    // These will always succeed or always fail.
    auto it = file_map.find(file_entry.index_id());
    if (it == file_map.end()) {
      LOG(WARNING) << "ReadAhead entry has no corresponding file in index: "
                   << "index_id=" << file_entry.index_id() << ", skipping";
      continue;
    }

    if (file_entry.file_length() < 0 || file_entry.file_offset() < 0) {
      LOG(WARNING) << "ReadAhead entry negative file length or offset, illegal: "
                   << "index_id=" << file_entry.index_id() << ", skipping";
      continue;
    }

    // Attempt to perform readahead. This can generate more warnings dynamically.
    if (PerformReadAhead(it->second, file_entry.file_length(), file_entry.file_offset())) {
      succeeded_entries++;
      sum_length += file_entry.file_length();
    }
    sum_length_total += file_entry.file_length();
  }

  double success_rate = (total_entries == 0 ? 1.0 : (succeeded_entries * 1.0 / total_entries)) * 100.0;
  double byte_success_rate = (total_entries == 0 ? 1.0 : (sum_length * 1.0 / sum_length_total)) * 100.0;
  LOG(INFO) << "ReadAhead completed (" << id.path << "), "
            << "duration(total): " << timer << ", "
            << "duration(open): " << open_duration_ms.count() << "ms, "
            << "success rate entries: " << success_rate << "%, "
            << "successful entries: " << succeeded_entries << ", "
            << "entries: " << total_entries << ", "
            << "byte success rate: " << byte_success_rate << "%, "
            << "bytes readahead: " << sum_length << ", "
            << "strategy: " << GetPrefetchStrategy();
  // Implicit close() calls to all the opened files in file_map.

  g_read_ahead_file_map[id.id] = std::move(file_map);
}

}  // namespace prefetcher
}  // namespace iorap

