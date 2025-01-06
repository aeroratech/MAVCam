#pragma once

#include <atomic>
#include <string>

namespace mavsdk {
class CameraServer;
class ParamServer;
}  // namespace mavsdk

namespace mavcam {

class CameraClient;

class MavClient {
public:
    MavClient() {}
    ~MavClient() {}
public:
    bool init(std::string &connection_url, bool use_local, int32_t rpc_port,
              std::string &ftp_root_path, bool compatible_qgc, std::string &log_path);
    bool start_runloop();
    void stop_runloop();
private:
    void subscribe_camera_operation(mavsdk::CameraServer &camera_server,
                                    mavsdk::ParamServer &param_server);
    void subscribe_param_operation(mavsdk::ParamServer &param_server);
    void fill_param(mavsdk::ParamServer &param_server);
    void init_mavsdk_log(std::string &log_path);
private:
    std::atomic<bool> _running;
    std::string _connection_url;
    int32_t _rpc_port;
    CameraClient *_camera_client;
    std::string _ftp_root_path;
    bool _compatible_qgc;
};

}  // namespace mavcam
