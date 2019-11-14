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

#ifndef IORAP_SRC_COMMON_CMD_UTILS_H_
#define IORAP_SRC_COMMON_CMD_UTILS_H_

#include <iostream>
#include <sstream>
#include <optional>
#include <vector>

namespace iorap::common {
// Create execve-compatible argv.
// The lifetime is tied to that of vector.
inline std::unique_ptr<const char*[]> VecToArgv(const char* program_name,
                                              const std::vector<std::string>& vector) {
  // include program name in argv[0]
  // include a NULL sentinel in the end.
  std::unique_ptr<const char*[]> ptr{new const char*[vector.size() + 2]};

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

// Appends an args to the argv.
template <class T>
void AppendArgs(std::vector<std::string>& argv,
                const T& value) {
  std::stringstream ss;
  ss << value;
  argv.push_back(ss.str());
}

// Appends an args to the argv.
template <class T, class T2>
void AppendArgs(std::vector<std::string>& argv,
                const T& value,
                const T2& value2) {
  AppendArgs(argv, value);
  AppendArgs(argv, value2);
}

// Appends args from a vector to the argv repeatedly to argv.
//
// For example, if <args> is "--timestamp" and <values> is [100, 200].
// The "--timestamp 100" and "--timestamp 200" are appended.
template <class T>
void AppendArgsRepeatedly(std::vector<std::string>& argv,
                          std::string args,
                          const std::vector<T>& values) {
  for (const T& v : values) {
     AppendArgs(argv, args, v);
  }
}

// Appends args from a vector to the argv repeatedly to argv.
//
// For example, if values is [input1.pb, input2.pb],
// then the "input1.pb" and "input2.pb" are appended.
template <class T>
void AppendArgsRepeatedly(std::vector<std::string>& argv,
                          const std::vector<T>& values) {
  for (const T& v : values) {
     AppendArgs(argv, v);
  }
}

}   // namespace iorap::common

#endif  // IORAP_SRC_COMMON_CMD_UTILS_H_
