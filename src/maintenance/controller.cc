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

#include "compiler/compiler.h"
#include "maintenance/controller.h"

#include "common/debug.h"
#include "common/expected.h"

#include "db/models.h"
#include "db/file_models.h"
#include "inode2filename/inode.h"
#include "inode2filename/search_directories.h"

#include <android-base/file.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>
#include <string>

namespace iorap::maintenance {

// Gets the perfetto trace infos in the histories.
std::vector<compiler::CompilationInput> GetPerfettoTraceInfo(
    const db::DbHandle& db,
    const std::vector<db::AppLaunchHistoryModel>& histories) {
  std::vector<compiler::CompilationInput> perfetto_traces;

  for(db::AppLaunchHistoryModel history : histories) {
    // Get perfetto trace.
    std::optional<db::RawTraceModel> raw_trace =
        db::RawTraceModel::SelectByHistoryId(db, history.id);
    if (!raw_trace) {
      LOG(ERROR) << "Cannot find raw trace for history_id: "
                 << history.id;
      continue;
    }

    uint64_t timestamp_limit = std::numeric_limits<uint64_t>::max();
    // Get corresponding timestamp limit.
    if (history.report_fully_drawn_ns) {
      timestamp_limit = *history.report_fully_drawn_ns;
    } else if (history.total_time_ns) {
      timestamp_limit = *history.total_time_ns;
    } else {
      LOG(ERROR) << " No timestamp exists. Using the max value.";
    }
    perfetto_traces.push_back({raw_trace->file_path, timestamp_limit});
  }
  return perfetto_traces;
}

// Gets the path of output compiled trace.
db::CompiledTraceFileModel GetOutputFile(const std::string& package_name,
                                         const std::string& activity_name,
                                         const std::optional<int> version) {
   db::VersionedComponentName versioned_component_name{
     package_name, activity_name, version};

   db::CompiledTraceFileModel output_file =
       db::CompiledTraceFileModel::CalculateNewestFilePath(versioned_component_name);

   return output_file;
}

// Helper struct for printing vector.
template <class T>
struct VectorPrinter {
  std::vector<T>& values;
};

std::ostream& operator<<(std::ostream& os,
                      const struct compiler::CompilationInput& perfetto_trace) {
  os << "file_path: " << perfetto_trace.filename << " "
     << "timestamp_limit: " << perfetto_trace.timestamp_limit_ns;
  return os;
}

template <class T>
std::ostream& operator<<(std::ostream& os, const struct VectorPrinter<T>& printer) {
  os << "[\n";
  for (T i : printer.values) {
    os << i << ",\n";
  }
  os << "]\n";
  return os;
}

// Compiled the perfetto traces for an activity.
bool CompileActivity(const db::DbHandle& db,
                     int package_id,
                     const std::string& package_name,
                     const std::string& activity_name,
                     bool output_text,
                     const inode2filename::InodeResolverDependencies& dependencies,
                     bool recompile) {
  db::CompiledTraceFileModel output_file = GetOutputFile(package_name,
                                                         activity_name,
                                                         /* version= */std::nullopt);
  std::string file_path = output_file.FilePath();

  if (!recompile && std::filesystem::exists(file_path)) {
    LOG(DEBUG) << "compiled trace exists in " << file_path;
    return true;
  }

  if (!output_file.MkdirWithParents()) {
    LOG(ERROR) << "Compile activity failed. Failed to mkdirs " << file_path;
    return false;
  }

  std::optional<db::ActivityModel> activity =
      db::ActivityModel::SelectByNameAndPackageId(db, activity_name.c_str(), package_id);
  if (!activity) {
    LOG(ERROR) << "Cannot find activity for package_id: " << package_id
               <<" activity_name: " <<activity_name;
    return false;
  }

  int activity_id = activity->id;

  std::vector<db::AppLaunchHistoryModel> histories =
      db::AppLaunchHistoryModel::SelectActivityHistoryForCompile(db, activity_id);

  {
    std::vector<compiler::CompilationInput> perfetto_traces =
        GetPerfettoTraceInfo(db, histories);

    // Show the compilation config.
    LOG(DEBUG) << "Try to compiled package_id: " << package_id
               << " package_name: " << package_name
               << " activity_name: " << activity_name
               << " file_path: " << file_path
               << " perfetto_traces: "
               << VectorPrinter<compiler::CompilationInput>{perfetto_traces};

    if (!PerformCompilation(std::move(perfetto_traces),
                            file_path,
                            !output_text,
                            dependencies)) {
      LOG(ERROR) << "Compilation failed for package_id:" << package_id
                 <<" activity_name: " <<activity_name;
      return false;
    }
  }

  std::optional<db::PrefetchFileModel> compiled_trace =
      db::PrefetchFileModel::Insert(db, activity_id, file_path);
  if (!compiled_trace) {
    LOG(ERROR) << "Cannot insert compiled trace activity_id: " << activity_id
               << " file_path: " << file_path;
    return false;
  }
  return true;
}

// Compiled the perfetto traces for activities in an package.
bool CompilePackage(const db::DbHandle& db,
                    const std::string& package_name,
                    bool output_text,
                    const inode2filename::InodeResolverDependencies& dependencies,
                    bool recompile) {

  std::optional<db::PackageModel> package =
      db::PackageModel::SelectByName(db, package_name.c_str());

  if (!package) {
    LOG(ERROR) << "Cannot find package for package_name: " << package_name;
    return false;
  }

  std::vector<db::ActivityModel> activities =
      db::ActivityModel::SelectByPackageId(db, package->id);

  bool ret = true;
  for (db::ActivityModel activity : activities) {
    if (!CompileActivity(db,
                         package->id,
                         package->name,
                         activity.name,
                         output_text,
                         dependencies,
                         recompile)) {
      ret = false;
    }
  }
  return ret;
}

// Compiled the perfetto traces for packages in a device.
bool CompileAppsOnDevice(const db::DbHandle& db,
                         bool output_text,
                         const inode2filename::InodeResolverDependencies& dependencies,
                         bool recompile) {
  std::vector<db::PackageModel> packages = db::PackageModel::SelectAll(db);
  bool ret = true;
  for (db::PackageModel package : packages) {
    if (!CompilePackage(db,
                        package.name,
                        output_text,
                        dependencies,
                        recompile)) {
      ret = false;
    }
  }

  return ret;
}

bool Compile(const std::string& db_path,
             bool output_text,
             const inode2filename::InodeResolverDependencies& dependencies,
             bool recompile) {
  iorap::db::SchemaModel db_schema = db::SchemaModel::GetOrCreate(db_path);
  db::DbHandle db{db_schema.db()};
  return CompileAppsOnDevice(db, output_text, dependencies, recompile);
}

bool Compile(const std::string& db_path,
             const std::string& package_name,
             bool output_text,
             const inode2filename::InodeResolverDependencies& dependencies,
             bool recompile) {
  iorap::db::SchemaModel db_schema = db::SchemaModel::GetOrCreate(db_path);
  db::DbHandle db{db_schema.db()};
  return CompilePackage(db, package_name, output_text, dependencies, recompile);
}

bool Compile(const std::string& db_path,
             const std::string& package_name,
             const std::string& activity_name,
             bool output_text,
             const inode2filename::InodeResolverDependencies& dependencies,
             bool recompile) {
  iorap::db::SchemaModel db_schema = db::SchemaModel::GetOrCreate(db_path);
  db::DbHandle db{db_schema.db()};

  std::optional<db::PackageModel> package =
      db::PackageModel::SelectByName(db, package_name.c_str());
  if (!package) {
    LOG(ERROR) << "Cannot find package with name " << package_name;
    return false;
  }
  return CompileActivity(db,
                         package->id,
                         package_name,
                         activity_name,
                         output_text,
                         dependencies,
                         recompile) ;
}

}  // namespace iorap::maintenance
