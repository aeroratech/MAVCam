#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mavcam {

struct TrackingObject {
    std::string name;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct TrackingFrame {
    int32_t stream_id = 0;
    int32_t frame_id = 0;
    int32_t object_count = 0;
    std::vector<TrackingObject> objects;
};

class TrackingServer {
public:
    using TrackingCallback = std::function<void(const TrackingFrame &)>;

    TrackingServer();
    ~TrackingServer();

    bool start(const std::string &address, int port);
    void stop();
    bool running() const;

    void set_callback(TrackingCallback callback);
    TrackingFrame latest_frame() const;

private:
    void run_loop();
    void handle_client(int client_fd);
    bool parse_line(const std::string &line, TrackingFrame &frame) const;

    std::string _address;
    int _port = 0;
    int _server_fd = -1;
    std::atomic<bool> _running{false};
    std::thread _thread;

    mutable std::mutex _mutex;
    TrackingFrame _latest_frame;
    TrackingCallback _callback;
};

}  // namespace mavcam
