#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "base/file_operation.h"
#include "base/log.h"
#include "mav_client.h"
#include "version.h"

static auto constexpr default_connection = "udp://192.168.251.2:14550";
static auto constexpr default_rpc_port = 50051;
static std::string default_ftp_path = "/usr/share/mav-cam/";
static std::string default_log_path = "/data/camera/";
static std::fstream *default_log_stream = nullptr;
static bool compatible_qgc = false;

static void usage(const char *bin_name);
static void init_log();
static bool is_integer(const std::string &tested_integer);
void signal_handler(int signum);

static mavcam::MavClient client;
int main(int argc, const char *argv[]) {
    std::ios::sync_with_stdio(true);

    base::create_folder_if_not_exit(default_log_path);
    init_log();
    signal(SIGINT, signal_handler);

    base::LogDebug() << "Launch mav client";

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
        } else if (current_arg == "--qgc") {
            compatible_qgc = true;
        } else {
            std::cout << "Invalid option : " << current_arg << std::endl;
            usage(argv[0]);
            return 1;
        }
    }

    if (!client.init(connection_url, use_local, rpc_port, default_ftp_path, compatible_qgc)) {
        std::cout << "Cannot init mav client " << connection_url << std::endl;
        return 1;
    }

    client.start_runloop();

    base::LogDebug() << "Quit mav client";
    if (default_log_stream != nullptr) {
        default_log_stream->close();
        delete default_log_stream;
        default_log_stream = nullptr;
    }
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
              << "\t--qgc          : work compatible with QGC(make mav_client work as Autopilot)"
              << '\n';
}

static void init_log() {
    if (!default_log_path.empty()) {
        std::string full_path = default_log_path + "mav_client.log";
        default_log_stream = new std::fstream(full_path, std::fstream::out | std::fstream::binary);
        if (!default_log_stream->is_open()) {
            base::LogError() << "Failed to open log file: " + full_path;
        }
        base::log::subscribe([](base::log::Level level, const std::string &message,
                                const std::string &file, int line) -> bool {
            std::stringstream ss;
            time_t rawtime;
            time(&rawtime);
            struct tm *timeinfo = localtime(&rawtime);
            char time_buffer[10]{};
            strftime(time_buffer, sizeof(time_buffer), "%I:%M:%S", timeinfo);
            ss << "[" << time_buffer;

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
            if (default_log_stream->good()) {
                default_log_stream->write(ss.str().c_str(), ss.str().size());
                default_log_stream->flush();
            }
            return false;
        });
    }
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