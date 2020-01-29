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

namespace iorap::binder {

std::shared_ptr<PackageManagerRemote> PackageManagerRemote::Create() {
  android::sp<IPackageManager> package_service = GetPackageService();
  if (package_service == nullptr) {
    return nullptr;
  }
  return std::make_shared<PackageManagerRemote>(package_service);
}

android::sp<IPackageManager> PackageManagerRemote::GetPackageService() {
  auto binder = android::defaultServiceManager()->getService(
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
  auto status = package_service_->getVersionCodeForPackage(
      android::String16(package_name.c_str()), &version_code);
  if (!status.isOk()) {
    LOG(WARNING) << "Failed to get version: " << status.toString8().c_str()
                 << " for " << package_name;
    return std::nullopt;
  }

  return std::optional<int64_t>{version_code};
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
  CHECK(package_service_ != nullptr);
  std::vector<std::string> packages;
  auto status = package_service_->getAllPackages(/*out*/&packages);
  if (!status.isOk()) {
    LOG(ERROR) << "Failed to get all packages: " << status.toString8().c_str();
  }
  return packages;
}
}  // namespace iorap::package_manager
