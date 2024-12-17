#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "boson-sdk-interface.h"
#include "camera_client.h"
#include "camera_param/camera_param.h"
#include "mav_camera.h"

namespace mavcam {

class CameraLocalClient : public CameraClient {
public:
    CameraLocalClient();
    virtual ~CameraLocalClient();
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
    mavsdk::CameraServer::Result set_setting(mavsdk::Camera::Setting setting) override;
    std::pair<mavsdk::CameraServer::Result, mavsdk::Camera::Setting> get_setting(
        mavsdk::Camera::Setting setting) const override;
public:
    bool init();
private:
    /**
     * @brief deinit instance
     */
    void deinit();
    /**
     * @brief build setting with name and value
     */
    mavsdk::Camera::Setting build_setting(std::string name, std::string value);
    /**
     * @brief init camera display mode
     * @details prefer to use store display mode
     * @return current camera display mode string value
     */
    std::string init_camera_display_mode();
    /**
     * @brief set camera display mode
     */
    bool set_camera_display_mode(std::string mode);
    /**
     * @brief init camera whitebalance mode
     * @details prefer to use store whitebalance mode
     * @return current white balance string value
    */
    std::string init_whitebalance_mode();
    /**
     * @brief set whitebalance mode
    */
    bool set_whitebalance_mode(std::string mode);
    /**
     * @brief init exposure mode
     * @details prefer to use store exposure mode
     * @return current exposure mode
     */
    std::string init_exposure_mode();
    /**
     * @brief init camera exposure value
     * @details prefer to use store exposure value
     * @return current exposure value
     */
    std::string init_exposure_value();
    /**
     * @brief set exposure value
     */
    bool set_exposure_value(std::string exposure_value);
    /**
     * @brief get camera iso value
     */
    std::string get_iso_value();
    /**
     * @brief get shutter speed value
     */
    std::string get_shutter_speed_value();
    /**
     * @brief get video resoltuion
     */
    std::string get_video_resolution();
    /**
     * @brief set video resoltuion
     */
    bool set_video_resolution(std::string value);
    /**
     * @brief init camera metering mode
     * @details prefer to use store metering mode
     * @return current metering mode
     */
    std::string init_metering_mode();
    /**
     * @brief set metering mode
     */
    bool set_metering_mode(std::string value);
    /**
     * @brief init ir camera
     */
    bool init_ir_camera();
    /**
     * @breif free ir camera
     */
    void free_ir_camera();
    /**
     * @brief set ir camera palette
     */
    bool set_ir_palette(std::string color_mode);
    /**
     * @brief execute ir camera FFC
     */
    bool set_ir_FFC(std::string ignore);
    /**
     * @brief convert mav_camera::Result to mavsdk::CameraServer::Result
     */
    mavsdk::CameraServer::Result convert_camera_result_to_mav_server_result(
        mav_camera::Result input_result);
private:
    mutable mavsdk::CameraServer::Mode _current_mode{mavsdk::CameraServer::Mode::Unknown};
    int32_t _framerate;
    std::atomic<int> _image_count;
    std::atomic<bool> _is_recording_video;
    std::chrono::steady_clock::time_point _start_video_time;
    mutable std::unordered_map<std::string, std::string> _settings;
private:
    std::mutex _mutex{};
    mutable std::mutex _storage_information_mutex;
    mutable mav_camera::StorageInformation _current_storage_information;
private:
    void *_plugin_handle{NULL};
    mav_camera::MavCamera *_mav_camera{nullptr};
private:
    void *_ir_camera_handle{NULL};
    struct boson_extension_api *_ir_camera{nullptr};
private:
    CameraParam _camera_param;
};

}  // namespace mavcam
