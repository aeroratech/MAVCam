#include "camera_local_client.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>  // for std::setprecision
#include <regex>
#include <sstream>
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
//IR Camera
const std::string kIrCamPalette = "IR_PALETTE";
const std::string kIrCamFFCMode = "IR_FFC_MODE";
const std::string kIrCamFFC = "IR_FFC";
const std::string kIrTemperature = "IR_TEMPERATURE";
//AI Function
const std::string kAIFunction = "AI_FUNCTION";
//OSD
const std::string kOSDDisplay = "CAM_OSD";
const std::string kOSDStartX = "OSD_START_X";
const std::string kOSDStartY = "OSD_START_Y";

namespace {

constexpr const char *kLaserShmName = "/laser_shm";
constexpr const char *kTrackingAddress = "127.0.0.1";
constexpr int kTrackingPort = 14600;
constexpr const char *kDefinitionDirectory = "/usr/share/mav-cam/definition/";
constexpr const char *kDefinitionFileName = "Q50MZ.xml";
constexpr const char *kVideoPreviewModeProperty = "persist.video.preview.mode";
constexpr const char *kVideoPreviewBitrateProperty = "persist.video.preview.bitrate";

struct LaserSharedMemory {
    std::uint32_t sequence{0};
    std::uint16_t distance_mm{0};
    std::uint8_t sensor_status{0};
};

constexpr float fusion_zoom_change_threshold = 25.0F;

constexpr float kZoomRangeMin = 1.0F;
constexpr float kZoomRangeMax = 100.0F;
constexpr float kFusionZoomInputThreshold = 60.0F;

std::string firmware_version_from_device() {
    std::ifstream version_file("/etc/aerora-version");
    if (!version_file.is_open()) {
        base::LogWarn() << "Unable to open /etc/aerora-version";
        return "0.0.0.0";
    }

    const std::regex sdk_version_regex(R"(^SDK_VERSION=([0-9]+)\.([0-9]+)\.([0-9]+)\r?$)");
    std::string line;
    std::smatch match;
    while (std::getline(version_file, line)) {
        if (std::regex_match(line, match, sdk_version_regex)) {
            // Aerora's SDK version has no development component.
            return match[1].str() + "." + match[2].str() + "." + match[3].str() + ".0";
        }
    }

    base::LogWarn() << "No valid SDK_VERSION found in /etc/aerora-version";
    return "0.0.0.0";
}

std::string product_name_from_device() {
    constexpr const char *kFallbackProductName = "D64TR";
    std::ifstream version_file("/etc/aerora-version");
    if (!version_file.is_open()) {
        base::LogWarn() << "Unable to open /etc/aerora-version";
        return kFallbackProductName;
    }

    constexpr const char *kProductNamePrefix = "PRODUCTNAME=";
    std::string line;
    while (std::getline(version_file, line)) {
        if (line.rfind(kProductNamePrefix, 0) == 0) {
            std::string product_name = line.substr(std::char_traits<char>::length(kProductNamePrefix));
            if (!product_name.empty() && product_name.back() == '\r') {
                product_name.pop_back();
            }
            if (!product_name.empty()) {
                return product_name;
            }
        }
    }

    base::LogWarn() << "No valid PRODUCTNAME found in /etc/aerora-version";
    return kFallbackProductName;
}

std::optional<int32_t> parse_int32(const std::string &value) {
    int32_t result = 0;
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, result);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return result;
}

bool read_preview_stream_from_device(int &width, int &height, float &frame_rate) {
    const std::string command = std::string("getprop ") + kVideoPreviewModeProperty;
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        base::LogWarn() << "Unable to read " << kVideoPreviewModeProperty;
        return false;
    }

    char buffer[128];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    if (pclose(pipe) != 0) {
        base::LogWarn() << "Unable to read " << kVideoPreviewModeProperty;
        return false;
    }

    const std::regex preview_mode_regex(R"(^\s*([0-9]+)x([0-9]+)@([0-9]+(?:\.[0-9]+)?)\s*$)");
    std::smatch match;
    if (!std::regex_match(output, match, preview_mode_regex)) {
        base::LogWarn() << "Invalid " << kVideoPreviewModeProperty << ": " << output;
        return false;
    }

    const auto parsed_width = parse_int32(match[1].str());
    const auto parsed_height = parse_int32(match[2].str());
    const std::string frame_rate_string = match[3].str();
    char *end = nullptr;
    errno = 0;
    const float parsed_frame_rate = std::strtof(frame_rate_string.c_str(), &end);
    if (errno == ERANGE || end == frame_rate_string.c_str() || *end != '\0') {
        return false;
    }
    if (!parsed_width.has_value() || !parsed_height.has_value() || *parsed_width <= 0 ||
        *parsed_height <= 0 || parsed_frame_rate <= 0.0F) {
        base::LogWarn() << "Invalid " << kVideoPreviewModeProperty << ": " << output;
        return false;
    }

    width = *parsed_width;
    height = *parsed_height;
    frame_rate = parsed_frame_rate;
    return true;
}

