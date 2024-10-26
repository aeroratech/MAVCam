#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/camera/camera.h>
#include <mavsdk/plugins/ftp/ftp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

static std::string download_camera_definition_file_by_ftp(std::shared_ptr<mavsdk::System> system,
                                                          std::string file_uri);

static void do_camera_settings(mavsdk::Camera &camera);

static inline void set_camera_settings(mavsdk::Camera &camera, const std::string &name,
                                       const std::string &value);
static inline std::string get_camera_setting(mavsdk::Camera &camera, const std::string &name);

int main(int argc, const char *argv[]) {
    // we run client plugins to act as the GCS
    // to communicate with the camera server plugins.
    mavsdk::Mavsdk mavsdk{
        mavsdk::Mavsdk::Configuration{mavsdk::Mavsdk::ComponentType::GroundStation}};

    auto result = mavsdk.add_any_connection("udp://:14550");
    if (result == mavsdk::ConnectionResult::Success) {
        std::cout << "Connected success !" << std::endl;
    }
    //wait for all component added
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto prom = std::promise<std::shared_ptr<mavsdk::System>>{};
    auto fut = prom.get_future();
    mavsdk::Mavsdk::NewSystemHandle handle =
        mavsdk.subscribe_on_new_system([&mavsdk, &prom, &handle]() {
            auto system = mavsdk.systems().back();

            if (system->has_camera()) {
                std::cout << "Discovered camera from Client" << std::endl;

                // Unsubscribe again as we only want to find one system.
                mavsdk.unsubscribe_on_new_system(handle);
                prom.set_value(system);
            } else {
                std::cout << "System has no camera" << std::endl;
            }
        });

    if (fut.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
        std::cout << "No camera found, exiting" << std::endl;
        return -1;
    }

    auto system = fut.get();
    auto camera = mavsdk::Camera{system};

    auto prom_definition = std::promise<std::string>{};
    auto fut_definition = prom_definition.get_future();
    std::atomic<bool> already_get_definition{false};
    camera.subscribe_information(
        [&prom_definition, &already_get_definition](mavsdk::Camera::Information info) {
            std::cout << "Camera information:" << std::endl;
            std::cout << info << std::endl;
            if (!info.definition_file_uri.empty() && !already_get_definition) {
                already_get_definition = true;
                prom_definition.set_value(info.definition_file_uri);
            }
        });

    if (fut_definition.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
        std::cout << "Not definition file found" << std::endl;
    } else {
        std::string file_uri = fut_definition.get();
        if (file_uri.find("mftp://") == 0) {
            file_uri = file_uri.substr(7);
            std::string define_data = download_camera_definition_file_by_ftp(system, file_uri);

            camera.subscribe_current_settings([](std::vector<mavsdk::Camera::Setting> settings) {
                std::cout << "get current settings :" << std::endl;
                for (const auto &setting : settings) {
                    std::cout << "  - " << setting.setting_id << " : " << setting.option.option_id
                              << '\n';
                }
            });

            if (define_data.size() > 0) {
                auto result = camera.set_definition_data(define_data);
                std::cout << "set camera definition data result : " << result << std::endl;
                do_camera_settings(camera);
            } else {
                std::cout << "cannot download definition data file" << std::endl;
                return 1;
            }
        }
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

// demo for use mavlink ftp to download camera config file
static std::string download_camera_definition_file_by_ftp(std::shared_ptr<mavsdk::System> system,
                                                          std::string file_uri) {
    auto ftp = mavsdk::Ftp{system};
    std::filesystem::path download_path = std::filesystem::current_path().append("build");
    // std::string path
    std::cout << "Download camera define file to " << download_path << std::endl;
    auto prom = std::promise<std::string>{};
    auto fut = prom.get_future();
    ftp.download_async(
        file_uri, download_path, false,
        [&download_path, &file_uri, &prom](mavsdk::Ftp::Result result,
                                           mavsdk::Ftp::ProgressData progress_data) {
            if (result == mavsdk::Ftp::Result::Success) {
                std::cout << "download camera config file success" << std::endl;
                std::string file_path_with_name = download_path;
                file_path_with_name += "/" + file_uri.substr(file_uri.rfind("/"));
                std::cout << "download file in path " << file_path_with_name << std::endl;
                std::ifstream file_stream(file_path_with_name);
                std::stringstream buffer;
                buffer << file_stream.rdbuf();
                prom.set_value(buffer.str());
            } else if (result == mavsdk::Ftp::Result::Next) {
                std::cout << "Download progress: " << progress_data.bytes_transferred << "/"
                          << progress_data.total_bytes << " bytes" << std::endl;
            } else {
                std::cout << "Download result : " << result << std::endl;
            }
        });

    if (fut.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
        std::cout << "Download camera define file failed" << std::endl;
        return "";
    }

    return fut.get();
}

static void do_camera_settings(mavsdk::Camera &camera) {
    std::vector<std::pair<std::string, std::string>> settings;
    camera.set_mode(mavsdk::Camera::Mode::Video);
    settings.push_back({"CAM_WBMODE", "1"});
    settings.push_back({"CAM_EXPMODE", "0"});
    settings.push_back({"CAM_EV", "2.0"});
    settings.push_back({"CAM_EXPMODE", "1"});
    settings.push_back({"CAM_SHUTTERSPD", "0.01"});
    settings.push_back({"CAM_ISO", "6400"});

    for (auto &it : settings) {
        set_camera_settings(camera, it.first, it.second);
        auto value = get_camera_setting(camera, it.first);
        if (value.find(it.second, 0) != 0) {
            std::cerr << "invalid result : " << it.first << " origin value " << it.second
                      << " new value " << value << std::endl;
        }
    }

    set_camera_settings(camera, "CAM_VIDFMT", "2");
    set_camera_settings(camera, "CAM_VIDRES", "5");
}

static inline void set_camera_settings(mavsdk::Camera &camera, const std::string &name,
                                       const std::string &value) {
    mavsdk::Camera::Setting setting;
    setting.setting_id = name;
    setting.option.option_id = value;
    auto operation_result = camera.set_setting(setting);
    std::cout << "set " << name << " value : " << value << " result : " << operation_result
              << std::endl;
}

static inline std::string get_camera_setting(mavsdk::Camera &camera, const std::string &name) {
    mavsdk::Camera::Setting setting;
    setting.setting_id = name;
    auto [result, out_setting] = camera.get_setting(setting);
    std::cout << "get " << out_setting.setting_id << " value : " << out_setting.option.option_id
              << " result : " << result << std::endl;
    return out_setting.option.option_id;
}