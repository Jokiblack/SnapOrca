// Ported from OrcaFS-NeotkoCM by Neotko (AGPL-3.0), neotko/main e68dcc8551.
#include "NeoDebug.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

namespace Slic3r {

namespace NeoDebug {

namespace {

struct ChanInfo { const char* env_var; const char* name; };
constexpr ChanInfo k_chans[CH_COUNT] = {
    { "ORCA_DEBUG_COLORSTITCH",  "COLORSTITCH"  },
    { "ORCA_DEBUG_MULTIPASS",    "MULTIPASS"    },
    { "ORCA_DEBUG_PENULTIMATE",  "PENULTIMATE"  },
    { "ORCA_DEBUG_TOOLORDER",    "TOOLORDER"    },
    { "ORCA_DEBUG_ZBLEND",       "ZBLEND"       },
    { "ORCA_DEBUG_WIPETOWER",    "WIPETOWER"    },
    { "ORCA_DEBUG_PROFILE",      "PROFILE"      },
    { "ORCA_DEBUG_BOTTOM",       "BOTTOM"       },
    { "ORCA_DEBUG_CONTACT",      "CONTACT"      },
    { "ORCA_DEBUG_XOBJ",         "XOBJ"         },
    { "ORCA_DEBUG_INFILL",       "INFILL"       },
    { "ORCA_DEBUG_SUPPORTZONES", "SUPPORTZONES" },
};

std::atomic<bool> s_on[CH_COUNT];
std::atomic<bool> s_paused{false};

void init_state()
{
    static std::once_flag once;
    std::call_once(once, []() {
        const bool all = std::getenv("ORCA_DEBUG_ALL") != nullptr;
        for (int i = 0; i < CH_COUNT; ++i)
            s_on[i].store(all || std::getenv(k_chans[i].env_var) != nullptr, std::memory_order_relaxed);
    });
}

// Created on first write. If it cannot be created nothing is logged: a debug facility must never stop the slicer.
void ensure_dir()
{
    static std::once_flag once;
    std::call_once(once, []() {
        boost::system::error_code ec;
        boost::filesystem::create_directories(log_dir(), ec);
    });
}

bool any_env_set(const char* a, const char* b)
{
    return std::getenv(a) != nullptr || (b != nullptr && std::getenv(b) != nullptr);
}

} // namespace

const char* channel_name(Channel c) { return k_chans[static_cast<int>(c)].name; }

std::string log_dir()
{
    static const std::string dir = [] {
        boost::system::error_code ec;
        boost::filesystem::path tmp = boost::filesystem::temp_directory_path(ec);
        if (ec)
            tmp = boost::filesystem::path(".");
        return (tmp / "neotko_logs").string();
    }();
    return dir;
}

std::string log_path(Channel c)
{
    std::string n = channel_name(c);
    for (char& ch : n)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return (boost::filesystem::path(log_dir()) / (n + ".log")).string();
}

bool is_enabled(Channel c)
{
    init_state();
    return s_on[static_cast<int>(c)].load(std::memory_order_relaxed);
}

void set_enabled(Channel c, bool on)
{
    init_state();
    s_on[static_cast<int>(c)].store(on, std::memory_order_relaxed);
}

bool paused() { return s_paused.load(std::memory_order_relaxed); }

void set_paused(bool on) { s_paused.store(on, std::memory_order_relaxed); }

bool enabled(Channel c) { return !paused() && is_enabled(c); }

long long log_size(Channel c)
{
    boost::system::error_code ec;
    const auto sz = boost::filesystem::file_size(log_path(c), ec);
    return ec ? -1LL : static_cast<long long>(sz);
}

// Removed rather than truncated, so that only the channels that wrote something have a file.
void clear(Channel c)
{
    boost::system::error_code ec;
    boost::filesystem::remove(log_path(c), ec);
}

void clear_all()
{
    for (int i = 0; i < CH_COUNT; ++i)
        clear(static_cast<Channel>(i));
}

bool render_panels_enabled()
{
    static const bool on = any_env_set("ORCA_DEBUG_RENDER", nullptr);
    return on;
}

bool console_enabled()
{
    static const bool on = any_env_set("ORCA_DEBUG_ALL", "ORCA_DEBUG_RENDER");
    return on;
}

void write(Channel c, const std::string& msg)
{
    ensure_dir();
    static std::mutex s_mtx;
    std::lock_guard<std::mutex> lk(s_mtx);
    // Opened per call, so that clear() never leaves a writer on a removed file.
    std::ofstream f(log_path(c), std::ios::app);
    if (f.is_open())
        f << msg << "\n";
}

void write_session_banner(const std::string& tag)
{
    static std::atomic<int> s_slice_n{0};
    const int n = ++s_slice_n;

    const std::time_t now = std::time(nullptr);
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif
    char ts[16] = "??:??:??";
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_local);

    std::ostringstream oss;
    oss << "\n============= [" << ts << "] SLICE #" << n << "  " << tag << "  =============";
    const std::string banner = oss.str();
    for (int i = 0; i < CH_COUNT; ++i)
        if (enabled(static_cast<Channel>(i)))
            write(static_cast<Channel>(i), banner);
}

} // namespace NeoDebug

} // namespace Slic3r
