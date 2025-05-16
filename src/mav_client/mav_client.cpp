#include "mav_client.h"

#include <mavsdk/log_callback.h>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action_server/action_server.h>
#include <mavsdk/plugins/camera_server/camera_server.h>
#include <mavsdk/plugins/ftp_server/ftp_server.h>
#include <mavsdk/plugins/mission_raw_server/mission_raw_server.h>
#include <mavsdk/plugins/param_server/param_server.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/telemetry_server/telemetry_server.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>  // for std::setprecision
#include <thread>

#include "base/log.h"
#include "camera_client.h"
#include "led_control/led_control.h"

namespace mavcam {

bool MavClient::init(std::string &connection_url, bool use_local, int32_t rpc_port,
                     std::string &ftp_root_path, bool work_as_autopilot) {
    // TODO need check connection url first
    _connection_url = connection_url;
    _rpc_port = rpc_port;
    _ftp_root_path = ftp_root_path;
    _work_as_autopilot = work_as_autopilot;

    if (use_local) {
        _camera_client = CreateLocalCameraClient();  // use local client
    } else {
        _camera_client = CreateRpcCameraClient(_rpc_port);  // use rpc client
    }
    if (_camera_client == nullptr) {
        return false;
    }
    return true;
}

bool MavClient::start_runloop() {
    auto component_type = mavsdk::Mavsdk::ComponentType::Camera;
    if (_work_as_autopilot) {
        component_type = mavsdk::Mavsdk::ComponentType::Autopilot;
    }
    auto configuration = mavsdk::Mavsdk::Configuration{component_type};
    mavsdk::Mavsdk mavsdk{configuration};
    mavsdk.set_timeout_s(5);

    auto result = mavsdk.add_any_connection(_connection_url);
    if (result != mavsdk::ConnectionResult::Success) {
        base::LogError() << "Could not establish connection: " << result;
        return false;
    }
    base::LogInfo() << "Created mav client success";

    // run camera server, param server and ftp server in camera component
    auto camera_component = mavsdk.server_component_by_type(mavsdk::Mavsdk::ComponentType::Camera);
    if (camera_component == nullptr) {
        base::LogError() << "cannot create camera component";
        return false;
    }
    auto camera_server = mavsdk::CameraServer{camera_component};
    auto param_server = mavsdk::ParamServer{camera_component};
    subscribe_camera_operation(camera_server, param_server);
    subscribe_param_operation(param_server);
    auto ftp_server = mavsdk::FtpServer{camera_component};
    ftp_server.set_root_dir(_ftp_root_path);
    base::LogInfo() << "Launch ftp server with root path " << _ftp_root_path;

    _running = true;
    switch_led_mode(LedMode::Normal);
    while (_running.load(std::memory_order_consume)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    base::LogDebug() << "quit run loop";
    return true;
}

void MavClient::stop_runloop() {
    _running.store(false, std::memory_order_release);
}

void MavClient::subscribe_camera_operation(mavsdk::CameraServer &camera_server,
                                           mavsdk::ParamServer &param_server) {
    camera_server.subscribe_system_time(
        [this](int64_t time_unix_msec) { _camera_client->set_timestamp(time_unix_msec); });

    camera_server.subscribe_zoom_range([this, &camera_server](float range) {
        auto result = _camera_client->set_zoom_range(range);
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_zoom_range(mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_zoom_range(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_take_photo([this, &camera_server](int32_t index) {
        auto result = _camera_client->take_photo(index);
        auto feedback_result = mavsdk::CameraServer::CameraFeedback::Ok;
        auto success = true;
        if (result != mavsdk::CameraServer::Result::Success) {
            feedback_result = mavsdk::CameraServer::CameraFeedback::Failed;
            success = false;
        }
        // TODO no position info for now
        auto position = mavsdk::CameraServer::Position{};
        auto attitude = mavsdk::CameraServer::Quaternion{};

        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        camera_server.respond_take_photo(feedback_result,
                                         mavsdk::CameraServer::CaptureInfo{
                                             .position = position,
                                             .attitude_quaternion = attitude,
                                             .time_utc_us = static_cast<uint64_t>(timestamp),
                                             .is_success = success,
                                             .index = index,
                                             .file_url = {},
                                         });

        // the qgc use capture status to change the take photo status
        mavsdk::CameraServer::CaptureStatus capture_status;
        _camera_client->fill_capture_status(capture_status);
        camera_server.respond_capture_status(mavsdk::CameraServer::CameraFeedback::Ok,
                                             capture_status);
    });

    camera_server.subscribe_start_video([this, &camera_server](int32_t stream_id) {
        auto result = _camera_client->start_video();
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_start_video(mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_start_video(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_stop_video([this, &camera_server](int32_t stream_id) {
        auto result = _camera_client->stop_video();
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_stop_video(mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_stop_video(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_start_video_streaming([this, &camera_server](int32_t stream_id) {
        auto result = _camera_client->start_video_streaming(stream_id);
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_start_video_streaming(
                mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_start_video_streaming(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_stop_video_streaming([this, &camera_server](int32_t stream_id) {
        auto result = _camera_client->stop_video_streaming(stream_id);
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_stop_video_streaming(
                mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_stop_video_streaming(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_set_mode([this, &camera_server](mavsdk::CameraServer::Mode mode) {
        auto result = _camera_client->set_mode(mode);
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_set_mode(mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_set_mode(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_storage_information([this, &camera_server](int32_t storage_id) {
        mavsdk::CameraServer::StorageInformation storage_information;
        _camera_client->fill_storage_information(storage_information);
        camera_server.respond_storage_information(mavsdk::CameraServer::CameraFeedback::Ok,
                                                  storage_information);
    });

    camera_server.subscribe_capture_status([this, &camera_server](int32_t reserved) {
        mavsdk::CameraServer::CaptureStatus capture_status;
        _camera_client->fill_capture_status(capture_status);
        camera_server.respond_capture_status(mavsdk::CameraServer::CameraFeedback::Ok,
                                             capture_status);
    });

    camera_server.subscribe_format_storage([this, &camera_server](int storage_id) {
        auto result = _camera_client->format_storage(storage_id);
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_format_storage(mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            camera_server.respond_format_storage(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_reset_settings([this, &camera_server, &param_server](int camera_id) {
        auto result = _camera_client->reset_settings();
        if (result != mavsdk::CameraServer::Result::Success) {
            camera_server.respond_reset_settings(mavsdk::CameraServer::CameraFeedback::Failed);
        } else {
            //reset settings need fill param again
            fill_param(param_server);
            camera_server.respond_reset_settings(mavsdk::CameraServer::CameraFeedback::Ok);
        }
    });

    camera_server.subscribe_settings([this, &camera_server](int reserved) {
        mavsdk::CameraServer::Settings settings;
        auto result = _camera_client->fill_settings(settings);
        camera_server.respond_settings(settings);
    });
    // Then set the initial state of everything.

    // Finally call set_information() to "activate" the camera plugin.
    mavsdk::CameraServer::Information information;
    _camera_client->fill_information(information);
    auto ret = camera_server.set_information(information);
    if (ret != mavsdk::CameraServer::Result::Success) {
        base::LogError() << "Failed to set camera info";
    } else {
        base::LogInfo() << "Success set camera info";
    }

    // fill video stream info
    std::vector<mavsdk::CameraServer::VideoStreamInfo> video_stream_infos;
    _camera_client->fill_video_stream_info(video_stream_infos);
    if (video_stream_infos.size() > 0) {
        ret = camera_server.set_video_stream_info(video_stream_infos);
    }
}

void MavClient::subscribe_param_operation(mavsdk::ParamServer &param_server) {
    param_server.subscribe_changed_param_float([this](mavsdk::ParamServer::FloatParam float_param) {
        base::LogDebug() << "param server change float " << float_param.name << " to "
                         << float_param.value;
        mavsdk::Camera::Setting setting;
        setting.setting_id = float_param.name;
        setting.option.option_id = std::to_string(float_param.value);
        _camera_client->set_setting(setting);
    });
    param_server.subscribe_changed_param_int([this](mavsdk::ParamServer::IntParam int_param) {
        base::LogDebug() << "param server change int " << int_param.name << " to "
                         << int_param.value;
        mavsdk::Camera::Setting setting;
        setting.setting_id = int_param.name;
        setting.option.option_id = std::to_string(int_param.value);
        _camera_client->set_setting(setting);
    });
    param_server.subscribe_changed_param_custom(
        [this](mavsdk::ParamServer::CustomParam custom_param) {
            base::LogDebug() << "param server change custom " << custom_param.name << " to "
                             << custom_param.value;
            mavsdk::Camera::Setting setting;
            setting.setting_id = custom_param.name;
            setting.option.option_id = custom_param.value;
            _camera_client->set_setting(setting);
        });

    fill_param(param_server);
}

void MavClient::fill_param(mavsdk::ParamServer &param_server) {
    std::vector<mavsdk::Camera::Setting> settings;
    _camera_client->retrieve_current_settings(settings);

    for (auto &setting : settings) {
        base::LogDebug() << "fill param " << setting.setting_id
                         << " to value: " << setting.option.option_id;
        // TODO hard code
        if (setting.setting_id == "CAM_EV") {
            param_server.provide_param_float(setting.setting_id,
                                             std::stof(setting.option.option_id));
        } else if (setting.setting_id == "CAM_SHUTTERSPD") {
            param_server.provide_param_custom(setting.setting_id, setting.option.option_id);
        } else {
            auto result = param_server.provide_param_int(setting.setting_id,
                                                         std::stoi(setting.option.option_id));
        }
    }
}

}  // namespace mavcam
