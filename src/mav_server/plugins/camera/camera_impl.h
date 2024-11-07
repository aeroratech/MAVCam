#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "boson-sdk-interface.h"
#include "mav_camera.h"
#include "plugins/camera/camera.h"

namespace mavcam {

class CameraImpl final {
public:
    explicit CameraImpl();
    ~CameraImpl();

    /**
     * @brief Prepare the camera plugin (e.g. download the camera definition, etc).
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result prepare();

    /**
     * @brief Take one photo.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result take_photo();

    /**
     * @brief Start photo timelapse with a given interval.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result start_photo_interval(float interval_s);

    /**
     * @brief Stop a running photo timelapse.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result stop_photo_interval();

    /**
     * @brief Start a video recording.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result start_video();

    /**
     * @brief Stop a running video recording.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result stop_video();

    /**
     * @brief Start video streaming.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result start_video_streaming(int32_t stream_id);

    /**
     * @brief Stop current video streaming.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result stop_video_streaming(int32_t stream_id);

    /**
     * @brief Set camera mode.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result set_mode(Camera::Mode mode);

    /**
     * @brief List photos available on the camera.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    std::pair<Camera::Result, std::vector<Camera::CaptureInfo>> list_photos(
        Camera::PhotosRange photos_range);

    /**
     * @brief Subscribe to camera mode updates.
     */
    void mode_async(const Camera::ModeCallback &callback);

    /**
     * @brief Poll for 'Mode' (blocking).
     *
     * @return One Mode update.
     */
    Camera::Mode mode() const;

    /**
     * @brief Subscribe to camera information updates.
     */
    void information_async(const Camera::InformationCallback &callback);

    /**
     * @brief Poll for 'Information' (blocking).
     *
     * @return One Information update.
     */
    Camera::Information information() const;

    /**
     * @brief Subscribe to video stream info updates.
     */
    void video_stream_info_async(const Camera::VideoStreamInfoCallback &callback);

    /**
     * @brief Poll for 'std::vector<VideoStreamInfo>' (blocking).
     *
     * @return One std::vector<VideoStreamInfo> update.
     */
    std::vector<Camera::VideoStreamInfo> video_stream_info() const;

    /**
     * @brief Subscribe to capture info updates.
     */
    void capture_info_async(const Camera::CaptureInfoCallback &callback);

    /**
     * @brief Poll for 'CaptureInfo' (blocking).
     *
     * @return One CaptureInfo update.
     */
    Camera::CaptureInfo capture_info() const;

    /**
     * @brief Subscribe to camera status updates.
     */
    void status_async(const Camera::StatusCallback &callback);

    /**
     * @brief Poll for 'Status' (blocking).
     *
     * @return One Status update.
     */
    Camera::Status status() const;

    /**
     * @brief Get the list of current camera settings.
     */
    void current_settings_async(const Camera::CurrentSettingsCallback &callback);

    /**
     * @brief Poll for 'std::vector<Setting>' (blocking).
     *
     * @return One std::vector<Setting> update.
     */
    std::vector<Camera::Setting> current_settings() const;

    /**
     * @brief Get the list of settings that can be changed.
     */
    void possible_setting_options_async(const Camera::PossibleSettingOptionsCallback &callback);

    /**
     * @brief Poll for 'std::vector<SettingOptions>' (blocking).
     *
     * @return One std::vector<SettingOptions> update.
     */
    std::vector<Camera::SettingOptions> possible_setting_options() const;

    /**
     * @brief Set a setting to some value.
     *
     * Only setting_id of setting and option_id of option needs to be set.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result set_setting(Camera::Setting setting);

    /**
     * @brief Get a setting.
     *
     * Only setting_id of setting needs to be set.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    std::pair<Camera::Result, Camera::Setting> get_setting(Camera::Setting setting);

    /**
     * @brief Format storage (e.g. SD card) in camera.
     *
     * This will delete all content of the camera storage!
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result format_storage(int32_t storage_id);

    /**
     * @brief Select current camera .
     *
     * Bind the plugin instance to a specific camera_id
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result select_camera(int32_t camera_id);

    /**
     * @brief Reset all settings in camera.
     *
     * This will reset all camera settings to default value
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result reset_settings();

    /**
     * @brief Set camera timestamp.
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result set_timestamp(int64_t timestamp);

    /**
     * @brief set zoom to value as proportion of full camera range (percentage between 0.0 and 100.0).
     *
     * This function is blocking.
     *
     * @return Result of request.
     */
    Camera::Result set_zoom_range(float range) const;
private:
    /**
     * @brief close camera and release resource
     */
    void close_camera();
    /**
     * @brief build setting with name and value
     */
    mavcam::Camera::Setting build_setting(std::string name, std::string value);
    /**
     * @brief set camera display mode
     */
    bool set_camera_display_mode(std::string mode);
    /**
     * @brief get current camera display mode
     */
    std::string get_camera_display_mode();
    /**
     * @brief set whitebalance mode
    */
    bool set_whitebalance_mode(std::string mode);
    /**
     * @brief get camera whitebalance mode
    */
    std::string get_whitebalance_mode();
    /**
     * @brief get camera exposure value
     */
    std::string get_ev_value();
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
     * @brief convert mav_camera::Result to mavcam::Camera::Result
     */
    Camera::Result convert_camera_result_to_mav_result(mav_camera::Result input_result);
    /**
     * @brief stop video async
     */
    void stop_video_async();
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
private:
    Camera::ModeCallback _camera_mode_callback;
    Camera::CaptureInfoCallback _capture_info_callback;
    mutable Camera::Status _status;
    Camera::StatusCallback _status_callback;
private:
    mutable Camera::Mode _current_mode{Camera::Mode::Unknown};
    mutable std::chrono::steady_clock::time_point _start_video_time;
    mutable std::vector<Camera::Setting> _settings;
    mutable std::mutex _storage_information_mutex;
    mutable mav_camera::StorageInformation _current_storage_information;
private:
    void *_plugin_handle{NULL};
    mav_camera::MavCamera *_mav_camera{nullptr};
    int32_t _framerate;
private:
    void *_ir_camera_handle{NULL};
    struct boson_extension_api *_ir_camera{nullptr};
};

}  // namespace mavcam