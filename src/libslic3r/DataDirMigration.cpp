#include "DataDirMigration.hpp"

#include <exception>
#include <sstream>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include "Preset.hpp" // PRESET_USER_DIR
#include "Utils.hpp"  // copy_directory_recursively, copy_file

namespace Slic3r {

namespace fs = boost::filesystem;

// The app config is JSON, followed on Windows by an MD5 checksum comment line.
static bool is_parsable_app_config(const fs::path &path)
{
    boost::nowide::ifstream ifs(path.string());
    if (!ifs)
        return false;
    std::stringstream buffer;
    buffer << ifs.rdbuf();
    const std::string text = buffer.str();
    const size_t      end  = text.find_last_of('}');
    return end != std::string::npos && !nlohmann::json::parse(text.substr(0, end + 1), nullptr, false).is_discarded();
}

static std::string migrate_user_presets(const fs::path &legacy_dir, const fs::path &new_dir)
{
    const fs::path source = legacy_dir / PRESET_USER_DIR;
    if (!fs::is_directory(source))
        return "no user presets to copy";
    // Copy into a staging folder and rename it once complete, so a failure never leaves half of
    // the presets behind.
    const fs::path target  = new_dir / PRESET_USER_DIR;
    const fs::path staging = new_dir / (std::string(PRESET_USER_DIR) + ".migrating");
    try {
        if (copy_directory_recursively(source, staging)) {
            fs::rename(staging, target);
            return "copied user presets from " + source.string();
        }
    } catch (const std::exception &ex) {
        boost::system::error_code ec;
        fs::remove_all(staging, ec);
        return "failed to copy user presets from " + source.string() + ": " + ex.what();
    }
    boost::system::error_code ec;
    fs::remove_all(staging, ec);
    return "failed to copy user presets from " + source.string();
}

static std::string migrate_app_config(const fs::path &legacy_config, const fs::path &new_config)
{
    if (!fs::is_regular_file(legacy_config))
        return "no app config to copy";
    std::string error;
    try {
        if (copy_file(legacy_config.string(), new_config.string(), error, false) == SUCCESS) {
            // The other app may have been writing the file at that very moment.
            if (is_parsable_app_config(new_config))
                return "copied app config " + legacy_config.string() + " to " + new_config.filename().string();
            error = "the copy is not a valid config";
        }
    } catch (const std::exception &ex) {
        error = ex.what();
    }
    boost::system::error_code ec;
    fs::remove(new_config, ec);
    return "failed to copy app config " + legacy_config.string() + ": " + error;
}

std::vector<std::string> migrate_legacy_data_dir(const fs::path    &legacy_dir,
                                                 const std::string &legacy_config_name,
                                                 const fs::path    &new_dir,
                                                 const std::string &new_config_name)
{
    std::vector<std::string> log;
    boost::system::error_code ec;
    // An unreadable new_dir counts as existing: when in doubt, copy nothing.
    const fs::file_status new_status = fs::status(new_dir, ec);
    if (new_status.type() != fs::file_not_found || !fs::is_directory(legacy_dir, ec))
        return log;
    if (!fs::create_directories(new_dir, ec) && !fs::is_directory(new_dir)) {
        log.emplace_back("cannot create " + new_dir.string() + ": " + ec.message());
        return log;
    }
    log.emplace_back("seeding " + new_dir.string() + " from " + legacy_dir.string());
    log.emplace_back(migrate_user_presets(legacy_dir, new_dir));
    log.emplace_back(migrate_app_config(legacy_dir / legacy_config_name, new_dir / new_config_name));
    return log;
}

} // namespace Slic3r
