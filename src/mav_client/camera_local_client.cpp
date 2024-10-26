#include "camera_local_client.h"

#include <chrono>
#include <thread>

#include "base/log.h"

namespace mavcam {

static std::string kCameraModeName = "CAM_MODE";

CameraLocalClient::CameraLocalClient() {
    _is_capture_in_progress = false;
    _image_count = 0;
    _is_recording_video = false;

    _total_storage_mib = 4 * 1024;
    _available_storage_mib = 3 * 1024;

    // TODO just demo for settings
    _settings[kCameraModeName] = "0";
    _settings["CAM_WBMODE"] = "4";
    _settings["CAM_EXPMODE"] = "0";
    _settings["CAM_EV"] = "1";
    _settings["CAM_ISO"] = "200";
    _settings["CAM_SHUTTERSPD"] = "0.01";
    _settings["CAM_VIDFMT"] = "2";
    _settings["CAM_VIDRES"] = "0";
    _settings["CAM_PHOTORATIO"] = "1";
}

CameraLocalClient::~CameraLocalClient() {}

mavsdk::CameraServer::Result CameraLocalClient::take_photo(int index) {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call take photo " << index;
    _is_capture_in_progress = true;
    auto result = mavsdk::CameraServer::Result::Success;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    _is_capture_in_progress = false;
    _image_count++;
    return result;
}

mavsdk::CameraServer::Result CameraLocalClient::start_video() {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call start video";
    _is_recording_video = true;
    _start_video_time = std::chrono::steady_clock::now();
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::stop_video() {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call stop video";
    _is_recording_video = false;
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::start_video_streaming(int stream_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call start video streaming";
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::stop_video_streaming(int stream_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call stop video streaming";
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::set_mode(mavsdk::CameraServer::Mode mode) {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call set mode " << mode;
    if (mode == mavsdk::CameraServer::Mode::Photo) {
        _settings[kCameraModeName] = "0";
    } else {
        _settings[kCameraModeName] = "1";
    }
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::format_storage(int storage_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    _available_storage_mib = _total_storage_mib.load();
    // clear image count
    _image_count = 0;
    base::LogDebug() << "locally call format storage " << storage_id;
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::reset_settings() {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call reset settings";
    // reset settings
    _settings[kCameraModeName] = "0";
    _settings["CAM_WBMODE"] = "0";
    _settings["CAM_EXPMODE"] = "0";
    _settings["CAM_EV"] = "0";
    _settings["CAM_ISO"] = "100";
    _settings["CAM_SHUTTERSPD"] = "0.01";
    _settings["CAM_VIDFMT"] = "1";
    _settings["CAM_VIDRES"] = "0";
    _settings["CAM_PHOTORATIO"] = "1";

    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::set_timestamp(int64_t time_unix_msec) {
    base::LogDebug() << "local call set timestamp " << time_unix_msec;
    return mavsdk::CameraServer::Result::Denied;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_information(
    mavsdk::CameraServer::Information &information) {
    information.vendor_name = "Aeroratech";
    information.model_name = "D64TR.xml";
    information.firmware_version = "0.0.1";
    information.focal_length_mm = 3.0;
    information.horizontal_sensor_size_mm = 3.68;
    information.vertical_sensor_size_mm = 2.76;
    information.horizontal_resolution_px = 3280;
    information.vertical_resolution_px = 2464;
    information.lens_id = 0;
    information.definition_file_version = 1;
    information.definition_file_uri = "mftp://definition/D64TR.xml";
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::CaptureImage);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::CaptureVideo);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::HasModes);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::HasVideoStream);
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_video_stream_info(
    std::vector<mavsdk::CameraServer::VideoStreamInfo> &video_stream_infos) {
    video_stream_infos.clear();

    mavsdk::CameraServer::VideoStreamInfo normal_video_stream;
    normal_video_stream.stream_id = 1;

    normal_video_stream.settings.frame_rate_hz = 60.0;
    normal_video_stream.settings.horizontal_resolution_pix = 1920;
    normal_video_stream.settings.vertical_resolution_pix = 1080;
    normal_video_stream.settings.bit_rate_b_s = 4 * 1024 * 1024;
    normal_video_stream.settings.rotation_deg = 0;
    normal_video_stream.settings.uri = "udp://192.168.10.64:5600";
    normal_video_stream.settings.horizontal_fov_deg = 0;
    normal_video_stream.status =
        mavsdk::CameraServer::VideoStreamInfo::VideoStreamStatus::InProgress;
    normal_video_stream.spectrum =
        mavsdk::CameraServer::VideoStreamInfo::VideoStreamSpectrum::VisibleLight;

    video_stream_infos.emplace_back(normal_video_stream);

    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_storage_information(
    mavsdk::CameraServer::StorageInformation &storage_information) {
    storage_information.total_storage_mib = _total_storage_mib;
    storage_information.used_storage_mib = _total_storage_mib - _available_storage_mib;
    storage_information.available_storage_mib = _available_storage_mib;
    storage_information.storage_status =
        mavsdk::CameraServer::StorageInformation::StorageStatus::Formatted;
    storage_information.storage_type =
        mavsdk::CameraServer::StorageInformation::StorageType::Microsd;
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_capture_status(
    mavsdk::CameraServer::CaptureStatus &capture_status) {
    // not need lock guard
    capture_status.available_capacity_mib = _available_storage_mib;
    capture_status.image_count = _image_count;
    capture_status.image_status =
        _is_capture_in_progress
            ? mavsdk::CameraServer::CaptureStatus::ImageStatus::CaptureInProgress
            : mavsdk::CameraServer::CaptureStatus::ImageStatus::Idle;
    capture_status.video_status =
        _is_recording_video ? mavsdk::CameraServer::CaptureStatus::VideoStatus::CaptureInProgress
                            : mavsdk::CameraServer::CaptureStatus::VideoStatus::Idle;
    if (_is_recording_video) {
        auto current_time = std::chrono::steady_clock::now();
        capture_status.recording_time_s =
            std::chrono::duration_cast<std::chrono::seconds>(current_time - _start_video_time)
                .count();
    } else {
        capture_status.recording_time_s = 0;
    }
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_settings(
    mavsdk::CameraServer::Settings &settings) {
    base::LogDebug() << "locally call fill settings ";
    if (_settings[kCameraModeName] == "0") {
        settings.mode = mavsdk::CameraServer::Mode::Photo;
    } else {
        settings.mode = mavsdk::CameraServer::Mode::Video;
    }
    settings.zoom_level = 0;
    settings.focus_level = 0;
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::retrieve_current_settings(
    std::vector<mavsdk::Camera::Setting> &settings) {
    settings.clear();
    for (auto &it : _settings) {
        settings.emplace_back(build_setting(it.first, it.second));
    }

    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::set_setting(mavsdk::Camera::Setting setting) {
    base::LogDebug() << "change " << setting.setting_id << " to " << setting.option.option_id;
    if (_settings.count(setting.setting_id) == 0) {
        base::LogError() << "Unsupport setting " << setting.setting_id;
        return mavsdk::CameraServer::Result::WrongArgument;
    }
    _settings[setting.setting_id] = setting.option.option_id;
    return mavsdk::CameraServer::Result::Success;
}

std::pair<mavsdk::CameraServer::Result, mavsdk::Camera::Setting> CameraLocalClient::get_setting(
    mavsdk::Camera::Setting setting) const {
    if (_settings.count(setting.setting_id) == 0) {
        return {mavsdk::CameraServer::Result::WrongArgument, setting};
    }
    setting.option.option_id = _settings[setting.setting_id];
    base::LogDebug() << "get " << setting.setting_id << " return " << setting.option.option_id;
    return {mavsdk::CameraServer::Result::Success, setting};
}

mavsdk::Camera::Setting CameraLocalClient::build_setting(std::string name, std::string value) {
    mavsdk::Camera::Setting setting;
    setting.setting_id = name;
    setting.option.option_id = value;
    return setting;
}

}  // namespace mavcam
