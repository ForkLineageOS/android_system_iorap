// Copyright (C) 2020 The Android Open Source Project
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

#include "package_manager_remote.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <chrono>
#include <thread>

namespace iorap::binder {

const constexpr int64_t kTimeoutMs = 60 * 1000; // 1 min
const constexpr int64_t kIntervalMs = 1000; // 1 sec

std::shared_ptr<PackageManagerRemote> PackageManagerRemote::Create() {
  std::shared_ptr<PackageManagerRemote> package_manager =
      std::make_shared<PackageManagerRemote>();
  if (package_manager->ReconnectWithTimeout(kTimeoutMs)) {
    return package_manager;
  }
  return nullptr;
}

android::sp<IPackageManager> PackageManagerRemote::GetPackageService() {
  android::sp<android::IBinder> binder = android::defaultServiceManager()->getService(
      android::String16{"package_native"});
  if (binder == nullptr) {
    LOG(ERROR) << "Cannot get package manager service!";
    return nullptr;
  }

  return android::interface_cast<IPackageManager>(binder);
}

std::optional<int64_t> PackageManagerRemote::GetPackageVersion(
    const std::string& package_name) {
  int64_t version_code;
  android::binder::Status status = InvokeRemote(
      [this, &version_code, &package_name]() {
        return package_service_->getVersionCodeForPackage(
            android::String16(package_name.c_str()), /*out*/&version_code);
      });
  if (status.isOk()) {
    return version_code;
  } else {
    LOG(WARNING) << "Failed to get version: "
                 << status.toString8().c_str()
                 << " for " << package_name
                 << ". Retry to connect package manager service.";
    return std::nullopt;
  }
}

VersionMap PackageManagerRemote::GetPackageVersionMap() {
  VersionMap package_version_map;
  std::vector<std::string> packages = GetAllPackages();
  LOG(DEBUG) << "PackageManagerRemote::GetPackageVersionMap: "
             << packages.size()
             << " packages are found.";

  for (std::string package : packages) {
    std::optional<int64_t> version = GetPackageVersion(package);
    if (!version) {
      LOG(DEBUG) << "Cannot get version for " << package;
      continue;
    }
    package_version_map[package] = *version;
  }

  return package_version_map;
}

std::vector<std::string> PackageManagerRemote::GetAllPackages() {
  std::vector<std::string> packages;
  android::binder::Status status = InvokeRemote(
      [this, &packages]() {
        return package_service_->getAllPackages(/*out*/&packages);
      });

  if (status.isOk()) {
    return packages;
  }

  LOG(FATAL) << "Failed to get all packages: "
             << status.toString8().c_str();
  return packages;

}

bool PackageManagerRemote::ReconnectWithTimeout(int64_t timeout_ms) {
  int64_t count = 0;
  package_service_ = nullptr;
  std::chrono::duration interval = std::chrono::milliseconds(1000);
  std::chrono::duration timeout = std::chrono::milliseconds(timeout_ms);

  while (package_service_ == nullptr) {
    LOG(WARNING) << "Reconnect to package manager service: " << ++count << " times";
    package_service_ = GetPackageService();
    std::this_thread::sleep_for(interval);
    if (count * interval >= timeout) {
      LOG(FATAL) << "Fail to create version map in "
                 << timeout.count()
                 << " seconds.";
      return false;
    }
  }

  return true;
}

template <typename T>
android::binder::Status PackageManagerRemote::InvokeRemote(T&& lambda) {
  android::binder::Status status =
      static_cast<android::binder::Status>(lambda());
  if (status.isOk()) {
    return status;
  }

  if (!ReconnectWithTimeout(kTimeoutMs)) {
    return status;
  }

  return lambda();
}
}  // namespace iorap::package_manager
