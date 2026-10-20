#ifndef _common_func_hppp_
#define _common_func_hppp_
#include <iostream>


#define SLIC3R_APP_NAME "Snapmaker Orca"
#define SLIC3R_APP_KEY "Snapmaker_Orca"
// SnapOrca: name of the user data folder (%APPDATA%, ~/Library/Application Support, $XDG_CONFIG_HOME)
// and of its app config file. It differs from SLIC3R_APP_KEY so that SnapOrca never shares its data
// with an official Snapmaker Orca installed side by side; SLIC3R_APP_KEY keeps naming everything
// else (translation catalogs, 3MF metadata, bundle id, executables), which must stay unchanged.
#define SLIC3R_APP_DATA_KEY "SnapOrca"
#define SLIC3R_VERSION "01.10.01.50"
#define Snapmaker_VERSION "2.4.1"
#define MIN_FIRM_VER "2.0.0"
#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "0000000" // 0000000 means uninitialized
#endif
#define SLIC3R_BUILD_ID "2.4.1"
// #define SLIC3R_RC_VERSION "01.10.01.50"
#define BBL_RELEASE_TO_PUBLIC 1
#define BBL_INTERNAL_TESTING 0
#define ORCA_CHECK_GCODE_PLACEHOLDERS 0

namespace common 
{
	std::string get_pc_name();

	std::string get_flutter_version();

	std::string get_profile_version();

	std::string getMachineId();

	std::string getLocalArea();

	std::string getLanguage();

    } // namespace common

#endif