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

#include "common/debug.h"
#include "compiler/compiler.h"

#include <android-base/parseint.h>
#include <android-base/logging.h>

#include <iostream>

#if defined(IORAP_COMPILER_MAIN)

namespace iorap::compiler {

void Usage(char** argv) {
  std::cerr << "Usage: " << argv[0] << " [--output-proto=output.pb] input1.pb [input2.pb ...]" << std::endl;
  std::cerr << "" << std::endl;
  std::cerr << "  Request a compilation of multiple inputs (format: PerfettoTraceProto)." << std::endl;
  std::cerr << "  The result is a CompiledTraceProto, representing a merged compiled trace with inodes resolved." << std::endl;
  std::cerr << "" << std::endl;
  std::cerr << "  Optional flags:" << std::endl;
  std::cerr << "    --help,-h                  Print this Usage." << std::endl;
  std::cerr << "    --output-proto $,-op $     CompiledTraceBuffer tracebuffer output file (default stdout)." << std::endl;
  std::cerr << "    --verbose,-v               Set verbosity (default off)." << std::endl;
  std::cerr << "    --wait,-w                  Wait for key stroke before continuing (default off)." << std::endl;
  exit(1);
}

int Main(int argc, char** argv) {
  android::base::InitLogging(argv);
  android::base::SetLogger(android::base::StderrLogger);

  bool wait_for_keystroke = false;
  bool enable_verbose = false;

  std::string arg_output_proto;

  if (argc == 1) {
    // Need at least 1 input file to do anything.
    Usage(argv);
  }

  std::vector<std::string> arg_input_filenames;

  for (int arg = 1; arg < argc; ++arg) {
    std::string argstr = argv[arg];
    bool has_arg_next = (arg+1)<argc;
    std::string arg_next = has_arg_next ? argv[arg+1] : "";

    if (argstr == "--help" || argstr == "-h") {
      Usage(argv);
    } else if (argstr == "--output-proto" || argstr == "-op") {
      if (!has_arg_next) {
        std::cerr << "Missing --output-proto <value>" << std::endl;
        return 1;
      }
      arg_output_proto = arg_next;
      ++arg;
    } else if (argstr == "--verbose" || argstr == "-v") {
      enable_verbose = true;
    } else if (argstr == "--wait" || argstr == "-w") {
      wait_for_keystroke = true;
    } else {
      arg_input_filenames.push_back(argstr);
    }
  }

  if (enable_verbose) {
    android::base::SetMinimumLogSeverity(android::base::VERBOSE);

    LOG(VERBOSE) << "Verbose check";
    LOG(VERBOSE) << "Debug check: " << ::iorap::kIsDebugBuild;
  } else {
    android::base::SetMinimumLogSeverity(android::base::DEBUG);
  }

  // Useful to attach a debugger...
  // 1) $> iorap.cmd.compiler -w <args>
  // 2) $> gdbclient <pid>
  if (wait_for_keystroke) {
    LOG(INFO) << "Self pid: " << getpid();
    LOG(INFO) << "Press any key to continue...";
    std::cin >> wait_for_keystroke;
  }

  int return_code = 0;
  return_code = !PerformCompilation(std::move(arg_input_filenames), std::move(arg_output_proto));

  // Uncomment this if we want to leave the process around to inspect it from adb shell.
  // sleep(100000);

  // 0 -> successfully wrote the proto out to file.
  // 1 -> failed along the way (#on_error and also see the error logs).
  return return_code;
}

}  // namespace iorap::compiler

int main(int argc, char** argv) {
  return ::iorap::compiler::Main(argc, argv);
}

#endif  // IORAP_COMPILER_MAIN
