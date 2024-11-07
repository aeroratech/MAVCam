#pragma once

#include <mavsdk/plugins/camera/camera.h>
#include <mavsdk/plugins/camera_server/camera_server.h>

#include <vector>

namespace mavcam {

class CameraClient {
public:
    virtual ~CameraClient() {}
protected:
    CameraClient() {}
public:  // operation
    virtual mavsdk::CameraServer::Result take_photo(int index) = 0;
    virtual mavsdk::CameraServer::Result start_video() = 0;
    virtual mavsdk::CameraServer::Result stop_video() = 0;
    virtual mavsdk::CameraServer::Result start_video_streaming(int stream_id) = 0;
    virtual mavsdk::CameraServer::Result stop_video_streaming(int stream_id) = 0;
    virtual mavsdk::CameraServer::Result set_mode(mavsdk::CameraServer::Mode mode) = 0;
    virtual mavsdk::CameraServer::Result format_storage(int storage_id) = 0;
    virtual mavsdk::CameraServer::Result reset_settings() = 0;
    virtual mavsdk::CameraServer::Result set_timestamp(int64_t time_unix_msec) = 0;
    virtual mavsdk::CameraServer::Result set_zoom_range(float range) = 0;
public:  // subscribe
    virtual mavsdk::CameraServer::Result fill_information(
        mavsdk::CameraServer::Information &information) = 0;
    virtual mavsdk::CameraServer::Result fill_video_stream_info(
        std::vector<mavsdk::CameraServer::VideoStreamInfo> &video_stream_infos) = 0;
    virtual mavsdk::CameraServer::Result fill_storage_information(
        mavsdk::CameraServer::StorageInformation &storage_information) = 0;
    virtual mavsdk::CameraServer::Result fill_capture_status(
        mavsdk::CameraServer::CaptureStatus &capture_status) = 0;
    virtual mavsdk::CameraServer::Result fill_settings(
        mavsdk::CameraServer::Settings &settings) = 0;
public:  // settings
    virtual mavsdk::CameraServer::Result retrieve_current_settings(
        std::vector<mavsdk::Camera::Setting> &settings) = 0;
    virtual mavsdk::CameraServer::Result set_setting(mavsdk::Camera::Setting setting) = 0;
    virtual std::pair<mavsdk::CameraServer::Result, mavsdk::Camera::Setting> get_setting(
        mavsdk::Camera::Setting setting) const = 0;
};

CameraClient *CreateLocalCameraClient();
CameraClient *CreateRpcCameraClient(int rpc_port);

}  // namespace mavcam
