/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "binder/iiorap_impl.h"
#include "binder/iiorap_def.h"
#include "common/macros.h"

#include <android-base/logging.h>
#include <binder/BinderService.h>
#include <binder/IPCThreadState.h>
#include <include/binder/request_id.h>

/*
 * Definitions for the IIorap binder native service implementation.
 * See also IIorap.aidl.
 */

using Status = ::android::binder::Status;
using ITaskListener = ::com::google::android::startop::iorap::ITaskListener;

namespace iorap {
namespace binder {

namespace {
// Forward declarations.
template<typename ... Args>
Status Send(const char* function_name, Args&& ... args);
}

// Join all parameter declarations by splitting each parameter with a comma.
// Types are used fully.
#define IIORAP_IMPL_ARG_DECLARATIONS(...) \
  IORAP_PP_MAP_SEP(IORAP_BINDER_PARAM_JOIN_ALL, IORAP_PP_COMMA, __VA_ARGS__)
#define IIORAP_IMPL_ARG_NAMES(...) \
  IORAP_PP_MAP_SEP(IORAP_BINDER_PARAM_JOIN_NAMES, IORAP_PP_COMMA, __VA_ARGS__)
#define IIORAP_IMPL_BODY(name, ...)                                                                \
  ::android::binder::Status IIorapImpl::name(IIORAP_IMPL_ARG_DECLARATIONS(__VA_ARGS__)) {          \
    return Send(#name, impl_.get(), IIORAP_IMPL_ARG_NAMES(__VA_ARGS__));                           \
  }

IIORAP_IFACE_DEF(/*begin*/IORAP_PP_NOP, IIORAP_IMPL_BODY, /*end*/IORAP_PP_NOP);

#undef IIORAP_IMPL_BODY
#undef IIORAP_IMPL_ARG_NAMES
#undef IIORAP_IMPL_ARGS

class IIorapImpl::Impl {
 public:
  void SetTaskListener(const ::android::sp<ITaskListener>& listener) {
    ::android::sp<ITaskListener> old_listener = listener_;
    if (old_listener != nullptr && listener != nullptr) {
      LOG(WARNING) << "IIorap::setTaskListener: already had a task listener set";
    }
    listener_ = listener;
  }

  void ReplyWithResult(const RequestId& request_id, TaskResult::State result_state) {
    ::android::sp<ITaskListener> listener = listener_;
    if (listener == nullptr) {
      // No listener. Cannot send anything back to the client.
      // This could be normal, e.g. client had set listener to null before disconnecting.
      LOG(WARNING) << "Drop result, no listener registered.";
      // TODO: print the result with ostream operator<<
      return;
    }

    TaskResult result;
    result.state = result_state;

    // TODO: verbose, not info.
    if (result_state == TaskResult::State::kCompleted) {
      LOG(VERBOSE) << "ITaskListener::onComplete (request_id=" << request_id.request_id << ")";
      listener->onComplete(request_id, result);
    } else {
      LOG(VERBOSE) << "ITaskListener::onProgress (request_id=" << request_id.request_id << ")";
      listener->onProgress(request_id, result);
    }
  }

  ::android::sp<ITaskListener> listener_;
};

using Impl = IIorapImpl::Impl;

IIorapImpl::IIorapImpl() : impl_(new Impl()) {}

namespace {
  static bool started_ = false;
}
bool IIorapImpl::Start() {
  if (started_) {
    LOG(ERROR) << "service was already started";
    return false;  // Already started
  }

  ::android::IPCThreadState::self()->disableBackgroundScheduling(/*disable*/true);
  ::android::status_t ret = android::BinderService<IIorapImpl>::publish();
  if (ret != android::OK) {
    LOG(ERROR) << "BinderService::publish failed with error code: " << ret;
    return false;
  }
  android::sp<android::ProcessState> ps = android::ProcessState::self();
  // Reduce thread consumption by only using 1 thread.
  // We should also be able to leverage this by avoiding locks, etc.
  ps->setThreadPoolMaxThreadCount(/*maxThreads*/1);
  ps->startThreadPool();
  ps->giveThreadPoolName();

  started_ = true;

  return true;
}

namespace {
template <typename ... Args>
void SendArgs(const char* function_name,
              Impl* self,
              const RequestId& request_id,
              Args&&... /*rest*/) {
  // TODO: verbose, not INFO
  LOG(VERBOSE) << "IIorap::" << function_name << " (request_id = " << request_id.request_id << ")";
  // TODO: implementation.

  // Send these dummy callbacks for testing only.
  // TODO: these should only be sent back when the client connects in a special 'test' mode.
  self->ReplyWithResult(request_id, TaskResult::State::kBegan);
  self->ReplyWithResult(request_id, TaskResult::State::kOngoing);
  self->ReplyWithResult(request_id, TaskResult::State::kCompleted);
}

template <typename ... Args>
void SendArgs(const char* /*function_name*/, Impl* self, Args&&... rest) {
  // TODO: may want an assert here for readability.
  LOG(VERBOSE) << "IIorap::setTaskListener";
  self->SetTaskListener(std::forward<Args&&>(rest)...);
}

template <typename ... Args>
Status Send(const char* function_name, Args&&... args) {
  LOG(VERBOSE) << "IIorap::Send(" << function_name << ")";

  SendArgs(function_name, std::forward<Args>(args)...);

  // Note: The exact return code doesn't matter: all the AIDL methods are oneway.
  return Status::ok();
}
}

}
}