bool read_preview_stream_bitrate_from_device(int &bitrate) {
    const std::string command = std::string("getprop ") + kVideoPreviewBitrateProperty;
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        base::LogWarn() << "Unable to read " << kVideoPreviewBitrateProperty;
        return false;
    }

    char buffer[128];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    if (pclose(pipe) != 0) {
        base::LogWarn() << "Unable to read " << kVideoPreviewBitrateProperty;
        return false;
    }

    const std::regex bitrate_regex(R"(^\s*([0-9]+)\s*$)");
    std::smatch match;
    if (!std::regex_match(output, match, bitrate_regex)) {
        base::LogWarn() << "Invalid " << kVideoPreviewBitrateProperty << ": " << output;
        return false;
    }

    const auto parsed_bitrate = parse_int32(match[1].str());
    if (!parsed_bitrate.has_value() || *parsed_bitrate <= 0) {
        base::LogWarn() << "Invalid " << kVideoPreviewBitrateProperty << ": " << output;
        return false;
    }

    bitrate = *parsed_bitrate;
    return true;
}

int32_t definition_file_version_from_device() {
    const std::string definition_file_path =
        std::string(kDefinitionDirectory) + kDefinitionFileName;
    std::ifstream definition_file(definition_file_path);
    if (!definition_file.is_open()) {
        base::LogWarn() << "Unable to open " << definition_file_path;
        return 0;
    }

    const std::regex definition_version_regex(
        R"definition(^\s*<definition\s+version="([0-9]+)"[^>]*>\s*\r?$)definition");
    std::string line;
    std::smatch match;
    while (std::getline(definition_file, line)) {
        if (std::regex_match(line, match, definition_version_regex)) {
            const auto version = parse_int32(match[1].str());
            if (version.has_value()) {
                base::LogInfo() << "Definition file version: " << *version;
                return *version;
            }
        }
    }

    base::LogWarn() << "No valid definition version found in " << definition_file_path;
    return 0;
}

// Map the UI's linear range to the camera range while preserving the endpoints.
// An input value of 60 maps to the lens-switch threshold.
float map_zoom_range(float range) {
    const float clamped_range = std::clamp(range, kZoomRangeMin, kZoomRangeMax);
    if (clamped_range <= kFusionZoomInputThreshold) {
        return kZoomRangeMin + (clamped_range - kZoomRangeMin) *
                                   (fusion_zoom_change_threshold - kZoomRangeMin) /
                                   (kFusionZoomInputThreshold - kZoomRangeMin);
    }

    return fusion_zoom_change_threshold + (clamped_range - kFusionZoomInputThreshold) *
                                              (kZoomRangeMax - fusion_zoom_change_threshold) /
                                              (kZoomRangeMax - kFusionZoomInputThreshold);
}
}  // namespace

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

void MainCameraCallback(mav_camera::MAVFrame *frame, void *context) {
    if (context != NULL) {
        CameraLocalClient *client = (CameraLocalClient *)context;
        client->capture_callback(frame, NULL, NULL);
    }
}

void TelephotoRGBCaptureCallback(mav_camera::MAVFrame *frame, void *context) {
    if (context != NULL) {
        CameraLocalClient *client = (CameraLocalClient *)context;
        client->capture_callback(NULL, frame, NULL);
    }
}

void IRCaptureCallback(ir_camera::IRFrame *frame, void *context) {
    if (context != NULL) {
        CameraLocalClient *client = (CameraLocalClient *)context;
        client->capture_callback(NULL, NULL, frame);
    }
}

CameraLocalClient::CameraLocalClient(std::string rtsp_ip)
    : _image_count(0), _is_recording_video(false), _rtsp_ip(std::move(rtsp_ip)) {}

CameraLocalClient::~CameraLocalClient() {
    deinit();
}

