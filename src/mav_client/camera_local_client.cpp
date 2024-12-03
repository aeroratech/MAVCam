#include "camera_local_client.h"

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <iomanip>  // for std::setprecision
#include <regex>
#include <thread>

#include "base/log.h"

namespace mavcam {

const std::string kCameraModeName = "CAM_MODE";
const std::string kCameraDisplayModeName = "CAM_DIS_MODE";
const std::string kPhotoResolution = "CAM_PHOTO_RES";
const std::string kVideoResolution = "CAM_VIDRES";
const std::string kVideoFormat = "CAM_VIDFMT";
const std::string kWhitebalanceModeName = "CAM_WBMODE";
const std::string kExposureMode = "CAM_EXPMODE";
const std::string kEVName = "CAM_EV";
const std::string kISOName = "CAM_ISO";
const std::string kShutterSpeedName = "CAM_SHUTTERSPD";
const std::string kMeteringModeName = "CAM_METER";

const std::string kIrCamPalette = "IRCAM_PALETTE";
const std::string kIrCamFFC = "IRCAM_FFC";

static const int32_t kPreviewWidth = 1920;
static const int32_t kPreviewPhotoHeight = 1440;
static const int32_t kPreviewVideoHeight = 1080;

static int32_t kSnapshotWidth = 1920;
static int32_t kSnapshotHeight = 1440;
static int32_t kSnapshotHalfWidth = 1920;
static int32_t kSnapshotHalfHeight = 1440;
static const int32_t kVideoWidth = 3840;
static const int32_t kVideoHeight = 2160;

#define QCOM_CAMERA_LIBERAY "libqcom_camera.so"
#define BOSON_CAMERA_LIBRARY "libboson-sdk-clientfiles_64.so"

typedef mav_camera::MavCamera *(*create_qcom_camera_fun)();

CameraLocalClient::CameraLocalClient() {
    _framerate = 30;
    _is_capture_in_progress = false;
    _image_count = 0;
    _is_recording_video = false;

    // TODO just demo for settings
    _settings[kCameraModeName] = "0";
    _settings["CAM_DISPLAY_MODE"] = "0";
    _settings["CAM_PHOTO_RES"] = "1";
    _settings["CAM_WBMODE"] = "4";
    _settings["CAM_EXPMODE"] = "0";
    _settings["CAM_EV"] = "1";
    _settings["CAM_ISO"] = "200";
    _settings["CAM_SHUTTERSPD"] = "0.01";
    _settings["CAM_VIDFMT"] = "2";
    _settings["CAM_VIDRES"] = "0";
    _settings["CAM_VIDFMT"] = "0";
    _settings["CAM_PHOTORATIO"] = "1";
    _settings["CAM_METER"] = "0";
    _settings["IRCAM_PALETTE"] = "1";
    _settings["IRCAM_FFC"] = "0";
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
    auto result = _mav_camera->start_video();
    auto mav_result = convert_camera_result_to_mav_server_result(result);
    if (mav_result == mavsdk::CameraServer::Result::Success) {
        _is_recording_video = true;
        _start_video_time = std::chrono::steady_clock::now();
    }
    return mav_result;
}

mavsdk::CameraServer::Result CameraLocalClient::stop_video() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_is_recording_video) {
        base::LogWarn() << "call stop video without video is recording";
        return mavsdk::CameraServer::Result::Success;
    }
    base::LogDebug() << "locally call stop video";
    auto result = _mav_camera->stop_video();
    auto mav_result = convert_camera_result_to_mav_server_result(result);
    if (mav_result == mavsdk::CameraServer::Result::Success) {
        _is_recording_video = false;
    }
    return mav_result;
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
    if (_current_mode == mode) {
        // same mode do not change again
        return mavsdk::CameraServer::Result::Success;
    }
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
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call format storage " << storage_id;
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::reset_settings() {
    std::lock_guard<std::mutex> lock(_mutex);
    base::LogDebug() << "locally call reset settings";
    // reset settings
    _settings[kCameraModeName] = "0";
    _settings["CAM_DISPLAY_MODE"] = "0";
    _settings["CAM_PHOTO_RES"] = "1";
    _settings["CAM_WBMODE"] = "0";
    _settings["CAM_EXPMODE"] = "0";
    _settings["CAM_EV"] = "0";
    _settings["CAM_ISO"] = "100";
    _settings["CAM_SHUTTERSPD"] = "0.01";
    _settings["CAM_VIDFMT"] = "1";
    _settings["CAM_VIDRES"] = "0";
    _settings["CAM_PHOTORATIO"] = "1";
    _settings["CAM_METER"] = "0";
    _settings["IRCAM_PALETTE"] = "1";
    _settings["IRCAM_FFC"] = "0";

    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::set_timestamp(int64_t time_unix_msec) {
    base::LogDebug() << "local call set timestamp " << time_unix_msec;
    return mavsdk::CameraServer::Result::Denied;
}

mavsdk::CameraServer::Result CameraLocalClient::set_zoom_range(float range) {
    base::LogDebug() << "local call set zoom range " << range;
    return mavsdk::CameraServer::Result::Denied;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_information(
    mavsdk::CameraServer::Information &information) {
    information.vendor_name = "Aeroratech";
    information.model_name = "D64TR";
    information.firmware_version = "0.0.1";
    information.focal_length_mm = 3.0;
    information.horizontal_sensor_size_mm = 3.68;
    information.vertical_sensor_size_mm = 2.76;
    information.horizontal_resolution_px = 3280;
    information.vertical_resolution_px = 2464;
    information.lens_id = 0;

    information.definition_file_version = 6;
    information.definition_file_uri = "mftp://definition/D64TR.xml";

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

    normal_video_stream.settings.frame_rate_hz = 60.0;
    normal_video_stream.settings.horizontal_resolution_pix = 1920;
    normal_video_stream.settings.vertical_resolution_pix = 1080;
    normal_video_stream.settings.bit_rate_b_s = 4 * 1024 * 1024;
    normal_video_stream.settings.rotation_deg = 0;
    normal_video_stream.settings.uri = "rtsp://192.168.251.1/live";
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
    std::lock_guard<std::mutex> lock(_storage_information_mutex);
    storage_information.total_storage_mib = _current_storage_information.total_storage_mib;
    storage_information.used_storage_mib = _current_storage_information.used_storage_mib;
    storage_information.available_storage_mib = _current_storage_information.available_storage_mib;

    switch (_current_storage_information.storage_status) {
        case mav_camera::StorageInformation::StorageStatus::Formatted:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::Formatted;
            break;
        case mav_camera::StorageInformation::StorageStatus::Unformatted:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::Unformatted;
            break;
        case mav_camera::StorageInformation::StorageStatus::NotAvailable:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::NotAvailable;
            break;
        case mav_camera::StorageInformation::StorageStatus::NotSupported:
            storage_information.storage_status =
                mavsdk::CameraServer::StorageInformation::StorageStatus::NotSupported;
            break;
    }

    switch (_current_storage_information.storage_type) {
        case mav_camera::StorageType::Hd:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Hd;
            break;
        case mav_camera::StorageType::Microsd:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Microsd;
            break;
        case mav_camera::StorageType::Other:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Other;
            break;
        case mav_camera::StorageType::Sd:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Sd;
            break;
        case mav_camera::StorageType::Unknown:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::Unknown;
            break;
        case mav_camera::StorageType::UsbStick:
            storage_information.storage_type =
                mavsdk::CameraServer::StorageInformation::StorageType::UsbStick;
            break;
    }
    return mavsdk::CameraServer::Result::Success;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_capture_status(
    mavsdk::CameraServer::CaptureStatus &capture_status) {
    // not need lock guard
    capture_status.available_capacity_mib = _current_storage_information.available_storage_mib;
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
    bool set_success = false;
    if (setting.setting_id == kCameraModeName) {
        mav_camera::Mode set_mode = mav_camera::Mode::Unknown;
        if (setting.option.option_id == "0") {
            set_mode = mav_camera::Mode::Photo;
        } else {
            set_mode = mav_camera::Mode::Video;
        }
        auto result = _mav_camera->set_mode(set_mode);
        set_success = result == mav_camera::Result::Success;
        if (set_success) {
            if (set_mode == mav_camera::Mode::Photo) {
                _current_mode = mavsdk::CameraServer::Mode::Photo;
            } else {
                _current_mode = mavsdk::CameraServer::Mode::Video;
            }
        }
    }
    if (set_success) {
        _settings[setting.setting_id] = setting.option.option_id;
    }
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

bool CameraLocalClient::init() {
    if (_mav_camera != nullptr) {
        return true;
    }

    _plugin_handle = dlopen(QCOM_CAMERA_LIBERAY, RTLD_NOW);
    if (_plugin_handle == NULL) {
        char const *err_str = dlerror();
        base::LogError() << "load module " << QCOM_CAMERA_LIBERAY << " failed "
                         << (err_str != NULL ? err_str : "unknown");
        return false;
    }

    create_qcom_camera_fun create_camera_fun =
        (create_qcom_camera_fun)dlsym(_plugin_handle, "create_qcom_camera");
    if (create_camera_fun == NULL) {
        base::LogError() << "cannot find symbol create_qcom_camera";
        dlclose(_plugin_handle);
        _plugin_handle = NULL;
        return false;
    }

    _mav_camera = create_camera_fun();
    if (_mav_camera == nullptr) {
        base::LogError() << "cannot create mav camera instance";
        dlclose(_plugin_handle);
        _plugin_handle = NULL;
        return false;
    }

    _mav_camera->set_log_path("/data/camera/qcom_cam.log");
    mav_camera::Result result = _mav_camera->prepare();
    if (result != mav_camera::Result::Success) {
        base::LogDebug() << "cannot find qcom camera";
        return false;
    }

    mav_camera::Options options;
    options.preview_drm_output = false;
    options.preview_v4l2_output = false;
    options.preview_weston_output = true;

    auto camera_mode = mav_camera::Mode::Photo;
    const char *init_camera_mode = getenv("MAVCAM_INIT_CAMERA_MODE");
    if (init_camera_mode != NULL) {
        if (strncmp(init_camera_mode, "0", 1) == 0) {
            camera_mode = mav_camera::Mode::Photo;
            base::LogInfo() << "Manually init camera to photo mode";
        } else if (strncmp(init_camera_mode, "1", 1) == 0) {
            camera_mode = mav_camera::Mode::Video;
            base::LogInfo() << "Manually init camera to video mode";
        }
    }
    options.init_mode = camera_mode;

    const char *init_snapshot_resoltuion = getenv("MAVCAM_INIT_SNAPSHOT_RES");
    if (init_snapshot_resoltuion != NULL) {
        std::regex resolutionRegex(R"(^(\d+)x(\d+)$)");
        std::smatch match;
        const auto str_snapshot_resoltuion = std::string(init_snapshot_resoltuion);
        if (std::regex_match(str_snapshot_resoltuion, match, resolutionRegex)) {
            // Extract width and height from the match results
            kSnapshotWidth = std::stoi(match[1].str());
            kSnapshotHeight = std::stoi(match[2].str());

            kSnapshotHalfWidth = kSnapshotWidth / 2;
            kSnapshotHalfHeight = kSnapshotHeight / 2;

            // for manually set snapshot resolution, not use half snapshot resolution
            options.snapshot_width = kSnapshotWidth;
            options.snapshot_height = kSnapshotHeight;
            _settings[kPhotoResolution] = "0";
        }
    } else {
        int32_t snapshot_width = 0;
        int32_t snpashot_height = 0;
        std::tie(result, kSnapshotWidth, kSnapshotHeight) = _mav_camera->get_snapshot_resolution();
        kSnapshotHalfWidth = kSnapshotWidth / 2;
        kSnapshotHalfHeight = kSnapshotHeight / 2;
        if (kSnapshotWidth > 8000) {  // for 64M mode, use half width and height
            options.snapshot_width = kSnapshotHalfWidth;
            options.snapshot_height = kSnapshotHalfHeight;
            _settings[kPhotoResolution] = "1";  // 1 for 1/4 resolution
        } else {
            options.snapshot_width = kSnapshotWidth;
            options.snapshot_height = kSnapshotHeight;
            _settings[kPhotoResolution] = "1";
        }
    }

    if (options.init_mode == mav_camera::Mode::Photo) {
        options.preview_width = kPreviewWidth;
        options.preview_height = kPreviewPhotoHeight;
    } else {
        options.preview_width = kPreviewWidth;
        options.preview_height = kPreviewPhotoHeight;
    }

    options.video_width = kVideoWidth;
    options.video_height = kVideoHeight;

    options.framerate = _framerate;
    options.debug_calc_fps = false;

    const char *store_prefix = getenv("MAVCAM_DEFAULT_STORE_PREFIX");
    if (store_prefix == NULL) {
        base::LogWarn() << "No store prefix found";
    } else {
        options.store_prefix = store_prefix;
        base::LogInfo() << "Set store prefix to " << options.store_prefix;
    }

    result = _mav_camera->open(options);
    if (result == mav_camera::Result::Success) {
        base::LogDebug() << "open qcom camera success";
    }

    if (options.init_mode == mav_camera::Mode::Photo) {
        _settings[kCameraModeName] = "0";
    } else {
        _settings[kCameraModeName] = "1";
    }

    _mav_camera->subscribe_storage_information(
        [&](mav_camera::Result result, mav_camera::StorageInformation storage_information) {
            std::lock_guard<std::mutex> lock(_storage_information_mutex);
            _current_storage_information = storage_information;
        });

    return true;
}

mavsdk::Camera::Setting CameraLocalClient::build_setting(std::string name, std::string value) {
    mavsdk::Camera::Setting setting;
    setting.setting_id = name;
    setting.option.option_id = value;
    return setting;
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

}  // namespace mavcam
