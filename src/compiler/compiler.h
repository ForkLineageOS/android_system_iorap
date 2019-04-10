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

#ifndef IORAP_SRC_COMPILER_COMPILER_H_
#define IORAP_SRC_COMPILER_COMPILER_H_

#include <string>
#include <vector>

namespace iorap::compiler {
// Compile one or more perfetto TracePacket protobufs that are stored on the filesystem
// by the filenames given with `input_file_names`.
//
// The result is stored into the file path specified by `output_file_name`.
//
// This is a blocking function which returns only when compilation finishes. The return value
// determines success/failure.
//
// Operation is transactional -- that is if there is a failure, `output_file_name` is untouched.
bool PerformCompilation(std::vector<std::string> input_file_names,
                        std::string output_file_name,
                        bool output_proto = true);
}

#endif  // IORAP_SRC_COMPILER_COMPILER_H_
