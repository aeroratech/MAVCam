#include <mavsdk/log_callback.h>

#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>

#include "base/file_operation.h"
#include "base/log.h"
#include "mav_client.h"
#include "version.h"

static auto constexpr default_connection = "udp://192.168.251.2:14550";
static auto constexpr default_rpc_port = 50051;
static std::string default_ftp_path = "/usr/share/mav-cam/";
static std::string default_log_path = "/data/camera/";
static bool work_as_autopilot = false;
static std::string default_store_prefix = "NDAA";

static void usage(const char *bin_name);
static void init_log();
static bool is_integer(const std::string &tested_integer);
void signal_handler(int signum);

static mavcam::MavClient client;
int main(int argc, const char *argv[]) {
    std::ios::sync_with_stdio(true);

    std::string connection_url = default_connection;
    int rpc_port = default_rpc_port;
    bool use_local = false;

    for (int i = 1; i < argc; i++) {
        const std::string current_arg = argv[i];

        if (current_arg == "-h" || current_arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (current_arg == "-v" || current_arg == "--version") {
            std::cout << "Mavlink client version : " << VERSION << std::endl;
            std::cout << "build time : " << BUILD_TIME << std::endl;
            return 0;
        } else if (current_arg == "-u") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            connection_url = std::string(argv[i + 1]);
            i++;
        } else if (current_arg == "-l") {
            use_local = true;
        } else if (current_arg == "-r") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            const std::string rpc_port_string(argv[i + 1]);
            i++;
            if (!is_integer(rpc_port_string)) {
                usage(argv[0]);
                return 1;
            }
            rpc_port = std::stoi(rpc_port_string);
        } else if (current_arg == "-f" || current_arg == "--ftp_path") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            default_ftp_path = std::string(argv[i + 1]);
            i++;
        } else if (current_arg == "--log_path") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            default_log_path = std::string(argv[i + 1]);
            i++;
        } else if (current_arg == "--store_prefix") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            default_store_prefix = std::string(argv[i + 1]);
            i++;
        } else if (current_arg == "--camera_mode") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            auto camera_mode = std::string(argv[i + 1]);
            i++;
            if (camera_mode != "0" && camera_mode != "1") {
                usage(argv[0]);
                return 1;
            }
            setenv("MAVCAM_INIT_CAMERA_MODE", camera_mode.c_str(), 1);
        } else if (current_arg == "--snapshot_resolution") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            auto snapshot_resolution = std::string(argv[i + 1]);
            std::regex resolutionRegex(R"(^\d+x\d+$)");
            if (!std::regex_match(snapshot_resolution, resolutionRegex)) {
                std::cout << "Invalid snapshot resolution " << snapshot_resolution;
            }
            i++;
            setenv("MAVCAM_INIT_SNAPSHOT_RES", snapshot_resolution.c_str(), 1);
        } else if (current_arg == "--autopilot") {
            work_as_autopilot = true;
        } else {
            std::cout << "Invalid option : " << current_arg << std::endl;
            usage(argv[0]);
            return 1;
        }
    }

    base::create_folder_if_not_exit(default_log_path);
    init_log();
    signal(SIGINT, signal_handler);

    base::LogInfo() << "Launch mav client";
    base::LogInfo() << "MavCam version is " << VERSION;
    base::LogInfo() << "MavCam build time is " << BUILD_TIME;
    if (work_as_autopilot) {
        base::LogInfo() << "Work as autopilot";
    }

    setenv("MAVCAM_DEFAULT_STORE_PREFIX", default_store_prefix.c_str(), 1);
    base::LogInfo() << "Store prefix is " << default_store_prefix;
    const char *init_camera_mode = getenv("MAVCAM_INIT_CAMERA_MODE");
    if (init_camera_mode != NULL) {
        base::LogInfo() << "Init camera mode is " << init_camera_mode;
    }
    const char *init_snapshot_resolution = getenv("MAVCAM_INIT_SNAPSHOT_RES");
    if (init_snapshot_resolution != NULL) {
        base::LogInfo() << "Init camera snapshot resolution is " << init_snapshot_resolution;
    }

    if (!client.init(connection_url, use_local, rpc_port, default_ftp_path, work_as_autopilot)) {
        std::cout << "Cannot init mav client " << connection_url << std::endl;
        return 1;
    }

    client.start_runloop();

    base::LogDebug() << "Quit mav client";
    return 0;
}