mavsdk::CameraServer::Result CameraLocalClient::take_photo(int index) {
    base::LogDebug() << "locally call take photo " << index;
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
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
        auto result = rgb_camera->take_photo(file_path);
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
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    if (_sensor_mode == SensorMode::IR && _ir_camera != nullptr) {
        std::string file_path = generate_new_storage_path();
        success = _ir_camera->start_video_recording(file_path);
        if (!success) {
            return_result = mavsdk::CameraServer::Result::Error;
        }
    } else if (_sensor_mode == SensorMode::Normal || _sensor_mode == SensorMode::Dual) {
        std::string file_path = generate_new_storage_path();
        auto result = rgb_camera->start_video(file_path);
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
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    std::lock_guard<std::mutex> lock(_action_mutex);
    if (!_is_recording_video) {
        base::LogWarn() << "call stop video without video is recording";
        return mavsdk::CameraServer::Result::Success;
    }

    bool success = false;
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto mav_result = mavsdk::CameraServer::Result::Success;
    if (_sensor_mode == SensorMode::IR && _ir_camera != nullptr) {
        success = _ir_camera->stop_video_recording();
        if (!success) {
            mav_result = mavsdk::CameraServer::Result::Error;
        }
    } else if (_sensor_mode == SensorMode::Normal || _sensor_mode == SensorMode::Dual) {
        auto result = rgb_camera->stop_video();
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
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
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
    base::LogDebug() << "locally call format storage " << storage_id;
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
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
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    {
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
    }
    std::async(std::launch::async, [this, callback]() {
        std::lock_guard<std::mutex> lock(_action_mutex);
        auto final_result = mavsdk::CameraServer::Result::Unknown;
        {
            auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
            auto result = rgb_camera->reset_settings();
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
                _settings[kAIFunction] = "0";
                _camera_param.set_value(kAIFunction, _settings[kAIFunction]);
                set_ai_function(_settings[kAIFunction]);
                _settings[kOSDDisplay] = "0";
                _camera_param.set_value(kOSDDisplay, _settings[kOSDDisplay]);

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
    std::lock_guard<std::mutex> lock(_action_mutex);
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_timestamp(time_unix_msec);
    return convert_camera_result_to_mav_server_result(result);
}

mavsdk::CameraServer::Result CameraLocalClient::set_zoom_range(float range) {
    base::LogDebug() << "local call set zoom range " << range;
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
        return mavsdk::CameraServer::Result::NoSystem;
    }
    std::lock_guard<std::mutex> lock(_action_mutex);
    float real_range = map_zoom_range(range);
    if (real_range >= fusion_zoom_change_threshold) {
        if (!set_camera_display_mode("1")) {
            return mavsdk::CameraServer::Result::Error;
        }
        // The telephoto lens needs a smaller digital zoom range than the wide lens.
        real_range = std::max(kZoomRangeMin, (real_range - fusion_zoom_change_threshold) / 3.0F);
    } else {
        if (!set_camera_display_mode("0")) {
            return mavsdk::CameraServer::Result::Error;
        }
    }
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_zoom(real_range);
    const auto camera_result = convert_camera_result_to_mav_server_result(result);
    if (camera_result == mavsdk::CameraServer::Result::Success) {
        _zoom_level.store(std::clamp(range, kZoomRangeMin, kZoomRangeMax));
    }
    return camera_result;
}

mavsdk::CameraServer::Result CameraLocalClient::fill_information(
    mavsdk::CameraServer::Information &information) {
    std::lock_guard<std::mutex> lock(_action_mutex);
    mav_camera::Information in_info;
    mav_camera::Result result = mav_camera::Result::NoSystem;
    if (_main_camera != nullptr) {
        result = _main_camera->get_information(in_info);
    }
    if (result == mav_camera::Result::Success) {
        information.vendor_name = "Aeroratech";
        information.model_name = product_name_from_device();
        information.firmware_version = firmware_version_from_device();
        information.focal_length_mm = in_info.focal_length_mm;
        information.horizontal_sensor_size_mm = in_info.horizontal_sensor_size_mm;
        information.vertical_sensor_size_mm = in_info.vertical_sensor_size_mm;
        information.horizontal_resolution_px = in_info.horizontal_resolution_px;
        information.vertical_resolution_px = in_info.vertical_resolution_px;
        information.lens_id = in_info.lens_id;
        information.definition_file_version = definition_file_version_from_device();
        information.definition_file_uri = std::string("mftp://definition/") + kDefinitionFileName;
    } else {
        information.vendor_name = "Unknown";
        information.model_name = "Unknown";
        information.firmware_version = "0.0.0.0";
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

    normal_video_stream.settings.frame_rate_hz = _preview_stream_frame_rate;
    normal_video_stream.settings.horizontal_resolution_pix = _preview_stream_width;
    normal_video_stream.settings.vertical_resolution_pix = _preview_stream_height;
    normal_video_stream.settings.bit_rate_b_s = _preview_stream_bitrate;
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
    settings.zoom_level = _zoom_level.load();
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
    std::lock_guard<std::mutex> lock(_action_mutex);
    if (_main_camera == nullptr && _telephoto_camera == nullptr) {
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
    } else if (setting.setting_id == kIrTemperature) {
        set_success = set_ir_temperature(setting.option.option_id);
    } else if (setting.setting_id == kAIFunction) {
        set_success = set_ai_function(setting.option.option_id);
    } else if (setting.setting_id == kOSDDisplay) {
        set_success = setting.option.option_id == "0" || setting.option.option_id == "1";
    } else {
        base::LogError() << "Not implement setting" << setting.setting_id;
        set_success = false;
    }

    // when set success update the settings value and store value
    if (set_success) {
        _settings[setting.setting_id] = setting.option.option_id;
        if (setting.setting_id != kAIFunction) {
            _camera_param.set_value(setting.setting_id, setting.option.option_id);
        }

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
    if (!init_main_camera()) {
        return false;
    }
    if (!init_storage_manager()) {
        return false;
    }

    // init main camera settings
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
    _settings[kIrTemperature] = init_ir_temperature();
    _settings[kAIFunction] = init_ai_function();

    auto osd_display = _camera_param.get_value(kOSDDisplay);
    if (osd_display != "0" && osd_display != "1") {
        osd_display = "0";
        _camera_param.set_value(kOSDDisplay, osd_display);
    }
    _settings[kOSDDisplay] = osd_display;

    const auto init_osd_coordinate = [this](const std::string &key, int32_t default_value) {
        const auto stored_value = _camera_param.get_value(key);
        const auto value = parse_int32(stored_value);
        if (value.has_value()) {
            return *value;
        }
        _camera_param.set_value(key, std::to_string(default_value));
        return default_value;
    };
    _osd_start_x = init_osd_coordinate(kOSDStartX, 260);
    _osd_start_y = init_osd_coordinate(kOSDStartY, 160);

    init_laser_sensor();
    init_backend_thread();

    read_preview_stream_from_device(_preview_stream_width, _preview_stream_height,
                                    _preview_stream_frame_rate);
    read_preview_stream_bitrate_from_device(_preview_stream_bitrate);
    base::LogInfo() << "Preview stream: " << _preview_stream_width << "x" << _preview_stream_height
                    << "@" << _preview_stream_frame_rate << ", " << _preview_stream_bitrate
                    << " bps";

    _detection_server.set_callback(
        [this](const TrackingFrame &frame) { tracking_callback(frame); });

    base::LogDebug() << "Init settings :";
    for (const auto &setting : _settings) {
        base::LogDebug() << "  - " << setting.first << " : " << setting.second;
    }
    return true;
}

void CameraLocalClient::capture_callback(mav_camera::MAVFrame *main_frame,
                                         mav_camera::MAVFrame *telephoto_frame,
                                         ir_camera::IRFrame *ir_frame) {
    std::unique_lock<std::mutex> callback_lock(_capture_callback_mutex);
    if (_capture_switching) {
        return;
    }
    if (_render_bridge == nullptr) {
        return;
    }
    if (_preview_type == PreivewStreamType::MainOnly && main_frame != nullptr) {
        _render_bridge->draw_rgb_frame_in_full_screen((uint8_t *)main_frame->vaddr,
                                                      main_frame->width, main_frame->height,
                                                      main_frame->stride, main_frame->slice);
    } else if (_preview_type == PreivewStreamType::TelephotoOnly && telephoto_frame != nullptr) {
        _render_bridge->draw_rgb_frame_in_full_screen(
            (uint8_t *)telephoto_frame->vaddr, telephoto_frame->width, telephoto_frame->height,
            telephoto_frame->stride, telephoto_frame->slice);
    } else if (_preview_type == PreivewStreamType::InfraredStreamOnly && ir_frame != nullptr) {
        _render_bridge->draw_ir_frame_in_full_screen(
            ir_frame->vaddr, ir_frame->width, ir_frame->height, ir_frame->width, ir_frame->height);
    } else if (_preview_type == PreivewStreamType::SideBySide) {
        if (main_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_left((uint8_t *)main_frame->vaddr, main_frame->width,
                                                   main_frame->height, main_frame->stride,
                                                   main_frame->slice);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_right(ir_frame->vaddr, ir_frame->width,
                                                   ir_frame->height, ir_frame->width,
                                                   ir_frame->height);
        }
    } else if (_preview_type == PreivewStreamType::PIP) {
        if (main_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_PIP((uint8_t *)main_frame->vaddr, main_frame->width,
                                                  main_frame->height, main_frame->stride,
                                                  main_frame->slice);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_PIP(ir_frame->vaddr, ir_frame->width, ir_frame->height,
                                                 ir_frame->width, ir_frame->height);
        }
    } else if (_preview_type == PreivewStreamType::Superimpose) {
        if (main_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_superimpose(
                (uint8_t *)main_frame->vaddr, main_frame->width, main_frame->height,
                main_frame->stride, main_frame->slice, _render_mode);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_superimpose(ir_frame->vaddr, ir_frame->width,
                                                         ir_frame->height, ir_frame->width,
                                                         ir_frame->height);
        }
    } else if (_preview_type == PreivewStreamType::Mix) {
        if (main_frame != NULL) {
            _render_bridge->draw_rgb_frame_in_mix((uint8_t *)main_frame->vaddr, main_frame->width,
                                                  main_frame->height, main_frame->stride,
                                                  main_frame->slice, _render_mode);
        }
        if (ir_frame != NULL) {
            _render_bridge->draw_ir_frame_in_mix(ir_frame->vaddr, ir_frame->width, ir_frame->height,
                                                 ir_frame->width, ir_frame->height);
        }
    }

    if (_settings[kAIFunction] == "1") {
        TrackingFrame tracking_frame;
        bool has_tracking_frame = false;
        {
            std::lock_guard<std::mutex> lock(_tracking_frame_mutex);
            tracking_frame = _tracking_frame;
            has_tracking_frame = _has_tracking_frame;
        }

        std::vector<BoundingBox> boxes;
        if (has_tracking_frame) {
            boxes.reserve(tracking_frame.objects.size());
            for (const auto &object : tracking_frame.objects) {
                BoundingBox box;
                box.x = object.x;
                box.y = object.y;
                box.width = object.width;
                box.height = object.height;
                box.label = object.name;
                boxes.emplace_back(std::move(box));
            }
        }
        _render_bridge->draw_bounding_boxes(boxes);
    }

    if (_settings[kOSDDisplay] != "1") {
        _render_bridge->draw_osd_texts({});
        return;
    }

    ///< draw osd info
    const int32_t current_iso = _current_iso.load();
    const float current_shutter_speed = _current_shuter_speed.load();
    std::vector<std::tuple<int32_t, int32_t, std::string>> texts;
    const int32_t start_x = _osd_start_x;
    const int32_t start_y = _osd_start_y;
    if (current_iso >= 0) {
        texts.emplace_back(start_x, start_y, "ISO: " + std::to_string(current_iso));
    }
    if (current_shutter_speed >= 0) {
        texts.emplace_back(start_x, start_y + 30,
                           "ShutterSpeed: " + std::to_string(current_shutter_speed) + " s");
    }

    const int laser_distance_raw = _laser_distance_raw.load();
    if (laser_distance_raw >= 0) {
        std::ostringstream laser_text;
        laser_text << std::fixed << std::setprecision(1)
                   << "Distance: " << (laser_distance_raw / 10.0f) << " m";
        texts.emplace_back(start_x, start_y + 80, laser_text.str());
    }

    if (_settings[kIrTemperature] == "1") {
        texts.emplace_back(start_x, start_y + 140, "Temperature ");

        std::ostringstream max_temperature_text;
        max_temperature_text << std::fixed << std::setprecision(1)
                             << "Max: " << _ir_temperature_max.load() << " ℃";
        texts.emplace_back(start_x, start_y + 180, max_temperature_text.str());

        std::ostringstream min_temperature_text;
        min_temperature_text << std::fixed << std::setprecision(1)
                             << "Min: " << _ir_temperature_min.load() << " ℃";
        texts.emplace_back(start_x, start_y + 210, min_temperature_text.str());

        std::ostringstream ave_temperature_text;
        ave_temperature_text << std::fixed << std::setprecision(1)
                             << "Average: " << _ir_temperature_ave.load() << " ℃";
        texts.emplace_back(start_x, start_y + 240, ave_temperature_text.str());
    }
    _render_bridge->draw_osd_texts(texts);
}

void CameraLocalClient::tracking_callback(const TrackingFrame &frame) {
    std::lock_guard<std::mutex> lock(_tracking_frame_mutex);
    _tracking_frame = frame;
    _has_tracking_frame = true;
}

void CameraLocalClient::deinit() {
    {
        std::lock_guard<std::mutex> lock(_capture_callback_mutex);
        _capture_switching = true;
    }
    stop_ir_temperature();
    set_ai_function("0");
    free_laser_sensor();
    free_backend_thread();
    {
        std::lock_guard<std::mutex> lock(_action_mutex);
        free_main_camera(true);
        free_telephoto_camera(true);
    }
    free_ir_camera();
    free_render_bridge();
    free_storage_manager();
}

bool CameraLocalClient::init_laser_sensor() {
    if (_laser_shm_data != nullptr) {
        return true;
    }

    _laser_shm_fd = ::shm_open(kLaserShmName, O_RDONLY, 0);
    if (_laser_shm_fd < 0) {
        base::LogError() << "Failed to open laser shared memory " << kLaserShmName << ": "
                         << strerror(errno);
        return false;
    }

    _laser_shm_data =
        ::mmap(nullptr, sizeof(LaserSharedMemory), PROT_READ, MAP_SHARED, _laser_shm_fd, 0);
    if (_laser_shm_data == MAP_FAILED) {
        base::LogError() << "Failed to map laser shared memory " << kLaserShmName << ": "
                         << strerror(errno);
        _laser_shm_data = nullptr;
        ::close(_laser_shm_fd);
        _laser_shm_fd = -1;
        return false;
    }

    base::LogInfo() << "Laser shared memory initialized from " << kLaserShmName;
    return true;
}

void CameraLocalClient::free_laser_sensor() {
    _laser_distance_raw = -1;
    if (_laser_shm_data != nullptr) {
        ::munmap(_laser_shm_data, sizeof(LaserSharedMemory));
        _laser_shm_data = nullptr;
    }
    if (_laser_shm_fd >= 0) {
        ::close(_laser_shm_fd);
        _laser_shm_fd = -1;
    }
}

bool CameraLocalClient::init_backend_thread() {
    if (_backend_thread.joinable()) {
        return true;
    }

    _backend_running = true;
    _backend_thread = std::thread(&CameraLocalClient::backend_read_loop, this);
    return true;
}

void CameraLocalClient::free_backend_thread() {
    _backend_running = false;
    if (_backend_thread.joinable()) {
        _backend_thread.join();
    }
}

void CameraLocalClient::backend_read_loop() {
    auto delay_time = std::chrono::milliseconds(100);
    auto next_laser_retry_time = std::chrono::steady_clock::now();
    while (_backend_running) {
        if (_laser_shm_data == nullptr &&
            std::chrono::steady_clock::now() >= next_laser_retry_time) {
            init_laser_sensor();
            next_laser_retry_time = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }

        if (_laser_shm_data != nullptr) {
            const auto *shared = static_cast<const LaserSharedMemory *>(_laser_shm_data);
            LaserSharedMemory snapshot{};
            std::uint32_t seq_begin = 0;
            std::uint32_t seq_end = 0;

            do {
                seq_begin = shared->sequence;
                snapshot = *shared;
                seq_end = shared->sequence;
            } while ((seq_begin != seq_end) || ((seq_begin & 1U) != 0U));

            if (snapshot.sensor_status == 1) {
                _laser_distance_raw = static_cast<int>(snapshot.distance_mm);
                // base::LogDebug() << "distance is " << _laser_distance_raw;
            }
        }

        {
            std::lock_guard<std::mutex> lock(_action_mutex);
            auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
            if (rgb_camera != nullptr) {
                auto [result1, iso] = rgb_camera->get_iso();
                if (result1 == mav_camera::Result::Success) {
                    _current_iso = iso;
                }
                auto [result2, shutter_speed] = rgb_camera->get_shutter_speed();
                if (result2 == mav_camera::Result::Success) {
                    auto temp_value = std::stof(shutter_speed);
                    float factor = 1e6f;
                    _current_shuter_speed = std::round(temp_value * factor) / factor;
                }
            }
        }
        std::this_thread::sleep_for(delay_time);
    }
}

bool CameraLocalClient::init_main_camera() {
    base::LogDebug() << "call main camera init";
    if (_main_camera != nullptr) {
        return true;
    }
    bool opened_library = false;
    if (_main_camera_handle == NULL) {
        _main_camera_handle = dlopen(QCOM_CAMERA_LIBERAY, RTLD_NOW);
        if (_main_camera_handle == NULL) {
            char const *err_str = dlerror();
            base::LogError() << "load module " << QCOM_CAMERA_LIBERAY << " failed "
                             << (err_str != NULL ? err_str : "unknown");
            return false;
        }
        opened_library = true;
    }

    typedef mav_camera::MavCamera *(*create_qcom_camera_fun)();
    create_qcom_camera_fun create_camera_fun =
        (create_qcom_camera_fun)dlsym(_main_camera_handle, "create_qcom_camera");
    if (create_camera_fun == NULL) {
        base::LogError() << "cannot find symbol create_qcom_camera";
        if (opened_library) {
            dlclose(_main_camera_handle);
            _main_camera_handle = NULL;
        }
        return false;
    }

    _main_camera = create_camera_fun();
    if (_main_camera == nullptr) {
        base::LogError() << "cannot create mav camera instance";
        if (opened_library) {
            dlclose(_main_camera_handle);
            _main_camera_handle = NULL;
        }
        return false;
    }

    _main_camera->set_log_path("/data/camera/main_cam.log");
    mav_camera::Result result = _main_camera->prepare();
    if (result != mav_camera::Result::Success) {
        base::LogDebug() << "cannot find main camera";
        delete _main_camera;
        _main_camera = nullptr;
        if (opened_library) {
            dlclose(_main_camera_handle);
            _main_camera_handle = NULL;
        }
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
    options.camera_id = 0;
    options.enable_shared_preview_frame = true;
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
            auto [_, photo_resolution_mode] = _main_camera->get_photo_resolution_mode();
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
        auto [_, video_resolution_mode] = _main_camera->get_video_resolution_mode();
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
    _main_camera->set_capture_callback(MainCameraCallback, this);
    result = _main_camera->open(options);
    if (result == mav_camera::Result::Success) {
        base::LogDebug() << "open qcom camera success";
    } else {
        free_main_camera(opened_library);
    }
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::init_telephoto_camera() {
    base::LogDebug() << "call telephoto init";
    if (_telephoto_camera != nullptr) {
        return true;
    }
    bool opened_library = false;
    if (_telephoto_camera_handle == NULL) {
        _telephoto_camera_handle = dlopen(QCOM_CAMERA_LIBERAY, RTLD_NOW);
        if (_telephoto_camera_handle == NULL) {
            char const *err_str = dlerror();
            base::LogError() << "load module " << QCOM_CAMERA_LIBERAY << " failed "
                             << (err_str != NULL ? err_str : "unknown");
            return false;
        }
        opened_library = true;
    }

    typedef mav_camera::MavCamera *(*create_qcom_camera_fun)();
    create_qcom_camera_fun create_camera_fun =
        (create_qcom_camera_fun)dlsym(_telephoto_camera_handle, "create_qcom_camera");
    if (create_camera_fun == NULL) {
        base::LogError() << "cannot find symbol create_qcom_camera";
        if (opened_library) {
            dlclose(_telephoto_camera_handle);
            _telephoto_camera_handle = NULL;
        }
        return false;
    }

    _telephoto_camera = create_camera_fun();
    if (_telephoto_camera == nullptr) {
        base::LogError() << "cannot create telephoto camera instance";
        if (opened_library) {
            dlclose(_telephoto_camera_handle);
            _telephoto_camera_handle = NULL;
        }
        return false;
    }

    _telephoto_camera->set_log_path("/data/camera/telephoto_cam.log");
    mav_camera::Result result = _telephoto_camera->prepare();
    if (result != mav_camera::Result::Success) {
        base::LogDebug() << "cannot find telephoto camera";
        delete _telephoto_camera;
        _telephoto_camera = nullptr;
        if (opened_library) {
            dlclose(_telephoto_camera_handle);
            _telephoto_camera_handle = NULL;
        }
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
    options.camera_id = 1;
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
            auto [_, photo_resolution_mode] = _telephoto_camera->get_photo_resolution_mode();
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
        auto [_, video_resolution_mode] = _telephoto_camera->get_video_resolution_mode();
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
    _telephoto_camera->set_capture_callback(TelephotoRGBCaptureCallback, this);
    result = _telephoto_camera->open(options);
    if (result == mav_camera::Result::Success) {
        base::LogDebug() << "open telephoto camera success";
    } else {
        free_telephoto_camera(opened_library);
    }
    return result == mav_camera::Result::Success;
}

void CameraLocalClient::free_main_camera(bool unload_library) {
    if (_main_camera != nullptr) {
        _main_camera->close();
        delete _main_camera;
        _main_camera = nullptr;
    }
    if (unload_library && _main_camera_handle != NULL) {
        dlclose(_main_camera_handle);
        _main_camera_handle = NULL;
    }
}

void CameraLocalClient::free_telephoto_camera(bool unload_library) {
    if (_telephoto_camera != nullptr) {
        _telephoto_camera->close();
        delete _telephoto_camera;
        _telephoto_camera = nullptr;
    }
    if (unload_library && _telephoto_camera_handle != NULL) {
        dlclose(_telephoto_camera_handle);
        _telephoto_camera_handle = NULL;
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_mode(set_mode);
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
    // TODO (thomas) : need repair
    if (true || store_display_mode.empty()) {
        // default display mode is RGB
        _preview_type = mavcam::PreivewStreamType::MainOnly;
        std::string string_type = std::to_string(static_cast<int>(_preview_type));
        _camera_param.set_value(kCameraDisplayModeName, string_type);
        return string_type;
    } else {
        set_camera_display_mode(store_display_mode);
        return store_display_mode;
    }
}

bool CameraLocalClient::set_camera_display_mode(std::string mode) {
    int mode_value = 0;
    const auto parse_result = std::from_chars(mode.data(), mode.data() + mode.size(), mode_value);
    if (parse_result.ec != std::errc{} || parse_result.ptr != mode.data() + mode.size() ||
        mode_value < 0 || mode_value > 6) {
        base::LogError() << "Invalid camera display mode " << mode;
        return false;
    }

    auto temp_preview_type = static_cast<PreivewStreamType>(mode_value);
    PreivewStreamType previous_preview_type;

    // close() may race with a pending capture callback in the camera library.
    // Block new callbacks and wait for the callback currently in progress by
    // taking this mutex before tearing down either camera.
    {
        std::lock_guard<std::mutex> lock(_capture_callback_mutex);
        if (_preview_type == temp_preview_type) {
            return true;
        }
        previous_preview_type = _preview_type;
        _capture_switching = true;
    }

    bool switch_success = true;
    // switch to telephoto mode
    if (temp_preview_type == PreivewStreamType::TelephotoOnly) {
        free_main_camera();
        switch_success = init_telephoto_camera();
        if (!switch_success) {
            // Keep a valid camera selected if the target camera cannot open.
            init_main_camera();
        }
    } else if (_preview_type == PreivewStreamType::TelephotoOnly) {
        base::LogDebug() << "switch back to main mode";
        free_telephoto_camera();
        switch_success = init_main_camera();
        if (!switch_success) {
            init_telephoto_camera();
        }
    }

    {
        std::lock_guard<std::mutex> lock(_capture_callback_mutex);
        _preview_type = switch_success ? temp_preview_type : previous_preview_type;
        _capture_switching = false;
    }
    return switch_success;
}

bool CameraLocalClient::set_photo_resolution(std::string value) {
    mav_camera::PhotoResolutionMode mode;
    if (value == "0") {
        mode = mav_camera::PhotoResolutionMode::Full;
    } else if (value == "1") {
        mode = mav_camera::PhotoResolutionMode::Quarter;
    }
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_photo_resolution_mode(mode);
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_video_resolution_mode(mode);
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_jpeg_quality(jpeg_quality);
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_photo_format(photo_format);
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
        auto [result, value] = _main_camera->get_white_balance();
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    if (mode == "0") {  // Auto
        result = rgb_camera->set_white_balance(mav_camera::kAutoWhitebalanceValue);
    } else if (mode == "1") {  // Daylight
        result = rgb_camera->set_white_balance(5500);
    } else if (mode == "2") {  // Cloudy
        result = rgb_camera->set_white_balance(6500);
    } else if (mode == "3") {  // Shady
        result = rgb_camera->set_white_balance(7500);
    } else if (mode == "4") {  // Incandescent
        result = rgb_camera->set_white_balance(2700);
    } else if (mode == "5") {  // Fluorescent
        result = rgb_camera->set_white_balance(4000);
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    if (mode == "0") {
        result = rgb_camera->set_ae_mode(mav_camera::AEMode::Auto);
    } else {
        result = rgb_camera->set_ae_mode(mav_camera::AEMode::Manual);
    }
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_exposure_value() {
    auto store_ev = _camera_param.get_value(kEVName);
    if (store_ev.empty()) {
        std::string ev = "0.0";
        auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
        auto [result, value] = rgb_camera->get_exposure_value();
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_exposure_value(std::stof(exposure_value));
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_iso() {
    auto store_iso = _camera_param.get_value(kISOName);
    if (store_iso.empty()) {
        auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
        auto [result, value] = rgb_camera->get_iso();
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_iso(std::stoi(iso));
    return result == mav_camera::Result::Success;
}

std::string CameraLocalClient::init_shutter_speed() {
    auto store_shutter_speed = _camera_param.get_value(kShutterSpeedName);
    if (store_shutter_speed.empty()) {
        std::string shutter_speed = "";
        auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
        auto [result, value] = rgb_camera->get_shutter_speed();
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_shutter_speed(shutter_speed);
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_metering_mode(metering_mode);
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
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_sharpness(convert_sharpness);
    if (result != mav_camera::Result::Success) {
        base::LogError() << "Failed to set sharpness : " << sharpness;
    }
    return result == mav_camera::Result::Success;
}

bool CameraLocalClient::set_ae_lock(std::string value) {
    int32_t ae_lock = std::stoi(value);
    auto rgb_camera = (_main_camera != nullptr) ? _main_camera : _telephoto_camera;
    auto result = rgb_camera->set_ae_lock(ae_lock != 0);
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
    stop_ir_temperature();
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
            ffc_mode = "0";
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

std::string CameraLocalClient::init_ir_temperature() {
    auto store_ir_temperature = _camera_param.get_value(kIrTemperature);
    if (store_ir_temperature.empty()) {
        store_ir_temperature = "0";
        _camera_param.set_value(kIrTemperature, store_ir_temperature);
    }

    if (!set_ir_temperature(store_ir_temperature)) {
        store_ir_temperature = "0";
        set_ir_temperature(store_ir_temperature);
        _camera_param.set_value(kIrTemperature, store_ir_temperature);
    }
    return store_ir_temperature;
}

bool CameraLocalClient::set_ir_temperature(std::string mode) {
    if (mode == "1") {
        if (_ir_camera == nullptr) {
            base::LogError() << "Cannot enable ir temperature without ir camera";
            return false;
        }
        if (_ir_temperature_thread.joinable()) {
            return true;
        }
        if (!_ir_camera->open_thermal_function()) {
            base::LogError() << "Open ir temperature function failed";
            return false;
        }
        _ir_temperature_running = true;
        _ir_temperature_thread = std::thread(&CameraLocalClient::ir_temperature_loop, this);
        return true;
    }

    if (mode == "0") {
        stop_ir_temperature();
        return true;
    }

    base::LogError() << "Invalid ir temperature mode " << mode;
    return false;
}

void CameraLocalClient::stop_ir_temperature() {
    bool was_running = _ir_temperature_running.exchange(false);
    bool had_thread = _ir_temperature_thread.joinable();
    if (_ir_temperature_thread.joinable()) {
        _ir_temperature_thread.join();
    }
    if ((was_running || had_thread) && _ir_camera != nullptr) {
        _ir_camera->close_thermal_function();
    }
}

void CameraLocalClient::ir_temperature_loop() {
    const ir_camera::Rect measure_rect{0, 0, 640, 512};
    while (_ir_temperature_running) {
        if (_ir_camera != nullptr) {
            ir_camera::ThermalTemperatures temperatures{};
            if (_ir_camera->measure_temperature(measure_rect, &temperatures)) {
                _ir_temperature_min = temperatures.min_temperature;
                _ir_temperature_max = temperatures.max_temperature;
                _ir_temperature_ave = temperatures.ave_temperature;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

std::string CameraLocalClient::init_ai_function() {
    auto store_ai_function = _camera_param.get_value(kAIFunction);
    if (store_ai_function.empty()) {
        store_ai_function = "0";
        _camera_param.set_value(kAIFunction, store_ai_function);
        return store_ai_function;
    }

    if (store_ai_function == "1") {
        set_ai_function(store_ai_function);
    }
    return store_ai_function;
}

bool CameraLocalClient::set_ai_function(std::string mode) {
    if (mode == "1") {
        _settings[kAIFunction] = "1";
        {
            std::lock_guard<std::mutex> lock(_tracking_frame_mutex);
            _tracking_frame = TrackingFrame{};
            _has_tracking_frame = false;
        }

        if (!_detection_server.running() &&
            !_detection_server.start(kTrackingAddress, kTrackingPort)) {
            base::LogError() << "Failed to start tracking server";
            _settings[kAIFunction] = "0";
            return false;
        }

        int ret = std::system("systemctl start object_detection.service");
        if (ret != 0) {
            base::LogError() << "Failed to start object_detection.service, ret: " << ret;
            _detection_server.stop();
            _settings[kAIFunction] = "0";
            return false;
        }
        return true;
    }

    _settings[kAIFunction] = "0";
    int ret = std::system("systemctl stop object_detection.service");
    if (ret != 0) {
        base::LogWarn() << "Failed to stop object_detection.service, ret: " << ret;
    }
    _detection_server.stop();
    {
        std::lock_guard<std::mutex> lock(_tracking_frame_mutex);
        _tracking_frame = TrackingFrame{};
        _has_tracking_frame = false;
    }
    if (_render_bridge != nullptr) {
        _render_bridge->draw_bounding_boxes({});
    }
    return true;
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
