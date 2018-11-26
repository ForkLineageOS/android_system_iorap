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

#ifndef IORAP_BINDER_APP_LAUNCH_EVENT_H_
#define IORAP_BINDER_APP_LAUNCH_EVENT_H_

#include "binder/common.h"
#include "common/introspection.h"

namespace iorap {
namespace binder {

struct AppLaunchEvent : public ::android::Parcelable {
  enum class Type : int32_t {
    kIntentStarted = 0,
    kIntentFailed = 1,
    kActivityLaunched = 2,
    kActivityLaunchCancelled = 3,
    kActivityLaunchFinished = 4,
  };

  enum class Temperature : int32_t {
    kCold = 1,
    kWarm = 2,
    kHot = 3,
  };

  Type type;
  int64_t sequenceId;
  std::string intent;                               // kIntentStarted only.
  Temperature temperature;                          // kActivityLaunched only.
  std::vector<uint8_t> activity_record_proto;       // kActivityLaunch*

  ::android::status_t readFromParcel(const android::Parcel* parcel) override {
    ::android::status_t res = android::NO_ERROR;

    type = static_cast<Type>(parcel->readInt32());
    sequenceId = parcel->readInt64();

    switch (type) {
      case Type::kIntentStarted:
        return readIntent(parcel);
      case Type::kIntentFailed:
        // No extra arguments.
        break;
      case Type::kActivityLaunched:
        res = readActivityRecordProto(parcel);
        if (res != android::NO_ERROR) {
          return res;
        }
        temperature = static_cast<Temperature>(parcel->readInt32());
        break;
      case Type::kActivityLaunchFinished:
        return readActivityRecordProto(parcel);
      case Type::kActivityLaunchCancelled:
        return readActivityRecordProtoNullable(parcel);
      default:
        return android::BAD_VALUE;
    }
    return res;

    // TODO: std::variant + protobuf implementation in AutoParcelable.
  }

  ::android::status_t writeToParcel(android::Parcel* parcel) const override {
    parcel->writeInt32(static_cast<int32_t>(type));
    parcel->writeInt64(sequenceId);
    // TODO rest.
    return android::NO_ERROR;
  }

 private:
  android::status_t readIntent(const android::Parcel* parcel) {
    android::status_t res = parcel->readUtf8FromUtf16(/*out*/&intent);
    if (res != android::NO_ERROR) return res;
    // FIXME: This is a placeholder which only reads the 'action'. Replace with IntentProto.
    return android::NO_ERROR;
  }

  android::status_t readActivityRecordProto(const android::Parcel* parcel) {
    // TODO: replace with ActivityRecordProto instead of just reading the byte[].
    return parcel->readByteVector(/*out*/&activity_record_proto);
  }

  android::status_t readActivityRecordProtoNullable(const android::Parcel* parcel) {
    bool has_value = parcel->readBool();
    if (has_value) {
      return readActivityRecordProto(parcel);
    }
    return android::NO_ERROR;
  }
};

IORAP_INTROSPECT_ADAPT_STRUCT(AppLaunchEvent, type, intent, activity_record_proto);

}
}
IORAP_JAVA_NAMESPACE_BINDER_TYPEDEF(AppLaunchEvent)

#endif  // IORAP_BINDER_APP_LAUNCH_EVENT_H_
