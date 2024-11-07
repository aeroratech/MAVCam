#pragma once

#include <grpc++/grpc++.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "camera/camera.grpc.pb.h"
#include "camera/camera.pb.h"
#include "camera_client.h"

namespace mavcam {

class CameraRpcClient : public CameraClient {
public:
    CameraRpcClient();
    virtual ~CameraRpcClient();
public:  // operation
    virtual mavsdk::CameraServer::Result take_photo(int index) override;
    virtual mavsdk::CameraServer::Result start_video() override;
    virtual mavsdk::CameraServer::Result stop_video() override;
    virtual mavsdk::CameraServer::Result start_video_streaming(int stream_id) override;
    virtual mavsdk::CameraServer::Result stop_video_streaming(int stream_id) override;
    virtual mavsdk::CameraServer::Result set_mode(mavsdk::CameraServer::Mode mode) override;
    virtual mavsdk::CameraServer::Result format_storage(int storage_id) override;
    virtual mavsdk::CameraServer::Result reset_settings() override;
    virtual mavsdk::CameraServer::Result set_timestamp(int64_t time_unix_msec) override;
    virtual mavsdk::CameraServer::Result set_zoom_range(float range) override;
public:  // subscribe
    virtual mavsdk::CameraServer::Result fill_information(
        mavsdk::CameraServer::Information &information) override;
    virtual mavsdk::CameraServer::Result fill_video_stream_info(
        std::vector<mavsdk::CameraServer::VideoStreamInfo> &video_stream_infos) override;
    virtual mavsdk::CameraServer::Result fill_storage_information(
        mavsdk::CameraServer::StorageInformation &storage_information) override;
    virtual mavsdk::CameraServer::Result fill_capture_status(
        mavsdk::CameraServer::CaptureStatus &capture_status) override;
    virtual mavsdk::CameraServer::Result fill_settings(
        mavsdk::CameraServer::Settings &settings) override;
public:  // settings
    virtual mavsdk::CameraServer::Result retrieve_current_settings(
        std::vector<mavsdk::Camera::Setting> &settings) override;
    virtual mavsdk::CameraServer::Result set_setting(mavsdk::Camera::Setting setting) override;
    virtual std::pair<mavsdk::CameraServer::Result, mavsdk::Camera::Setting> get_setting(
        mavsdk::Camera::Setting setting) const override;
public:
    bool init(int rpc_port);
private:
    void stop();
    static void work_thread(CameraRpcClient *self);
private:
    std::atomic<bool> _is_capture_in_progress;
    std::atomic<int> _image_count;
    std::chrono::steady_clock::time_point _start_video_time;
private:
    std::atomic<bool> _init_information{false};
    mavsdk::CameraServer::Information _information;
private:
    std::atomic<bool> _init_video_stream_info{false};
    std::vector<mavsdk::CameraServer::VideoStreamInfo> _video_stream_infos;
private:
    mavsdk::CameraServer::StorageInformation _storage_information;
    mavsdk::CameraServer::CaptureStatus _capture_status;
private:
    mavsdk::CameraServer::Mode _current_mode;
private:
    std::unique_ptr<grpc::ClientReader<mavcam::rpc::camera::CurrentSettingsResponse>>
        _current_settings_reader;
private:
    std::shared_ptr<grpc::Channel> _channel;
    std::unique_ptr<mavcam::rpc::camera::CameraService::Stub> _stub;
private:  // backend work thread
    std::thread *_work_thread{nullptr};
    std::atomic<bool> _should_exit{false};
    mutable std::mutex _mutex{};
};

}  // namespace mavcam
