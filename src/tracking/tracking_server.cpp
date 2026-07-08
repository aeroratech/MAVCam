#include "tracking_server.h"

#include <arpa/inet.h>
#include <json/json.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <array>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <memory>
#include <utility>

#include "base/log.h"

namespace mavcam {

namespace {

void close_fd(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

int read_dimension(const Json::Value &position, const char *short_key, const char *long_key) {
    if (position.isMember(short_key)) {
        return position[short_key].asInt();
    }
    return position[long_key].asInt();
}

constexpr size_t kControlPacketSize = 23;
constexpr const char *kTrackingLogDir = "/data/camera";
constexpr const char *kTrackingLogPath = "/data/camera/tracking.log";
constexpr off_t kTrackingLogMaxSize = 10 * 1024 * 1024;

std::mutex &tracking_log_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string tracking_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    struct tm local_time;
    localtime_r(&tv.tv_sec, &local_time);

    char time_buffer[32];
    snprintf(time_buffer, sizeof(time_buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
             local_time.tm_hour, local_time.tm_min, local_time.tm_sec, tv.tv_usec / 1000);
    return std::string(time_buffer);
}

void append_tracking_log(const std::string &message) {
    std::lock_guard<std::mutex> lock(tracking_log_mutex());
    mkdir(kTrackingLogDir, 0755);

    static bool checked_log_size = false;
    if (!checked_log_size) {
        struct stat log_stat;
        if (stat(kTrackingLogPath, &log_stat) == 0 && log_stat.st_size > kTrackingLogMaxSize) {
            std::ofstream truncate_file(kTrackingLogPath,
                                        std::ofstream::out | std::ofstream::trunc);
        }
        checked_log_size = true;
    }

    std::ofstream log_file(kTrackingLogPath, std::ofstream::out | std::ofstream::app);
    if (!log_file.is_open()) {
        return;
    }

    log_file << tracking_timestamp() << " " << message << '\n';
}

}  // namespace

TrackingServer::TrackingServer() = default;

TrackingServer::~TrackingServer() {
    stop();
}

bool TrackingServer::start(const std::string &address, int port) {
    if (_running.load(std::memory_order_relaxed)) {
        return true;
    }

    _address = address.empty() ? "0.0.0.0" : address;
    _port = port;
    if (_port <= 0) {
        base::LogError() << "Invalid tracking server port: " << _port;
        append_tracking_log("invalid port=" + std::to_string(_port));
        return false;
    }

    _running = true;
    append_tracking_log("start requested address=" + _address + " port=" + std::to_string(_port));
    _thread = std::thread(&TrackingServer::run_loop, this);
    return true;
}

void TrackingServer::stop() {
    if (!_running.exchange(false)) {
        return;
    }

    append_tracking_log("stop requested");
    if (_server_fd >= 0) {
        shutdown(_server_fd, SHUT_RDWR);
    }
    close_fd(_server_fd);
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool TrackingServer::running() const {
    return _running.load(std::memory_order_relaxed);
}

void TrackingServer::set_callback(TrackingCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _callback = std::move(callback);
}

TrackingFrame TrackingServer::latest_frame() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _latest_frame;
}

bool TrackingServer::enable_detection() {
    return send_control_command(true, false, 0, 0);
}

bool TrackingServer::enable_tracking_point(uint16_t x, uint16_t y) {
    return send_control_command(true, true, x, y);
}

bool TrackingServer::disable_tracking() {
    return send_control_command(true, false, 0, 0);
}

void TrackingServer::run_loop() {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    std::string port_string = std::to_string(_port);
    struct addrinfo *result = nullptr;
    int rc = getaddrinfo(_address.c_str(), port_string.c_str(), &hints, &result);
    if (rc != 0) {
        base::LogError() << "Tracking server getaddrinfo failed: " << gai_strerror(rc);
        append_tracking_log(std::string("getaddrinfo failed error=") + gai_strerror(rc));
        _running = false;
        return;
    }

    for (struct addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
        _server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (_server_fd < 0) {
            continue;
        }

        int reuse = 1;
        setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (bind(_server_fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(_server_fd, 1) == 0) {
            break;
        }

        close_fd(_server_fd);
    }
    freeaddrinfo(result);

    if (_server_fd < 0) {
        base::LogError() << "Failed to start tracking server on " << _address << ":" << _port
                         << " : " << strerror(errno);
        append_tracking_log("listen failed address=" + _address + " port=" + std::to_string(_port) +
                            " error=" + strerror(errno));
        _running = false;
        return;
    }

    base::LogInfo() << "Tracking server listening on " << _address << ":" << _port;
    append_tracking_log("listening address=" + _address + " port=" + std::to_string(_port));

    while (_running.load(std::memory_order_relaxed)) {
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd =
            accept(_server_fd, reinterpret_cast<struct sockaddr *>(&client_addr), &client_addr_len);
        if (client_fd < 0) {
            if (_running.load(std::memory_order_relaxed)) {
                base::LogWarn() << "Tracking server accept failed: " << strerror(errno);
            }
            continue;
        }

        char host[NI_MAXHOST] = {};
        char service[NI_MAXSERV] = {};
        int name_result =
            getnameinfo(reinterpret_cast<struct sockaddr *>(&client_addr), client_addr_len, host,
                        sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
        std::string client_endpoint =
            (name_result == 0) ? std::string(host) + ":" + service : "unknown";

        {
            std::lock_guard<std::mutex> lock(_client_mutex);
            _client_fd = client_fd;
            if (!_latest_control_packet.empty()) {
                ssize_t sent = send(_client_fd, _latest_control_packet.data(),
                                    _latest_control_packet.size(), MSG_NOSIGNAL);
                if (sent != static_cast<ssize_t>(_latest_control_packet.size())) {
                    base::LogWarn() << "Failed to send cached tracking control command";
                    append_tracking_log("send cached control failed");
                }
            }
        }

        base::LogInfo() << "Tracking client connected from " << client_endpoint;
        append_tracking_log("client connected endpoint=" + client_endpoint);
        handle_client(client_fd);
        {
            std::lock_guard<std::mutex> lock(_client_mutex);
            if (_client_fd == client_fd) {
                _client_fd = -1;
            }
        }
        base::LogInfo() << "Tracking client disconnected from " << client_endpoint;
        append_tracking_log("client disconnected endpoint=" + client_endpoint);
    }

    close_fd(_server_fd);
}

void TrackingServer::handle_client(int client_fd) {
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string pending;
    char buffer[4096];

    while (_running.load(std::memory_order_relaxed)) {
        ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (received <= 0) {
            break;
        }

        pending.append(buffer, static_cast<size_t>(received));
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);

            TrackingFrame frame;
            if (!parse_line(line, frame)) {
                base::LogWarn() << "Failed to parse tracking object payload";
                append_tracking_log("parse frame failed bytes=" + std::to_string(line.size()));
                continue;
            }

            TrackingCallback callback;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _latest_frame = frame;
                callback = _callback;
            }

            append_tracking_log("frame received objects=" + std::to_string(frame.object_count));

            if (callback) {
                callback(frame);
            }
        }
    }

