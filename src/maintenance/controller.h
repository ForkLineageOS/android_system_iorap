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

#ifndef IORAP_SRC_MAINTENANCE_COMPILER_CONTROLLER_H_
#define IORAP_SRC_MAINTENANCE_COMPILER_CONTROLLER_H_

#include "inode2filename/inode_resolver.h"

#include <string>
#include <vector>

namespace iorap::maintenance {
// Control the compilation of perfetto traces in the sqlite db.
//
// The strategy now is to compile all the existing perfetto traces for an activity.
//
// By default, the program doesn't replace the existing compiled trace, it just
// return true. To force replace the existing compiled trace, set `force` to true.
//
// The timestamp limit of the each perfetto trace is determined by `report_fully_drawn_ns`
// timestamp. If it doesn't exists, use `total_time_ns`. If neither of them exists,
// use the max.

// Compile all activities of all packages in the database.
bool Compile(const std::string& db_path,
             bool output_text,
             const inode2filename::InodeResolverDependencies& dependencies,
             bool recompile);

// Compile all activities in the package.
bool Compile(const std::string& db_path,
             const std::string& package_name,
             bool output_text,
             const inode2filename::InodeResolverDependencies& dependencies,
             bool recompile);

// Compile trace for the activity.
bool Compile(const std::string& db_path,
             const std::string& package_name,
             const std::string& activity_name,
             bool output_text,
             const inode2filename::InodeResolverDependencies& dependencies,
             bool recompile);
} // iorap::compiler_controller

#endif  // IORAP_SRC_MAINTENANCE_COMPILER_CONTROLLER_H_