void usage(const char *bin_name) {
    std::cout << "Usage: " << bin_name << " [Options] [Connection URL]" << '\n'
              << '\n'
              << "Connection URL format should be:" << '\n'
              << "\tSerial: serial:///path/to/serial/dev[:baudrate]" << '\n'
              << "\tUDP:    udp://[bind_host][:bind_port]" << '\n'
              << "\tTCP:    tcp://[server_host][:server_port]" << '\n'
              << '\n'
              << "Options:" << '\n'
              << "\t-h | --help    : show this help" << '\n'
              << "\t-v | --version : show version information " << '\n'
              << "\t-u             : set the url on which the mavsdk server is running,"
              << " (default is " << default_connection << ")" << '\n'
              << "\t-l             : use local client" << '\n'
              << "\t-r             : set the remote port,"
              << " (default is " << default_rpc_port << ")\n"
              << "\t-f | --ftp_path: set the ftp root path,"
              << " (default is " << default_ftp_path << ")" << '\n'
              << "\t--log_path     : store output log to file path, default is " << default_log_path
              << '\n'
              << "\t--store_prefix : store folder and file prefix, default is "
              << default_store_prefix << '\n'
              << "\t--camera_mode  : init camera mode, 0 for photo mode 1 for video mode" << '\n'
              << "\t--snapshot_resolution : init snapshot resoltuion" << '\n'
              << "\t--autopilot           : make mav_client work as Autopilot" << '\n';
}

static void init_log() {
    if (default_log_path.empty()) {
        return;
    }
    std::string full_path = default_log_path + "mav_client.log";
    auto log_stream =
        std::make_shared<std::fstream>(full_path, std::fstream::out | std::fstream::binary);
    if (!log_stream->is_open()) {
        base::LogError() << "Failed to open log file: " + full_path;
        return;
    }

    static std::mutex log_mutex;
    base::log::subscribe([log_stream](base::log::Level level, const std::string &message,
                                      const std::string &file, int line) -> bool {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::stringstream ss;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        time_t rawtime = tv.tv_sec;
        struct tm *timeinfo = localtime(&rawtime);

        char time_buffer[10]{};
        strftime(time_buffer, sizeof(time_buffer), "%I:%M:%S", timeinfo);

        ss << "[" << time_buffer << "." << std::setfill('0') << std::setw(3) << (tv.tv_usec / 1000);

        switch (level) {
            case base::log::Level::Debug:
                ss << "|Debug] ";
                break;
            case base::log::Level::Info:
                ss << "|Info ] ";
                break;
            case base::log::Level::Warn:
                ss << "|Warn ] ";
                break;
            case base::log::Level::Err:
                ss << "|Error] ";
                break;
        }
        ss << " " << message << "\n";
        if (log_stream->good()) {
            log_stream->write(ss.str().c_str(), ss.str().size());
            log_stream->flush();
        }
        return false;
    });

    mavsdk::log::subscribe([log_stream](mavsdk::log::Level level, const std::string &message,
                                        const std::string &file, int line) -> bool {
        // ignore debug log in mavsdk
        if (level == mavsdk::log::Level::Debug) {
            return false;
        }
        std::lock_guard<std::mutex> lock(log_mutex);
        std::stringstream ss;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        time_t rawtime = tv.tv_sec;
        struct tm *timeinfo = localtime(&rawtime);
        char time_buffer[10]{};
        strftime(time_buffer, sizeof(time_buffer), "%I:%M:%S", timeinfo);
        ss << "[MAVSDK|" << time_buffer << "." << std::setfill('0') << std::setw(3)
           << (tv.tv_usec / 1000);

        switch (level) {
            case mavsdk::log::Level::Debug:
                ss << "|Debug] ";
                break;
            case mavsdk::log::Level::Info:
                ss << "|Info ] ";
                break;
            case mavsdk::log::Level::Warn:
                ss << "|Warn ] ";
                break;
            case mavsdk::log::Level::Err:
                ss << "|Error] ";
                break;
        }
        ss << " " << message << "\n";
        if (log_stream->good()) {
            log_stream->write(ss.str().c_str(), ss.str().size());
            log_stream->flush();
        }
        return false;
    });
}

bool is_integer(const std::string &tested_integer) {
    for (const auto &digit : tested_integer) {
        if (!std::isdigit(digit)) {
            return false;
        }
    }
    return true;
}

void signal_handler(int signum) {
    base::LogDebug() << "Interrupt signal (" << signum << ") received.";
    client.stop_runloop();
}