    close(client_fd);
}

bool TrackingServer::send_control_command(bool enable_detection, bool enable_tracking, uint16_t x,
                                          uint16_t y) {
    std::array<uint8_t, kControlPacketSize> packet{};
    packet[16] = ++_control_packet_index;
    packet[17] = static_cast<uint8_t>(x & 0xff);
    packet[18] = static_cast<uint8_t>((x >> 8) & 0xff);
    packet[19] = static_cast<uint8_t>(y & 0xff);
    packet[20] = static_cast<uint8_t>((y >> 8) & 0xff);
    packet[21] = enable_detection ? 1 : 0;
    packet[22] = enable_tracking ? 1 : 0;

    std::lock_guard<std::mutex> lock(_client_mutex);
    _latest_control_packet.assign(packet.begin(), packet.end());

    if (_client_fd < 0) {
        base::LogInfo() << "Cached tracking control command detection=" << enable_detection
                        << " tracking=" << enable_tracking << " x=" << x << " y=" << y;
        append_tracking_log("control cached detection=" + std::to_string(enable_detection) +
                            " tracking=" + std::to_string(enable_tracking) +
                            " x=" + std::to_string(x) + " y=" + std::to_string(y));
        return true;
    }

    ssize_t sent = send(_client_fd, packet.data(), packet.size(), MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(packet.size())) {
        base::LogWarn() << "Failed to send tracking control command";
        append_tracking_log("control send failed detection=" + std::to_string(enable_detection) +
                            " tracking=" + std::to_string(enable_tracking) +
                            " x=" + std::to_string(x) + " y=" + std::to_string(y));
        return false;
    }

    base::LogInfo() << "Sent tracking control command detection=" << enable_detection
                    << " tracking=" << enable_tracking << " x=" << x << " y=" << y;
    append_tracking_log("control sent detection=" + std::to_string(enable_detection) +
                        " tracking=" + std::to_string(enable_tracking) +
                        " x=" + std::to_string(x) + " y=" + std::to_string(y));
    return true;
}

bool TrackingServer::parse_line(const std::string &line, TrackingFrame &frame) const {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(line.data(), line.data() + line.size(), &root, &errors)) {
        return false;
    }

    frame.stream_id = root["stream_id"].asInt();
    frame.frame_id = root["frame_id"].asInt();
    frame.object_count = root["object_count"].asInt();
    frame.objects.clear();

    const Json::Value objects = root["objects"];
    if (!objects.isArray()) {
        return frame.object_count == 0;
    }

    for (const Json::Value &object : objects) {
        const Json::Value position = object["position"];
        TrackingObject tracking_object;
        tracking_object.name = object["name"].asString();
        tracking_object.x = position["x"].asInt();
        tracking_object.y = position["y"].asInt();
        tracking_object.width = read_dimension(position, "w", "width");
        tracking_object.height = read_dimension(position, "h", "height");
        frame.objects.push_back(tracking_object);
    }

    frame.object_count = static_cast<int32_t>(frame.objects.size());
    return true;
}

}  // namespace mavcam
