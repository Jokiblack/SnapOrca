#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/DataDirMigration.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

static void write_file(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    boost::nowide::ofstream(path.string()) << text;
}

static std::string read_file(const fs::path &path)
{
    boost::nowide::ifstream ifs(path.string());
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

TEST_CASE("Migration copies only user presets and the app config into a new data folder", "[DataDirMigration]")
{
    ScopedTemporaryDir root;
    const fs::path legacy = root.path() / "Snapmaker_Orca";
    const fs::path target = root.path() / "SnapOrca";
    const std::string config = "{\"app\":{\"language\":\"es_ES\"}}\n# MD5 checksum 0123\n";
    write_file(legacy / "Snapmaker_Orca.conf", config);
    write_file(legacy / "user" / "default" / "filament" / "My PLA.json", "{}");
    write_file(legacy / "system" / "Snapmaker.json", "{}");
    write_file(legacy / "web" / "flutter_web" / "version.json", "{}");

    const auto log = migrate_legacy_data_dir(legacy, "Snapmaker_Orca.conf", target, "SnapOrca.conf");

    CHECK_FALSE(log.empty());
    CHECK(read_file(target / "SnapOrca.conf") == config);
    CHECK(fs::exists(target / "user" / "default" / "filament" / "My PLA.json"));
    CHECK_FALSE(fs::exists(target / "user.migrating"));
    CHECK_FALSE(fs::exists(target / "system"));
    CHECK_FALSE(fs::exists(target / "web"));
    // The legacy folder is left as it was.
    CHECK(fs::exists(legacy / "Snapmaker_Orca.conf"));
    CHECK(fs::exists(legacy / "user" / "default" / "filament" / "My PLA.json"));
}

TEST_CASE("Migration does nothing once the new data folder exists", "[DataDirMigration]")
{
    ScopedTemporaryDir root;
    const fs::path legacy = root.path() / "Snapmaker_Orca";
    const fs::path target = root.path() / "SnapOrca";
    write_file(legacy / "Snapmaker_Orca.conf", "{}");
    fs::create_directories(target);

    CHECK(migrate_legacy_data_dir(legacy, "Snapmaker_Orca.conf", target, "SnapOrca.conf").empty());
    CHECK_FALSE(fs::exists(target / "SnapOrca.conf"));
}

TEST_CASE("Migration does nothing without a legacy data folder", "[DataDirMigration]")
{
    ScopedTemporaryDir root;
    const fs::path target = root.path() / "SnapOrca";

    CHECK(migrate_legacy_data_dir(root.path() / "Snapmaker_Orca", "Snapmaker_Orca.conf", target, "SnapOrca.conf").empty());
    CHECK_FALSE(fs::exists(target));
}

TEST_CASE("Migration drops an app config that does not parse", "[DataDirMigration]")
{
    ScopedTemporaryDir root;
    const fs::path legacy = root.path() / "Snapmaker_Orca";
    const fs::path target = root.path() / "SnapOrca";
    write_file(legacy / "Snapmaker_Orca.conf", "{\"app\":{\"language\":");

    migrate_legacy_data_dir(legacy, "Snapmaker_Orca.conf", target, "SnapOrca.conf");

    CHECK(fs::is_directory(target));
    CHECK_FALSE(fs::exists(target / "SnapOrca.conf"));
}
