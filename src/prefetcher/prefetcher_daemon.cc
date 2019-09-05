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

#include <android-base/logging.h>

#include <deque>
#include <string>
#include <sstream>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace iorap::prefetcher {

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

std::ostream& operator<<(std::ostream& os, const Command& command) {
  os << "Command{";
  os << "choice=";
  switch (command.choice) {
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
    default:
      CHECK(false) << "forgot to handle this choice";
      break;
  }
  os << ",";
  os << "id=" << command.id << ",";
  os << "file_path=";

  if (command.file_path) {
    os << *(command.file_path);
  } else {
    os << "(nullopt)";
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

static constexpr bool kDebugParsingRead = true;

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

    char buf[1024] = {};

    // Binary only parsing. The higher level code can parse text
    // with ifstream if it really wants to.
    char* stream = &buf[0];
    size_t stream_size = sizeof(buf);

    while (true) {
      if (stream_size == 0) {
        // TODO: reply with an overflow command.
        LOG(WARNING) << "prefetcher_daemon command overflow, dropping all commands.";
        stream = &buf[0];
        stream_size = sizeof(buf);
        memset(&buf[0], /*c*/0, sizeof(buf));
      }

      LOG(VERBOSE) << "PrefetcherDaemon block in read for commands";
      ssize_t count = TEMP_FAILURE_RETRY(read(params_.input_fd, stream, stream_size));
      LOG(VERBOSE) << "PrefetcherDaemon::read " << count << " for stream size:" << stream_size;

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
      LOG(VERBOSE) << "PrefetcherDaemon updated longbuf size: " << longbuf_.size();

      std::optional<Command> maybe_command;
      {
        if (longbuf_.size() == 0) {
          break;
        }

        std::vector<char> v(longbuf_.begin(),
                            longbuf_.end());

        LOG(VERBOSE) << "PrefetcherDaemon temp v size: " << v.size();

        size_t v_off = 0;
        size_t consumed_bytes = std::numeric_limits<size_t>::max();
        size_t consumed_total = 0;

        while (true) {
          maybe_command = Command::Read(&v[v_off], v.size() - v_off, &consumed_bytes);
          consumed_total += consumed_bytes;
          if (!maybe_command) {
            LOG(VERBOSE) << "failed to read command, v_off=" << v_off << ",v_size:" << v.size();
            break;
          }

          // in the next pass ignore what we already consumed.
          v_off += consumed_bytes;

          // true as long we don't hit the 'break' above.
          DCHECK_EQ(v_off, consumed_total);
          LOG(VERBOSE) << "success to read command, v_off=" << v_off << ",v_size:" << v.size()
                       << "," << *maybe_command;

          // Pretty-print a single command for debugging/testing.
          LOG(VERBOSE) << *maybe_command;

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

static constexpr bool kDebugCommandRead = true;

#define DEBUG_READ if (kDebugCommandRead) LOG(VERBOSE) << "Command::Read "

std::optional<Command> Command::Read(char* buf, size_t buf_size, /*out*/size_t* consumed_bytes) {
  *consumed_bytes = 0;
  if (buf == nullptr) {
    return std::nullopt;
  }

  Command cmd;
  ParseResult<CommandChoice> parsed_choice = ParsingRead<CommandChoice>(buf, buf_size);
  cmd.choice = parsed_choice.value;

  if (!parsed_choice) {
    DEBUG_READ << "no choice";
    return std::nullopt;
  }

  switch (parsed_choice.value) {
    case CommandChoice::kRegisterFilePath: {
      ParseResult<uint32_t> parsed_id = ParsingRead<uint32_t>(parsed_choice);
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

      cmd.choice = parsed_choice.value;
      cmd.id = parsed_id.value;
      cmd.file_path = parsed_file_path.value;

      break;
    }
    case CommandChoice::kUnregisterFilePath:
      // fall-through
    case CommandChoice::kReadAhead: {
      ParseResult<uint32_t> parsed_id = ParsingRead<uint32_t>(parsed_choice);

      if (!parsed_id) {
        DEBUG_READ << "no parsed id";
        return std::nullopt;
      }
      *consumed_bytes = parsed_id.next_token - buf;

      cmd.choice = parsed_choice.value;
      cmd.id = parsed_id.value;

      break;
    }
    case CommandChoice::kExit:
      *consumed_bytes = parsed_choice.next_token - buf;
      // Only need to parse the choice.
      break;
    default:
      LOG(FATAL) << "unrecognized command number " << static_cast<uint32_t>(parsed_choice.value);
      break;
  }

  return cmd;
}

bool Command::Write(char* buf, size_t buf_size, /*out*/size_t* produced_bytes) {
  *produced_bytes = 0;
  if (buf == nullptr) {
    return false;
  }

  bool has_enough_space = false;
  size_t space_requirement = std::numeric_limits<size_t>::max();

  space_requirement = sizeof(choice);

  switch (choice) {
    case CommandChoice::kRegisterFilePath:
      space_requirement += sizeof(id);
      space_requirement += sizeof(uint32_t);    // string length

      if (!file_path) {
        LOG(WARNING) << "Missing file path for kRegisterFilePath";
        return false;
      }

      space_requirement += file_path->size(); // string contents
      break;
    case CommandChoice::kUnregisterFilePath:
      // fall-through
    case CommandChoice::kReadAhead:
      space_requirement += sizeof(id);
      break;
    case CommandChoice::kExit:
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
      memcpy(&buf[buf_offset], &id, sizeof(id));
      buf_offset += sizeof(id);

      {
        uint32_t string_length = static_cast<uint32_t>(file_path->size());
        memcpy(&buf[buf_offset], &string_length, sizeof(string_length));
        buf_offset += sizeof(string_length);
      }

      DCHECK(file_path.has_value());

      memcpy(&buf[buf_offset], file_path->c_str(), file_path->size());
      buf_offset += sizeof(file_path->size());
      break;
    case CommandChoice::kUnregisterFilePath:
      // fall-through
    case CommandChoice::kReadAhead:
      memcpy(&buf[buf_offset], &id, sizeof(id));
      buf_offset += sizeof(id);
      break;
    case CommandChoice::kExit:
      // Only need to write out the choice.
      break;
    default:
      LOG(FATAL) << "should have fallen out in the above switch"
                 << static_cast<uint32_t>(choice);
      break;
  }

  DCHECK_EQ(buf_offset, space_requirement);
  DCHECK_EQ(buf_offset, *produced_bytes);

  return true;
}

class PrefetcherDaemon::Impl {
 public:
  std::optional<PrefetcherForkParameters> StartPipesViaFork() {
    int pipefds[2];
    if (!pipe(&pipefds[0])) {
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
    parent_ = fork();

    if (parent_ == -1) {
      LOG(FATAL) << "Failed to fork PrefetcherDaemon";
    } else if (parent_ > 0) {  // we are the caller of this function
      return true;
    } else {
      // we are the child that was forked.

      std::stringstream argv;  // for logging

      std::vector<std::string> argv_vec;

      {
        std::stringstream s;
        s << "--input-fd";
        argv_vec.push_back(s.str());
        s.clear();
        s << params.input_fd;
        argv_vec.push_back(s.str());

        argv << "--input-fd" << " " << params.input_fd;
      }

      {
        std::stringstream s;
        s << "--output-fd";
        argv_vec.push_back(s.str());
        s.clear();
        s << params.output_fd;
        argv_vec.push_back(s.str());

        argv << "--output-fd" << " " << params.output_fd;
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

  bool IsDaemon() {
    // In the child the pid is always 0.
    return parent_ > 0;
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
        LOG(VERBOSE) << "PrefetcherDaemon got command: " << command;

        if (command.choice == CommandChoice::kExit) {
          LOG(DEBUG) << "PrefetcherDaemon got kExit command, terminating";
          return true;
        }
      }
    }

    LOG(VERBOSE) << "PrefetcherDaemon::Main got exit, terminating";

    return true;
    // Terminate.
  }

  ~Impl() {
    // Don't do anything if we never called 'StartViaFork'
    if (forked_) {
      if (!IsDaemon()) {
        int status;
        waitpid(parent_, /*out*/&status, /*options*/0);
      } else {
        DCHECK(false) << "not possible because the execve would avoid this path";
      }
    }
  }

  pid_t parent_;
  bool forked_;
  int pipefd_read_;
  int pipefd_write_;
  PrefetcherForkParameters params_;
};

PrefetcherDaemon::PrefetcherDaemon()
  : impl_{new Impl{}} {
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

PrefetcherDaemon::~PrefetcherDaemon() {
  // required for unique_ptr for incomplete types.
}

}  // namespace iorap::prefetcher
