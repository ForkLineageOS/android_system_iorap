// Copyright (C) 2019 The Android Open Source Project
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

#include "prefetcher/prefetcher_daemon.h"
#include "prefetcher/session_manager.h"
#include "prefetcher/session.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <deque>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace iorap::prefetcher {

// Gate super-spammy IPC logging behind a property.
// This is beyond merely annoying, enabling this logging causes prefetching to be about 1000x slower.
static bool LogVerboseIpc() {
  static bool initialized = false;
  static bool verbose_ipc;

  if (initialized == false) {
    initialized = true;

    verbose_ipc =
        ::android::base::GetBoolProperty("iorapd.readahead.verbose_ipc", /*default*/false);
  }

  return verbose_ipc;
}

static constexpr const char kCommandFileName[] = "/system/bin/iorap.prefetcherd";

using ArgString = const char*;

// Create execve-compatible argv.
// The lifetime is tied to that of vector.
std::unique_ptr<ArgString[]> VecToArgv(const char* program_name,
                                       const std::vector<std::string>& vector) {
  // include program name in argv[0]
  // include a NULL sentinel in the end.
  std::unique_ptr<ArgString[]> ptr{new ArgString[vector.size() + 2]};

  // program name
  ptr[0] = program_name;

  // all the argv
  for (size_t i = 0; i < vector.size(); ++i) {
    ptr[i+1] = vector[i].c_str();
  }

  // null sentinel
  ptr[vector.size() + 1] = nullptr;

  return ptr;
}

