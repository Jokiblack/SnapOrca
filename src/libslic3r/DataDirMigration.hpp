#pragma once

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// SnapOrca keeps its user data in its own folder rather than in the one of the official Snapmaker
// Orca. On the first launch, before that folder exists, this seeds it from the legacy folder with
// the user's own data only: the user presets (user/) and the app config, renamed to the new config
// file name. Everything else (system/, web/, ota/, plugins, caches, logs) is rebuilt by the app,
// and copying it would carry over the other app's versions.
//
// Nothing happens when new_dir already exists or legacy_dir is not a directory. The legacy folder
// is only read, never changed. Errors never propagate: a step that fails is rolled back and the
// app starts with whatever was copied. Returns one line per step, for the log.
std::vector<std::string> migrate_legacy_data_dir(const boost::filesystem::path &legacy_dir,
                                                 const std::string             &legacy_config_name,
                                                 const boost::filesystem::path &new_dir,
                                                 const std::string             &new_config_name);

} // namespace Slic3r
