#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "led_control/led_control.h"

void notify(const std::string &message) {
    // Use system notification (Linux 'notify-send')
    std::string command = "notify-send 'mav_client Alert' '" + message + "'";
    std::system(command.c_str());
    std::cout << message << std::endl;
}

bool isProcessRunning(const std::string &processName) {
    std::string command = "pgrep -x " + processName + " > /dev/null 2>&1";
    int ret = std::system(command.c_str());

    if (ret == -1) {
        std::cerr << "Failed to execute system command." << std::endl;
        return false;  // Treat as not running if command execution fails
    }

    return (ret == 0);
}

int main() {
    const std::string processName = "mav_client";
    const int checkInterval = 5;  // in seconds

    std::cout << "Monitoring process: " << processName << std::endl;

    while (true) {
        if (!isProcessRunning(processName)) {
            notify("mav_client process has died!");
            mavcam::switch_led_mode(mavcam::LedMode::Dead);
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(checkInterval));
    }

    return 0;
}