std::ostream& operator<<(std::ostream& os, ReadAheadKind ps) {
  switch (ps) {
    case ReadAheadKind::kFadvise:
      os << "fadvise";
      break;
    case ReadAheadKind::kMmapLocked:
      os << "mmap";
      break;
    case ReadAheadKind::kMlock:
      os << "mlock";
      break;
    default:
      os << "<invalid>";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, CommandChoice choice) {
  switch (choice) {
    case CommandChoice::kRegisterFilePath:
      os << "kRegisterFilePath";
      break;
    case CommandChoice::kUnregisterFilePath:
      os << "kUnregisterFilePath";
      break;
    case CommandChoice::kReadAhead:
      os << "kReadAhead";
      break;
    case CommandChoice::kExit:
      os << "kExit";
      break;
    case CommandChoice::kCreateSession:
      os << "kCreateSession";
      break;
    case CommandChoice::kDestroySession:
      os << "kDestroySession";
      break;
    case CommandChoice::kDumpSession:
      os << "kDumpSession";
      break;
    case CommandChoice::kDumpEverything:
      os << "kDumpEverything";
      break;
    default:
      CHECK(false) << "forgot to handle this choice";
      break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Command& command) {
  os << "Command{";
  os << "choice=" << command.choice << ",";

  bool has_session_id = true;
  bool has_id = true;
  switch (command.choice) {
    case CommandChoice::kDumpEverything:
    case CommandChoice::kExit:
      has_session_id = false;
      FALLTHROUGH_INTENDED;
    case CommandChoice::kCreateSession:
    case CommandChoice::kDestroySession:
    case CommandChoice::kDumpSession:
      has_id = false;
      break;
    default:
      break;
  }

  if (has_session_id) {
    os << "sid=" << command.session_id << ",";
  }

  if (has_id) {
    os << "id=" << command.id << ",";
  }

  switch (command.choice) {
    case CommandChoice::kRegisterFilePath:
      os << "file_path=";

      if (command.file_path) {
        os << *(command.file_path);
      } else {
        os << "(nullopt)";
      }
      break;
    case CommandChoice::kUnregisterFilePath:
      break;
    case CommandChoice::kReadAhead:
      os << "read_ahead_kind=" << command.read_ahead_kind << ",";
      os << "length=" << command.length << ",";
      os << "offset=" << command.offset << ",";
      break;
    case CommandChoice::kExit:
      break;
    case CommandChoice::kCreateSession:
      os << "description=";
      if (command.file_path) {
        os << "'" << *(command.file_path) << "'";
      } else {
        os << "(nullopt)";
      }
      break;
    case CommandChoice::kDestroySession:
      break;
    case CommandChoice::kDumpSession:
      break;
    case CommandChoice::kDumpEverything:
      break;
    default:
      CHECK(false) << "forgot to handle this choice";
      break;
  }

  os << "}";

  return os;
}

template <typename T>
struct ParseResult {
  T value;
  char* next_token;
  size_t stream_size;

  ParseResult() : value{}, next_token{nullptr}, stream_size{} {
  }

  constexpr operator bool() const {
    return next_token != nullptr;
  }
};

// Very spammy: Keep it off by default. Set to true if changing this code.
static constexpr bool kDebugParsingRead = false;

#define DEBUG_PREAD if (kDebugParsingRead) LOG(VERBOSE) << "ParsingRead "



// Parse a strong type T from a buffer stream.
// If there's insufficient space left to parse the value, an empty ParseResult is returned.
template <typename T>
ParseResult<T> ParsingRead(char* stream, size_t stream_size) {
  if (stream == nullptr) {
    DEBUG_PREAD << "stream was null";
    return {};
  }

  if constexpr (std::is_same_v<T, std::string>) {
    ParseResult<uint32_t> length = ParsingRead<uint32_t>(stream, stream_size);

    if (!length) {
      DEBUG_PREAD << "could not find length";
      // Not enough bytes left?
      return {};
    }

    ParseResult<std::string> string_result;
    string_result.value.reserve(length);

    stream = length.next_token;
    stream_size = length.stream_size;

    for (size_t i = 0; i < length.value; ++i) {
      ParseResult<char> char_result = ParsingRead<char>(stream, stream_size);

      stream = char_result.next_token;
      stream_size = char_result.stream_size;

      if (!char_result) {
        DEBUG_PREAD << "too few chars in stream, expected length: " << length.value;
        // Not enough bytes left?
        return {};
      }

      string_result.value += char_result.value;

      DEBUG_PREAD << "string preliminary is : " << string_result.value;
    }

    DEBUG_PREAD << "parsed string to: " << string_result.value;
    string_result.next_token = stream;
    return string_result;
  } else {
    if (sizeof(T) > stream_size) {
      return {};
    }

    ParseResult<T> result;
    result.next_token = stream + sizeof(T);
    result.stream_size = stream_size - sizeof(T);

    memcpy(&result.value, stream, sizeof(T));

    return result;
  }
}

// Convenience overload to chain multiple ParsingRead together.
template <typename T, typename U>
ParseResult<T> ParsingRead(ParseResult<U> result) {
  return ParsingRead<T>(result.next_token, result.stream_size);
}

class CommandParser {
 public:
  CommandParser(PrefetcherForkParameters params) {
    params_ = params;
  }

  std::vector<Command> ParseCommands(bool& eof) {
    eof = false;

    std::vector<Command> commands_vec;

    std::vector<char> buf_vector;
    buf_vector.resize(1024*1024);  // 1MB.
    char* buf = &buf_vector[0];

    // Binary only parsing. The higher level code can parse text
    // with ifstream if it really wants to.
    char* stream = &buf[0];
    size_t stream_size = buf_vector.size();

    while (true) {
      if (stream_size == 0) {
        // TODO: reply with an overflow command.
        LOG(WARNING) << "prefetcher_daemon command overflow, dropping all commands.";
        stream = &buf[0];
        stream_size = buf_vector.size();
        memset(&buf[0], /*c*/0, buf_vector.size());
      }

      if (LogVerboseIpc()) {
        LOG(VERBOSE) << "PrefetcherDaemon block read for commands (fd=" << params_.input_fd << ")";
      }
      ssize_t count = TEMP_FAILURE_RETRY(read(params_.input_fd, stream, stream_size));
      if (LogVerboseIpc()) {
        LOG(VERBOSE) << "PrefetcherDaemon::read " << count << " for stream size:" << stream_size;
      }

      if (count < 0) {
        PLOG(ERROR) << "failed to read from input fd";
        break;
        // TODO: let the daemon be restarted by higher level code?
      } else if (count == 0) {
        LOG(WARNING) << "prefetcher_daemon input_fd end-of-file; terminating";
        eof = true;
        break;
        // TODO: let the daemon be restarted by higher level code?
      }

      longbuf_.insert(longbuf_.end(), stream, stream + count);
      if (LogVerboseIpc()) {
        LOG(VERBOSE) << "PrefetcherDaemon updated longbuf size: " << longbuf_.size();
      }

      std::optional<Command> maybe_command;
      {
        if (longbuf_.size() == 0) {
          break;
        }

        std::vector<char> v(longbuf_.begin(),
                            longbuf_.end());

        if (LogVerboseIpc()) {
          LOG(VERBOSE) << "PrefetcherDaemon longbuf_ size: " << v.size();
          if (WOULD_LOG(VERBOSE)) {
            std::stringstream dump;
            dump << std::hex << std::setfill('0');
            for (size_t i = 0; i < v.size(); ++i) {
              dump << std::setw(2) << static_cast<unsigned>(v[i]);
            }

            LOG(VERBOSE) << "PrefetcherDaemon longbuf_ dump: " << dump.str();
          }
        }

        size_t v_off = 0;
        size_t consumed_bytes = std::numeric_limits<size_t>::max();
        size_t consumed_total = 0;

        while (true) {
          maybe_command = Command::Read(&v[v_off], v.size() - v_off, &consumed_bytes);
          consumed_total += consumed_bytes;
          // Normal every time we get to the end of a buffer.
          if (!maybe_command) {
              if (LogVerboseIpc()) {
                LOG(VERBOSE) << "failed to read command, v_off=" << v_off << ",v_size:" << v.size();
              }
              break;
          }

          // in the next pass ignore what we already consumed.
          v_off += consumed_bytes;

          // true as long we don't hit the 'break' above.
          DCHECK_EQ(v_off, consumed_total);
          if (LogVerboseIpc()) {
            LOG(VERBOSE) << "success to read command, v_off=" << v_off << ",v_size:" << v.size()
                         << "," << *maybe_command;

            // Pretty-print a single command for debugging/testing.
            LOG(VERBOSE) << *maybe_command;
          }

          // add to the commands we parsed.
          commands_vec.push_back(*maybe_command);
        }

        // erase however many were consumed
        longbuf_.erase(longbuf_.begin(), longbuf_.begin() + consumed_total);
      }
      break;
    }

    return commands_vec;
  }

 private:
  bool IsTextMode() const {
    return params_.format_text;
  }

  PrefetcherForkParameters params_;

  // A buffer long enough to contain a lot of buffers.
  // This handles reads that only contain a partial command.
  std::deque<char> longbuf_;
};

static constexpr bool kDebugCommandRead = false;

#define DEBUG_READ if (kDebugCommandRead) LOG(VERBOSE) << "Command::Read "

std::optional<Command> Command::Read(char* buf, size_t buf_size, /*out*/size_t* consumed_bytes) {
  *consumed_bytes = 0;
  if (buf == nullptr) {
    return std::nullopt;
  }

  Command cmd{};  // zero-initialize any unused fields
  ParseResult<CommandChoice> parsed_choice = ParsingRead<CommandChoice>(buf, buf_size);
  cmd.choice = parsed_choice.value;

  if (!parsed_choice) {
    DEBUG_READ << "no choice";
    return std::nullopt;
  }

  switch (parsed_choice.value) {
    case CommandChoice::kRegisterFilePath: {
      ParseResult<uint32_t> parsed_session_id = ParsingRead<uint32_t>(parsed_choice);
      if (!parsed_session_id) {
        DEBUG_READ << "no parsed session id";
        return std::nullopt;
      }

      ParseResult<uint32_t> parsed_id = ParsingRead<uint32_t>(parsed_session_id);
      if (!parsed_id) {
        DEBUG_READ << "no parsed id";
        return std::nullopt;
      }

      ParseResult<std::string> parsed_file_path = ParsingRead<std::string>(parsed_id);

      if (!parsed_file_path) {
        DEBUG_READ << "no file path";
        return std::nullopt;
      }
      *consumed_bytes = parsed_file_path.next_token - buf;

      cmd.session_id = parsed_session_id.value;
      cmd.id = parsed_id.value;
      cmd.file_path = parsed_file_path.value;

      break;
    }
    case CommandChoice::kUnregisterFilePath: {
      ParseResult<uint32_t> parsed_session_id = ParsingRead<uint32_t>(parsed_choice);
      if (!parsed_session_id) {
        DEBUG_READ << "no parsed session id";
        return std::nullopt;
      }

      ParseResult<uint32_t> parsed_id = ParsingRead<uint32_t>(parsed_session_id);
      if (!parsed_id) {
        DEBUG_READ << "no parsed id";
        return std::nullopt;
      }
      *consumed_bytes = parsed_id.next_token - buf;

      cmd.session_id = parsed_session_id.value;
      cmd.id = parsed_id.value;

      break;
    }
    case CommandChoice::kReadAhead: {
      ParseResult<uint32_t> parsed_session_id = ParsingRead<uint32_t>(parsed_choice);
      if (!parsed_session_id) {
        DEBUG_READ << "no parsed session id";
        return std::nullopt;
      }

      ParseResult<uint32_t> parsed_id = ParsingRead<uint32_t>(parsed_session_id);
      if (!parsed_id) {
        DEBUG_READ << "no parsed id";
        return std::nullopt;
      }

      ParseResult<ReadAheadKind> parsed_kind = ParsingRead<ReadAheadKind>(parsed_id);
      if (!parsed_kind) {
        DEBUG_READ << "no parsed kind";
        return std::nullopt;
      }
      ParseResult<uint64_t> parsed_length = ParsingRead<uint64_t>(parsed_kind);
      if (!parsed_length) {
        DEBUG_READ << "no parsed length";
        return std::nullopt;
      }
      ParseResult<uint64_t> parsed_offset = ParsingRead<uint64_t>(parsed_length);
      if (!parsed_offset) {
        DEBUG_READ << "no parsed offset";
        return std::nullopt;
      }
      *consumed_bytes = parsed_offset.next_token - buf;

      cmd.session_id = parsed_session_id.value;
      cmd.id = parsed_id.value;
      cmd.read_ahead_kind = parsed_kind.value;
      cmd.length = parsed_length.value;
      cmd.offset = parsed_offset.value;

      break;
    }
    case CommandChoice::kCreateSession: {
      ParseResult<uint32_t> parsed_session_id = ParsingRead<uint32_t>(parsed_choice);
      if (!parsed_session_id) {
        DEBUG_READ << "no parsed session id";
        return std::nullopt;
      }

      ParseResult<std::string> parsed_description = ParsingRead<std::string>(parsed_session_id);

      if (!parsed_description) {
        DEBUG_READ << "no description";
        return std::nullopt;
      }
      *consumed_bytes = parsed_description.next_token - buf;

      cmd.session_id = parsed_session_id.value;
      cmd.file_path = parsed_description.value;

      break;
    }
    case CommandChoice::kDestroySession:
    case CommandChoice::kDumpSession: {
      ParseResult<uint32_t> parsed_session_id = ParsingRead<uint32_t>(parsed_choice);
      if (!parsed_session_id) {
        DEBUG_READ << "no parsed session id";
        return std::nullopt;
      }

      *consumed_bytes = parsed_session_id.next_token - buf;

      cmd.session_id = parsed_session_id.value;

      break;
    }
    case CommandChoice::kExit:
    case CommandChoice::kDumpEverything:
      *consumed_bytes = parsed_choice.next_token - buf;
      // Only need to parse the choice.
      break;
    default:
      LOG(FATAL) << "unrecognized command number " << static_cast<uint32_t>(parsed_choice.value);
      break;
  }

  return cmd;
}

bool Command::Write(char* buf, size_t buf_size, /*out*/size_t* produced_bytes) const {
  *produced_bytes = 0;
  if (buf == nullptr) {
    LOG(WARNING) << "null buf, is this expected?";
    return false;
  }

  bool has_enough_space = false;
  size_t space_requirement = std::numeric_limits<size_t>::max();

  space_requirement = sizeof(choice);

  switch (choice) {
    case CommandChoice::kRegisterFilePath:
      space_requirement += sizeof(session_id);
      space_requirement += sizeof(id);
      space_requirement += sizeof(uint32_t);    // string length

      if (!file_path) {
        LOG(WARNING) << "Missing file path for kRegisterFilePath";
        return false;
      }

      space_requirement += file_path->size(); // string contents
      break;
    case CommandChoice::kUnregisterFilePath:
      space_requirement += sizeof(session_id);
      space_requirement += sizeof(id);
      break;
    case CommandChoice::kReadAhead:
      space_requirement += sizeof(session_id);
      space_requirement += sizeof(id);
      space_requirement += sizeof(read_ahead_kind);
      space_requirement += sizeof(length);
      space_requirement += sizeof(offset);
      break;
    case CommandChoice::kCreateSession:
      space_requirement += sizeof(session_id);
      space_requirement += sizeof(uint32_t);    // string length

      if (!file_path) {
        LOG(WARNING) << "Missing file path for kCreateSession";
        return false;
      }

      space_requirement += file_path->size(); // string contents
      break;
    case CommandChoice::kDestroySession:
    case CommandChoice::kDumpSession:
      space_requirement += sizeof(session_id);
      break;
    case CommandChoice::kExit:
    case CommandChoice::kDumpEverything:
      // Only need space for the choice.
      break;
    default:
      LOG(FATAL) << "unrecognized command number " << static_cast<uint32_t>(choice);
      break;
  }

  if (buf_size < space_requirement) {
    return false;
  }

  *produced_bytes = space_requirement;

  // Always write out the choice.
  size_t buf_offset = 0;

  memcpy(&buf[buf_offset], &choice, sizeof(choice));
  buf_offset += sizeof(choice);

  switch (choice) {
    case CommandChoice::kRegisterFilePath:
      memcpy(&buf[buf_offset], &session_id, sizeof(session_id));
      buf_offset += sizeof(session_id);
      memcpy(&buf[buf_offset], &id, sizeof(id));
      buf_offset += sizeof(id);

      {
        uint32_t string_length = static_cast<uint32_t>(file_path->size());
        memcpy(&buf[buf_offset], &string_length, sizeof(string_length));
        buf_offset += sizeof(string_length);
      }

      DCHECK(file_path.has_value());

      memcpy(&buf[buf_offset], file_path->c_str(), file_path->size());
      buf_offset += file_path->size();
      break;
    case CommandChoice::kUnregisterFilePath:
      memcpy(&buf[buf_offset], &session_id, sizeof(session_id));
      buf_offset += sizeof(session_id);
      memcpy(&buf[buf_offset], &id, sizeof(id));
      buf_offset += sizeof(id);
      break;
    case CommandChoice::kReadAhead:
      memcpy(&buf[buf_offset], &session_id, sizeof(session_id));
      buf_offset += sizeof(session_id);
      memcpy(&buf[buf_offset], &id, sizeof(id));
      buf_offset += sizeof(id);
      memcpy(&buf[buf_offset], &read_ahead_kind, sizeof(read_ahead_kind));
      buf_offset += sizeof(read_ahead_kind);
      memcpy(&buf[buf_offset], &length, sizeof(length));
      buf_offset += sizeof(length);
      memcpy(&buf[buf_offset], &offset, sizeof(offset));
      buf_offset += sizeof(offset);
      break;
    case CommandChoice::kCreateSession:
      memcpy(&buf[buf_offset], &session_id, sizeof(session_id));
      buf_offset += sizeof(session_id);

      {
        uint32_t string_length = static_cast<uint32_t>(file_path->size());
        memcpy(&buf[buf_offset], &string_length, sizeof(string_length));
        buf_offset += sizeof(string_length);
      }

      DCHECK(file_path.has_value());

      memcpy(&buf[buf_offset], file_path->c_str(), file_path->size());
      buf_offset += file_path->size();

      DCHECK_EQ(buf_offset, space_requirement) << *this << ",file_path_size:" << file_path->size();
      DCHECK_EQ(buf_offset, *produced_bytes) << *this;

      break;
    case CommandChoice::kDestroySession:
    case CommandChoice::kDumpSession:
      memcpy(&buf[buf_offset], &session_id, sizeof(session_id));
      buf_offset += sizeof(session_id);
      break;
    case CommandChoice::kExit:
    case CommandChoice::kDumpEverything:
      // Only need to write out the choice.
      break;
    default:
      LOG(FATAL) << "should have fallen out in the above switch"
                 << static_cast<uint32_t>(choice);
      break;
  }

  DCHECK_EQ(buf_offset, space_requirement) << *this;
  DCHECK_EQ(buf_offset, *produced_bytes) << *this;

  return true;
}

class PrefetcherDaemon::Impl {
 public:
  std::optional<PrefetcherForkParameters> StartPipesViaFork() {
    int pipefds[2];
    if (pipe(&pipefds[0]) != 0) {
      PLOG(FATAL) << "Failed to create read/write pipes";
    }

    pipefd_read_ = pipefds[0];
    pipefd_write_ = pipefds[1];

    PrefetcherForkParameters params;
    params.input_fd = pipefd_read_;
    params.output_fd = pipefd_write_;
    params.format_text = false;

    bool res = StartViaFork(params);
    if (res) {
      return params;
    } else {
      return std::nullopt;
    }
  }

  bool StartViaFork(PrefetcherForkParameters params) {
    params_ = params;

    forked_ = true;
    child_ = fork();

    if (child_ == -1) {
      LOG(FATAL) << "Failed to fork PrefetcherDaemon";
    } else if (child_ > 0) {  // we are the caller of this function
      LOG(DEBUG) << "forked into iorap.prefetcherd, pid = " << child_;

      return true;
    } else {
      // we are the child that was forked.

      std::stringstream argv;  // for logging

      std::vector<std::string> argv_vec;

      {
        std::stringstream s;
        s << "--input-fd";
        argv_vec.push_back(s.str());

        std::stringstream s2;
        s2 << params.input_fd;
        argv_vec.push_back(s2.str());

        argv << " --input-fd" << " " << params.input_fd;
      }

      {
        std::stringstream s;
        s << "--output-fd";
        argv_vec.push_back(s.str());

        std::stringstream s2;
        s2 << params.output_fd;
        argv_vec.push_back(s2.str());

        argv << " --output-fd" << " " << params.output_fd;
      }

      if (WOULD_LOG(VERBOSE)) {
        std::stringstream s;
        s << "--verbose";
        argv_vec.push_back(s.str());

        argv << " --verbose";
      }

      std::unique_ptr<ArgString[]> argv_ptr = VecToArgv(kCommandFileName, argv_vec);

      LOG(DEBUG) << "fork+exec: " << kCommandFileName << " "
                 << argv.str();
      execve(kCommandFileName, (char **)argv_ptr.get(), /*envp*/nullptr);
      // This should never return.
      _exit(EXIT_FAILURE);
    }

    DCHECK(false);
    return false;
  }

  // TODO: Not very useful since this can never return 'true'
  // -> in the child we would've already execd which loses all this code.
  bool IsDaemon() {
    // In the child the pid is always 0.
    return child_ > 0;
  }

  bool Main(PrefetcherForkParameters params) {
    LOG(VERBOSE) << "PrefetcherDaemon::Main " << params;

    CommandParser command_parser{std::move(params)};

    Command next_command{};

    std::vector<Command> many_commands;

    while (true) {
      bool eof = false;
      many_commands = command_parser.ParseCommands(/*out*/eof);
      sleep(1);

      if (eof) {
        LOG(WARNING) << "PrefetcherDaemon got EOF, terminating";
        return true;
      }

      for (auto& command : many_commands) {
        if (LogVerboseIpc()) {
          LOG(VERBOSE) << "PrefetcherDaemon got command: " << command;
        }

        if (command.choice == CommandChoice::kExit) {
          LOG(DEBUG) << "PrefetcherDaemon got kExit command, terminating";
          return true;
        }

        if (!ReceiveCommand(command)) {
          LOG(WARNING) << "PrefetcherDaemon command processing failure: " << command;
        }
      }
    }

    LOG(VERBOSE) << "PrefetcherDaemon::Main got exit, terminating";

    return true;
    // Terminate.
  }

  Impl(PrefetcherDaemon* daemon) {
    session_manager_ = SessionManager::CreateManager(SessionKind::kInProcessDirect);
    DCHECK(session_manager_ != nullptr);
  };

  ~Impl() {
    // Don't do anything if we never called 'StartViaFork'
    if (forked_) {
      if (!IsDaemon()) {
        int status;
        waitpid(child_, /*out*/&status, /*options*/0);
      } else {
        DCHECK(false) << "not possible because the execve would avoid this path";
      }
    }
  }

  bool SendCommand(const Command& command) {
    // Only parent is the sender.
    DCHECK(forked_);
    //DCHECK(!IsDaemon());

    char buf[1024];
    size_t stream_size;
    if (!command.Write(buf, sizeof(buf), /*out*/&stream_size)) {
      PLOG(ERROR) << "Failed to serialize command: " << command;
      return false;
    }

    if (TEMP_FAILURE_RETRY(write(pipefd_write_, buf, stream_size)) < 0) {
      PLOG(ERROR) << "Failed to write command: " << command;
      return false;
    }

    if (LogVerboseIpc()) {
      LOG(VERBOSE) << "write(fd=" << pipefd_write_ << ", buf=" << buf
                   << ", size=" << stream_size<< ")";
    }

    // TODO: also read the reply?
    return true;
  }

  bool ReceiveCommand(const Command& command) {
    // Only child is the command receiver.
    // DCHECK(IsDaemon());

    switch (command.choice) {
      case CommandChoice::kRegisterFilePath: {
        std::shared_ptr<Session> session = session_manager_->FindSession(command.session_id);

        if (!session) {
          LOG(ERROR) << "ReceiveCommand: Could not find session for command: " << command;
          return false;
        }

        CHECK(command.file_path.has_value()) << command;
        return session->RegisterFilePath(command.id, *command.file_path);
      }
      case CommandChoice::kUnregisterFilePath: {
        std::shared_ptr<Session> session = session_manager_->FindSession(command.session_id);

        if (!session) {
          LOG(ERROR) << "ReceiveCommand: Could not find session for command: " << command;
          return false;
        }

        return session->UnregisterFilePath(command.id);
      }
      case CommandChoice::kReadAhead: {
        std::shared_ptr<Session> session = session_manager_->FindSession(command.session_id);

        if (!session) {
          LOG(ERROR) << "ReceiveCommand: Could not find session for command: " << command;
          return false;
        }

        return session->ReadAhead(command.id, command.read_ahead_kind, command.length, command.offset);
      }
      // TODO: unreadahead
      case CommandChoice::kExit: {
        LOG(WARNING) << "kExit should be handled earlier.";
        return true;
      }
      case CommandChoice::kCreateSession: {
        std::shared_ptr<Session> session = session_manager_->FindSession(command.session_id);
        if (session != nullptr) {
          LOG(ERROR) << "ReceiveCommand: session for ID already exists: " << command;
          return false;
        }
        CHECK(command.file_path.has_value()) << command;
        if (session_manager_->CreateSession(command.session_id, /*description*/*command.file_path)
                == nullptr) {
          LOG(ERROR) << "ReceiveCommand: Failure to kCreateSession: " << command;
          return false;
        }
        return true;
      }
      case CommandChoice::kDestroySession: {
        if (!session_manager_->DestroySession(command.session_id)) {
          LOG(ERROR) << "ReceiveCommand: Failure to kDestroySession: " << command;
          return false;
        }
        return true;
      }
      case CommandChoice::kDumpSession: {
        std::shared_ptr<Session> session = session_manager_->FindSession(command.session_id);

        if (!session) {
          LOG(ERROR) << "ReceiveCommand: Could not find session for command: " << command;
          return false;
        }

        // TODO: Consider doing dumpsys support somehow?
        session->Dump(LOG_STREAM(DEBUG), /*multiline*/true);
        return true;
      }
      case CommandChoice::kDumpEverything: {
        session_manager_->Dump(LOG_STREAM(DEBUG), /*multiline*/true);
        break;
      }
    }

    // TODO.
    return true;
  }

  pid_t child_;
  bool forked_;
  int pipefd_read_;
  int pipefd_write_;
  PrefetcherForkParameters params_;
  // do not ever use an indirect session manager here, as it would cause a lifetime cycle.
  std::unique_ptr<SessionManager> session_manager_; // direct only.
};

PrefetcherDaemon::PrefetcherDaemon()
  : impl_{new Impl{this}} {
  LOG(VERBOSE) << "PrefetcherDaemon() constructor";
}

bool PrefetcherDaemon::StartViaFork(PrefetcherForkParameters params) {
  return impl_->StartViaFork(std::move(params));
}


std::optional<PrefetcherForkParameters> PrefetcherDaemon::StartPipesViaFork() {
  return impl_->StartPipesViaFork();
}

bool PrefetcherDaemon::Main(PrefetcherForkParameters params) {
  return impl_->Main(params);
}

bool PrefetcherDaemon::SendCommand(const Command& command) {
  return impl_->SendCommand(command);
}

PrefetcherDaemon::~PrefetcherDaemon() {
  // required for unique_ptr for incomplete types.
}

}  // namespace iorap::prefetcher
