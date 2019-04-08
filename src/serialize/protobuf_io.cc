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

#include "protobuf_io.h"

#include "serialize/arena_ptr.h"

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "system/iorap/src/serialize/TraceFile.pb.h"

namespace iorap {
namespace serialize {

ArenaPtr<proto::TraceFile> ProtobufIO::Open(std::string file_path) {
  // TODO: file a bug about this.
  // Note: can't use {} here, clang think it's narrowing from long->int.
  android::base::unique_fd fd(TEMP_FAILURE_RETRY(::open(file_path.c_str(), O_RDONLY)));
  if (fd.get() < 0) {
    PLOG(ERROR) << "ProtobufIO: open failed: " << file_path;
    return nullptr;
  }

  return Open(fd.get(), file_path.c_str());
}

ArenaPtr<proto::TraceFile> ProtobufIO::Open(int fd, const char* file_path) {
  android::base::Timer timer{};

  struct stat buf;
  if (fstat(fd, /*out*/&buf) < 0) {
    PLOG(ERROR) << "ProtobufIO: open error, fstat failed: " << file_path;
    return nullptr;
  }
  // XX: off64_t for stat::st_size ?

  // Using the mmap appears to be the only way to do zero-copy with protobuf lite.
  void* data = mmap(/*addr*/nullptr,
                    buf.st_size,
                    PROT_READ, MAP_SHARED | MAP_POPULATE,
                    fd,
                    /*offset*/0);
  if (data == nullptr) {
    PLOG(ERROR) << "ProtobufIO: open error, mmap failed: " << file_path;
    return nullptr;
  }

  ArenaPtr<proto::TraceFile> protobuf_trace_file = ArenaPtr<proto::TraceFile>::Make();
  if (protobuf_trace_file == nullptr) {
    LOG(ERROR) << "ProtobufIO: open error, failed to create arena: " << file_path;
    return nullptr;
  }

  google::protobuf::io::ArrayInputStream protobuf_input_stream{data, static_cast<int>(buf.st_size)};
  if (!protobuf_trace_file->ParseFromZeroCopyStream(/*in*/&protobuf_input_stream)) {
    // XX: Does protobuf on android already have the right LogHandler ?
    LOG(ERROR) << "ProtobufIO: open error, protobuf parsing failed: " << file_path;
    return nullptr;
  }

  if (munmap(data, buf.st_size) < 0) {
    PLOG(WARNING) << "ProtobufIO: open problem, munmap failed, possibly memory leak? "
                  << file_path;
  }

  LOG(VERBOSE) << "ProtobufIO: open succeeded: " << file_path << ", duration: " << timer;
  return protobuf_trace_file;
}

}  // namespace serialize
}  // namespace iorap

