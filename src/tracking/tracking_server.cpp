#include "tracking_server.h"

#include <arpa/inet.h>
#include <json/json.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
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
        return false;
    }

    _running = true;
    _thread = std::thread(&TrackingServer::run_loop, this);
    return true;
}

void TrackingServer::stop() {
    if (!_running.exchange(false)) {
        return;
    }

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
        _running = false;
        return;
    }

    base::LogInfo() << "Tracking server listening on " << _address << ":" << _port;

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

        base::LogInfo() << "Tracking client connected from " << client_endpoint;
        handle_client(client_fd);
        base::LogInfo() << "Tracking client disconnected from " << client_endpoint;
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
                continue;
            }

            TrackingCallback callback;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _latest_frame = frame;
                callback = _callback;
            }

            if (callback) {
                callback(frame);
            }
        }
    }

    close(client_fd);
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
