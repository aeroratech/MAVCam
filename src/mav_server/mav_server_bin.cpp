#include <csignal>
#include <fstream>
#include <iostream>

#include "base/file_operation.h"
#include "base/log.h"
#include "mav_server.h"
#include "version.h"

static auto constexpr default_rpc_port = 50051;
static std::string default_log_path = "/data/camera/";
static std::fstream *default_log_stream = nullptr;
static std::string default_store_prefix = "NDAA";

static void usage(const char *bin_name);
static void init_log();
static bool is_integer(const std::string &tested_integer);
void signal_handler(int signum);

mavcam::MavServer server;

int main(int argc, const char *argv[]) {
    std::ios::sync_with_stdio(true);

    int rpc_port = default_rpc_port;
    for (int i = 1; i < argc; i++) {
        const std::string current_arg = argv[i];

        if (current_arg == "-h" || current_arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (current_arg == "-v" || current_arg == "--version") {
            std::cout << "MAVCam server version : " << VERSION << std::endl;
            std::cout << "build time : " << BUILD_TIME << std::endl;
            return 0;
        } else if (current_arg == "-r") {
            if (argc <= i + 1) {
                usage(argv[0]);
                return 1;
            }
            const std::string rpc_port_string(argv[i + 1]);
            if (!is_integer(rpc_port_string)) {
                usage(argv[0]);
                return 1;
            }
            rpc_port = std::stoi(rpc_port_string);
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
        }
    }

    base::create_folder_if_not_exit(default_log_path);
    init_log();
    signal(SIGINT, signal_handler);
    base::LogDebug() << "Launch mav server";
    setenv("DEFAULT_STORE_PREFIX", default_store_prefix.c_str(), 1);
    base::LogInfo() << "Store prefix is " << default_store_prefix;

    if (!server.init(rpc_port)) {
        std::cout << "Init rpc server failed";
        return 1;
    }

    server.start_runloop();
    base::LogDebug() << "Quit mav server";
    if (default_log_stream != nullptr) {
        default_log_stream->close();
        delete default_log_stream;
        default_log_stream = nullptr;
    }
    return 0;
}

void usage(const char *bin_name) {
    std::cout << "Usage: " << bin_name << " [Options]" << '\n'
              << '\n'
              << "Options:" << '\n'
              << "\t-h | --help     : show this help" << '\n'
              << "\t-v | --version  : show version information " << '\n'
              << "\t-r              : set the rpc port,"
              << "(default is " << default_rpc_port << ")\n"
              << "\t--log_path     : store output log to file path, default is " << default_log_path
              << '\n'
              << "\t--store_prefix : store folder and file prefix, default is "
              << default_store_prefix << '\n';
}

static void init_log() {
    if (!default_log_path.empty()) {
        std::string full_path = default_log_path + "mav_server.log";
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
    server.stop_runloop();

    exit(signum);  // Exit the process
}