#include "camera_local_client.h"

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <iomanip>  // for std::setprecision
#include <regex>
#include <thread>
#include <utility>

#include "base/log.h"
#include "led_control/led_control.h"

namespace mavcam {

const std::string kTakePhotoInterval = "CAM_TAKE_INTERVAL";

const std::string kCameraModeName = "CAM_MODE";
const std::string kCameraSensorModeName = "CAM_SENS_MODE";
const std::string kCameraDisplayModeName = "CAM_DIS_MODE";
const std::string kPhotoResolution = "CAM_PHOTO_RES";
const std::string kPhotoQuality = "CAM_PHOTO_QC";
const std::string kPhotoFormat = "CAM_PHOTO_FMT";
const std::string kVideoResolution = "CAM_VIDRES";
const std::string kVideoFormat = "CAM_VIDFMT";
const std::string kWhitebalanceModeName = "CAM_WBMODE";
const std::string kExposureMode = "CAM_EXPMODE";
const std::string kEVName = "CAM_EV";
const std::string kISOName = "CAM_ISO";
const std::string kShutterSpeedName = "CAM_SHUTTERSPD";
const std::string kMeteringModeName = "CAM_METER";
const std::string kSharpnessName = "CAM_SHARPNESS";
const std::string kAELockName = "CAM_AE_LOCK";

const std::string kIrCamPalette = "IRCAM_PALETTE";
const std::string kIrCamFFCMode = "IRCAM_FFCMODE";
const std::string kIrCamFFC = "IRCAM_FFC";

static const int32_t kSDCardMinAvaliableMB = 200;  ///< min sdcard avaiable MB

#define QCOM_CAMERA_LIBERAY "libqcom_camera.so"
#define IR_CAMERA_LIBRARY "libir_camera.so"
#define RENDER_BRIDGE_LIBRARY "librender_bridge.so"
#define STORAGE_MANAGER_LIBRARY "libstorage_manager.so"

static std::string kCameraBrand = []() {
    const char *env = std::getenv("CAM_BRAND");
    return env ? std::string(env) : "AERORA";
}();

static std::string kCameraModule = []() {
    const char *env = std::getenv("CAM_MODEL");
    return env ? std::string(env) : "AERORA";
}();

void RGBCaptureCallback(mav_camera::MAVFrame *frame, void *context) {
    if (context != NULL) {
        CameraLocalClient *client = (CameraLocalClient *)context;
        client->capture_callback(frame, NULL);
    }
}

void IRCaptureCallback(ir_camera::IRFrame *frame, void *context) {
    if (context != NULL) {
        CameraLocalClient *client = (CameraLocalClient *)context;
        client->capture_callback(NULL, frame);
    }
}

CameraLocalClient::CameraLocalClient(std::string rtsp_ip) : _rtsp_ip(std::move(rtsp_ip)) {
    _image_count = 0;
    _is_recording_video = false;
}

CameraLocalClient::~CameraLocalClient() {
    deinit();
}

mavsdk::CameraServer::Result CameraLocalClient::take_photo(int index) {
    base::LogDebug() << "locally call take photo " << index;
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    {  // when sdcard storage is less then avaliable space just return failed
        std::lock_guard<std::mutex> lock(_storage_information_mutex);
        if (_current_storage_information.available_storage_mib < kSDCardMinAvaliableMB) {
            return mavsdk::CameraServer::Result::Denied;
        }
    }
    std::string storage_path = _storage_manager->get_storage_path();
    if (storage_path.empty()) {
        return mavsdk::CameraServer::Result::Denied;
    }

    std::lock_guard<std::mutex> lock(_action_mutex);

    int file_index = _storage_manager->get_file_index();
    auto generate_new_storage_path = [&](bool ir_photo) -> std::string {
        std::ostringstream oss;
        oss << storage_path << "/" << kCameraBrand << std::setw(4) << std::setfill('0')
            << file_index << ".";
        if (ir_photo) {
            oss << "jpg";
        } else {  // TODO (thomas) : not support jpg+dng now
            if (_settings[kPhotoFormat] == "0") {
                oss << "jpg";
            } else {
                oss << "dng";
            }
        }
        return oss.str();
    };

    auto return_result = mavsdk::CameraServer::Result::Success;
    bool success = false;
    if (_sensor_mode == SensorMode::IR) {
        if (_ir_camera != nullptr) {
            std::string file_path = generate_new_storage_path(true);
            success = _ir_camera->take_photo(file_path);
            if (!success) {
                return_result = mavsdk::CameraServer::Result::Error;
            }
        } else {
            base::LogDebug() << "Take ir photo without ir camera";
        }
    } else if (_sensor_mode == SensorMode::Normal || _sensor_mode == SensorMode::Dual) {
        std::string file_path = generate_new_storage_path(false);
        auto result = _mav_camera->take_photo(file_path);
        return_result = convert_camera_result_to_mav_server_result(result);
        if (return_result != mavsdk::CameraServer::Result::Success) {
            base::LogInfo() << "Take rgb photo failed with result " << return_result;
        }
        success = (return_result == mavsdk::CameraServer::Result::Success);
        if (_sensor_mode == SensorMode::Dual) {
            if (_ir_camera != nullptr) {
                file_index++;
                std::string file_path = generate_new_storage_path(true);
                success = _ir_camera->take_photo(file_path);
            } else {
                base::LogDebug() << "Take dual photo without ir camera";
            }
        }
    }
    if (success) {
        _image_count++;
        switch_led_mode(LedMode::TakePhoto);
    }
    return return_result;
}

mavsdk::CameraServer::Result CameraLocalClient::start_video() {
    base::LogDebug() << "locally call start video";
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    {  // when sdcard storage is less then avaliable space just return failed
        std::lock_guard<std::mutex> lock(_storage_information_mutex);
        if (_current_storage_information.available_storage_mib < kSDCardMinAvaliableMB) {
            return mavsdk::CameraServer::Result::Denied;
        }
    }
    std::string storage_path = _storage_manager->get_storage_path();
    if (storage_path.empty()) {
        return mavsdk::CameraServer::Result::Denied;
    }

    int file_index = _storage_manager->get_file_index();
    std::lock_guard<std::mutex> lock(_action_mutex);
    auto generate_new_storage_path = [&]() -> std::string {
        std::ostringstream oss;
        oss << storage_path << "/" << kCameraBrand << std::setw(4) << std::setfill('0')
            << file_index << "."
            << "mp4";
        return oss.str();
    };

    auto return_result = mavsdk::CameraServer::Result::Success;
    bool success = false;
    if (_sensor_mode == SensorMode::IR && _ir_camera != nullptr) {
        std::string file_path = generate_new_storage_path();
        success = _ir_camera->start_video_recording(file_path);
        if (!success) {
            return_result = mavsdk::CameraServer::Result::Error;
        }
    } else if (_sensor_mode == SensorMode::Normal || _sensor_mode == SensorMode::Dual) {
        std::string file_path = generate_new_storage_path();
        auto result = _mav_camera->start_video(file_path);
        return_result = convert_camera_result_to_mav_server_result(result);
        if (return_result != mavsdk::CameraServer::Result::Success) {
            base::LogInfo() << "start video recording failed with result " << return_result;
        }
        success = (return_result == mavsdk::CameraServer::Result::Success);
        if (_sensor_mode == SensorMode::Dual && _ir_camera != nullptr) {
            file_index++;
            std::string file_path = generate_new_storage_path();
            success = _ir_camera->start_video_recording(file_path);
            if (!success) {
                return_result = mavsdk::CameraServer::Result::Error;
            }
        }
    }
    if (success) {
        _is_recording_video = true;
        _start_video_time = std::chrono::steady_clock::now();
        switch_led_mode(LedMode::Recording);
    }
    return return_result;
}

mavsdk::CameraServer::Result CameraLocalClient::stop_video() {
    base::LogDebug() << "locally call stop video";
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    std::lock_guard<std::mutex> lock(_action_mutex);
    if (!_is_recording_video) {
        base::LogWarn() << "call stop video without video is recording";
        return mavsdk::CameraServer::Result::Success;
    }

    bool success = false;
    auto mav_result = mavsdk::CameraServer::Result::Success;
    if (_sensor_mode == SensorMode::IR && _ir_camera != nullptr) {
        success = _ir_camera->stop_video_recording();
        if (!success) {
            mav_result = mavsdk::CameraServer::Result::Error;
        }
    } else if (_sensor_mode == SensorMode::Normal || _sensor_mode == SensorMode::Dual) {
        auto result = _mav_camera->stop_video();
        mav_result = convert_camera_result_to_mav_server_result(result);
        if (mav_result != mavsdk::CameraServer::Result::Success) {
            base::LogInfo() << "Stop video recording failed with result " << mav_result;
        }
        success = (mav_result == mavsdk::CameraServer::Result::Success);

        if (_sensor_mode == SensorMode::Dual && _ir_camera != nullptr) {
            success = _ir_camera->stop_video_recording();
        }
    }
    if (success) {
        _is_recording_video = false;
        auto current_time = std::chrono::steady_clock::now();
        auto recording_time_s =
            std::chrono::duration_cast<std::chrono::seconds>(current_time - _start_video_time)
                .count();
        base::LogInfo() << "Stop rgb video recording after " << recording_time_s << " s";
        switch_led_mode(LedMode::Normal);
    }
    return mav_result;
}

mavsdk::CameraServer::Result CameraLocalClient::start_video_streaming(int stream_id) {
    std::lock_guard<std::mutex> lock(_action_mutex);
    base::LogDebug() << "locally call start video streaming";
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::stop_video_streaming(int stream_id) {
    std::lock_guard<std::mutex> lock(_action_mutex);
    base::LogDebug() << "locally call stop video streaming";
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::set_mode(mavsdk::CameraServer::Mode mode) {
    base::LogDebug() << "locally call set mode " << mode;
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    std::lock_guard<std::mutex> lock(_action_mutex);
    mav_camera::Result result = mav_camera::Result::Unknown;
    std::string setting_mode = "0";
    if (mode == mavsdk::CameraServer::Mode::Photo) {
        setting_mode = "0";
    } else {
        setting_mode = "1";
    }
    // use set setting to change camera mode
    auto setting = build_setting(kCameraModeName, setting_mode);
    return set_setting(setting);
}

mavsdk::CameraServer::Result CameraLocalClient::format_storage(int storage_id) {
    base::LogDebug() << "locally call format storage " << storage_id;
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    if (_is_formatting.exchange(true)) {
        return mavsdk::CameraServer::Result::Busy;
    }
    std::async(std::launch::async, [this, storage_id]() {
        {
            auto result = _storage_manager->format_storage();
            base::LogInfo() << "format sdcard result is " << result;
        }
        _is_formatting.store(false);  // format complete and relase
    });
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::reset_settings(
    std::function<void(mavsdk::CameraServer::Result)> callback) {
    base::LogDebug() << "locally call reset settings";
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    std::lock_guard<std::mutex> lock(_action_mutex);
    if (_is_reseting.exchange(true)) {
        return mavsdk::CameraServer::Result::Busy;
    }
    /**
     * @brief the camera reset will cost some time and the uvc client will read wrong value on reseting.
     * So just set camera mode to photo before execute reset function. The reset function will always success.
     */
    _settings[kCameraModeName] = "0";
    _camera_param.set_value(kCameraModeName, _settings[kCameraModeName]);
    std::async(std::launch::async, [this, callback]() {
        auto final_result = mavsdk::CameraServer::Result::Unknown;
        {
            auto result = _mav_camera->reset_settings();
            if (result == mav_camera::Result::Success) {
                // reset settings value
                _settings[kCameraSensorModeName] = "0";  // default sensor mode is Normal
                _camera_param.set_value(kCameraSensorModeName, _settings[kCameraSensorModeName]);
                _sensor_mode = SensorMode::Normal;
                _settings[kCameraDisplayModeName] = "0";
                _camera_param.set_value(kCameraDisplayModeName, _settings[kCameraDisplayModeName]);
                _settings[kPhotoResolution] = "1";  // default photo resolution is 16M mode
                _camera_param.set_value(kPhotoResolution, _settings[kPhotoResolution]);
                _settings[kPhotoQuality] = "0";
                _camera_param.set_value(kPhotoQuality, _settings[kPhotoQuality]);
                _settings[kPhotoFormat] = "0";
                _camera_param.set_value(kPhotoFormat, _settings[kPhotoFormat]);
                _settings[kWhitebalanceModeName] = "0";
                _camera_param.set_value(kWhitebalanceModeName, _settings[kWhitebalanceModeName]);
                _settings[kExposureMode] = "0";
                _camera_param.set_value(kExposureMode, _settings[kExposureMode]);
                _settings[kEVName] = "0";
                _camera_param.set_value(kEVName, _settings[kEVName]);
                _settings[kISOName] = "125";
                _camera_param.set_value(kISOName, _settings[kISOName]);
                _settings[kShutterSpeedName] = "0.01";
                _camera_param.set_value(kShutterSpeedName, _settings[kShutterSpeedName]);
                _settings[kVideoResolution] = "1";  // default video resolution is 4k 30fps
                _camera_param.set_value(kVideoResolution, _settings[kVideoResolution]);
                _settings[kVideoFormat] = "1";
                _camera_param.set_value(kVideoFormat, _settings[kVideoFormat]);
                _settings[kMeteringModeName] = "0";
                _camera_param.set_value(kMeteringModeName, _settings[kMeteringModeName]);
                _settings[kSharpnessName] = "0";
                _camera_param.set_value(kSharpnessName, _settings[kSharpnessName]);
                _settings[kAELockName] = "0";  // ae lock don't store to param

                init_render_mode();

                set_camera_display_mode(_settings[kCameraDisplayModeName]);

                final_result = mavsdk::CameraServer::Result::Success;
            } else {
                final_result = mavsdk::CameraServer::Result::Error;
            }
        }
        _is_reseting.store(false);  // reset complete and relase
        if (callback) {
            callback(final_result);
        }
    });
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::set_timestamp(int64_t time_unix_msec) {
    base::LogDebug() << "local call set timestamp " << time_unix_msec;
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    auto result = _mav_camera->set_timestamp(time_unix_msec);
    return convert_camera_result_to_mav_server_result(result);
}

mavsdk::CameraServer::Result CameraLocalClient::set_zoom_range(float range) {
    base::LogDebug() << "local call set zoom range " << range;
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    std::lock_guard<std::mutex> lock(_action_mutex);
    auto result = _mav_camera->set_zoom(range);
    return convert_camera_result_to_mav_server_result(result);
}

mavsdk::CameraServer::Result CameraLocalClient::fill_information(
    mavsdk::CameraServer::Information &information) {
    mav_camera::Information in_info;
    mav_camera::Result result = mav_camera::Result::NoSystem;
    if (_mav_camera != nullptr) {
        result = _mav_camera->get_information(in_info);
    }
    if (result == mav_camera::Result::Success) {
        information.vendor_name = "Aeroratech";
        information.model_name = "D64TR";
        information.firmware_version = "0.6.0";
        information.focal_length_mm = in_info.focal_length_mm;
        information.horizontal_sensor_size_mm = in_info.horizontal_sensor_size_mm;
        information.vertical_sensor_size_mm = in_info.vertical_sensor_size_mm;
        information.horizontal_resolution_px = in_info.horizontal_resolution_px;
        information.vertical_resolution_px = in_info.vertical_resolution_px;
        information.lens_id = in_info.lens_id;
        //TODO (Thomas) : hard code
        information.definition_file_version = 18;
        information.definition_file_uri = "mftp://definition/D64TR.xml";
    } else {
        information.vendor_name = "Unknown";
        information.model_name = "Unknown";
        information.firmware_version = "0.0.0";
        information.focal_length_mm = 0;
        information.horizontal_sensor_size_mm = 0;
        information.vertical_sensor_size_mm = 0;
        information.horizontal_resolution_px = 0;
        information.vertical_resolution_px = 0;
        information.lens_id = 0;
        information.definition_file_version = 0;
        information.definition_file_uri = "";
        return mavsdk::CameraServer::Result::NoSystem;
    }

    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::CaptureImage);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::CaptureVideo);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::HasModes);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::HasVideoStream);
    information.camera_cap_flags.emplace_back(
        mavsdk::CameraServer::Information::CameraCapFlags::HasBasicZoom);
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_video_stream_info(
    std::vector<mavsdk::CameraServer::VideoStreamInfo> &video_stream_infos) {
    video_stream_infos.clear();

    mavsdk::CameraServer::VideoStreamInfo normal_video_stream;
    normal_video_stream.stream_id = 1;

    normal_video_stream.settings.frame_rate_hz = 30.0;
    normal_video_stream.settings.horizontal_resolution_pix = 1280;
    normal_video_stream.settings.vertical_resolution_pix = 720;
    normal_video_stream.settings.bit_rate_b_s = 1 * 1024 * 1024;
    normal_video_stream.settings.rotation_deg = 0;
    normal_video_stream.settings.uri = "rtsp://" + _rtsp_ip + "/live";
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
    // base::LogDebug() << "locally call fill storage information";
    std::lock_guard<std::mutex> lock(_storage_information_mutex);
    storage_information.total_storage_mib = _current_storage_information.total_storage_mib;
    storage_information.used_storage_mib = _current_storage_information.used_storage_mib;
    storage_information.available_storage_mib = _current_storage_information.available_storage_mib;

    switch (_current_storage_information.storage_status) {
        case StorageInformation::StorageStatus::Formatted:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::Formatted;
            break;
        case StorageInformation::StorageStatus::Unformatted:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::Unformatted;
            break;
        case StorageInformation::StorageStatus::NotAvailable:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::NotAvailable;
            break;
        case StorageInformation::StorageStatus::NotSupported:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::NotSupported;
            break;
    }

    switch (_current_storage_information.storage_type) {
        case StorageType::UsbStick:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::UsbStick;
            break;
        case StorageType::SD:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Microsd;
            break;
        case StorageType::Internal:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Other;
            break;
        default:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Unknown;
            break;
    }
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_capture_status(
    mavsdk::CameraServer::CaptureStatus &capture_status) {
    // not need lock guard
    capture_status.available_capacity_mib = _current_storage_information.available_storage_mib;
    capture_status.image_count = _image_count;
    capture_status.image_status = mavsdk::CameraServer::CaptureStatus::ImageStatus::Idle;
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
    // base::LogDebug() << "locally call fill settings ";
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
    if (_mav_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    base::LogDebug() << "change " << setting.setting_id << " to " << setting.option.option_id;
    if (_settings.count(setting.setting_id) == 0) {
        base::LogError() << "Unsupport setting " << setting.setting_id;
        return mavsdk::CameraServer::Result::WrongArgument;
    }

    bool set_success = false;
    bool need_refresh_render_mode = false;
    if (setting.setting_id == kCameraModeName) {
        set_success = set_camera_mode(setting.option.option_id);
        need_refresh_render_mode = true;
    } else if (setting.setting_id == kCameraSensorModeName) {
        set_success = set_camera_sensor_mode(setting.option.option_id);
    } else if (setting.setting_id == kCameraDisplayModeName) {
        set_success = set_camera_display_mode(setting.option.option_id);
    } else if (setting.setting_id == kPhotoResolution) {
        set_success = set_photo_resolution(setting.option.option_id);
        need_refresh_render_mode = true;
    } else if (setting.setting_id == kVideoResolution) {
        set_success = set_video_resolution(setting.option.option_id);
        need_refresh_render_mode = true;
    } else if (setting.setting_id == kPhotoQuality) {
        set_success = set_photo_quality(setting.option.option_id);
    } else if (setting.setting_id == kPhotoFormat) {
        set_success = set_photo_format(setting.option.option_id);
        need_refresh_render_mode = true;
    } else if (setting.setting_id == kWhitebalanceModeName) {
        set_success = set_whitebalance_mode(setting.option.option_id);
    } else if (setting.setting_id == kExposureMode) {
        set_success = set_exposure_mode(setting.option.option_id);
    } else if (setting.setting_id == kEVName) {
        set_success = set_exposure_value(setting.option.option_id);
    } else if (setting.setting_id == kISOName) {
        set_success = set_iso(setting.option.option_id);
    } else if (setting.setting_id == kShutterSpeedName) {
        set_success = set_shutter_speed(setting.option.option_id);
    } else if (setting.setting_id == kMeteringModeName) {
        set_success = set_metering_mode(setting.option.option_id);
    } else if (setting.setting_id == kSharpnessName) {
        set_success = set_sharpness(setting.option.option_id);
    } else if (setting.setting_id == kAELockName) {
        set_success = set_ae_lock(setting.option.option_id);
    } else if (setting.setting_id == kIrCamPalette) {
        set_success = set_ir_palette(setting.option.option_id);
    } else if (setting.setting_id == kIrCamFFCMode) {
        set_success = set_ir_ffc_mode(setting.option.option_id);
    } else if (setting.setting_id == kIrCamFFC) {
        set_success = set_ir_FFC(setting.option.option_id);
    } else {
        base::LogError() << "Not implement setting" << setting.setting_id;
        set_success = false;
    }

    // when set success update the settings value and store value
    if (set_success) {
        _settings[setting.setting_id] = setting.option.option_id;
        _camera_param.set_value(setting.setting_id, setting.option.option_id);

        if (need_refresh_render_mode) {
            init_render_mode();
        }
    }
    return mavsdk::CameraServer::Result::Success;
}

std::pair<mavsdk::CameraServer::Result, mavsdk::Camera::Setting> CameraLocalClient::get_setting(
    mavsdk::Camera::Setting setting) const {
    base::LogDebug() << "call get_setting " << setting.setting_id;
    if (_settings.count(setting.setting_id) == 0) {
        return {mavsdk::CameraServer::Result::WrongArgument, setting};
    }
    setting.option.option_id = _settings[setting.setting_id];
    base::LogDebug() << "get " << setting.setting_id << " return " << setting.option.option_id;
    return {mavsdk::CameraServer::Result::Success, setting};
}

bool CameraLocalClient::init() {
    if (!init_render_bridge()) {
        return false;
    }
    if (!init_mav_camera()) {
        return false;
    }
    if (!init_storage_manager()) {
        return false;
    }

    // init mav camera settings
    _settings[kCameraSensorModeName] = init_camera_sensor_mode();
    _settings[kCameraDisplayModeName] = init_camera_display_mode();
    _settings[kWhitebalanceModeName] = init_whitebalance_mode();
    _settings[kExposureMode] = init_exposure_mode();
    _settings[kEVName] = init_exposure_value();
    _settings[kISOName] = init_iso();
    _settings[kShutterSpeedName] = init_shutter_speed();
    _settings[kVideoFormat] = init_video_format();
    _settings[kMeteringModeName] = init_metering_mode();
    _settings[kSharpnessName] = init_sharpness();

    init_render_mode();

    // always disable ae lock on init
    _settings[kAELockName] = "0";

    // NOTE (thomas): don't check ir camera status, because camera can init without ir camera
    init_ir_camera();
    _settings[kIrCamPalette] = init_ir_palette();
    _settings[kIrCamFFCMode] = init_ir_ffc_mode();
    _settings[kIrCamFFC] = "0";

    base::LogDebug() << "Init settings :";
    for (const auto &setting : _settings) {
        base::LogDebug() << "  - " << setting.first << " : " << setting.second;
    }
    return true;
}

void CameraLocalClient::capture_callback(mav_camera::MAVFrame *rgb_frame,
                                         ir_camera::IRFrame *ir_frame) {
    if (_render_bridge == nullptr) {
        return;
    }
    if (_preview_type == PreivewStreamType::RGBStreamOnly && rgb_frame != nullptr) {
        _render_bridge->draw_rgb_frame_in_full_screen((uint8_t *)rgb_frame->vaddr, rgb_frame->width,
                                                      rgb_frame->height, rgb_frame->stride,
                                                      rgb_frame->slice);
    } else if (_preview_type == PreivewStreamType::InfraredStreamOnly && ir_frame != nullptr) {
        _render_bridge->draw_ir_frame_in_full_screen(
            ir_frame->vaddr, ir_frame->width, ir_frame->height, ir_frame->width, ir_frame->height);
    } else if (_preview_type == PreivewStreamType::SideBySide) {
        if (rgb_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_left((uint8_t *)rgb_frame->vaddr, rgb_frame->width,
                                                   rgb_frame->height, rgb_frame->stride,
                                                   rgb_frame->slice);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_right(ir_frame->vaddr, ir_frame->width,
                                                   ir_frame->height, ir_frame->width,
                                                   ir_frame->height);
        }
    } else if (_preview_type == PreivewStreamType::PIP) {
        if (rgb_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_PIP((uint8_t *)rgb_frame->vaddr, rgb_frame->width,
                                                  rgb_frame->height, rgb_frame->stride,
                                                  rgb_frame->slice);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_PIP(ir_frame->vaddr, ir_frame->width, ir_frame->height,
                                                 ir_frame->width, ir_frame->height);
        }
    } else if (_preview_type == PreivewStreamType::Superimpose) {
        if (rgb_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_superimpose(
                (uint8_t *)rgb_frame->vaddr, rgb_frame->width, rgb_frame->height, rgb_frame->stride,
                rgb_frame->slice, _render_mode);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_superimpose(ir_frame->vaddr, ir_frame->width,
                                                         ir_frame->height, ir_frame->width,
                                                         ir_frame->height);
        }
    } else if (_preview_type == PreivewStreamType::Mix) {
        if (rgb_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_mix((uint8_t *)rgb_frame->vaddr, rgb_frame->width,
                                                  rgb_frame->height, rgb_frame->stride,
                                                  rgb_frame->slice, _render_mode);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_mix(ir_frame->vaddr, ir_frame->width, ir_frame->height,
                                                 ir_frame->width, ir_frame->height);
        }
    }
}

void CameraLocalClient::deinit() {
    free_ir_camera();
    free_render_bridge();
    free_storage_manager();
}

bool CameraLocalClient::init_mav_camera() {
    if (_mav_camera != nullptr) {
        return true;
    }
    _mav_camera_handle = dlopen(QCOM_CAMERA_LIBERAY, RTLD_NOW);
    if (_mav_camera_handle == NULL) {
        char const *err_str = dlerror();
        base::LogError() << "load module " << QCOM_CAMERA_LIBERAY << " failed "
                         << (err_str != NULL ? err_str : "unknown");
        return false;
    }

    typedef mav_camera::MavCamera *(*create_qcom_camera_fun)();
    create_qcom_camera_fun create_camera_fun =
        (create_qcom_camera_fun)dlsym(_mav_camera_handle, "create_qcom_camera");
    if (create_camera_fun == NULL) {
        base::LogError() << "cannot find symbol create_qcom_camera";
        dlclose(_mav_camera_handle);
        _mav_camera_handle = NULL;
        return false;
    }

    _mav_camera = create_camera_fun();
    if (_mav_camera == nullptr) {
        base::LogError() << "cannot create mav camera instance";
        dlclose(_mav_camera_handle);
        _mav_camera_handle = NULL;
        return false;
    }

    _mav_camera->set_log_path("/data/camera/qcom_cam.log");
    mav_camera::Result result = _mav_camera->prepare();
    if (result != mav_camera::Result::Success) {
        base::LogDebug() << "cannot find qcom camera";
        dlclose(_mav_camera_handle);
        _mav_camera_handle = NULL;
        return false;
    }

    mav_camera::Options options;

    ///< init priority is env > store > default
    /************** Camera Mode *************/
    auto camera_mode = mav_camera::Mode::Photo;
    const char *env_camera_mode = getenv("MAVCAM_INIT_CAMERA_MODE");
    if (env_camera_mode != NULL) {
        if (strncmp(env_camera_mode, "0", 1) == 0) {
            camera_mode = mav_camera::Mode::Photo;
            base::LogInfo() << "Manually init camera to photo mode";
        } else if (strncmp(env_camera_mode, "1", 1) == 0) {
            camera_mode = mav_camera::Mode::Video;
            base::LogInfo() << "Manually init camera to video mode";
        }
    }

    auto store_mode = _camera_param.get_value(kCameraModeName);
    if (store_mode.empty()) {  // init default param to local storage
        if (camera_mode == mav_camera::Mode::Photo) {
            _camera_param.set_value(kCameraModeName, "0");
        } else {
            _camera_param.set_value(kCameraModeName, "1");
        }
    } else {
        if (store_mode == "0") {
            camera_mode = mav_camera::Mode::Photo;
        } else {
            camera_mode = mav_camera::Mode::Video;
        }
    }

    options.brand = kCameraBrand;
    options.module = kCameraModule;
    options.init_mode = camera_mode;
    if (options.init_mode == mav_camera::Mode::Photo) {
        _settings[kCameraModeName] = "0";
    } else {
        _settings[kCameraModeName] = "1";
    }

    /************** Photo Resolution *************/
    char *env_photo_resoltuion_mode = getenv("MAVCAM_INIT_PHOTO_RESOLUTION");
    if (env_photo_resoltuion_mode != NULL) {
        std::string mode(env_photo_resoltuion_mode);
        if (mode == "0") {
            options.photo_resolution_mode = mav_camera::PhotoResolutionMode::Full;
            _settings[kPhotoResolution] = "0";
        } else if (mode == "1") {
            options.photo_resolution_mode = mav_camera::PhotoResolutionMode::Quarter;
            _settings[kPhotoResolution] = "1";
        }
    } else {
        auto store_resolution = _camera_param.get_value(kPhotoResolution);
        // use default param and store to storage
        if (store_resolution.empty()) {
            auto [_, photo_resolution_mode] = _mav_camera->get_photo_resolution_mode();
            options.photo_resolution_mode = photo_resolution_mode;
            if (photo_resolution_mode == mav_camera::PhotoResolutionMode::Full) {
                _settings[kPhotoResolution] = "0";
            } else {
                _settings[kPhotoResolution] = "1";
            }
            // store value
            _camera_param.set_value(kPhotoResolution, _settings[kPhotoResolution]);
        } else {
            _settings[kPhotoResolution] = store_resolution;
            if (store_resolution == "0") {
                options.photo_resolution_mode = mav_camera::PhotoResolutionMode::Full;
            } else {
                options.photo_resolution_mode = mav_camera::PhotoResolutionMode::Quarter;
            }
        }
    }

    /************** Video Resolution *************/
    auto store_video_resolution = _camera_param.get_value(kVideoResolution);
    // use default param and store to storage
    if (store_video_resolution.empty()) {
        auto [_, video_resolution_mode] = _mav_camera->get_video_resolution_mode();
        options.video_resolution_mode = video_resolution_mode;
        switch (video_resolution_mode) {
            case mav_camera::VideoResolutionMode::UHD60FPS:
                _settings[kVideoResolution] = "0";
                break;
            case mav_camera::VideoResolutionMode::UHD30FPS:
                _settings[kVideoResolution] = "1";
                break;
            case mav_camera::VideoResolutionMode::FHD60FPS:
                _settings[kVideoResolution] = "2";
                break;
            case mav_camera::VideoResolutionMode::FHD30FPS:
                _settings[kVideoResolution] = "3";
                break;
        }
        // store value
        _camera_param.set_value(kVideoResolution, _settings[kVideoResolution]);
    } else {
        _settings[kVideoResolution] = store_video_resolution;
        if (store_video_resolution == "0") {
            options.video_resolution_mode = mav_camera::VideoResolutionMode::UHD60FPS;
        } else if (store_video_resolution == "1") {
            options.video_resolution_mode = mav_camera::VideoResolutionMode::UHD30FPS;
        } else if (store_video_resolution == "2") {
            options.video_resolution_mode = mav_camera::VideoResolutionMode::FHD60FPS;
        } else if (store_video_resolution == "3") {
            options.video_resolution_mode = mav_camera::VideoResolutionMode::FHD30FPS;
        }
    }

    /************** Jpeg Quality *************/
    auto store_jpeg_quality = _camera_param.get_value(kPhotoQuality);
    if (store_jpeg_quality.empty()) {
        options.jpeg_quality = mav_camera::JpegQuality::SuperFine;
        _settings[kPhotoQuality] = "0";  // 0 for jpeg super fine
        _camera_param.set_value(kPhotoQuality, "0");
    } else {
        _settings[kPhotoQuality] = store_jpeg_quality;
        if (store_jpeg_quality == "0") {
            options.jpeg_quality = mav_camera::JpegQuality::SuperFine;
        } else if (store_jpeg_quality == "1") {
            options.jpeg_quality = mav_camera::JpegQuality::Fine;
        } else {
            options.jpeg_quality = mav_camera::JpegQuality::Normal;
        }
    }

    /************** Photo format *************/
    auto store_photo_format = _camera_param.get_value(kPhotoFormat);
    if (store_photo_format.empty()) {
        options.photo_format = mav_camera::PhotoFormat::JPEG;
        _settings[kPhotoFormat] = "0";  // 0 for jpeg
        _camera_param.set_value(kPhotoFormat, "0");
    } else {
        _settings[kPhotoFormat] = store_photo_format;
        if (store_photo_format == "0") {
            options.photo_format = mav_camera::PhotoFormat::JPEG;
        } else if (store_photo_format == "1") {
            options.photo_format = mav_camera::PhotoFormat::DNG;
        } else {
            options.photo_format = mav_camera::PhotoFormat::JPEG_DNG;
        }
    }

    /************** take photo interval *************/
    auto take_photo_interval = _camera_param.get_value(kTakePhotoInterval);
    if (take_photo_interval.empty()) {
        take_photo_interval = "1800";  // default take photo interval is 1800ms
        _camera_param.set_value(kTakePhotoInterval, take_photo_interval);
        options.photo_min_interval_in_millisecond = std::stoi(take_photo_interval);
    } else {
        options.photo_min_interval_in_millisecond = std::stoi(take_photo_interval);
    }

    // other param
    options.preview_resolution_mode = mav_camera::PreviewResolutionMode::FHD;
    options.debug_calc_fps = false;

    // subscribe capture callback
    _mav_camera->set_capture_callback(RGBCaptureCallback, this);
    result = _mav_camera->open(options);
    if (result == mav_camera::Result::Success) {
        base::LogDebug() << "open qcom camera success";
    }
    return result == mav_camera::Result::Success;
}

void CameraLocalClient::free_mav_camera() {
    if (_mav_camera != nullptr) {
        _mav_camera->close();
        delete _mav_camera;
        _mav_camera = nullptr;
    }
    if (_mav_camera_handle != NULL) {
        dlclose(_mav_camera_handle);
        _mav_camera_handle = NULL;
    }
}

mavsdk::Camera::Setting CameraLocalClient::build_setting(std::string name, std::string value) {
    mavsdk::Camera::Setting setting;
    setting.setting_id = name;
    setting.option.option_id = value;
    return setting;
}

bool CameraLocalClient::set_camera_mode(std::string mode) {
    mav_camera::Mode set_mode = mav_camera::Mode::Unknown;
    if (mode == "0") {
        set_mode = mav_camera::Mode::Photo;
    } else {
        set_mode = mav_camera::Mode::Video;
    }
    auto result = _mav_camera->set_mode(set_mode);
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_camera_sensor_mode() {
    auto store_sensor_mode = _camera_param.get_value(kCameraSensorModeName);
    if (store_sensor_mode.empty()) {
        //init default sensor mode to Normal
        _sensor_mode = SensorMode::Normal;
        std::string str_sensor_mode = "";
        switch (_sensor_mode) {
            case SensorMode::Normal:
                str_sensor_mode = "0";
                break;
            case SensorMode::IR:
                str_sensor_mode = "1";
                break;
            case SensorMode::Dual:
                str_sensor_mode = "2";
                break;
        }
        _camera_param.set_value(kCameraSensorModeName, str_sensor_mode);
        return str_sensor_mode;
    } else {
        set_camera_sensor_mode(store_sensor_mode);
        return store_sensor_mode;
    }
}

bool CameraLocalClient::set_camera_sensor_mode(std::string sensor_mode) {
    if (sensor_mode == "0") {
        _sensor_mode = SensorMode::Normal;
    } else if (sensor_mode == "1") {
        _sensor_mode = SensorMode::IR;
    } else if (sensor_mode == "2") {
        _sensor_mode = SensorMode::Dual;
    }
    return true;
}

std::string CameraLocalClient::init_camera_display_mode() {
    auto store_display_mode = _camera_param.get_value(kCameraDisplayModeName);
    if (store_display_mode.empty()) {
        // default display mode is RGB
        _preview_type = mavcam::PreivewStreamType::RGBStreamOnly;
        std::string string_type = std::to_string(static_cast<int>(_preview_type));
        _camera_param.set_value(kCameraDisplayModeName, string_type);
        return string_type;
    } else {
        set_camera_display_mode(store_display_mode);
        return store_display_mode;
    }
}

bool CameraLocalClient::set_camera_display_mode(std::string mode) {
    _preview_type = static_cast<PreivewStreamType>(std::stoi(mode));
    return true;
}

bool CameraLocalClient::set_photo_resolution(std::string value) {
    mav_camera::PhotoResolutionMode mode;
    if (value == "0") {
        mode = mav_camera::PhotoResolutionMode::Full;
    } else if (value == "1") {
        mode = mav_camera::PhotoResolutionMode::Quarter;
    }
    auto result = _mav_camera->set_photo_resolution_mode(mode);
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::set_video_resolution(std::string value) {
    mav_camera::VideoResolutionMode mode;
    if (value == "0") {
        mode = mav_camera::VideoResolutionMode::UHD60FPS;
    } else if (value == "1") {
        mode = mav_camera::VideoResolutionMode::UHD30FPS;
    } else if (value == "2") {
        mode = mav_camera::VideoResolutionMode::FHD60FPS;
    } else if (value == "3") {
        mode = mav_camera::VideoResolutionMode::FHD30FPS;
    }
    base::LogDebug() << "Set video resolution to " << mode;
    auto result = _mav_camera->set_video_resolution_mode(mode);
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::set_photo_quality(std::string value) {
    mav_camera::JpegQuality jpeg_quality;
    if (value == "0") {
        jpeg_quality = mav_camera::JpegQuality::SuperFine;
    } else if (value == "1") {
        jpeg_quality = mav_camera::JpegQuality::Fine;
    } else if (value == "2") {
        jpeg_quality = mav_camera::JpegQuality::Normal;
    }
    auto result = _mav_camera->set_jpeg_quality(jpeg_quality);
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::set_photo_format(std::string value) {
    mav_camera::PhotoFormat photo_format;
    if (value == "0") {
        photo_format = mav_camera::PhotoFormat::JPEG;
    } else if (value == "1") {
        photo_format = mav_camera::PhotoFormat::DNG;
        _settings[kPhotoResolution] = "0";  //dng must be full resolution
    } else if (value == "2") {
        photo_format = mav_camera::PhotoFormat::JPEG_DNG;
    }
    auto result = _mav_camera->set_photo_format(photo_format);
    return result == mav_camera::Result::Success;
}

/**
    <option name="Auto" value="0" />
    <option name="Incandescent" value="1" />
    <option name="Sunrise" value="2" />
    <option name="Sunset" value="3" />
    <option name="Sunny" value="4" />
    <option name="Cloudy" value="5" />
    <option name="Fluorescent" value="7" />
*/
std::string CameraLocalClient::init_whitebalance_mode() {
    auto store_whitebalance = _camera_param.get_value(kWhitebalanceModeName);
    if (store_whitebalance.empty()) {
        auto [result, value] = _mav_camera->get_white_balance();
        std::string whitebalance = "0";
        if (result != mav_camera::Result::Success) {
            base::LogError() << "Cannot get whitebalance mode"
                             << convert_camera_result_to_mav_server_result(result);
            whitebalance = "0";
        } else {
            if (value == mav_camera::kAutoWhitebalanceValue) {
                whitebalance = "0";
            } else if (value == 5500) {
                whitebalance = "1";
            } else if (value == 6500) {
                whitebalance = "2";
            } else if (value == 7500) {
                whitebalance = "3";
            } else if (value == 2700) {
                whitebalance = "4";
            } else if (value == 4000) {
                whitebalance = "5";
            } else {
                base::LogWarn() << "invalid white balance value " << value;
                whitebalance = "0";
            }
        }
        _camera_param.set_value(kWhitebalanceModeName, whitebalance);
        return whitebalance;
    } else {
        set_whitebalance_mode(store_whitebalance);
        return store_whitebalance;
    }
}

bool CameraLocalClient::set_whitebalance_mode(std::string mode) {
    mav_camera::Result result;
    if (mode == "0") {  // Auto
        result = _mav_camera->set_white_balance(mav_camera::kAutoWhitebalanceValue);
    } else if (mode == "1") {  // Daylight
        result = _mav_camera->set_white_balance(5500);
    } else if (mode == "2") {  // Cloudy
        result = _mav_camera->set_white_balance(6500);
    } else if (mode == "3") {  // Shady
        result = _mav_camera->set_white_balance(7500);
    } else if (mode == "4") {  // Incandescent
        result = _mav_camera->set_white_balance(2700);
    } else if (mode == "5") {  // Fluorescent
        result = _mav_camera->set_white_balance(4000);
    }
    base::LogDebug() << "set whitebalance mode to " << mode << " result " << (int)result;

    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_exposure_mode() {
    auto store_exposure_mode = _camera_param.get_value(kExposureMode);
    if (store_exposure_mode.empty()) {
        std::string exposure_mode = "0";  // default exposure mode is Auto
        _camera_param.set_value(kExposureMode, exposure_mode);
        return exposure_mode;
    } else {
        set_exposure_mode(store_exposure_mode);
        return store_exposure_mode;
    }
}

bool CameraLocalClient::set_exposure_mode(std::string mode) {
    mav_camera::Result result;
    if (mode == "0") {
        result = _mav_camera->set_ae_mode(mav_camera::AEMode::Auto);
    } else {
        result = _mav_camera->set_ae_mode(mav_camera::AEMode::Manual);
    }
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_exposure_value() {
    auto store_ev = _camera_param.get_value(kEVName);
    if (store_ev.empty()) {
        std::string ev = "0.0";
        auto [result, value] = _mav_camera->get_exposure_value();
        if (result != mav_camera::Result::Success) {
            base::LogError() << "Cannot get exposure value"
                             << convert_camera_result_to_mav_server_result(result);
            ev = "0.0";
        } else {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << value;
            ev = oss.str();
        }
        _camera_param.set_value(kEVName, ev);
        return ev;
    } else {
        // in auto exposure mode need set value again
        if (_settings[kExposureMode] == "0") {
            set_exposure_value(store_ev);
        }
        return store_ev;
    }
}

bool CameraLocalClient::set_exposure_value(std::string exposure_value) {
    auto result = _mav_camera->set_exposure_value(std::stof(exposure_value));
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_iso() {
    auto store_iso = _camera_param.get_value(kISOName);
    if (store_iso.empty()) {
        auto [result, value] = _mav_camera->get_iso();
        std::string iso;
        if (result != mav_camera::Result::Success) {
            base::LogError() << "Cannot get iso value"
                             << convert_camera_result_to_mav_server_result(result);
            iso = "100";
        }
        iso = std::to_string(value);
        _camera_param.set_value(kISOName, iso);
        return iso;
    } else {
        // in manual exposure mode need set value again
        if (_settings[kExposureMode] == "1") {
            set_iso(store_iso);
        }
        return store_iso;
    }
}

bool CameraLocalClient::set_iso(std::string iso) {
    auto result = _mav_camera->set_iso(std::stoi(iso));
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_shutter_speed() {
    auto store_shutter_speed = _camera_param.get_value(kShutterSpeedName);
    if (store_shutter_speed.empty()) {
        std::string shutter_speed = "";
        auto [result, value] = _mav_camera->get_shutter_speed();
        if (result != mav_camera::Result::Success) {
            base::LogDebug() << "Cannot get shutterspeed"
                             << convert_camera_result_to_mav_server_result(result);
            shutter_speed = "0.01";  // default value
        }
        std::size_t pos = value.find('/');
        if (pos != std::string::npos) {
            // Split the string at '/'
            std::string num_str = value.substr(0, pos);
            std::string den_str = value.substr(pos + 1);

            // Convert to float
            float numerator = std::stof(num_str);
            float denominator = std::stof(den_str);

            // Perform the division
            auto convert_result = std::to_string(numerator / denominator);
            base::LogDebug() << "current shutter speed is : " << convert_result;
            shutter_speed = convert_result;
        } else {
            // If there is no '/', assume it's a whole number
            shutter_speed = value;
        }
        _camera_param.set_value(kShutterSpeedName, shutter_speed);
        return shutter_speed;
    } else {
        // in manual exposure mode need set value again
        if (_settings[kExposureMode] == "1") {
            set_shutter_speed(store_shutter_speed);
        }
        return store_shutter_speed;
    }
}

bool CameraLocalClient::set_shutter_speed(std::string shutter_speed) {
    auto result = _mav_camera->set_shutter_speed(shutter_speed);
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_video_format() {
    auto store_video_format = _camera_param.get_value(kVideoFormat);
    if (store_video_format.empty()) {
        std::string video_format = "1";
        _camera_param.set_value(kVideoFormat, video_format);
        return video_format;
    } else {
        return store_video_format;
    }
}

std::string CameraLocalClient::init_metering_mode() {
    auto store_metering = _camera_param.get_value(kMeteringModeName);
    if (store_metering.empty()) {
        std::string metering = "0";
        _camera_param.set_value(kMeteringModeName, metering);
        return metering;
    } else {
        set_metering_mode(store_metering);
        return store_metering;
    }
}

bool CameraLocalClient::set_metering_mode(std::string value) {
    int32_t metering_mode = std::stoi(value);
    if (metering_mode < 0 || metering_mode > 4) {
        base::LogError() << "Invalid metering mode";
        return false;
    }
    auto result = _mav_camera->set_metering_mode(metering_mode);
    if (result != mav_camera::Result::Success) {
        base::LogError() << "Failed to set metering mode : " << metering_mode;
    }
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_sharpness() {
    auto store_sharpness = _camera_param.get_value(kSharpnessName);
    if (store_sharpness.empty()) {
        std::string sharpness = "0";
        _camera_param.set_value(kSharpnessName, sharpness);
        return sharpness;
    } else {
        set_sharpness(store_sharpness);
        return store_sharpness;
    }
}

bool CameraLocalClient::set_sharpness(std::string value) {
    int32_t sharpness = std::stoi(value);
    if (sharpness < 0 || sharpness > 2) {
        base::LogError() << "Invalid sharpness value " << sharpness;
        return false;
    }
    int sharpness_values[] = {2, 4, 6};
    int convert_sharpness = sharpness_values[sharpness];
    auto result = _mav_camera->set_sharpness(convert_sharpness);
    if (result != mav_camera::Result::Success) {
        base::LogError() << "Failed to set sharpness : " << sharpness;
    }
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::set_ae_lock(std::string value) {
    int32_t ae_lock = std::stoi(value);
    auto result = _mav_camera->set_ae_lock(ae_lock != 0);
    if (result != mav_camera::Result::Success) {
        base::LogError() << "Failed to set aelock";
    }
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::init_ir_camera() {
    if (_ir_camera != nullptr) {
        return true;
    }

    _ir_camera_handle = dlopen(IR_CAMERA_LIBRARY, RTLD_NOW);
    if (_ir_camera_handle == NULL) {
        char const *err_str = dlerror();
        base::LogError() << "Load module " << IR_CAMERA_LIBRARY << " failed "
                         << (err_str != NULL ? err_str : "unknown");
        return false;
    } else {
        base::LogDebug() << "Success load " << IR_CAMERA_LIBRARY;
    }

    typedef ir_camera::IRCamera *(*create_ir_camera_fun)();
    create_ir_camera_fun create_camera_fun =
        (create_ir_camera_fun)dlsym(_ir_camera_handle, "create_ir_camera");
    if (create_camera_fun == NULL) {
        base::LogError() << "Cannot find symbol create_ir_camera";
        dlclose(_ir_camera_handle);
        _ir_camera_handle = NULL;
        return false;
    }

    _ir_camera = create_camera_fun();
    if (_ir_camera == nullptr) {
        base::LogError() << "Cannot create ir camera instance";
        dlclose(_ir_camera_handle);
        _ir_camera_handle = NULL;
        return false;
    }

    _ir_camera->set_log_path("/data/camera/ir_cam.log");

    ir_camera::Options options;
    options.brand = kCameraBrand;
    options.module = kCameraModule;

    if (!_ir_camera->open(options)) {
        base::LogError() << "open ir camera failed";
        dlclose(_ir_camera_handle);
        _ir_camera_handle = NULL;
        delete _ir_camera;
        _ir_camera = nullptr;
        return false;
    }

    _ir_camera->start_capture(IRCaptureCallback, this);

    base::LogDebug() << "Load ir camera success";
    return true;
}

void CameraLocalClient::free_ir_camera() {
    if (_ir_camera != nullptr) {
        _ir_camera->close();
        delete _ir_camera;
        _ir_camera = nullptr;
    }
    if (_ir_camera_handle != NULL) {
        dlclose(_ir_camera_handle);
        _ir_camera_handle = NULL;
    }
}

std::string CameraLocalClient::init_ir_palette() {
    auto store_ir_palette = _camera_param.get_value(kIrCamPalette);
    if (store_ir_palette.empty()) {
        std::string palette;
        if (_ir_camera != nullptr) {
            ir_camera::ColorMode color_mode = _ir_camera->get_color_mode();
            base::LogDebug() << "Current ir palette is " << color_mode;
            palette = std::to_string(static_cast<int>(color_mode));
        } else {  // When ir camera init failed, just add empty value for settings
            palette = "0";
        }
        _camera_param.set_value(kIrCamPalette, palette);
        return palette;
    } else {
        set_ir_palette(store_ir_palette);
        return store_ir_palette;
    }
}

bool CameraLocalClient::set_ir_palette(std::string color_mode) {
    ir_camera::ColorMode convert_mode = (ir_camera::ColorMode)std::stoi(color_mode);
    if (_ir_camera != nullptr) {
        return _ir_camera->set_color_mode(convert_mode);
    }
    return false;
}

std::string CameraLocalClient::init_ir_ffc_mode() {
    auto store_ir_ffc_mode = _camera_param.get_value(kIrCamFFCMode);
    if (store_ir_ffc_mode.empty()) {
        std::string ffc_mode;
        if (_ir_camera != nullptr) {
            ir_camera::FFCMode current_mode = _ir_camera->get_ffc_mode();
            base::LogDebug() << "Current ir FFC mode is " << static_cast<int>(current_mode);
            ffc_mode = std::to_string(static_cast<int>(current_mode));
        } else {
            ffc_mode = "1";
        }
        _camera_param.set_value(kIrCamFFCMode, ffc_mode);
        return ffc_mode;
    } else {
        set_ir_ffc_mode(store_ir_ffc_mode);
        return store_ir_ffc_mode;
    }
}

bool CameraLocalClient::set_ir_ffc_mode(std::string ffc_mode) {
    ir_camera::FFCMode convert_mode = (ir_camera::FFCMode)std::stoi(ffc_mode);
    if (_ir_camera != nullptr) {
        return _ir_camera->set_ffc_mode(convert_mode);
    }
    return false;
}

bool CameraLocalClient::set_ir_FFC(std::string /*ignore*/) {
    if (_ir_camera != nullptr) {
        _ir_camera->run_ffc();
        return true;
    }
    return false;
}

bool CameraLocalClient::init_render_bridge() {
    if (_render_bridge != nullptr) {
        return true;
    }
    _render_bridge_handle = dlopen(RENDER_BRIDGE_LIBRARY, RTLD_NOW);
    if (_render_bridge_handle == NULL) {
        char const *err_str = dlerror();
        base::LogError() << "Load module " << RENDER_BRIDGE_LIBRARY << " failed "
                         << (err_str != NULL ? err_str : "unknown");
        return false;
    } else {
        base::LogDebug() << "Success load " << RENDER_BRIDGE_LIBRARY;
    }

    typedef RenderBridge *(*create_render_bridge_fun)(RenderType);
    create_render_bridge_fun create_render_bridge =
        (create_render_bridge_fun)dlsym(_render_bridge_handle, "create_render_bridge");
    if (create_render_bridge == NULL) {
        base::LogError() << "Cannot find symbol create_render_bridge";
        dlclose(_render_bridge_handle);
        _render_bridge_handle = NULL;
        return false;
    }

    _render_bridge = create_render_bridge(RenderType::Weston);
    if (!_render_bridge->open()) {
        base::LogError() << "Open render bridge failed";
        dlclose(_render_bridge_handle);
        _render_bridge_handle = NULL;
        return false;
    }
    return true;
}

void CameraLocalClient::free_render_bridge() {
    if (_render_bridge != nullptr) {
        _render_bridge->close();
        delete _render_bridge;
        _render_bridge = nullptr;
    }
    if (_render_bridge_handle != NULL) {
        dlclose(_render_bridge_handle);
        _render_bridge_handle = NULL;
    }
}

bool CameraLocalClient::init_storage_manager() {
    if (_storage_manager != nullptr) {
        return true;
    }
    _storage_manager_handle = dlopen(STORAGE_MANAGER_LIBRARY, RTLD_NOW);
    if (_storage_manager_handle == NULL) {
        char const *err_str = dlerror();
        base::LogError() << "Load module " << STORAGE_MANAGER_LIBRARY << " failed "
                         << (err_str != NULL ? err_str : "unknown");
        return false;
    } else {
        base::LogDebug() << "Success load " << STORAGE_MANAGER_LIBRARY;
    }

    typedef StorageManager *(*create_storage_manager_fun)(StorageType storage_type);
    create_storage_manager_fun create_storage_manager =
        (create_storage_manager_fun)dlsym(_storage_manager_handle, "create_storage_manager");
    if (create_storage_manager == NULL) {
        base::LogError() << "Cannot find symbol create_storage_manager";
        dlclose(_storage_manager_handle);
        _storage_manager_handle = NULL;
        return false;
    }

    _storage_manager = create_storage_manager(StorageType::SD);
    if (!_storage_manager->open(kCameraBrand)) {
        base::LogError() << "Open storage manager failed";
        dlclose(_storage_manager_handle);
        _storage_manager_handle = NULL;
        return false;
    }

    _storage_manager->subscribe_storage_info([&](StorageInformation storage_information) {
        std::lock_guard<std::mutex> lock(_storage_information_mutex);
        _current_storage_information = storage_information;
        check_sdcard_status();
    });
    return true;
}

void CameraLocalClient::free_storage_manager() {
    if (_storage_manager != nullptr) {
        _storage_manager->close();
        delete _storage_manager;
        _storage_manager = nullptr;
    }
    if (_storage_manager_handle != NULL) {
        dlclose(_storage_manager_handle);
        _storage_manager_handle = NULL;
    }
}

void CameraLocalClient::check_sdcard_status() {
    bool sdcard_is_formatted =
        _current_storage_information.storage_status == StorageInformation::StorageStatus::Formatted;
    bool sdcard_is_full =
        _current_storage_information.available_storage_mib < kSDCardMinAvaliableMB;
    // The SD card should be valid only if it is formatted AND not full
    bool should_be_valid = sdcard_is_formatted && !sdcard_is_full;
    // If unknown OR changed → handle state change
    if (!_sdcard_valid.has_value() || _sdcard_valid.value() != should_be_valid) {
        // SD card becomes invalid or full
        if (!should_be_valid) {
            if (_is_recording_video) {
                stop_video();
            }
            switch_led_mode(mavcam::LedMode::SDCardError);
        }
        // SD card becomes valid
        else {
            switch_led_mode(mavcam::LedMode::Normal);
        }

        // Update value (initializes optional on first run)
        _sdcard_valid = should_be_valid;
    }
}

mavsdk::CameraServer::Result CameraLocalClient::convert_camera_result_to_mav_server_result(
    mav_camera::Result input_result) {
    mavsdk::CameraServer::Result output_result = mavsdk::CameraServer::Result::Unknown;
    switch (input_result) {
        case mav_camera::Result::Success:
            output_result = mavsdk::CameraServer::Result::Success;
            break;
        case mav_camera::Result::Denied:
            output_result = mavsdk::CameraServer::Result::Denied;
            break;
        case mav_camera::Result::Busy:
            output_result = mavsdk::CameraServer::Result::Busy;
            break;
        case mav_camera::Result::Error:
            output_result = mavsdk::CameraServer::Result::Error;
            break;
        case mav_camera::Result::InProgress:
            output_result = mavsdk::CameraServer::Result::InProgress;
            break;
        case mav_camera::Result::NoSystem:
            output_result = mavsdk::CameraServer::Result::NoSystem;
            break;
        case mav_camera::Result::Timeout:
            output_result = mavsdk::CameraServer::Result::Timeout;
            break;
        case mav_camera::Result::Unknown:
            output_result = mavsdk::CameraServer::Result::Unknown;
            break;
        case mav_camera::Result::WrongArgument:
            output_result = mavsdk::CameraServer::Result::WrongArgument;
            break;
    }
    return output_result;
}

void CameraLocalClient::init_render_mode() {
    _render_mode = RenderMode::Photo_Quarter;
    if (_settings[kCameraModeName] == "0" && _settings[kPhotoResolution] == "0") {
        _render_mode = RenderMode::Photo_Full;
    } else if (_settings[kCameraModeName] == "0" && _settings[kPhotoResolution] == "0") {
        _render_mode = RenderMode::Photo_Quarter;
    } else if (_settings[kCameraModeName] == "1" && _settings[kVideoResolution] == "0") {
        _render_mode = RenderMode::Video_4K60;
    } else if (_settings[kCameraModeName] == "1" && _settings[kVideoResolution] == "1") {
        _render_mode = RenderMode::Video_4K30;
    } else if (_settings[kCameraModeName] == "1" && _settings[kVideoResolution] == "2") {
        _render_mode = RenderMode::Video_1080p60;
    } else if (_settings[kCameraModeName] == "1" && _settings[kVideoResolution] == "3") {
        _render_mode = RenderMode::Video_1080p30;
    }
}

}  // namespace mavcam
